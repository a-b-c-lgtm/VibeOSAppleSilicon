# Chapter 5 — Exception vectors, ESR, and the panic path

> **Milestone in this chapter:** 1 — vectors + MMU.
> **Code referenced:** [kernel/arch/vectors.S](../../../kernel/arch/vectors.S),
> [kernel/core/exception.c](../../../kernel/core/exception.c),
> [kernel/core/exception.h](../../../kernel/core/exception.h),
> [kernel/arch/boot.s](../../../kernel/arch/boot.s),
> [kernel/core/main.c](../../../kernel/core/main.c) (`DEMO_FAULT`).
>
> **At the end of this chapter** you will have a kernel that, on
> any fault — synchronous or asynchronous, from EL1 or EL0 — halts
> with a structured panic dump on the UART. The dump tells you the
> vector slot that fired, the decoded ESR exception class, the
> faulting virtual address (when applicable), and every general-
> purpose register at the moment of the trap.

## Why this comes before the MMU

Chapter 4 mentioned that in AArch64, vectors and the MMU come up
together. Here is the reason in one sentence: the MMU is the most
likely thing in the entire kernel to fault during bring-up, and
without an installed vector table the CPU has nowhere to *put* the
fault. The result is a guest that hangs or, on HVF, an opaque
hypervisor-side crash. By installing vectors *first*, every later
mistake produces a debuggable dump on the UART.

## The vector table layout

`VBAR_EL1` (Vector Base Address Register, EL1) holds the physical
or virtual address of the EL1 vector table. The CPU expects the
table to be 2 KiB-aligned and 2 KiB long. Inside the 2 KiB are
sixteen *slots*, each exactly 128 bytes (32 instructions) wide:

| Offset | Slot | Source EL & SP | Exception kind |
|--------|------|----------------|----------------|
| 0x000  | 0    | Current EL, SP_EL0 | Synchronous (sync abort, SVC, …) |
| 0x080  | 1    | Current EL, SP_EL0 | IRQ                              |
| 0x100  | 2    | Current EL, SP_EL0 | FIQ                              |
| 0x180  | 3    | Current EL, SP_EL0 | SError                           |
| 0x200  | 4    | Current EL, SP_ELx | Synchronous                      |
| 0x280  | 5    | Current EL, SP_ELx | IRQ                              |
| 0x300  | 6    | Current EL, SP_ELx | FIQ                              |
| 0x380  | 7    | Current EL, SP_ELx | SError                           |
| 0x400  | 8    | Lower EL, AArch64  | Synchronous                      |
| 0x480  | 9    | Lower EL, AArch64  | IRQ                              |
| 0x500  | 10   | Lower EL, AArch64  | FIQ                              |
| 0x580  | 11   | Lower EL, AArch64  | SError                           |
| 0x600  | 12   | Lower EL, AArch32  | Synchronous                      |
| 0x680  | 13   | Lower EL, AArch32  | IRQ                              |
| 0x700  | 14   | Lower EL, AArch32  | FIQ                              |
| 0x780  | 15   | Lower EL, AArch32  | SError                           |

The four "Lower EL, AArch32" slots will never fire for us — we
never run AArch32 code. The "Current EL, SP_EL0" slots can fire
only if we choose to run kernel code on `SP_EL0`, which we never
do (chapter 4: we always use `EL1h`, i.e. `SP_EL1`). So in
milestone 1 we have eight slots that can realistically fire:

- Slots 4–7: a fault while the kernel itself is running (e.g. a
  bad memory access in `kernel_main`).
- Slots 8–11: a fault while userspace is running (this becomes
  meaningful in milestone 4 when we ELF-load programs).

Each slot is 128 bytes because the architecture wanted a generous
amount of room for register-saving epilogues without forcing every
handler to start with an unconditional branch. Most kernels — ours
included — *do* start each slot with a branch to a shared handler,
so we waste most of those 128 bytes.

## What the table looks like in `vectors.S`

[kernel/arch/vectors.S](../../../kernel/arch/vectors.S) is the
canonical AArch64 vector-table skeleton:

```asm
    .section .text.vectors, "ax", @progbits
    .balign 2048
    .global vector_table
vector_table:
    /* Current EL with SP_EL0 — we never run on SP_EL0, but the
       table still has to be populated. */
    .balign 0x80; mov x0, #0; b panic_entry   // sync
    .balign 0x80; mov x0, #1; b panic_entry   // irq
    .balign 0x80; mov x0, #2; b panic_entry   // fiq
    .balign 0x80; mov x0, #3; b panic_entry   // serror

    /* Current EL with SP_ELx — kernel-mode faults arrive here. */
    .balign 0x80; mov x0, #4; b panic_entry   // sync
    .balign 0x80; mov x0, #5; b panic_entry   // irq
    .balign 0x80; mov x0, #6; b panic_entry   // fiq
    .balign 0x80; mov x0, #7; b panic_entry   // serror

    /* Lower EL, AArch64 — userspace faults arrive here. */
    .balign 0x80; mov x0, #8;  b panic_entry  // sync
    .balign 0x80; mov x0, #9;  b panic_entry  // irq
    .balign 0x80; mov x0, #10; b panic_entry  // fiq
    .balign 0x80; mov x0, #11; b panic_entry  // serror

    /* Lower EL, AArch32 — never fires; still must be there. */
    .balign 0x80; mov x0, #12; b panic_entry
    .balign 0x80; mov x0, #13; b panic_entry
    .balign 0x80; mov x0, #14; b panic_entry
    .balign 0x80; mov x0, #15; b panic_entry
```

Three things to notice:

1. The whole table sits in its own section, `.text.vectors`. The
   linker script captures it with `*(.text .text.*)`, but giving
   it its own section lets us inspect alignment in the disassembly
   without searching through the rest of `.text`.
2. `.balign 2048` aligns the *table*; `.balign 0x80` between slots
   aligns each *slot*. The two assemblers need both: the table
   alignment must match `VBAR_EL1`'s 2 KiB requirement, and the
   slot alignment is what guarantees the architectural 128-byte
   stride between them.
3. Each slot is exactly two instructions: `mov x0, #N` records
   which slot fired so the C handler can name it, and `b
   panic_entry` jumps to the shared save-and-dump path. We have
   six bytes of room before we run into the next `.balign 0x80`,
   which is more than enough — we never fill it.

`vectors_init` simply takes `vector_table`'s address and writes it
to `VBAR_EL1`:

```asm
    .global vectors_init
vectors_init:
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     VBAR_EL1, x0
    isb
    ret
```

The `isb` is mandatory — without it, the CPU's view of `VBAR_EL1`
and an in-flight exception could disagree.

## `panic_entry` and the saved-frame layout

The `panic_entry` shared handler has one job: build a complete
register snapshot on the stack and call into C. It is in
`vectors.S` because it has to use `stp/ldp` directly, which we do
not want to hide inside a C function (the compiler reserves the
right to choose addressing modes). The handler:

```asm
    .macro save_context
    sub     sp, sp, #272            // 31 GP regs + pad + ELR + SPSR
    stp     x0,  x1,  [sp, #16 *  0]
    stp     x2,  x3,  [sp, #16 *  1]
    stp     x4,  x5,  [sp, #16 *  2]
    stp     x6,  x7,  [sp, #16 *  3]
    stp     x8,  x9,  [sp, #16 *  4]
    stp     x10, x11, [sp, #16 *  5]
    stp     x12, x13, [sp, #16 *  6]
    stp     x14, x15, [sp, #16 *  7]
    stp     x16, x17, [sp, #16 *  8]
    stp     x18, x19, [sp, #16 *  9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]    // x30 alone → byte 240
    mrs     x0, ELR_EL1
    mrs     x1, SPSR_EL1
    stp     x0, x1,   [sp, #256]        // ELR_EL1, SPSR_EL1
    .endm

panic_entry:
    /* Save the vector ID into a callee-saved register before
     * save_context overwrites x0 with the saved-x0 slot. */
    mov     x19, x0

    save_context

    mov     x0, x19          /* vector ID */
    mov     x1, sp           /* pointer to saved frame */
    bl      kernel_panic_from_vector
    /* unreachable: panic loops in wfe */
1:  wfe
    b       1b
```

Two details that look harmless until they bite:

- `mov x19, x0` happens *before* `save_context`. The macro writes
  the original (faulting-context) x19 into the frame as part of
  `stp x18, x19, [sp, #144]`, so the dump still shows the right
  pre-fault `x19` to the user. Only the kernel's *post-mov* copy
  of x19 (= vector ID) is the saved value the C handler reads
  back via `mov x0, x19`.
- `save_context` clobbers x0 and x1 (it uses them to fetch ELR
  and SPSR before the final `stp`). Using callee-saved x19 is
  what makes the vector ID survive the macro.

The frame layout that `save_context` writes is exactly what
`kernel/core/exception.c` declares as `struct exception_frame`:

```c
struct exception_frame {
    uint64_t x[31];   // x0 .. x30, indexes 0 .. 30
    uint64_t pad;     // padding to 16-byte align ELR/SPSR (byte 248)
    uint64_t elr;     // ELR_EL1 (byte 256)
    uint64_t spsr;    // SPSR_EL1 (byte 264)
};
```

Total = 31 × 8 + 8 + 8 + 8 = 272 bytes, which matches the `sub sp,
sp, #272` at the top of `save_context`.

The order matters: x0/x1 first (so we can use them as scratch
inside the macro is *not* a property of this layout — we already
wrote them by the time we use x0/x1 to fetch ELR/SPSR), x30 alone
(it is one register, not paired), then the 8-byte pad to bring the
ELR/SPSR pair to a 16-byte boundary so the final `stp` is legal.

## What ESR tells you

The Exception Syndrome Register (`ESR_EL1`) is the single most
important register on a fault. Its 32 bits decompose as:

| Bits   | Field | Meaning                                                  |
|--------|-------|----------------------------------------------------------|
| 31:26  | EC    | Exception class                                          |
| 25     | IL    | Instruction length (1 = 32-bit, 0 = 16-bit Thumb)        |
| 24:0   | ISS   | Class-specific syndrome                                  |

The exception-class field (EC) is the first thing the C handler
decodes:

| EC value | Meaning                                                    |
|----------|------------------------------------------------------------|
| 0x00     | Unknown reason (default for slots that fire by accident)   |
| 0x07     | SIMD/FP access trap (FP disabled)                           |
| 0x0E     | Illegal execution state                                     |
| 0x15     | SVC instruction from AArch64 (this is how syscalls arrive)  |
| 0x18     | MSR/MRS trap                                                |
| 0x20     | Instruction abort from lower EL (userspace)                 |
| 0x21     | Instruction abort from same EL (kernel)                     |
| 0x22     | PC alignment fault                                          |
| 0x24     | Data abort from lower EL (userspace)                        |
| 0x25     | Data abort from same EL (kernel)                            |
| 0x26     | Stack pointer alignment fault                                |
| 0x2F     | SError                                                      |
| 0x30     | Breakpoint from lower EL                                    |
| 0x31     | Breakpoint from same EL                                     |
| 0x32     | Software step from lower EL                                 |
| 0x33     | Software step from same EL                                  |
| 0x34     | Watchpoint from lower EL                                    |
| 0x35     | Watchpoint from same EL                                     |
| 0x3C     | BRK instruction                                              |

For the data and instruction aborts (EC 0x20, 0x21, 0x24, 0x25)
the ISS field has further structure:

- bit 6 (`WnR`): write-not-read — set on a store fault, clear on a
  load.
- bits 5:0 (`DFSC`): data fault status code. The most common
  values:

| DFSC | Meaning                                |
|------|----------------------------------------|
| 0x04 | Translation fault, level 0             |
| 0x05 | Translation fault, level 1             |
| 0x06 | Translation fault, level 2             |
| 0x07 | Translation fault, level 3             |
| 0x09 | Access flag fault, level 1             |
| 0x0D | Permission fault, level 1              |
| 0x0F | Permission fault, level 3              |
| 0x21 | Alignment fault                        |
| 0x35 | Synchronous external abort             |

`FAR_EL1` (Faulting Address Register) holds the virtual address
that the access tried to touch. For aborts that have no address
(SVC, BRK, …) `FAR_EL1`'s contents are undefined and the C dump
prints them anyway as a diagnostic only.

## The C side — `kernel_panic_from_vector`

`kernel/core/exception.c`'s entry point is straightforward: read
`ESR_EL1` and `FAR_EL1` directly with `mrs`, decode EC, dump every
field of the saved frame. It deliberately uses *only* the
`serial_putc` / `serial_puts` / `serial_puthex` helpers from
chapter 3 (the inline-asm-MMIO ones), so there is zero chance of
the panic handler itself faulting and recursing.

The handler does not attempt to recover. After printing the dump,
it spins in `wfe` forever. Chapter 14 will introduce the syscall
path and chapter 15 will introduce true kernel-recoverable faults
(page fault → on-demand zero page); until then, every fault is
fatal by design.

## Verifying the panic path — `DEMO_FAULT`

`kernel/core/main.c` has a compile-time switch:

```c
/* Set to 1 to verify the panic path by deliberately faulting on
 * an unmapped address. Useful when refactoring vectors or the MMU. */
#define DEMO_FAULT 0
```

Flip it to 1, rebuild, and run:

```text
$ make run
...
EL1 vector table installed; faults will print a panic.

[DEMO_FAULT] dereferencing 0x80000000 (unmapped) ...

############### KERNEL PANIC ###############
vector: Sync   from current EL, SP_ELx
ESR_EL1  = 0x0000000096000045
  EC     = 0x0000000000000025  Data Abort from same EL (page fault?)
  ISS    = 0x0000000000000045
FAR_EL1  = 0x0000000080000000
ELR_EL1  = 0x00000000400810bc
SPSR_EL1 = 0x00000000600003c5
x0 = 0x0000000000000004   x1 = 0x0000000080000000
x2 = 0x00000000cafebabe   ...
```

Decode that, line by line:

- **vector: Sync from current EL, SP_ELx** → slot 4. We were in
  EL1, on `SP_EL1`, taking a synchronous exception.
- **EC = 0x25** → Data Abort from same EL. Kernel mode page fault.
- **ISS = 0x45** → bit 6 (WnR) is set, so this was a *store*. The
  low six bits are `0x05`, DFSC = "translation fault, level 1".
  Translation level 1 because our L1 table has only two valid
  block entries (chapter 6); the entry for `0x80000000` is zero.
- **FAR_EL1 = 0x80000000** → exactly the address `DEMO_FAULT`
  asked us to write. `kernel_main` did `*(volatile uint32_t
  *)0x80000000 = 0xCAFEBABE;`.
- **ELR_EL1 = 0x400810bc** → the address of the faulting `str`
  instruction. `aarch64-elf-objdump -d build/kernel.elf | less`,
  search for `400810bc`, and you can see the exact instruction.
- **SPSR_EL1 = 0x600003c5** → low nibble `0x5` = `EL1h` (chapter
  4's table); the `0x3c0` part is `DAIF = 1111` (everything
  masked, the architectural state on exception entry).
- **x1 = 0x80000000, x2 = 0xCAFEBABE** → the address and value
  about to be written. The disassembly will confirm `x1` was the
  base register and `x2` was the source.

Set `DEMO_FAULT` back to 0 once you have seen the dump once.
Refactoring `vectors.S` or the MMU bring-up sequence is much
faster when you can flip this switch and immediately confirm the
fault path still works end-to-end.

## What chapter 6 adds

The vector table by itself is enough to *catch* a fault, but most
useful kernel work needs the MMU to be on. The very next thing the
boot stub does after `bl vectors_init` is `bl mmu_enable`, which
is what chapter 6 walks through line by line — and which finally
retires the milestone-0 `stp/ldp`-on-Device-memory caveat by
moving the kernel stack into Normal-Cacheable RAM.
