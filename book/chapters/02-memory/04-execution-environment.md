# Chapter 4 — The AArch64 execution environment

> **Milestone in this chapter:** 1 — vectors + MMU.
> **Code referenced:** [kernel/arch/boot.s](../../../kernel/arch/boot.s),
> [kernel/arch/vectors.S](../../../kernel/arch/vectors.S),
> [kernel/arch/mmu.S](../../../kernel/arch/mmu.S),
> [kernel/core/main.c](../../../kernel/core/main.c).
>
> **At the end of this chapter** you will know exactly what state
> the CPU is in when our kernel starts, what the four exception
> levels mean, what `PSTATE` controls, and which registers we are
> required to save and restore on every exception entry.

The next two chapters (5 — exception vectors; 6 — the MMU) build
directly on this one. Read them in order: the table of registers
you meet here is the same table the panic dump prints in chapter 5
and the same table that the MMU bring-up in chapter 6 has to
manipulate carefully.

## Exception levels

AArch64 has four privilege rings, called exception levels:

| Level | Conventional use                                           |
|-------|------------------------------------------------------------|
| EL0   | User space — applications, no access to system registers   |
| EL1   | Kernel — what we are writing                               |
| EL2   | Hypervisor — runs guest VMs (Hypervisor.framework, KVM)   |
| EL3   | Secure monitor — TrustZone, firmware                       |

Higher numbers are more privileged. An exception (interrupt, fault,
SVC instruction, …) takes the CPU from a lower EL to a higher EL,
and the `eret` instruction returns from the higher EL back to the
lower EL.

Three of those levels are interesting to us:

- **EL0** is where every application will eventually run. We get to
  EL0 in milestone 4, when we ELF-load the first user program.
- **EL1** is where the kernel runs. QEMU drops us at EL1 directly
  when we use `-kernel`, so we never have to write EL2-to-EL1 or
  EL3-to-EL1 transition code.
- **EL2** is what HVF uses to run *us*. From inside the guest we
  cannot tell HVF is there; the `mrs x0, CurrentEL` instruction
  returns 1 either way.

Within EL1 there is one more sub-distinction: the *stack pointer
selection*. EL1 can run with either:

- `SP_EL0` — the stack pointer that EL0 also uses (the "thread
  stack pointer"), or
- `SP_EL1` — a dedicated kernel stack pointer.

We always use `SP_EL1` (the architectural notation is "EL1h"; the
"h" stands for "handler"). QEMU sets us up that way by default and
our boot stub never changes it.

## Registers

AArch64 has thirty-one general-purpose 64-bit registers, `x0`
through `x30`, plus a dedicated stack pointer `SP` and program
counter `PC`. There is no `x31`; the encoding that would name it
either reads as zero (`xzr`) or names the stack pointer (`sp`),
depending on the instruction.

The lower 32 bits of each `xN` register can be accessed as `wN`.
Writing a `wN` register zeroes the upper 32 bits of the
corresponding `xN`.

The Procedure Call Standard for AArch64 (AAPCS64) divides them by
caller/callee responsibility and by purpose. Memorise the column
borders here; you will be reading them off the screen for the rest
of this book:

| Register | Role                                              |
|----------|---------------------------------------------------|
| `x0`     | First argument, return value                      |
| `x1`–`x7`| Arguments 2 through 8                             |
| `x8`     | Indirect-result location / SVC syscall number     |
| `x9`–`x15`| Caller-saved scratch                             |
| `x16`,`x17`| Intra-procedure-call scratch (linker-veneer use)|
| `x18`    | Platform register (reserved on Apple platforms)   |
| `x19`–`x28`| Callee-saved                                    |
| `x29`    | Frame pointer (callee-saved)                      |
| `x30`    | Link register (return address)                    |
| `sp`     | Stack pointer (16-byte aligned at function entry) |
| `xzr`    | Zero register (always reads as 0; writes ignored) |

Three subtleties bite often enough to mention up front:

1. **The stack pointer must be 16-byte aligned at every function
   entry and exit.** Internal misalignment is allowed, but a
   misaligned `sp` at the moment of a `bl` causes a stack-alignment
   fault if `SCTLR_EL1.SA` is set. We rely on this and never turn
   `SA` off, so misaligned-stack bugs surface immediately.
2. **`x18` is reserved on Apple platforms.** It is not reserved by
   the architecture, but the macOS userspace ABI uses it. Our
   kernel runs in its own world so this would not matter, *except*
   that the system compilers (`aarch64-elf-gcc` from Homebrew is
   one of them) will not allocate `x18`, so we lose one register
   compared to a vanilla AArch64 toolchain. Do not try to use it
   from inline assembly.
3. **`x30` (LR) is callee-saved by convention, not by hardware.**
   A non-leaf function must save and restore it, almost always by
   pairing it with `x29` (FP) in a single `stp x29, x30, [sp,
   #-N]!` at entry and `ldp x29, x30, [sp], #N` at exit. This
   pattern is the source of the milestone-0 stp/ldp gotcha you met
   in chapter 3 — `stp/ldp` against Device memory is constrained-
   unpredictable, so we cannot allow non-leaf functions to run
   until the stack is in Normal-Cacheable memory.

In addition to the GP registers there are 32 SIMD/floating-point
registers `v0`–`v31` (each 128 bits, accessible as `q`, `d`, `s`,
`h`, `b` partial views). We compile with `-mgeneral-regs-only` so
the compiler never generates SIMD instructions in kernel code; this
means we do not have to save and restore the FP state on every
exception, and we do not need to enable FP/SIMD via `CPACR_EL1`
yet. Userspace is a different story — chapter 14 brings the FP/SIMD
state machine back to life when we ELF-load programs.

## PSTATE

There is no `EFLAGS` or `RFLAGS` in AArch64. Instead, the CPU
maintains a *processor state* called PSTATE that is a logical
collection of one-bit flags scattered across several
implementation registers. The most-used PSTATE fields are:

| Field | Bits  | Meaning                                                    |
|-------|-------|------------------------------------------------------------|
| N     | 31    | Negative result                                            |
| Z     | 30    | Zero result                                                |
| C     | 29    | Carry                                                      |
| V     | 28    | Overflow                                                   |
| D     | bit 9 | Mask debug exceptions                                      |
| A     | bit 8 | Mask SError                                                |
| I     | bit 7 | Mask IRQ                                                   |
| F     | bit 6 | Mask FIQ                                                   |
| M[3:0]|       | Current mode (see below)                                   |

`D`, `A`, `I`, `F` together are called DAIF; you can read them as a
group with `mrs x0, DAIF` and toggle them with `msr DAIFSet,
#imm` / `msr DAIFClr, #imm`. We use `DAIFClr, #2` (clear the I
bit) to enable IRQs in milestone 2 and `DAIFSet, #2` to disable
them again around critical sections.

`M[3:0]` encodes both the current EL *and* whether the CPU is in
the dedicated handler-mode stack pointer (`SP_ELx`) or the EL0
stack pointer (`SP_EL0`):

| `M[3:0]` value | EL  | SP selection | Mnemonic |
|----------------|-----|--------------|----------|
| 0b0000         | EL0 | SP_EL0       | EL0t     |
| 0b0100         | EL1 | SP_EL0       | EL1t     |
| 0b0101         | EL1 | SP_EL1       | EL1h     |
| 0b1000         | EL2 | SP_EL0       | EL2t     |
| 0b1001         | EL2 | SP_EL2       | EL2h     |

Our kernel always runs in `EL1h`. You will see the panic dump in
chapter 5 print `SPSR_EL1 = 0x...3c5`; the low nibble `0x5`
matches `EL1h`, and the `0x3c0` part is `DAIF = 1111` (everything
masked, which is automatic on exception entry).

The reason PSTATE is split across system registers instead of one
flags word is that AArch64 wants exceptions to *snapshot* it. When
an exception occurs the CPU writes the current PSTATE into
`SPSR_ELx` (saved program status register) atomically with the
write of `ELR_ELx` (exception link register, which holds the
return address). On `eret` the CPU reads SPSR back into PSTATE
atomically. The split layout makes this commit one register write
each way instead of dozens.

## System registers

The system registers (`SCTLR_EL1`, `MAIR_EL1`, `TTBR0_EL1`,
`VBAR_EL1`, `ESR_EL1`, `FAR_EL1`, …) are the heart of the AArch64
programming model. There are hundreds of them; we will need a
small fraction.

You access system registers with `mrs` (move *from* system
register to GP) and `msr` (move *to* system register from GP):

```asm
mrs   x0, SCTLR_EL1     // x0 = SCTLR_EL1
orr   x0, x0, #1        // set the M bit (MMU enable)
msr   SCTLR_EL1, x0     // SCTLR_EL1 = x0
isb                     // serialise: subsequent fetches see new state
```

The `isb` ("instruction synchronisation barrier") at the end is
non-negotiable. System register writes do not take effect for
already-in-flight instructions; without an `isb` you cannot rely
on the next instruction observing the new value. We will see
`isb`s sprinkled through `mmu.S`, `vectors.S`, and the IRQ enable
sites for exactly this reason.

A handful of system registers we will meet by name in the next two
chapters:

| Register      | Purpose                                                 |
|---------------|---------------------------------------------------------|
| `SCTLR_EL1`   | System control: MMU, cache, alignment, …                 |
| `VBAR_EL1`    | Vector base address (chapter 5)                          |
| `MAIR_EL1`    | Memory attribute indirection (chapter 6)                 |
| `TCR_EL1`     | Translation control: granule, VA size, walk attrs        |
| `TTBR0_EL1`   | Page-table base for low-half (TTBR1 = high-half)         |
| `ESR_EL1`     | Exception syndrome (the "what just happened" register)   |
| `FAR_EL1`     | Faulting virtual address (for data/instruction aborts)   |
| `ELR_EL1`     | Address to return to on `eret`                            |
| `SPSR_EL1`    | Saved PSTATE                                              |
| `CurrentEL`   | Current exception level (read-only)                       |
| `MPIDR_EL1`   | Multiprocessor affinity (CPU id; read-only)              |

`mrs`/`msr` also reach a few "special" PSTATE-like targets:
`DAIF`, `DAIFSet`, `DAIFClr`, `SP_EL0`, `SPSel`, `NZCV`. Those are
not "real" system registers in the same sense, but they are
accessed with the same instructions.

## What QEMU `-kernel` hands us

Pulling the strands together, the first instruction of `_start`
runs in this state:

- **EL:** EL1.
- **PSTATE.M:** EL1h (`SP_EL1` selected).
- **PSTATE.DAIF:** all four set — debug, SError, IRQ, FIQ all masked.
- **MMU:** off (`SCTLR_EL1.M = 0`).
- **Caches:** off (`SCTLR_EL1.C = 0`, `SCTLR_EL1.I = 0`).
- **Stack pointer:** undefined — *we have to set it before any `bl`*.
- **`x0`:** physical address of the device tree blob.
- **`x1`, `x2`, `x3`:** zero.
- **`PC`:** the ELF entry point (`_start`).
- **`VBAR_EL1`:** undefined or zero — *we have to set it before any
  exception can take a useful path*.

The boot stub in [kernel/arch/boot.s](../../../kernel/arch/boot.s)
addresses these in exactly that order — it sets the stack pointer
first, then preserves the DTB pointer (`mov x19, x0`) so that
later kernel code can find it, then clears BSS, then installs the
vector table (chapter 5), then enables the MMU (chapter 6), then
calls `kernel_main(x19)`. By the time `kernel_main` runs, every
"undefined" entry above has a known value.

That is the entire execution environment. The next chapter wires
up the vector table, which is what turns a fault from "kernel
disappears into the void" into a printable panic dump.
