# Chapter 14 — SVC and the syscall ABI

> **Where the code lives.**
> Vector slot: [kernel/arch/vectors.S](../../../kernel/arch/vectors.S) (slot 8 of `vector_table`)
> User → kernel trampoline: [kernel/arch/vectors.S](../../../kernel/arch/vectors.S) (`svc_entry`)
> EL1 → EL0 trampoline: [kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s) (`user_trampoline`)
> Dispatcher: [kernel/core/syscall.c](../../../kernel/core/syscall.c)
> Userspace wrappers: [userspace/libc/syscall.h](../../../userspace/libc/syscall.h)

So far every flow of control has run at EL1 — the same exception level
the kernel boots into. Threads of execution have switched between
each other, and IRQs have preempted them, but every instruction has
been a privileged one. That changes in this chapter.

The plan is short:

1. Pick an exception level for user code (EL0).
2. Pick a *channel* by which user code can ask the kernel to do
   things on its behalf (the `svc` instruction).
3. Pick a *contract* for that channel (the syscall ABI: which
   register holds the syscall number, which hold the arguments,
   how the return value comes back).
4. Add the kernel-side code that catches the SVC, decodes the
   contract, runs the right handler, and returns to the user.
5. Add the userspace-side code that talks to the kernel through
   that channel.

Step 6 — building a *user program* that exercises the new
machinery — is its own chapter: [Chapter 15](15-elf-and-first-user-program.md).

## What changes when we drop to EL0

EL0 strips two privileges away from the running code:

- **System registers become inaccessible.** Reads of `MAIR_EL1`,
  `SCTLR_EL1`, `TTBR0_EL1`, `VBAR_EL1`, etc. are illegal at EL0
  and trap to EL1. Even the small ones — `DAIFSet/Clr` to mask
  IRQs, `wfe` controls, MMU maintenance instructions — are gone.
- **Memory access goes through the user permission checks.** A
  page table entry that says "RW for EL1 only" causes a
  permission fault when accessed from EL0, even though the EL1
  kernel can still touch it without trouble.

The first restriction is the whole reason for going to EL0 in the
first place: if the kernel hands a pointer to user code and the
user can't program the MMU, the kernel can rely on the page table
being whatever it set last. The second restriction is what makes
process isolation possible — though we don't get true process
isolation until [Chapter 14B](#deferred-per-process-page-tables)
introduces per-process L1 tables.

## The vector table grows two more meaningful slots

Recall the EL1 vector table layout from Chapter 5. There are 16
slots of 128 bytes each, divided into four groups by the *source*
of the exception. We have been living in the middle two groups
("from current EL using SP_EL0" and "from current EL using
SP_ELx"). EL0 → EL1 transitions land in the third group, "from a
lower EL using AArch64":

| Offset  | Cause                              | Milestone-7 destination |
|---------|------------------------------------|-------------------------|
| 0x400   | Sync from EL0 using AArch64        | `svc_entry`             |
| 0x480   | IRQ from EL0 using AArch64         | `irq_entry`             |
| 0x500   | FIQ from EL0 using AArch64         | `panic_entry`           |
| 0x580   | SError from EL0 using AArch64      | `panic_entry`           |

The `svc` instruction at EL0 is a synchronous exception, so it
goes through 0x400. Timer interrupts that fire while we're at EL0
go through 0x480 — the same `irq_entry` we already use for IRQs
from EL1. That second part is important: we don't need a separate
preemption path for user threads, because the IRQ entry already
saves the full register frame and the same `restore_context`/`eret`
pair pops it back out, including the saved `SPSR_EL1` that was
captured on entry. If the saved `SPSR.M` says EL0, the eret goes
back to EL0 automatically.

```asm
    /* Lower EL using AArch64 — used once userspace lands. IDs 8..11. */
    .balign 0x80
    b       svc_entry           /* slot 8: Sync from EL0 — SVC + page faults */
    .balign 0x80
    b       irq_entry           /* slot 9: IRQ from EL0 — timer preemption */
    .balign 0x80
    mov     x0, #10
    b       panic_entry
    .balign 0x80
    mov     x0, #11
    b       panic_entry
```

Slot 8 catches not just SVC but every synchronous EL0 exception:
data aborts, instruction aborts, undefined instructions, alignment
faults. The dispatcher tells them apart by reading `ESR_EL1` and
inspecting the EC field — see below.

## The unified exception frame, once more

`svc_entry` reuses the very same `save_context`/`restore_context`
macros as every other vector slot. That is the single most
important design choice in this chapter. There is one frame
layout for the entire kernel — IRQ entry, SVC entry, panic entry,
context-switch entry, user thread launch — all 272 bytes, all
with the same field offsets. Mixing two flavors of frame is the
kind of subtle bug that takes hours to diagnose.

```asm
.global svc_entry
svc_entry:
    save_context
    mov     x0, sp
    bl      svc_dispatch
    restore_context
    eret
```

`save_context` pushes 272 bytes onto the kernel stack and writes
all 31 GPRs plus `ELR_EL1` and `SPSR_EL1` into them. The C
dispatcher then sees a `struct exception_frame *` containing every
register the user just had, including `x[8]` (the syscall
number) and `x[0]..x[5]` (the syscall arguments). Writing to
`frame->x[0]` becomes the syscall return value, because
`restore_context` reloads `x0` from that slot just before `eret`.

`restore_context` itself begins with a `msr daifset, #2` so the
`msr ELR / msr SPSR / restore-GPRs / eret` sequence runs as an
atomic block. Without that mask, a syscall path that re-enabled
IRQs (any caller that goes through `schedule()`, plus
`sys_sleep_ms`) would race with the next timer tick and
eventually `eret` EL0 to a kernel PC. The full diagnosis lives
in [chapter 12](../03-time-and-concurrency/12-preemption.md#the-eret-window-trap);
all you need to know here is that `restore_context` is safe to
use as the SVC return path even when the handler unmasked IRQs.

## The syscall ABI

Three decisions:

**Number register: x8.** This matches the AArch64 Linux syscall
ABI. The choice has nothing to do with Linux compatibility (we
have no intent to run Linux binaries) and everything to do with
*not having to shuffle arguments before calling the handler*. If
the syscall number lived in `x0`, the handler's `arg0` would have
to be moved up from `x1`, `arg1` from `x2`, and so on. Putting
the number in `x8` leaves AAPCS argument registers `x0..x5`
untouched, so the handler signature `long sys_foo(long a0, long
a1, ...)` already matches what the user passed in.

**Up to six arguments in x0..x5.** Six is the AAPCS integer
argument register count, which is what every C function the
handler can possibly call already understands. Larger argument
sets would require the user to push a struct and pass a pointer.

**Return value in x0.** Negative values are errno codes;
non-negative values are the syscall's actual return. This
classic UNIX convention works because the ABI specifies a signed
return type, and `errno` values stay positive on the user side
where the wrapper transforms `x0 < 0` into `errno = -x0; return
-1;`. Our wrappers are minimal enough that they don't bother with
that transformation yet.

## The dispatcher

```c
void svc_dispatch(struct exception_frame *frame)
{
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> ESR_EC_SHIFT) & 0x3F);

    if (ec != ESR_EC_SVC64) {
        /* Slot 8 caught a fault, not an SVC.  Print everything
         * we know and kill the offending thread. */
        ...
        thread_exit();
        return;
    }

    long num = (long)frame->x[8];
    long ret;
    switch (num) {
    case SYS_WRITE:  ret = sys_write(a0, a1, a2); break;
    case SYS_EXIT:   ret = sys_exit(a0);          break;
    case SYS_GETPID: ret = sys_getpid();          break;
    case SYS_YIELD:  ret = sys_yield();           break;
    default:         ret = -ENOSYS;
    }
    frame->x[0] = (uint64_t)ret;
}
```

Two details worth flagging:

- **ESR_EL1.EC = 0x15** is the encoding for "SVC instruction
  execution in AArch64 state". Every other value means the same
  vector slot caught a different kind of synchronous exception
  — most commonly a data abort (EC = 0x24) or an instruction
  abort (EC = 0x20). Returning from those without fixing the
  underlying problem would re-fault the same instruction
  forever, which is exactly the bug we ran into the first time
  we built milestone 7. The fix is to *kill the thread* (call
  `thread_exit()`) so the scheduler picks something else.
- **The argument types are `long`.** That's intentional: AArch64
  is a 64-bit ABI, and `long` is 64 bits in our build. Pointers
  also fit. We cast through `(int)` or `(size_t)` inside each
  handler as needed.

## Returning a value the user can see

`restore_context` reloads `x0` from `[sp + 0]` (the first slot of
the frame) just before `eret`. So writing to `frame->x[0]` from C
is *the* way to return a syscall value:

```c
frame->x[0] = (uint64_t)ret;
```

There is no other plumbing required. The user's `x0` becomes the
value we wrote, and the user's `x1..x30` stay exactly as they
were before `svc #0` because `save_context` saved them and
`restore_context` restored them. Subtle but correct: a
syscall must NOT clobber callee-saved registers, and our ABI
gives us that for free because we save and restore everything.

## Userspace wrappers

The user side is a couple of `static inline` functions wrapping
the syscall in inline asm:

```c
static inline long _svc1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}
```

The `register long x8 __asm__("x8")` syntax tells GCC to pin the
variable to the named register; the inline asm then references
it with `"r"(x8)`. The `"+r"(x0)` constraint says `x0` is both an
input (the first argument) and an output (the return value).
The `"memory"` clobber forces GCC to spill any pending stores
before the SVC, in case the kernel reads memory the user just
wrote. There is no need for an explicit `register` declaration on
clobbered registers because the `eret` path in `svc_entry`
preserves everything except `x0`.

Higher-level wrappers compose those primitives:

```c
static inline long write(int fd, const void *buf, size_t len) {
    return _svc3(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)len);
}

__attribute__((noreturn))
static inline void exit(int code) {
    _svc1(SYS_EXIT, code);
    __builtin_unreachable();
}
```

These wrappers live in `userspace/libc/syscall.h`. They are
header-only on purpose: the milestone-7 user binaries are too
small to justify a separate libc.a, and the inlining produces
code identical to what we'd write by hand.

## EL0 launch: the user trampoline

Every kernel thread we have spawned so far follows the same
recipe:

1. `thread_create` builds a 272-byte frame on a fresh stack.
2. The frame's `x19` holds the entry function, `x20` holds the
   argument, `ELR_EL1` slot points at `thread_trampoline`, and
   `SPSR_EL1` slot is `0x345` (EL1h, IRQs unmasked).
3. `cswitch_to` `eret`s into `thread_trampoline`, which calls the
   entry function.

A user thread needs almost the same shape, but with two changes:

- `SPSR_EL1` for the *user* code should be EL0t (`0x340`), not
  EL1h.
- `SP_EL0` (a separate register from `SP_EL1`) needs to be set to
  the user-space stack pointer.

We could change `restore_context` to handle these cases, but the
two-level nature of the launch (`cswitch_to` `eret` → kernel
trampoline → second `eret` to user) is cleaner as a dedicated
trampoline:

```asm
.global user_trampoline
user_trampoline:
    msr     sp_el0, x20         /* user SP */
    msr     elr_el1, x19        /* user PC */
    mov     x16, #0x340         /* EL0t, F=A=D=1, I=0 */
    msr     spsr_el1, x16
    /* Zero x0..x30 so user code does not see kernel state. */
    mov     x0, xzr  ...  mov   x30, xzr
    eret
```

`user_thread_create` wires this up: it creates a kernel-side
`struct thread` with its own kernel stack, builds a frame whose
`x19` is the user PC and `x20` is the user SP, and points the
frame's ELR slot at `user_trampoline` instead of
`thread_trampoline`.

The kernel stack the thread carries is *not* discarded once the
user code is running — it's the stack the SVC handler and the
timer IRQ handler use *for that user thread* on every entry into
the kernel. Without it, the very first SVC from the user would
have nowhere to push its 272-byte frame.

## Why the SPSR has to be exactly 0x340

`SPSR_EL1` packs PSTATE for the destination of the next `eret`:

| Bit  | Field | Meaning                            |
|------|-------|------------------------------------|
| 0–3  | M     | Target exception level + SP choice |
| 6    | F     | FIQ mask                           |
| 7    | I     | IRQ mask                           |
| 8    | A     | SError mask                        |
| 9    | D     | Debug mask                         |

For a user thread: `M[3:0] = 0000` (EL0 with SP_EL0), `F=1`,
`I=0` (IRQs ON so the timer can preempt), `A=1`, `D=1`. Pack:
`0x340`. For a kernel thread: `M[3:0] = 0101` (EL1h),
`F=1`, `I=0`, `A=1`, `D=1`. Pack: `0x345`.

The lesson here: the SPSR has to be **constructed**, not captured from
live `DAIF`. Capturing live `DAIF` from inside an exception handler
freezes the wrong mask state into the resumed thread.

## Verifying the ABI end-to-end

The smoke test is the `userspace_demo` in `kernel/core/main.c`.
It loads an ELF, spawns a user thread, and yields until the
thread exits. Expected serial output:

```
[user] loading embedded hello.bin (0x1230 bytes)
[user] entry = 0x22fffb000, sp = 0x22fffb000
[user] spawned pid 0x3
hello from EL0!
pid=0x00000003
after yield, still alive
[sys_exit] thread 'hello' exited with code 0x0
[user] hello exited cleanly
```

That single block exercises every milestone-7 path:

- `cswitch_to` → `user_trampoline` → `eret` to EL0
- `svc #0` from EL0 → `svc_entry` → `svc_dispatch(SYS_WRITE)` →
  return to EL0
- `svc #0` from EL0 → `svc_dispatch(SYS_GETPID)` → return value
  flows back through `frame->x[0]`
- `svc #0` from EL0 → `svc_dispatch(SYS_YIELD)` → cooperative
  context switch back to the boot thread, which `yield()`s
  again, eventually returning to the user thread
- `svc #0` from EL0 → `svc_dispatch(SYS_EXIT)` → `thread_exit()`
  → reaper frees the user thread's kernel stack

If any one of these were broken, the trace would stop short.

## Permission faults vs SVCs

The first time we tried to run the user code, the kernel printed
this on every iteration of the dispatcher:

```
[svc] non-SVC sync exception, EC = 0x24
```

EC=0x24 is "Data Abort taken from a lower EL". The user code
was hitting a permission fault on its very first stack write
because the L1 block descriptor for the user's RAM had AP[2:1]=00
(EL1-only). The fix was to mark RAM blocks above 1 GiB as
AP[2:1]=01 (EL1+EL0), but with a wrinkle: the L1[1] block (which
contains the kernel image, boot stack, and the page tables
themselves) must stay EL1-only. Re-installing L1[1] with AP=01
mid-boot hangs the CPU on both HVF and TCG.

The current `pmap_install_ram_block_1gib` refuses indexes ≤ 1
silently, so the kernel-side install loop can naively iterate
over every 1 GiB chunk in the DTB without special-casing the
kernel block.

## Deferred: per-process page tables

This chapter's design has a deliberately permissive memory model:
every user thread can read and write every page allocated from
pmem, including pages owned by other user threads or the kernel
heap. There is no isolation between processes, only between EL1
and the regions of L1[0] and L1[1] that we kept kernel-only.

Per-process L1 page tables, ASIDs, and a real `fork`/`exec`
implementation come in a later milestone (currently planned for
Chapter 17). At that point each `user_thread_create` will build
its *own* L1 table, and `cswitch_to` will swap `TTBR0_EL1` on the
way in.

## Summary

- EL0 is reached by setting `SPSR_EL1.M = 0` and `eret`ing from
  EL1 with `ELR_EL1` pointing at the user PC.
- The `svc` instruction at EL0 raises a synchronous exception
  routed through vector slot 8 (offset 0x400).
- The same `save_context`/`restore_context` macros used by every
  other vector slot work for SVC entry too. Writing to
  `frame->x[0]` is how a syscall returns a value.
- The ABI: `x8` = syscall number, `x0..x5` = arguments, `x0` =
  return value, negative values are errno.
- A two-stage trampoline (`cswitch_to` → `user_trampoline` →
  `eret`) launches a freshly created user thread.
- L1 RAM blocks above the kernel block carry AP[2:1]=01 so EL0
  can read and write them.

Next: [Chapter 15 — ELF loading and the first user program](15-elf-and-first-user-program.md).
