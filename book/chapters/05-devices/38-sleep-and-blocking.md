# Chapter 38 — Sleep and the THREAD_SLEEPING state

The shell can compose tools with `<` and inspect timing with
`time`, but every program runs to completion as fast as the
scheduler can drive it. There's no way to *pause*. Without a
sleep primitive, scripts can't stagger work, can't poll
external state, can't simulate delay. Sleep is also the
gentlest possible introduction to **blocking I/O**: a thread
gives up the CPU until a future event (here, "the wall clock
passes T") makes it ready again.

This chapter ships:

- `THREAD_SLEEPING` state on `enum thread_state`.
- A `wake_at_ms` field on `struct thread`.
- A wake-up walk inside `yield()` that re-readies any sleeper
  whose deadline has passed.
- `SYS_SLEEP_MS` (= 21) and a libc `sleep_ms()` wrapper.
- A `/bin/sleep N` user binary that takes seconds (or a
  fixed-point N.MMM).
- One subtle fix to make all of the above actually run during
  a syscall handler.

After this chapter:

```
/$ time sleep 1
[time] 1.000s real

/$ time sleep 2
[time] 2.000s real

/$ time sleep 0.5
[time] 0.500s real
```

## State machine extension

The thread state enum gets a new entry between `WAITING` and
`EXITED`:

```c
enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_WAITING,    /* blocked in wait(); not on runqueue */
    THREAD_SLEEPING,   /* blocked in sleep_ms(); woken by timer */
    THREAD_EXITED,
};
```

`SLEEPING` differs from `WAITING` only in *who wakes you*:

- `WAITING`: another thread (a child) reaches `EXITED` and the
  parent's `thread_wait` finds it during a runqueue walk.
- `SLEEPING`: a deadline (`wake_at_ms`) is reached and the
  scheduler's wake-up walk finds it.

Both states share "off the runqueue, not eligible to be picked
by `runq_pop`". The new state field on `struct thread`:

```c
uint64_t wake_at_ms;
```

is the absolute monotonic millisecond timestamp the thread
should be woken at. It's only meaningful when state ==
SLEEPING; we initialise it to 0 at all three thread-creation
sites for tidiness.

## The wake-up walk

In `yield()`, before picking the next thread, we walk every
live thread (`g_all_head` linked list) and re-ready any sleeper
whose deadline has passed:

```c
uint64_t now = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
for (struct thread *t = g_all_head; t; t = t->all_next) {
    if (t == g_current) continue;
    if (t->state == THREAD_SLEEPING && now >= t->wake_at_ms) {
        t->state = THREAD_READY;
        runq_push(t);
    }
}
```

We **skip g_current** for an important reason: we're about to
context-switch *out*, and pushing ourselves onto the runqueue
right before popping would mean either a degenerate
self-context-switch (`prev == next`) or a re-push on the
post-pop branch. Cleaner to leave g_current untouched and let
the sleep loop's wall-clock test handle it (see below).

The walk is O(n) per yield. n is small in our system (under 20
threads), so we don't bother with a sorted timer wheel.

## sleep_ms loops on the wall clock

```c
void thread_sleep_ms(uint64_t ms)
{
    if (!g_current) return;
    uint64_t target = timer_ticks() * (uint64_t)TICK_INTERVAL_MS + ms;
    while (timer_ticks() * (uint64_t)TICK_INTERVAL_MS < target) {
        g_current->wake_at_ms = target;
        g_current->state      = THREAD_SLEEPING;
        yield();
    }
    g_current->state = THREAD_RUNNING;
}
```

Loop condition tests the wall clock, not the state. That's
cheaper than fighting state-mutation races and is robust to
spurious wakeups. Each iteration:

1. Re-set state to SLEEPING and arm the deadline (in case it
   got mutated).
2. Yield.
3. On return (either context-switched in by another yield, or
   yield was a no-op because the runqueue was empty), test the
   wall clock again.

When the wall clock catches up, we exit the loop and set state
back to RUNNING.

## The IRQ-mask gotcha

This was the bug. The first attempt at sleep spun forever even
when other threads should have been advancing the tick counter.
The reason:

> SVC handlers run with IRQs masked by the architecture's
> exception-entry behaviour.

When user code does `svc #0`, the CPU takes a synchronous
exception to EL1 with `PSTATE.DAIF` fully masked (D, A, I, F
all set). The kernel's SVC handler stays in this masked state
until `eret`. So during a long-running syscall, the timer IRQ
*does not fire*. `timer_ticks()` never advances. Our sleep loop
checks the clock, sees no progress, sleeps again, and never
wakes.

Fix: unmask DAIF.I (the IRQ bit) in `sys_sleep_ms` for the
duration of the wait.

```c
static long sys_sleep_ms(long ms_signed)
{
    if (ms_signed <= 0) return 0;
    __asm__ volatile("msr daifclr, #2" ::: "memory");  /* unmask IRQs */
    thread_sleep_ms((uint64_t)ms_signed);
    __asm__ volatile("msr daifset, #2" ::: "memory");  /* re-mask */
    return 0;
}
```

The `#2` is bit 1 of the DAIF immediate field, which addresses
the I (IRQ) bit. We re-mask before returning so the rest of
the SVC return path stays in its expected IRQ-masked state.

We don't unmask globally for *all* syscalls because:

- Most syscalls are short and don't need preemption.
- Letting IRQs fire arbitrarily inside the SVC handler means
  scheduler re-entry on a per-thread kernel stack we haven't
  validated for that.
- Our existing milestone-12 preemption story works because
  yield-points are explicit; we don't need to add implicit
  ones across the entire syscall surface yet.

This is a deliberate, scoped relaxation. When pipes arrive,
`sys_read` on a pipe will need the same treatment (it can
block indefinitely). At that point we'll either spread the
unmask to several syscalls or move it to a wrapper.

## Userspace: /bin/sleep N

```c
int main(int argc, char **argv)
{
    unsigned long ms = 1000UL;
    if (argc >= 2 && argv[1] && argv[1][0]) {
        ms = parse_ms(argv[1]);
        if (ms == 0 && (argv[1][0] != '0' || argv[1][1])) {
            printf("sleep: bad duration: %s\n", argv[1]);
            return 1;
        }
    }
    sleep_ms(ms);
    return 0;
}
```

`parse_ms` accepts a decimal integer or a fixed-point
`N.MMM` (millisecond precision). Trailing digits past three
decimals are dropped. Trailing garbage is rejected.

## Verification

```
/$ time sleep 1
[time] 1.000s real

/$ time sleep 2
[time] 2.000s real

/$ time sleep 0.5
[time] 0.500s real
```

All three are accurate to within one tick (100 ms today). Any
drift you'd see compared with a hardware clock is dominated by
the tick granularity, not the sleep loop or the `time` builtin.

## What this unlocks

The same scaffolding (`THREAD_SLEEPING`, the wake-up walk,
`yield()` as a sleeping primitive) is the template for every
future blocking syscall:

- **Pipes**: `pipe_read` blocks the reader on `THREAD_BLOCKED`
  until `pipe_write` produces bytes; `pipe_write` blocks on
  full-buffer until reader drains. Same skeleton, different
  wake condition.
- **Network sockets**: similar.
- **Wait-on-IRQ in drivers**: virtio queues today are polled;
  with sleep + an IRQ-driven completion, drivers can park.

## What's still missing

- **A sorted timer wheel.** O(n) walk per yield is fine for
  our scale; bigger systems would use a min-heap or hashed
  wheel.
- **Cancelable sleeps.** No way to wake a sleeper early today.
  Will matter for `kill -SIGINT` (no signals yet) or for
  deadlines on an `accept()` (no sockets yet).
- **Sub-tick precision.** Granularity is 100 ms. A finer tick
  (or a one-shot timer programmed for the next deadline) gets
  us to ms-level precision.
- **Preemption inside more syscalls.** `sys_read` of a long
  file, `sys_write` of many bytes — all currently uninterruptible.

## What changed

```
kernel/core/thread.h        +THREAD_SLEEPING enum,
                             +wake_at_ms field,
                             +thread_sleep_ms prototype
kernel/core/thread.c        +#include "timer.h"
                             +wake-up walk inside yield(),
                             +thread_sleep_ms impl,
                             init wake_at_ms=0 at 3 sites
kernel/core/syscall.h       +SYS_SLEEP_MS=21
kernel/core/syscall.c       +sys_sleep_ms (with daifclr/daifset
                             around the wait), dispatch case
kernel/core/main.c          banner -> milestone 29
userspace/libc/syscall.h    +SYS_SLEEP_MS=21, +sleep_ms wrapper
userspace/sleep/sleep.c     new (~50 LOC)
userspace/sh/sh.c           help text mentions /bin/sleep
Makefile                    SLEEP_OBJS / ELF / STRIPPED / OSFS
```

Twenty files in the OSFS image now.
