# Chapter 12 — Timer-driven preemption

> **Milestone in this chapter:** 5 — preemptive scheduling.
> **Code referenced:** [kernel/core/irq.c](../../../kernel/core/irq.c),
> [kernel/core/thread.c](../../../kernel/core/thread.c) (`schedule`),
> [kernel/arch/context_switch.s](../../../kernel/arch/context_switch.s),
> [kernel/core/main.c](../../../kernel/core/main.c)
> (`preemption_demo`).
>
> **At the end of this chapter** the timer ISR will call into
> the scheduler on every tick, so a thread that never calls
> `yield()` is still rotated off the CPU after at most one tick
> period (100 ms by default). The code change itself is tiny.
> The interesting content is one architectural subtlety —
> *which* `SPSR_EL1` value the resumed thread should run with —
> and the demo that surfaces it.

## The four-line change

In milestone 4 [kernel/core/irq.c](../../../kernel/core/irq.c)
ended with

```c
case TIMER_CNTV_INTID:
    timer_rearm();
    timer_tick();
    break;
…
gic_end_of_irq(intid);
```

Milestone 5 adds a single boolean and one call:

```c
int do_schedule = 0;

switch (intid) {
case TIMER_CNTV_INTID:
    timer_rearm();
    timer_tick();
    do_schedule = 1;
    break;
…
}

gic_end_of_irq(intid);

if (do_schedule)
    schedule();
```

That is the entire mechanism. `schedule()` is just an alias for
`yield()` with one defensive guard:

```c
void schedule(void)
{
    if (!g_current)
        return;     /* IRQ before thread_init() */
    yield();
}
```

The guard is there because `gic_init`, `timer_init`, and
`irqs_enable` could conceivably interleave with `thread_init`
in some future refactor. Better a no-op than a NULL deref.

### Why EOI before scheduling

The order matters: `gic_end_of_irq` *must* run before
`schedule`. If we yielded with the IRQ still acknowledged-but-not-EOI'd
the CPU interface's *running priority* would stay elevated, and
the next timer tick on the freshly-switched-in thread would be
silently masked by the GIC. The thread would run forever
without a tick, and the heartbeat would hang. EOI tells the GIC
"this IRQ is fully done, drop the running priority back" before
we hand the CPU to anyone else.

## Why this works at all

`schedule()` calls `yield()`, which calls `cswitch_to`, which
builds a 272-byte exception frame on the current stack and
`eret`s into the new thread. The current stack, at the moment
`schedule()` is called, is the *interrupted thread's* stack —
which already contains a 272-byte IRQ-entry frame at the
bottom. That is fine: the cswitch frame goes on top, the IRQ
frame stays untouched underneath, and when the thread is
eventually resumed both frames unwind correctly:

```
high addr  ┌──────────────────────────┐
           │ thread-A's normal stack  │
           ├──────────────────────────┤  ← SP at moment of IRQ
           │ IRQ-entry frame (272 B)  │
           ├──────────────────────────┤  ← SP inside irq_dispatch
           │ C frames for irq_dispatch│
           │ → schedule → yield       │
           ├──────────────────────────┤  ← SP at start of cswitch_to
           │ cswitch frame (272 B)    │
           └──────────────────────────┘  ← SP saved into A->sp
low addr
```

When thread A is later switched back to, `cswitch_to` restores
the upper 272-byte frame and `eret`s back inside `cswitch_to`
itself. From there execution unwinds: cswitch_to returns,
`yield` returns, `schedule` returns, `irq_dispatch` returns,
and `irq_entry` does its own restore + `eret` of the lower
272-byte frame. That second `eret` is what actually returns
control to the instruction A was executing when the timer fired.

This whole cascade works *because* the cooperative and
preemptive paths use the same frame shape. If we had picked the
minimal AAPCS-callee-saved layout in chapter 11, this chapter
would have been twice as long.

## The SPSR trap

There is one architectural subtlety hidden in plain sight.
When the timer fires, the CPU automatically:

* writes `ELR_EL1` ← the interrupted PC,
* writes `SPSR_EL1` ← the interrupted PSTATE,
* sets `PSTATE.{D,A,I,F} = 1`.

That last step is what makes IRQ handlers "atomic" by default
— while we are servicing one IRQ, no further IRQ can preempt
us. But when our `cswitch_to` runs from inside that handler and
synthesises a SPSR for the resumed thread, *whose* PSTATE
should it use?

The wrong answer is "the current one":

```asm
mrs     x16, daif       /* I = 1 because we are in an IRQ */
…
stp     x30, x16, [sp, #256]
```

If you do that, the resumed thread gets `SPSR_EL1.I = 1` and
`eret`s into a state where IRQs are masked. It will run forever
without another timer tick and never get preempted again. The
demo below will show two CPU-bound threads, the first of which
runs all four of its iterations to completion, then yields
cooperatively (via `thread_exit`) to the second, which does
the same — exactly as if preemption did not exist.

The right answer is to hardcode the SPSR the resumed thread
*should* see:

```asm
mov     x16, #0x345     /* M=EL1h, I=0, F=A=D=1 */
stp     x30, x16, [sp, #256]
```

This is the same `0x345` that `thread_create` plants in the
freshly-forged frame for a new thread, and the same one that a
cooperative `yield` from a normal thread would have produced
(because *its* live `DAIF.I` was 0 anyway). Hardcoding makes
all three paths agree.

This is one of those bugs that is invisible in cooperative-only
testing and immediately fatal under preemption. The lesson is
worth a sentence: **a "unified exception frame" only unifies if
the synthesised SPSR reflects the desired *resumed* state, not
the live state of the code building the frame.**

## The eret-window trap

The SPSR trap above is a story about *constructing* the saved
PSTATE. There is a second, sneakier trap about *executing* the
return path. We did not notice this one until milestone 60 — at
which point the symptom had been latent in the codebase for
months, firing roughly once every few hours of idle uptime.

### The symptom

After M60 was built, the desktop would sit happily until — at
some random point, sometimes after seconds, sometimes hours —
a wedge:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x0000000002000000   (EC = 0, "unknown")
        FAR_EL1 = 0x0000000000000000
        ELR_EL1 = 0x0000000040081128   (a *kernel* address!)
        SPSR    = 0x0000000060000340   (M = 0, EL0t)
        thread  = /bin/launcher
```

`addr2line` resolves `0x40081128` to inside `svc_entry`'s
`restore_context` macro, specifically `msr spsr_el1, x1` —
the instruction immediately after `msr elr_el1, x0`. EC = 0
means EL0 tried to execute a privileged instruction. The kernel
had handed EL0 a *kernel* PC.

### What actually happened

The aarch64 architecture auto-masks IRQs on EL0 → EL1 exception
entry. Most syscalls run with IRQs masked end-to-end, and the
race below cannot fire. But several kernel paths re-enable IRQs
while still in EL1:

* `sys_sleep_ms` does an explicit `daifclr #2` so the timer can
  tick during the sleep.
* *Any* syscall that ends up in `schedule()` — `sys_yield`,
  blocking pipe / recv / wait, `sys_gui_poll_event` when there
  is nothing to poll. `cswitch_to`'s synthesised SPSR is
  hardcoded `0x345` with `I = 0` (the previous section's fix!),
  so on resume IRQs come back on even if they were masked at
  entry.

After such a syscall returns into `restore_context`, IRQs are on
and the old macro proceeded:

```asm
msr   elr_el1, x0     ; ELR_EL1 ← user_PC
msr   spsr_el1, x1
... 18 more loads ...
eret
```

Eighteen instructions of unmasked-IRQ window between `msr elr_el1`
and `eret`. If a timer tick lands inside it:

1. The architecture writes `ELR_EL1 ← next-kernel-PC` (the next
   instruction in `restore_context`, e.g. `0x40081128`) and
   vectors to `irq_entry`.
2. `irq_entry`'s `save_context` reads the current `ELR_EL1` and
   stores it in the IRQ frame.
3. `irq_dispatch` handles the tick.
4. `irq_entry`'s own `restore_context` writes the kernel PC back
   into `ELR_EL1` and `eret`s to it. Correct so far — that *is*
   where the kernel was interrupted.
5. The original `restore_context` resumes from the kernel PC,
   continues with `msr spsr_el1, x1`, restores GPRs, `eret`s.
   But `ELR_EL1` now holds the kernel PC, **not** the user PC.
6. `eret` jumps EL0 to the kernel PC. EL0 tries `msr spsr_el1`,
   which is privileged at EL0. EC = 0 panic.

The launcher catches it because its idle loop yields about ten
times a second, and each yield gives the timer ~10 ms of
unmasked-IRQ exposure inside `restore_context`. With only one
thread to schedule, eventually a tick falls in the wrong place.

### The fix

Mask IRQs at the top of `restore_context` and across the eret
tail of `cswitch_to`:

```asm
.macro restore_context
    msr     daifset, #2     ; <-- atomic eret window
    ldp     x0,  x1,  [sp, #256]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ...
    ldp     x0,  x1,  [sp, #0]
    add     sp, sp, #272
.endm
```

`PSTATE.I` is restored from the SPSR loaded by `eret` (kernel
threads = `0x345` with `I = 0`; user threads = `0x340` with
`I = 0`), so the masking does **not** leak into the resumed
context. The cost is one instruction per exception return.

### The general rule

Every `eret` from EL1 → EL{0,1} requires a manual IRQ mask across
the `msr ELR / msr SPSR / restore-GPRs / eret` sequence. The
architecture only auto-masks on the *entry* side. The exit side
is software's problem.

This is the second time the prior section's "cswitch_to resumes
with `I = 0`" decision has bitten us. That decision was correct
— preemption requires IRQs to come back on as fast as possible
— but it expanded the set of code paths that can find themselves
running in EL1 with IRQs unmasked. The new rule of thumb:
**whenever you write a new exception-return path, mask DAIF.I
at the top, even if you think it should already be masked.**
Defence in depth costs one instruction.

## The demo

`preemption_demo` in [kernel/core/main.c](../../../kernel/core/main.c)
spawns two CPU-bound workers that *never* call `yield`:

```c
static void busy_worker(void *arg)
{
    uintptr_t iters = (uintptr_t)arg;
    const char *name = thread_current()->name;
    for (uintptr_t i = 0; i < iters; i++) {
        for (volatile uint64_t spin = 0; spin < 60000000ULL; spin++)
            __asm__ volatile("" ::: "memory");
        serial_puts("[");
        serial_puts(name);
        serial_puts("] iter ");
        serial_puthex((uint64_t)i);
        serial_puts("\n");
    }
    serial_puts("[");
    serial_puts(name);
    serial_puts("] done\n");
}
```

Each iteration burns ≈150 ms of host CPU under HVF — comfortably
more than one 100 ms tick, so each iteration is guaranteed to be
sliced at least once.

A correct run produces:

```
[thread] spawning two CPU-bound busy workers
[thread] (no yield calls — only the timer can swap them)
[busy-A] iter 0x0000000000000000
[busy-B] iter 0x0000000000000000
[busy-A] iter 0x0000000000000001
[busy-B] iter 0x0000000000000001
[busy-A] iter 0x0000000000000002
[busy-B] iter 0x0000000000000002
[busy-A] iter 0x0000000000000003
[busy-A] done
[busy-B] iter 0x0000000000000003
[busy-B] done
[thread] all workers reaped
```

Strict A-B-A-B alternation, even though neither worker ever
yields. The very last pair — `A done` immediately before
`B iter 3` — happens because A's exit reaches `thread_exit`
*before* the next tick lands and cooperatively hands the CPU
to B.

If the SPSR bug from the previous section is present, you will
instead see

```
[busy-A] iter 0
[busy-A] iter 1
[busy-A] iter 2
[busy-A] iter 3
[busy-A] done
[busy-B] iter 0
[busy-B] iter 1
[busy-B] iter 2
[busy-B] iter 3
[busy-B] done
```

— A runs to completion *first*, then B runs to completion. The
output looks the same as cooperative scheduling because, with
IRQs masked, *that is what is happening*.

## The cost of preemption

Each timer tick now incurs:

* the IRQ entry/exit overhead (push/pop 272 bytes, GIC handshake);
* a full `cswitch_to` (push/pop another 272 bytes, two `msr`s, an `eret`);
* the `irq_dispatch` and `schedule` C frames in between.

Conservatively a few hundred cycles per tick, ten ticks per
second by default — sub-microsecond cost on any modern host.
Tick frequency is a tunable: `timer_init(100)` sets the period
in milliseconds, and there is nothing magical about 100. We
will leave it as-is until we have a reason to change it (likely
when the I/O subsystem starts showing per-tick jitter).

## What chapter 13 already did

Chapter 13 introduced the kernel heap, which made
`thread_create` possible (each thread allocates a 16 KiB stack
and a struct from `kmalloc`). The book deliberately keeps
chapter 13 in part 4 ("userspace and storage") rather than
here, even though the heap is what unblocks chapter 11. The
ordering reflects the "story" of the build: heap was the last
thing milestone 3 needed *before* threading became an option.

## What chapter 14 will need

The next part of the book moves into hardware enumeration: a
DTB parser to discover the real memory map, and a real page
allocator to replace the linker's fixed 16 MiB heap reservation.
Neither touches the scheduler. We close the concurrency arc
here.

## Checkpoint

A milestone-5 build should produce the alternating output above
under HVF. The standard run command remains:

```
qemu-system-aarch64 -M virt,gic-version=3 -cpu host -accel hvf \
    -m 2G -nographic -kernel build/kernel.elf
```

If you see ABBA-style alternation and clean reap, you have
preemption. From this point on the kernel can host arbitrary
non-cooperative workloads without losing responsiveness.
