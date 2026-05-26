# Chapter 92 — Real SMP scheduling: per-CPU timers, locked sleeper walks, CLONE_CPU

> **Milestone in this chapter:** close the three problems
> chapter 89 deferred — the secondary CPU's timer, the
> unlocked global thread list, and CPU placement for clones.
> **Code referenced:**
> - [kernel/core/timer.c](../../../kernel/core/timer.c)
>   (per-CPU `CNTV_*`)
> - [kernel/core/thread.c](../../../kernel/core/thread.c)
>   (locked sleeper walk)
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_CLONE2`)
>
> **At the end of this chapter** you will have both cores
> preempting their own threads, a thread-list walk that is
> safe under concurrent CPUs, and userspace able to ask the
> kernel to place a clone on a specific CPU.

Chapter 89 brought up the second core and gave us per-CPU
runqueues, but it deliberately punted on three problems:
the secondary CPU never received its own timer interrupt
(so a thread that ended up there could never be preempted
on a tight loop), the global thread list was walked
without locking (fine when only CPU 0 ever ran user code,
not fine when both do), and there was no way for a
userspace program to ask the kernel to run a thread on a
specific CPU. Chapter 91 then *relied* on every user
thread living on CPU 0 — its futex slow path used a single
`irq_save_disable()` to fence the wait/wake race because
masking IRQs on CPU 0 was sufficient when CPU 0 was the
only place user code ever ran.

Chapter 92 lifts all of these limitations at once. We
enable the CNTV PPI on every secondary at boot, lock every
walk over the global thread list with a single
`g_all_lock` spinlock, route cross-CPU wakes through the
target thread's home CPU, swap chapter 91's IRQ-disable
fence for a real spinlock-based one, and add two new
syscalls — `SYS_CLONE2` and `SYS_GETCPU` — so userspace
can opt into specific placements and verify that the
kernel honoured them.

The chapter ships with a new smoke test,
`userspace/threadtest2`, that pins half its workers on
each CPU and counts to 4000 under contention. Tests run
`-smp 2` with both cores actively executing user code for
the first time in the project.

## What this chapter adds

* **Per-CPU timer enable on secondaries.** `secondary_main`
  in `kernel/arch/cpu.c` now calls `gic_set_priority` +
  `gic_enable_irq(TIMER_CNTV_INTID)` and then
  `timer_init_per_cpu()` after the IPI plumbing, so CPU
  N gets its own quanta-driven preempt independent of
  CPU 0's tick.
* **Atomic `g_ticks`.** `kernel/core/timer.c` makes the
  global tick counter `volatile uint64_t` and uses
  `atomic_add_return64` / `atomic_load64` so two CPUs
  taking the timer interrupt simultaneously can't lose
  a tick.
* **`thread.home_cpu`.** Every `struct thread` records the
  CPU it should run on for life. Idle threads are pinned
  to the CPU that ran `secondary_main`; user threads
  inherit their parent's CPU unless `clone2` overrides.
* **`runq_push_to(t)`.** A new helper that routes the
  push at `t->home_cpu` — local if it matches the
  current CPU, `runq_push_remote` (which sends an
  `IPI_RESCHED`) otherwise.
* **`g_all_lock`.** A single global spinlock that
  protects the all-threads list and is taken around
  every walk: `thread_wake_blocked`, the sleeper walk
  inside `yield()`, the futex wait fence, and the
  existing `all_push` / `all_remove` mutators.
* **`prev == next` fast-path in `yield()`.** A subtle
  case unique to chapter 92: a cross-CPU wake can shove
  the very thread we're about to run back onto our
  runq before our `runq_pop` returns. Without the
  fast-path we'd `cswitch_to` ourselves and crash. With
  it, we simply mark ourselves RUNNING and return.
* **`SYS_CLONE2(entry, arg, stack_top, tls, cpu_id)`.**
  Like `SYS_CLONE` but binds the new thread to a
  specific CPU. `cpu_id == -1` inherits (identical to
  `SYS_CLONE`); `cpu_id` in `[0, SMP_MAX_CPUS)` pins
  to that CPU.
* **`SYS_GETCPU()`.** Returns the CPU id the calling
  thread is running on. Pinned threads see a stable
  value across the syscall return; lets tests verify
  the kernel actually placed a worker where they asked.
* **`thread_global_lock` / `thread_global_unlock` /
  `thread_block_on_held`.** New kernel helpers that
  give the futex slow path a clean way to fence
  "predicate-check then mark BLOCKED then yield" with
  the same lock that protects the waker's walk.
* **`userspace/libc/thread.h:thread_spawn_on()`.** A
  CPU-aware sibling of `thread_spawn`, identical
  except it uses `clone2` and forwards the cpu_id.
* **`userspace/threadtest2`.** A new smoke binary:
  four workers, half on CPU 0 and half on CPU 1, each
  incrementing a shared counter 1000 times under a
  mutex, plus a per-worker `getcpu()` check that
  verifies the placement actually happened.
* **`scripts/test_threads_smp.py`.** Boots `-smp 2`,
  runs `threadtest2`, asserts `[thread2] OK` *and*
  that both CPU 0 and CPU 1 produced "done" markers.

## Prerequisites

* **Chapter 86** — PSCI + secondary boot. We now reuse
  `secondary_main` to enable per-CPU timer state;
  chapter 86 set up the boot sequence into it.
* **Chapter 87** — atomics. `g_ticks` becomes atomic via
  `atomic_add_return64`; the address-space refcount
  reused by clone2 keeps using `atomic_add_return32`.
* **Chapter 88** — IPIs. `runq_push_remote` already
  sends `IPI_RESCHED`; chapter 92 just calls it more
  often, from places that previously assumed a single
  CPU.
* **Chapter 89** — SMP runqueue. Lifts the floor that
  said "user threads pin to CPU 0." The per-CPU
  runqueue infrastructure stays exactly the same; we
  just remove the gate that kept user threads off the
  secondary.
* **Chapter 91** — clone. `SYS_CLONE2` is a thin
  super-set of `SYS_CLONE`; the trampoline, AS
  refcounting, and futex helpers all come from there.

## Per-CPU timer on secondaries

Pre-chapter 92 `secondary_main` looked like this:

```c
void secondary_main(void) {
    /* ... GICR init ... */
    gic_enable_irq(IPI_RESCHED);
    gic_enable_irq(IPI_TLB_SHOOT);
    /* CNTV deliberately NOT enabled — user threads run on CPU 0 only. */
    enable_irq();
    for (;;) wfi();
}
```

The CNTV PPI carries the per-CPU virtual generic timer.
GICv3 routes PPIs (intids 16–31) per-CPU automatically
through the redistributor, but each CPU has to *enable*
its own copy. The chapter 89 secondary skipped that step
because no user thread could ever land here — the only
work CPU 1 did was idle, and the IPIs themselves were
enough to wake it.

Once CPU 1 starts running user threads (CLONE_CPU below),
that's no longer true: a worker can take a long mutex
spin without ever entering the kernel, and without a
timer the spinning thread will hold the CPU forever. The
fix is one chunk of code right after the IPI enable:

```c
/* kernel/arch/cpu.c */
void secondary_main(void) {
    /* ... GICR init ... */
    gic_set_priority(IPI_RESCHED, 0x80);
    gic_enable_irq(IPI_RESCHED);
    gic_set_priority(IPI_TLB_SHOOT, 0x80);
    gic_enable_irq(IPI_TLB_SHOOT);

    /* Chapter 92 — secondaries now run user code, so they
     * need their own preempt source.  TIMER_CNTV_INTID is a
     * PPI (id 27); the redistributor routes it per-CPU and
     * each CPU has to enable its own copy.  timer_init_per_cpu
     * programmes CNTV_TVAL_EL0 + CNTV_CTL_EL0 for the local
     * timer. */
    gic_set_priority(TIMER_CNTV_INTID, 0x80);
    gic_enable_irq(TIMER_CNTV_INTID);
    timer_init_per_cpu();

    enable_irq();
    for (;;) wfi();
}
```

`timer_init_per_cpu` already existed from chapter 89
(used by CPU 0 at boot). It writes the per-CPU
`CNTV_TVAL_EL0` countdown register and unmasks the timer
in `CNTV_CTL_EL0`. We just had to call it on the
secondaries too.

## Atomic `g_ticks`

The kernel keeps a global tick counter that everything
from `uptime` to the cooperative-yield heuristic reads.
With both CPUs taking timer interrupts simultaneously,
naive `g_ticks++` (load / inc / store) loses updates.

```c
/* kernel/core/timer.c */
#include "../arch/atomic.h"

static volatile uint64_t g_ticks = 0;

uint64_t timer_ticks(void) {
    return atomic_load64(&g_ticks);
}

void timer_handle_irq(void) {
    /* ... ack timer ... */
    (void)atomic_add_return64(&g_ticks, 1);
    /* ... reschedule decision ... */
}
```

The atomic primitives are exactly the chapter-87 LSE
ones; `atomic_add_return64` is one `LDADDAL` instruction
on the AArch64 implementations we target. No fast path
to optimise — the whole point is one strictly-ordered
RMW per tick.

## Locking the global thread list

`g_all_head` is a singly-linked list that every thread
joins at create time and leaves at exit time. Pre-chapter
91 it was walked unsynchronised, which was safe because
`fork`/`exec`/`exit` were the only mutators and they
all ran on CPU 0 with timer-driven scheduling that didn't
race against itself.

Chapter 91 added futex_wait/wake, both of which walk the
list. Chapter 92 puts user threads on CPU 1 too, so two
walks can now run simultaneously. The fix is a single
spinlock:

```c
/* kernel/core/thread.c */
static spinlock_t g_all_lock = SPINLOCK_INIT;

static void all_push(struct thread *t) {
    spin_lock(&g_all_lock);
    t->all_next = g_all_head;
    g_all_head  = t;
    spin_unlock(&g_all_lock);
}

static void all_remove(struct thread *t) {
    spin_lock(&g_all_lock);
    /* ... unlink ... */
    spin_unlock(&g_all_lock);
}
```

Every existing walker takes the lock too:

* `thread_wake_blocked(token)` — walks looking for
  blocked-on matches.
* `yield()`'s sleeper-wake walk — looks for sleepers
  whose `wake_at` has elapsed.
* The futex wait fence (covered below).

`thread_lookup` is *not* locked, on purpose: it's used
only from the slow-path debug printer and waitpid's
filter — both already hold higher-level invariants
that prevent the lookup from racing exit.

## Cross-CPU wakes via `runq_push_to`

Once both CPUs run user threads, "wake thread T" can
fire on a different CPU than the one T is pinned to.
The naive `runq_push(t)` always pushed to the *current*
CPU's runqueue, which would yank a CPU-0-pinned thread
onto CPU 1 — fine in spirit, but it bypasses our
home_cpu invariant and also dodges the IPI that the
target CPU needs to wake up from `wfi`.

The fix is a one-line helper:

```c
/* kernel/core/thread.c */
static void runq_push_to(struct thread *t) {
    if (t->home_cpu == cpu_current_id()) {
        runq_push(t);                     /* local — no IPI */
    } else {
        runq_push_remote(t->home_cpu, t); /* cross-CPU + IPI_RESCHED */
    }
}
```

Every place that previously called `runq_push(t)` to
wake a not-currently-running thread switches to
`runq_push_to(t)`:

* `thread_wake_blocked` (futex_wake, network rx,
  generic blocked-on wake)
* The sleeper walk in `yield()` (timed sleeps from
  `nanosleep`)
* SIGCHLD wake of a parent blocked in `waitpid`
* Signal-driven wake of a user thread from `kill()`

Threads created via `user_thread_create*` get their
home_cpu set at create time (`cpu_current_id()` for
plain `clone`, the explicit `cpu_id` for `clone2`).
Idle threads spawned by `secondary_main` get home_cpu
= the CPU running `secondary_main`. The boot thread is
home_cpu = 0.

## The `prev == next` fast-path

Chapter 92 hits a case `yield()` had never seen before:
the thread it's about to dispatch is itself.

The setup is: T1 is running on CPU 0, T2 is sitting
BLOCKED on a futex with home_cpu = 0. T1's mutex_unlock
calls futex_wake. The wake walks `g_all_head`, finds
T2, marks it READY, calls `runq_push_to(T2)` which —
since T2's home_cpu is 0 and we're on CPU 0 — pushes T2
to the local runq with no IPI. T1 then wraps up its
own work and calls `yield()`.

`yield()` does:

1. `prev = g_current` (T1)
2. push prev back on runq if still RUNNING (T1 goes
   back at the head of the local runq, *behind* T2)
3. `next = runq_pop()` — returns T2
4. `cswitch_to(prev, next)`

This is fine, but there's a sister case. If between
steps 1 and 3 a *different* thread on a *different*
CPU wakes T1 (e.g. T1 was BLOCKED, woken by
thread_wake_blocked from CPU 1), runq_push_to may push
T1 onto CPU 0's runq. Then when CPU 0's `runq_pop`
fires, it might return T1 itself. `cswitch_to(T1, T1)`
would copy T1's saved registers over its live ones and
crash.

Pre-chapter 92 this couldn't happen because cross-CPU
wakes didn't exist. Chapter 92 ships the fast-path:

```c
/* kernel/core/thread.c */
void yield(void) {
    /* ... DAIF mask, sleeper walk ... */
    struct thread *prev = g_current;
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        runq_push(prev);
    }
    struct thread *next = runq_pop();
    if (!next) next = g_idle[cpu_current_id()];

    /* Chapter 92 — prev==next fast-path.  Can happen if a
     * cross-CPU wake re-pushes prev to our runq between the
     * push above and the pop here.  cswitch_to(t, t) would
     * copy saved regs over live ones; we just stay running. */
    if (next == prev) {
        next->state = THREAD_RUNNING;
        irq_restore(daif);
        return;
    }

    /* ... real context switch ... */
}
```

Without this two-line check the test was completely
reliable on small workloads but flaky once cross-CPU futex
wakes were added. The two-line check buys back
correctness without measurably affecting hot-path
performance -- `runq_pop` returning ourselves is rare.

## Futex serialization across CPUs

Chapter 91's futex_wait used IRQ-disable to fence the
"check predicate, mark BLOCKED, yield" sequence:

```c
/* chapter 91 — single-CPU floor */
long sys_futex_wait(int *uaddr, int expected) {
    /* ... validate user ptr ... */
    uint64_t f = irq_save_disable();
    if (atomic_load32(uaddr) != expected) {
        irq_restore(f);
        return -EAGAIN;
    }
    thread_block_on(uaddr);  // sets state=BLOCKED, yields
    /* yield's eret restores SPSR — IRQs come back on automatically */
    return 0;
}
```

This worked because chapter 89's invariant pinned every
user thread to CPU 0. With IRQs masked on CPU 0 and no
user code on CPU 1, no waker could possibly run between
the predicate check and the state transition.

Chapter 92 puts user threads on CPU 1 too. A CPU-1
mutex_unlock can fire while CPU 0 is mid-fence. We need
real cross-CPU exclusion, which means a spinlock — and
the spinlock has to be the same one the waker uses
walking the BLOCKED list, so a wake "in progress" can't
overlap with a sleeper marking itself BLOCKED.

`g_all_lock` is exactly that lock. We lift the
single-CPU IRQ-disable into three new helpers that wrap
its acquisition:

```c
/* kernel/core/thread.c */
uint64_t thread_global_lock(void) {
    uint64_t f = irq_save_disable();
    spin_lock(&g_all_lock);
    return f;
}

void thread_global_unlock(uint64_t flags) {
    spin_unlock(&g_all_lock);
    irq_restore(flags);
}

void thread_block_on_held(void *token, uint64_t flags) {
    g_current->blocked_on = token;
    g_current->state      = THREAD_BLOCKED;
    spin_unlock(&g_all_lock);
    irq_restore(flags);
    yield();
}
```

The futex slow path becomes:

```c
/* kernel/core/syscall.c */
long sys_futex_wait(int *uaddr, int expected) {
    /* ... validate user ptr ... */
    uint64_t f = thread_global_lock();
    if (atomic_load32(uaddr) != expected) {
        thread_global_unlock(f);
        return -EAGAIN;
    }
    thread_block_on_held(uaddr, f);
    return 0;
}
```

The release-then-yield window in `thread_block_on_held`
looks racy at first glance — what if a wake fires on
the other CPU between `spin_unlock` and the actual
`yield`? The answer is: that wake will observe the
BLOCKED state we just wrote, mark us READY, and push us
back onto our home runq. Our about-to-fire `yield`
then either runs us (via the new prev==next fast-path)
or leaves us safely on the runq for later. Either
way we don't sleep forever.

`futex_wake` doesn't change at all — its existing
`thread_wake_blocked(token)` call already walks
g_all_head, and chapter 92 wraps that walk in the
same `g_all_lock`.

## `SYS_CLONE2` and `SYS_GETCPU`

The new syscalls are at slots 75 and 76, right after
chapter 91's SYS_FUTEX_WAKE = 74.

```c
/* kernel/core/syscall.h */
enum {
    /* ... existing ... */
    SYS_CLONE       = 72,
    SYS_FUTEX_WAIT  = 73,
    SYS_FUTEX_WAKE  = 74,
    /* Chapter 92 — explicit CPU placement + getcpu. */
    SYS_CLONE2      = 75,
    SYS_GETCPU      = 76,
};
```

`sys_clone2` validates `cpu_id`, calls
`user_thread_create_shared_on(..., cpu_id)`, and
returns the child's tid:

```c
/* kernel/core/syscall.c */
long sys_clone2(uintptr_t entry, uintptr_t arg,
                uintptr_t stack_top, uintptr_t tls,
                long cpu_id) {
    if (cpu_id < -1 || cpu_id >= SMP_MAX_CPUS) return -EINVAL;
    /* ... usual entry/stack validation ... */
    int tid = user_thread_create_shared_on(
        entry, stack_top, "clone", g_current->as,
        (void *)arg, (void *)tls, (int)cpu_id);
    return tid > 0 ? tid : tid;
}
```

`user_thread_create_shared_on` is a thin wrapper around
the chapter 91 sibling: it sets `t->home_cpu = cpu_id`
(or `cpu_current_id()` if `cpu_id == -1`), then ends
with `runq_push_to(t)` so a child pinned to the *other*
CPU lands on the right runq with an `IPI_RESCHED` to
wake CPU N from idle.

`sys_getcpu` is one line:

```c
long sys_getcpu(void) { return cpu_current_id(); }
```

Userspace gets thin wrappers in
`userspace/libc/syscall.h` (`clone2`, `getcpu`) plus a
`thread_spawn_on(entry, arg, cpu_id)` helper in
`userspace/libc/thread.h` that mmaps the worker stack
and calls `clone2` instead of `clone`. The new
`_svc5` static inline in syscall.h binds register x4
for the cpu_id arg — chapter 91 only ever needed up to
4 args, but clone2 needs 5.

## Demonstration: `threadtest2`

The new smoke binary is `userspace/threadtest2/threadtest2.c`.
It pins half its workers to each CPU, runs them
concurrently against a shared counter, and checks two
things:

1. The final counter is exactly `N_WORKERS * ITERS`
   (no lost increments — proves the cross-CPU mutex
   works).
2. Each worker reports the CPU it actually ran on,
   and both CPU 0 and CPU 1 appear in the output
   (proves clone2 placed them where we asked).

```c
/* userspace/threadtest2/threadtest2.c — abridged */
static volatile uint32_t counter   = 0;
static mutex_t           lock      = MUTEX_INIT;
static mutex_t           print_lock = MUTEX_INIT;

static void emit(const char *buf, size_t n) {
    mutex_lock(&print_lock);
    write(1, buf, n);
    mutex_unlock(&print_lock);
}

static void worker(void *arg) {
    long packed   = (long)arg;
    int  id       = (int)((packed >> 8) & 0xFF);
    int  want_cpu = (int)(packed & 0xFF);

    int start_cpu = getcpu();
    /* emit "[thread2] worker N start cpu=M\n" */
    if (start_cpu != want_cpu) { /* FAIL */; exit(1); }

    for (int i = 0; i < ITERS; i++) {
        mutex_lock(&lock);
        counter++;
        mutex_unlock(&lock);
        if ((i & 0x3F) == 0) yield();
    }

    int end_cpu = getcpu();
    /* emit "[thread2] worker N done cpu=M\n" */
    if (end_cpu != want_cpu) { /* FAIL */; exit(1); }
    exit(0);
}

int main(void) {
    for (int i = 0; i < N_WORKERS; i++) {
        int want = i & 1;            /* 0,1,0,1 */
        long packed = ((long)i << 8) | (long)want;
        int t = thread_spawn_on(worker, (void *)packed, want);
        /* ... track tid, error-check ... */
    }
    /* join all, check counter, print [thread2] OK */
}
```

A subtle gotcha worth calling out: the kernel's
`write()` to the serial console is *not* atomic across
CPUs. Each character goes through the UART's TX FIFO
one at a time, and a CPU-0 worker's `write` can be
preempted in the middle of its bytes by a CPU-1 worker
also calling `write`. Without a userspace `print_lock`,
status lines like `[thread2] worker 0 start cpu=0`
would interleave at byte boundaries with another
worker's `[thread2] worker 1 start cpu=1` and the test
harness would fail to match either. Chapter 92's
threadtest2 holds `print_lock` across each whole
`write()` call so the lines land intact.

This is exactly the pattern a real `printf` in a
threaded program needs (POSIX requires `printf` to
hold the file's internal lock for the whole format
operation). We don't ship that polish in libc yet —
each binary that mixes threads and stdout has to take
care of it locally.

## Floor caveats — DO NOT mix

* **Threads do not migrate.** Once `home_cpu` is set
  it never changes. There's no load balancer. A clone
  pool that goes lopsided stays lopsided. (Real load
  balance is a separate chapter; M58/M59 punted on it
  too.)
* **No FD sharing.** Each cloned thread still gets its
  own empty fd table. Threads that need shared fds
  have to refcount one in userspace and synchronise
  manually. The browser's parser-thread project (next
  chapter) is the first place this becomes a real
  problem.
* **`futex_wake` ignores n.** Still treated as "wake
  all" — chapter 91's floor still applies. Bounded
  wakes need a different walk and aren't on the
  critical path.
* **No kernel-side preemption disable.** A long
  syscall on either CPU still runs to completion
  without giving up the CPU. We never had per-CPU
  preempt counters, and chapter 92 doesn't add them.
* **`g_all_lock` is one global lock.** Walks of the
  thread list serialise across CPUs. With small
  process counts (≤ 100 threads, ≤ 4 CPUs) it's fine.
  Beyond that you'd want hashed buckets or RCU.

## Why not migrate?

The chapter title says "real SMP scheduling," but we
deliberately stop at static placement. Migration adds
three things we don't need yet:

1. **Live TLB invalidation** — moving a thread off
   CPU N means invalidating that CPU's TLB for the
   thread's AS, which in our setup means an
   `IPI_TLB_SHOOT` to the target. We have the IPI
   from chapter 88, but the bookkeeping (tracking
   which CPUs have ever run an AS) doesn't exist.
2. **Cache line ping-pong** — moving threads
   between CPUs trashes their L1 contents. A real
   scheduler picks migration carefully (e.g. NUMA
   nodes, load thresholds with hysteresis). We'd
   spend a chapter just on the policy.
3. **Per-CPU runqueue invariants** — code that
   reads `cpu_current()->runq` assumes the running
   thread is the only mutator. Migration breaks
   that and requires per-runq locks.

A "good enough" first cut is exactly what chapter 92
ships: choose at clone time, never move. Real workloads
get balance by spawning the right number of workers
into the right CPUs (e.g. one per CPU, or a fixed pool
sized to the topology).

## What this unlocks

* **Browser parser thread on CPU 1.** The HTML
  tokenizer + DOM builder are the slowest part of
  loading a page. Pinning them to CPU 1 while CPU 0
  drains input + repaints turns the browser into a
  visibly faster interactive program. That's the
  next chapter.
* **Background work without GUI hitches.** Any
  long-running computation (chunked downloads, image
  decode) can move to CPU 1 without the GUI thread
  ever skipping a frame. The wallpaper compositor,
  the future `image-cache` daemon, the eventual
  audio mixer — all candidates.
* **Honest per-CPU benchmarking.** With both cores
  running and `getcpu` reporting truthfully, we can
  measure scaling. Up until now `uptime` told us how
  much wall time CPU 0 had spent in user vs. kernel;
  now `uptime` accumulates from both CPUs.

## Files added

* `userspace/threadtest2/threadtest2.c`
* `scripts/test_threads_smp.py`

## Files modified

* `kernel/arch/cpu.c` — enable CNTV PPI on secondaries
* `kernel/core/timer.c` — atomic g_ticks
* `kernel/core/thread.h` — home_cpu, *_shared_on proto,
  thread_global_*/_block_on_held protos
* `kernel/core/thread.c` — runq_push_to, locked walks,
  prev==next fast-path, _shared_on, _global_*,
  _block_on_held
* `kernel/core/syscall.h` — SYS_CLONE2, SYS_GETCPU
* `kernel/core/syscall.c` — sys_clone2, sys_getcpu,
  futex_wait via thread_global_lock
* `userspace/libc/syscall.h` — _svc5, clone2, getcpu
* `userspace/libc/thread.h` — thread_spawn_on
* `Makefile` — threadtest2 binary + disk wiring
* `book/INDEX.md` — chapter 92 row added under Part XI

## Build & test

```
make all
python3 scripts/test_threads_smp.py     # chapter 92 smoke

# 27-test regression sweep (chapter 91's 26 + new SMP test).
# Run sequentially — they all bind QEMU + UNIX sockets and
# would collide if parallelised.
for t in test_directories test_journal test_osfs2 test_notepad \
         test_notepad_save_as test_notepad_save_as_nav test_wm \
         test_taskbar test_clock test_dns test_httpget test_dhcp \
         test_layout test_html_dom test_fork_exec test_cow \
         test_sigaction test_sigchld test_minimize test_launcher \
         test_html_tokenizer test_css test_arrow_keys test_gui_term \
         test_mmap test_threads test_threads_smp; do
  timeout 240 python3 scripts/$t.py >/tmp/sweep_$t.log 2>&1 \
    && echo "PASS $t" || echo "FAIL $t"
done
```

Expected: 27/27 PASS.

(`test_url_http.py` and `test_printftest.py` and
`test_virtio_input.py` were already failing on the
chapter 91 baseline before any chapter 92 work — they
are tracked separately and not chapter 92 regressions.)
