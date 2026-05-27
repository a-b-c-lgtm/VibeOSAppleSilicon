# Chapter 3 — First boot: QEMU virt, the boot stub, and PL011 UART

> **Milestone in this chapter:** 0 — boot + UART.
> **Code referenced:**
> - [kernel/arch/boot.s](../../../kernel/arch/boot.s)
> - [kernel/core/main.c](../../../kernel/core/main.c)
> - [kernel/core/serial.c](../../../kernel/core/serial.c)
> - [linker/kernel.ld](../../../linker/kernel.ld)
> - [Makefile](../../../Makefile)
>
> **At the end of this chapter** you will have a kernel that boots
> on QEMU under HVF, prints a banner over the PL011 UART, and halts
> cleanly in a low-power wait loop.

## The QEMU `-kernel` boot protocol

When QEMU starts an aarch64 guest with `-kernel build/kernel.elf`,
it does the following, in order:

1. Allocates `-m 2G` of guest RAM, mapped starting at physical
   `0x40000000`.
2. Generates a flat device-tree blob (DTB) describing the virt
   machine's hardware and writes it to physical `0x40000000`. The
   DTB is typically about 4 KiB.
3. Parses the ELF header of `build/kernel.elf`, walks its program
   headers, and copies each `PT_LOAD` segment to its declared
   physical address.
4. Sets `x0` to the DTB physical address (`0x40000000`).
5. Sets `x1`, `x2`, `x3` to zero.
6. Sets the program counter to the ELF entry point declared in the
   ELF header.
7. Drops the CPU at EL1 with the MMU off, caches off, FIQ/IRQ
   masked in PSTATE.

That is the entire boot protocol. There is no firmware, no
bootloader stage, no ACPI table. The kernel takes over on the very
first instruction.

QEMU loads our kernel at the address we ask for through the
linker script. We pick `0x40080000` — exactly 512 KiB into RAM —
because that is where Linux, U-Boot, and OVMF conventionally put
their first kernel image on the virt machine. The 512 KiB gap
between the start of RAM and the kernel is reserved for the DTB,
the initrd (when present), and a few other firmware structures.
Putting our kernel above that gap means we never have to worry
about clobbering them.

## The PL011 UART

The PL011 is ARM's reference UART, originally part of the PrimeCell
peripheral library. The QEMU virt machine exposes one at physical
address `0x09000000`, pre-configured for 115200 baud, 8 data bits,
no parity, 1 stop bit, with both FIFOs enabled.

That last point matters: because QEMU pre-configures the device, we
do not have to write any baud-divisor or line-control registers
before calling `serial_putc`. The very first store after `bl
kernel_main` will produce visible output on your terminal.

The register subset we need at this point is tiny:

| Offset | Register | Use                                    |
|--------|----------|----------------------------------------|
| `0x000` | `DR`    | Data register — write a byte to TX     |
| `0x018` | `FR`    | Flag register — TX-full and RX-empty   |

Polled output is two lines of C — but with one important wrinkle.
The naïve form

```c
#define PL011_DR  (*(volatile uint32_t *)0x09000000)
#define PL011_FR  (*(volatile uint32_t *)0x09000018)

void serial_putc(char c) {
    while (PL011_FR & FR_TXFF) { /* spin: TX FIFO full */ }
    PL011_DR = (uint32_t)(uint8_t)c;
}
```

compiles cleanly and works on TCG, but on Apple Silicon under HVF
GCC's optimizer will sometimes fold the two MMIO addresses (24
bytes apart) into one base register and emit a writeback store
like `str w1, [x2], #24`. Writeback addressing modes against
Device memory are constrained-unpredictable per ARMv8 ARM
B2.7.2, and HVF aborts the host process when the resulting fault
arrives without a usable syndrome. The detail is unpacked in the
"deferring DTB" sidebar later in this chapter; for now, the
takeaway is that every MMIO access in this book goes through two
inline-asm helpers that are structurally incapable of using
writeback forms:

```c
static inline uint32_t mmio_read32(uintptr_t addr) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}
static inline void mmio_write32(uintptr_t addr, uint32_t v) {
    __asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(addr) : "memory");
}

void serial_putc(char c) {
    while (mmio_read32(PL011_FR) & FR_TXFF) { /* spin */ }
    mmio_write32(PL011_DR, (uint32_t)(uint8_t)c);
}
```

The `[%1]` constraint with `"r"(addr)` forces a plain register
addressing mode — no pre- or post-index allowed. Each call also
re-materialises the address from its constant, which means the
optimizer never has two related MMIO bases hanging around to
fold.

Real-hardware PL011s would also need a baud-rate divisor, line
control, and a control register, but on QEMU we get all of that for
free. See `serial_init` in [serial.c](../../../kernel/core/serial.c)
for the no-op stub that keeps the API consistent for chapters where
we do need to program the device (mainly chapter 20, when we add
RX interrupt handling).

## The boot stub

QEMU drops us into 64-bit code with the program counter at our
entry symbol. We have a 64 KiB region of guest RAM identified by
the linker script as our stack, but the stack pointer register is
not initialised — *we have to set it ourselves before any `call`
instruction* (more precisely, before any function-call sequence
that needs to push a return address).

The boot stub in [kernel/arch/boot.s](../../../kernel/arch/boot.s)
does three things at this stage:

1. Saves the DTB pointer that QEMU left in `x0` into a callee-saved
   register (`x19`) so the BSS-clear loop below can clobber `x0`
   freely.
2. Loads the address of `stack_top` (the high end of the reserved
   stack region) and copies it into `sp`. AArch64 SP must be
   16-byte aligned at any "public" boundary; you get this for free
   because the linker script aligns the stack region to 16 bytes
   and reserves a multiple-of-16 size.
3. Walks `bss_start` to `bss_end` storing zeros, because QEMU does
   not pre-zero memory before loading the ELF and any uninitialised
   global must read as zero per the C standard.

Then it calls `kernel_main(dtb_phys)`. If `kernel_main` ever
returns, the stub falls through into a `wfe` halt loop. At this
point it will not return — `kernel_main` enters its own
infinite `wfe` loop after printing the banner — but defending
against an early return is cheap and keeps the boot path safe
during later refactors.

> **Note:** the version of `boot.s` shipped in the repository today
> has grown two extra steps — it installs the EL1 exception vector
> table (chapter 5) and enables the MMU (chapter 6) before calling
> `kernel_main`. The three-step form below is the initial
> kernel you write in this chapter; the file in the repo is what
> you'll have *after* finishing Part II.

The initial stub is short enough to read straight through:

```nasm
.section .text.boot, "ax"
.global _start
_start:
    mov     x19, x0                  // save DTB pointer

    adrp    x0, stack_top            // SP = stack_top
    add     x0, x0, :lo12:stack_top
    mov     sp, x0

    adrp    x0, bss_start            // zero BSS
    add     x0, x0, :lo12:bss_start
    adrp    x1, bss_end
    add     x1, x1, :lo12:bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    mov     x0, x19                  // hand DTB to C
    bl      kernel_main

.global _hang
_hang:
    wfe
    b       _hang
```

The two-instruction `adrp` + `add` sequence is how AArch64 forms
absolute addresses in position-independent code: `adrp` loads the
4 KiB-page-aligned base and `add` adds the 12-bit offset within
the page. We are not actually building a position-independent
kernel (the linker script pins us to `0x40080000`), but `adrp`
encodes more compactly than the alternatives, so it is what GCC
emits and what we follow in hand-written assembly.

## The kernel C entry

[kernel/core/main.c](../../../kernel/core/main.c) is even simpler
at this stage:

```c
void kernel_main(uint64_t dtb_phys) {
    (void)dtb_phys;       /* MMU off: do NOT format-print this yet. */

    serial_init();
    serial_puts("\n============================================================\n");
    serial_puts("osdev aarch64 — boot + PL011\n");
    serial_puts("============================================================\n");
    serial_puts("kernel_main reached — boot path is alive\n");
    serial_puts("MMU off; deferring DTB hex-dump\n");
    serial_puts("entering wfe halt loop (Ctrl-A X to quit QEMU)\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
```

> **Note:** the `main.c` in the repository today prints a much
> later banner because it has grown driver init, scheduler bring-up,
> and a userspace launcher across the rest of the book. The form
> above is what you write at this point; you'll grow it chapter
> by chapter.

The use of `wfe` ("wait for event") is deliberate. On HVF, `wfe`
yields the vCPU back to the host scheduler, so the QEMU process
sits at near-zero CPU usage instead of pegging a core spinning in
a hot loop. It is also the canonical way to "halt" an aarch64 core
that is waiting for an interrupt or a doorbell event.

You may have noticed the `dtb_phys` parameter is annotated `(void)`
and the banner explicitly says "deferring DTB hex-dump".  This is
a real constraint we hit during early boot,
documented in detail in the comment block at the top of
`main.c`. The short version is that with the MMU off there are
*two* AArch64 instructions that GCC will emit happily but that
fault unrecoverably on Apple Silicon under HVF:

1. **`stp x29, x30, [sp, #-N]!`** — paired loads/stores against
   memory that the architecture treats as Device (which is what
   you get with the MMU off) are constrained-unpredictable per
   ARMv8 ARM B2.7.2. GCC emits `stp/ldp` in any non-leaf function
   prologue, so the moment `kernel_main` calls a function whose
   prologue spills `x29`/`x30`, the host process aborts with
   `Assertion failed: (isv), function hvf_handle_exception, file
   hvf.c`. Until the MMU brings the kernel stack into
   Normal-Cacheable memory, the boot path must remain a chain of
   leaf functions only.
2. **Writeback addressing on Device memory** — even *with* the MMU
   on, the same B2.7.2 clause forbids `str w1, [x2], #N` (post-
   index) and `str w1, [x2, #N]!` (pre-index) against
   Device-mapped MMIO. The optimizer will gleefully fold two
   nearby `volatile uint32_t *` MMIO accesses into one base
   register and a writeback offset — for example PL011_DR at +0
   and PL011_FR at +0x18, exactly 24 bytes apart, can compile to
   `str w1, [x2], #24`. `volatile` preserves ordering but does
   not forbid that addressing mode. The fix is to wrap every MMIO
   load and store in inline asm with a plain `[Xn]` form so the
   compiler is structurally incapable of generating writeback
   instructions against MMIO. That is exactly how `serial.c`
   defines `mmio_read32` and `mmio_write32`.

Both gotchas are the kind of practical constraint that does not
appear in any textbook, and they are the reason this book and the
codebase travel together: the codebase comments capture the
constraint at the call site, the book chapter explains why it
exists, and chapters 4–6 are what finally retire the
first one. The second one stays with us forever — every MMIO
driver in this book uses the same `mmio_read32` / `mmio_write32`
helpers as a result.

## The linker script

[linker/kernel.ld](../../../linker/kernel.ld) is the contract
between us and QEMU. It declares:

- The entry symbol (`_start`).
- The base load address (`0x40080000`).
- Two `PT_LOAD` segments — one read-execute for code and rodata,
  one read-write for data, BSS, and the boot stack.
- The DWARF debug sections, captured as zero-VMA NOLOAD entries so
  GDB can find them but they do not bloat the runtime image.
- A `/DISCARD/` block that throws away `.note`, `.comment`,
  `.eh_frame`, vendor attributes, and the IFUNC plumbing some
  binutils versions emit by default.

The crucial line in the Makefile that makes the linker script
self-policing is:

```make
LDFLAGS  := -T linker/kernel.ld -nostdlib --orphan-handling=error
```

`--orphan-handling=error` makes the link fail loudly if any input
section is not explicitly named by the script. Without it, the
linker silently appends unknown sections after the last named
output — a class of bug that produces kernels that boot fine on
TCG, crash mysteriously on real hardware, and resist bisection
for days.

## Build and run

The Makefile has three primary targets:

```bash
make all        # build build/kernel.elf
make run        # boot under HVF (the M2 default)
make run-tcg    # boot under TCG (works on any host)
make debug      # boot under HVF, paused, GDB stub on :1234
```

A successful boot looks like this:

```text
$ make run
Running under HVF — Ctrl-A X to quit.
qemu-system-aarch64 -M virt,gic-version=3 -cpu host -accel hvf \
                    -m 2G -nographic \
                    -kernel build/kernel.elf

============================================================
osdev aarch64 — boot + PL011
============================================================
kernel_main reached — boot path is alive
MMU off; deferring DTB hex-dump
entering wfe halt loop (Ctrl-A X to quit QEMU)
```

Press `Ctrl-A X` to quit. (`Ctrl-A` is QEMU's escape prefix; `X`
is the "exit immediately" key. `Ctrl-A H` lists the rest.)

If you do not see the banner, the most likely causes are:

| Symptom                                          | Cause                                          | Fix                          |
|--------------------------------------------------|------------------------------------------------|------------------------------|
| Nothing printed at all                           | QEMU exited before booting                     | Check `make all` output      |
| `Assertion failed: (isv), function hvf_handle…`  | `stp`/`ldp` against Device memory (MMU off), or writeback addressing (`[xN], #imm` / `[xN, #imm]!`) against Device MMIO | Stay leaf-only until MMU is up; wrap MMIO in inline-asm `[Xn]` accessors |
| `qemu-system-aarch64: invalid accelerator hvf`   | QEMU built without HVF                         | `brew reinstall qemu`        |
| Banner printed, then QEMU exits                  | `kernel_main` returned without halting         | Confirm the `for(;;)` loop   |

## What you have at the end of this chapter

You have:

- A bare-metal kernel that runs natively on Apple Silicon.
- A boot stub written in AArch64 assembly that you wrote by hand.
- A working serial driver for a real ARM peripheral (the PL011).
- A clean linker script that fails the build if you ever
  accidentally introduce an unrecognised input section.
- A Makefile that supports both HVF (fast, default) and TCG
  (portable, slower) build paths.

You do not yet have:

- Exception handling.
- Memory protection (the MMU is off).
- Interrupts.
- Any concept of time.
- Threads, processes, or syscalls.
- A way to read input from the user.

All of those land in the next several chapters, in roughly that
order. The next chapter sets the stage for the AArch64 execution
environment: exception levels, registers, PSTATE, and the
mechanics of moving between EL1 and EL0. After that, in chapter 5,
we install our first exception vector table and start handling
synchronous faults — beginning with the page fault that we will
see the moment we turn the MMU on in chapter 6.
