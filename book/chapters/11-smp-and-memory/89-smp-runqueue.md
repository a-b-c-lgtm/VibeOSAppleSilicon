# Chapter 89 — SMP runqueue: per-CPU current, per-CPU runq, no migration

Three chapters of build-up:

- **86** woke CPU 1 with PSCI and parked it in WFE.
- **87** gave both CPUs the atomic vocabulary they need to share data.
- **88** wired SGIs so CPU 0 can poke CPU 1 (and CPU 1 can answer).

Now — finally — CPU 1 runs threads. By the end of this chapter:

- Each CPU has its own `current` pointer, its own runqueue, and
  its own idle thread.
- A new `thread_create_on(cpu, ...)` API enqueues a kernel
  thread on a chosen CPU and `IPI_RESCHED`s it awake.
- An SMP smoke test at boot launches four kernel threads on
  CPU 1 and verifies every one of them ran to completion.

What chapter 89 does **not** do, deliberately, is migrate
threads between CPUs. Threads are sticky-by-creation. That
restriction is the entire reason this chapter is small enough
to fit in a single milestone — the moment threads move between
CPUs, every one of:

- TLB shootdown for address-space teardown,
- per-CPU FPU/SIMD context save state,
- `wait()` cross-CPU notification,
- signal delivery against a thread "running elsewhere",

stops being optional. We'll do migration later. For now, every
thread is sticky to the CPU that created it; user threads (all
created from CPU 0's `init`) stay on CPU 0; only kernel
threads explicitly created with `thread_create_on(1, ...)` run
on CPU 1.

## Prerequisites

- Chapter 11 — uniprocessor scheduler, runqueue, `cswitch_to`.
- Chapter 86 — `struct cpu`, TPIDR_EL1, secondary boot.
- Chapter 87 — atomics (the per-CPU `g_thread_count`, RESCHED
  ack counter, etc).
- Chapter 88 — IPI vocabulary (we'll add a third vector,
  `IPI_RESCHED`, here).

## What "per-CPU" means in the data model

The pre-chapter-89 scheduler had three globals:

```c
static struct thread *g_current;           // running thread
static struct thread *g_runq_head, *g_runq_tail;  // FIFO of READY
static struct thread *g_stack_to_free;     // exit gravestone
```

…all of which were correct on uniprocessor because exactly one
CPU ever touched them. With CPU 1 running threads, every one
of those needs to become per-CPU. Chapter 87's audit table
flagged them as Category 2 ("per-CPU"), and chapter 89 acts
on that flag.

The fix is mechanically tiny: move them into `struct cpu` and
read them through `cpu_current()`:

```c
struct cpu {
    /* …chapter 86 fields… */
    struct thread *current;
    struct thread *idle;
    struct thread *runq_head;
    struct thread *runq_tail;
    spinlock_t     runq_lock;
    struct thread *stack_to_free;
};
```

…then in [`kernel/core/thread.c`](../../../kernel/core/thread.c):

```c
#define g_current        (cpu_current()->current)
#define g_stack_to_free  (cpu_current()->stack_to_free)
```

The macro is a deliberate tactical choice. It means none of the
thirty-odd `g_current->...` references in the file have to
change. Each one expands at compile time to "read TPIDR_EL1,
add the offset of `current`, dereference." On every call. That
costs us nothing measurable (TPIDR_EL1 is a register read; the
compiler hoists the load when it can), and it makes the diff
small enough to convince yourself that nothing semantic
changed besides the locality of `current`.

## The runqueue — local vs remote

Two operations on each CPU's runqueue:

```c
static void runq_push_local(struct thread *t);
static struct thread *runq_pop_local(void);
```

Both take `cpu_current()`'s lock with IRQs masked. The mask
matters: the timer IRQ on CPU 0 can call `runq_push_local`
(when an exiting thread re-queues) at any moment, and the
`IPI_RESCHED` handler on CPU 1 can call `runq_pop_local` to
pick up new work. Without IRQ masking we'd deadlock the
moment a yielder is pre-empted between `spin_lock` and
`spin_unlock` by an IRQ that wants the same lock.

The third operation is the new one:

```c
static void runq_push_remote(uint32_t cpu_id, struct thread *t)
{
    /* … take target CPU's runq_lock, splice t onto its tail … */

    if (cpu_current_id() != cpu_id)
        ipi_send(cpu_id, IPI_RESCHED);
}
```

The IPI is the wake-up signal: target CPU might be in WFI in
its idle loop. Without the kick it would sit there until its
next timer tick (which CPU 1 doesn't even have, see below).

Idempotency note: there's no harm in sending `IPI_RESCHED`
when nothing changed. The handler just runs through `yield()`
once and goes back to whatever it was doing. Linux uses a
per-CPU `pending_ipis` bitmask to coalesce repeated RESCHEDs
into one delivery; we don't, because at our IPI rate (≈ a few
per second) the coalescing buys nothing measurable.

## The idle thread

A CPU with an empty runqueue has nothing to switch to. The
old uniprocessor `yield()` handled this by simply returning —
"nothing else to run; keep running prev." On SMP that's still
correct on CPU 0 (where the boot thread is always runnable),
but on CPU 1 there's no boot thread; if `yield()` returns we
re-enter `cpu_idle_loop` and eventually take a `wfi`.

The cleanest way to handle this is to make every CPU have a
**permanent fall-back** thread — the *idle* thread — that
yield can switch into when the runqueue is empty:

```c
struct thread *idle = cpu_current()->idle;
if (idle && prev != idle) {
    next = idle;
} else {
    /* nothing to run AND we're already idle — stay put */
    return;
}
```

The idle thread runs `cpu_idle_loop`:

```c
void cpu_idle_loop(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile("wfi");
}
```

i.e. it sleeps until any IRQ arrives, then loops back to
`wfi`. The IRQ side will have called `schedule()` on its way
out, so by the time we get back to `wfi` we've already done a
yield attempt; if any new work is pending, we switched to it.

Idle is allocated **off-runqueue** and never re-enqueued. We
treat it as the special "always available, never wins against
real work" thread. yield()'s special-cases:

- If `prev == idle` and `next == NULL`: stay on idle (skip the
  cswitch entirely, return).
- If `prev != idle` and we're switching to idle: don't push
  prev to the runqueue (idle would shadow it forever).

## The timer story (or: why CPU 1 has no timer)

The reflexive thing to do, when handing CPU 1 its scheduler,
is to also enable its timer PPI and let pre-emptive ticks
drive the runqueue. The reflex is wrong in chapter 89.

Here's what happens if you do:

1. CPU 1 is running idle (`wfi`).
2. CPU 1's timer PPI fires. `irq_dispatch` calls `timer_tick()`
   (which happens to walk the global thread list to wake
   sleepers) then `schedule()`.
3. `schedule()` walks `g_all_head` looking for sleepers whose
   wake-time has passed.
4. It finds one — say, `/bin/sh`, which is sleeping on stdin.
5. **`runq_push(t)` pushes `/bin/sh` onto CPU 1's runqueue.**
6. `yield()` pops `/bin/sh` and `cswitch_to`s it.
7. CPU 1 is now running `/bin/sh` — using `/bin/sh`'s kernel
   stack (which lives in CPU 0's mappings), CPU 0's address
   space, CPU 0's per-thread state.
8. The first time `/bin/sh` does anything that reads
   `cpu_current()` (any `current`/`stack_to_free` access),
   it's reading and writing CPU 1's slot when it should be
   reading CPU 0's. State silently corrupts.
9. Eventually a `cswitch_to` unspools a corrupted frame; the
   `eret` lands at a wild address; you get a PC-alignment
   fault (EC=0x22) at PC=0x3296 with SPSR.IL set.

That's not a hypothetical — that's literally the panic that
fired on first interactive boot of chapter 89. Two interacting
bugs:

- **Bug A**: the timer PPI was enabled on CPU 1.
- **Bug B**: `yield()`'s sleeper-wake walk pushes to the
  *current CPU's* runqueue, not to the sleeper's owning CPU.

The chapter-89 floor is "no migration". So both fixes match
that floor:

1. `secondary_main` does NOT enable `TIMER_CNTV_INTID` on
   CPU 1. CPU 1 has no preemption — it runs cooperative
   kernel threads that exit via `thread_exit()` or yield
   explicitly.

2. The sleeper-wake walk in `yield()` is now guarded:

   ```c
   if (cpu_current_id() == 0) {
       /* walk g_all_head, push expired sleepers to runq */
   }
   ```

   This is correct because every thread that *can* sleep on
   the wall clock (via `thread_sleep_ms`) was created on
   CPU 0. CPU-1 kernel threads never call `thread_sleep_ms`
   (and even if they did, we'd want to push them onto CPU 1's
   own runqueue, which is what `runq_push_local` already does
   from CPU 1's context).

Both fixes are also captured in chapter 89's repo memory and
in the [no-migration trap section](#trap-yield-walking-shared-lists-with-runq_push) below.

## thread_create_on: spawning kernel work on CPU 1

The user-visible change in chapter 89 is one new function:

```c
struct thread *thread_create_on(uint32_t cpu_id,
                                void (*entry)(void *),
                                void *arg,
                                const char *name);
```

It's `thread_create()` plus a target CPU id. After building
the thread struct and stack frame, it calls
`runq_push_remote(cpu_id, t)` — which takes the target CPU's
`runq_lock`, splices `t` onto the tail, and IPIs the target.

Use cases (today):

- The chapter-89 SMP scheduler smoke test: launches four
  kernel threads on CPU 1 with names `worker/1`, `worker/2`,
  `worker/3`, `worker/4`.

Future use cases:

- Per-CPU work: a deferred-work queue handler. When chapter
  44 (CSS) needs to render N table cells in parallel, we
  could push N/2 cell jobs to CPU 1 with this API. (Not yet
  done; mentioned for orientation.)
- Network softirq processing on CPU 1, freeing CPU 0 for the
  shell + GUI. (Future.)

There is *no* user-facing version. Userspace forks land on the
forking thread's CPU, period. `clone(CLONE_NEWPID, …)` and
friends are out of scope.

## The smoke test: proof that CPU 1 actually ran threads

At the end of `smp_init_with_dtb`, after the chapter-87 atomic
smoke and the chapter-88 IPI smoke pass, CPU 0 launches four
threads on CPU 1:

```
[smp-sched] launching 4 workers on CPU 1
[smp-sched] cpu_1 ran 4 of 4 OK
```

Each worker increments a shared atomic counter and exits.
After all four are spawned, CPU 0 spin-waits for the counter
to reach 4 (with timeout). If we count fewer than 4 we log
`MISS` (chapter-86 rule about not using FAIL/PANIC/FATAL on
benign paths).

Like the prior smokes, this runs on every boot — and
therefore on every regression test. If anything regresses the
SMP scheduler bring-up (a stack alignment bug, a cswitch
clobber, a missing IPI), the smoke turns it into a clean
boot-time failure instead of a mysterious "GUI hangs after a
while."

## Trap: yield walking shared lists with runq_push

The bug above (PC=0x3296 alignment fault) was the only
implementation bug in chapter 89, but it was instructive
enough to deserve a name. The pattern is:

> Any global-list walk inside `yield()` that calls
> `runq_push()` is implicitly per-CPU and must be guarded by
> `cpu_current_id() == owner_cpu` until proper migration
> exists.

`yield()` runs on the *current* CPU. `runq_push()` enqueues
on the *current* CPU's runqueue. So every "this thread is
ready, schedule it" decision in `yield()` implicitly assumes
"the thread you're scheduling lives on this CPU." Until we
have migration, that assumption is wrong for any walk over a
*global* list (the all-threads list, the future signal-pending
list, the future wakeup-broadcast list).

The fix is always the same: check the CPU id and skip the
walk if you're not on the owner. Cheap, ugly, correct.

Other places this pattern will bite future me:

- Signal delivery. If chapter-67 ever delivers signals from
  inside `yield()` (unlikely, but possible), it'd hit the
  same trap.
- Wakeup broadcasts (`wake_all_blocked_on(token)`). Same.
- Anything that walks `g_all_head` and mutates state.

For chapter 89 we just fix the one offender (the
sleeper-wake walk). For chapter 90+ this becomes a general
pattern: write a `runq_push_to_owner(t)` that uses
`t->owner_cpu` to pick the target CPU's runqueue.

## What we did NOT do

- **No migration.** A thread is sticky to the CPU that
  created it. No work-stealing, no load balancer, no
  affinity policy.
- **No CPU 1 timer.** No preemption on CPU 1. CPU-1
  kernel threads are cooperative; they must call yield or
  exit.
- **No per-CPU heap or pmem locks.** Chapter 87's audit
  flagged `kheap` and `pmem` as Category 4 (lock-protected),
  but in practice the only paths that allocate from CPU 1
  today are `thread_secondary_init_idle` (once at boot) and
  the smoke-test workers (which exit without further
  allocation). The locks would be uncontended; we'll add
  them when a real CPU-1 workload needs them.
- **No userspace SMP.** Userspace processes still all run on
  CPU 0. Adding CPU-affinity syscalls is far future.
- **No TLB shootdown.** Address-space teardown still happens
  on the owning CPU, so its TLB is flushed in-place. No
  cross-CPU `tlbi vmalle1is` needed because no other CPU
  ever loaded that AS.

## Files added or changed

- **`kernel/arch/cpu.h`** — added `current`, `idle`,
  `runq_head`, `runq_tail`, `runq_lock`, `stack_to_free` to
  `struct cpu`. Added `cpu_register_boot()` (call from
  `kernel_main` before `thread_init`) and `cpu_idle_loop()`.
- **`kernel/arch/cpu.c`** — `cpu_register_boot()` sets
  TPIDR_EL1 to `&g_cpus[0]` early so `cpu_current()` works
  before `smp_init_with_dtb` runs. `cpu_idle_loop()` body.
  `secondary_main` enables `IPI_RESCHED`, allocates per-CPU
  idle, then enters `cpu_idle_loop`. (Crucially: does NOT
  enable the timer PPI — see [trap section](#the-timer-story-or-why-cpu-1-has-no-timer).)
  Added `smp_sched_smoke_test`.
- **`kernel/arch/ipi.h` / `ipi.c`** — added `IPI_RESCHED`
  vector. `handle_resched` returns 1 from `ipi_handle` so
  `irq_dispatch` calls `schedule()` on its way out.
- **`kernel/core/thread.c`** — `g_current` and
  `g_stack_to_free` are now per-CPU via macros. `g_runq_*`
  replaced with `runq_push_local/runq_pop_local/runq_push_remote`.
  `g_thread_count` upgraded to atomic. `g_all_head` access
  protected by `g_all_lock`. New `thread_create_on(cpu_id, …)`
  and `thread_secondary_init_idle(name)` APIs. `yield()`'s
  sleeper-wake walk guarded by `cpu_current_id() == 0`.
- **`kernel/core/main.c`** — calls `cpu_register_boot()`
  before `thread_init()`.
- **`scripts/_dbg_smp_boot.py`** — added 9th required marker
  (`[smp-sched] cpu_1 ran 4 of 4 OK`).

## Build & test

```
$ make all
$ python3 scripts/_dbg_smp_boot.py
…
[smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
[smp-ipi] cpu=1 OK round-trip
[smp-ipi] all OK
[smp-sched] launching 4 workers on CPU 1
[smp-sched] cpu_1 ran 4 of 4 OK
…
OK   [smp] bringing up additional cores
OK   [smp] DTB reports 2 cpu(s)
OK   [smp] PSCI CPU_ON cpu=1
OK   [smp] CPU 1 ready
OK   [smp] all CPUs online
OK   [smp-atomic] expected=0x0000000000030d40 got=0x0000000000030d40 OK
OK   [smp-ipi] cpu=1 OK round-trip
OK   [smp-ipi] all OK
OK   [smp-sched] cpu_1 ran 4 of 4 OK
```

The full single-CPU regression suite (24 tests, each booted
under `-smp 2`) still passes — which is the test that
matters, because each test boots a real userspace and
therefore exercises the per-CPU `current` reads and writes
under realistic load.

Manual interactive boot also works: launch desktop, taskbar,
launcher, click around, open notepad and gui_term, no panic.
This is the test that found the timer-PPI / sleeper-walk bug
in the first place; it would have been very easy to ship
chapter 89 with smoke tests passing and only discover the
bug interactively a week later.

## What this unlocks

- **Per-CPU softirq workers** — once a Category-4 lock lands
  on the network stack (today's TCB list and socket table
  are CPU-0-only), we can push softirq processing to CPU 1
  with `thread_create_on(1, net_softirq_loop, …)`.
- **Per-CPU rendering** — same idea for the CSS table
  layout, browser DOM diff, etc.
- **A real migration story** — the next time we touch this
  module. The pattern is well-understood (Linux's
  `try_to_wake_up` + `select_task_rq`); the work is
  shootdown for address-space teardown, not the migration
  itself.
- **A real preemption story on CPU 1** — once we have
  migration, enabling the timer PPI on CPU 1 becomes safe
  and we can run preemptive userspace there too.
