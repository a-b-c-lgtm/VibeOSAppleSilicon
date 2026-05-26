# Chapter 91 — Userspace threads (clone-shaped)

> **Milestone in this chapter:** add a Linux-shaped `SYS_CLONE`
> so a single process can spawn additional threads that share
> its address space.
> **Code referenced:**
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_CLONE`)
> - [kernel/core/thread.c](../../../kernel/core/thread.c)
> - [userspace/libc/thread.h](../../../userspace/libc/thread.h)
>
> **At the end of this chapter** you will have userspace able
> to create kernel threads inside an existing address space,
> enabling worker pools, parallel parsers, and anything else
> that wants `O(1)` access to shared data structures.

For ninety chapters every user process has been a single
thread. Chapter 73's `fork` lets a process duplicate itself
into two cooperating processes — but the new copy gets its
own *address space*, so the only way two pieces of the same
program can share state is via files, pipes, or sockets.
That's enough to build a Unix shell. It is not enough to
build a worker pool, a parallel parser, or anything else
that wants `O(1)` access to shared data structures.

This chapter closes the gap. We add a `SYS_CLONE` syscall
that spawns a thread *inside* the calling thread's address
space, plus the futex primitives needed to build a
user-space mutex. The tiny `userspace/libc/thread.h` layer
on top gives us `thread_spawn`, `thread_join`,
`mutex_lock`, and `mutex_unlock` — enough to write a
program that has four workers fighting over a single
counter and watch the count come out exact.

## What this chapter adds

* **AS reference counting.** `struct address_space` gains a
  `volatile uint32_t refcount`. `address_space_create`
  initialises it to 1; `address_space_destroy` decrements
  and only does the page-table teardown when the count
  hits zero. A new `address_space_share()` helper bumps
  the count atomically. Every existing
  `address_space_destroy` caller (sys_fork, sys_exec,
  sys_spawn, the spawn-failed-cleanup paths) keeps its
  semantics — they took the only reference at create
  time, so their destroy still triggers the teardown.
* **`user_thread_create_shared`** in the kernel — like
  `user_thread_create` but takes an existing AS by
  reference, sets up the new kernel stack with x0=arg
  and TPIDR_EL0=tls in the launch frame, and uses a
  dedicated `user_clone_trampoline` so the existing
  one-program-per-thread path stays branch-free.
* **`SYS_CLONE(entry, arg, stack_top, tls)`** — spawn a
  fresh thread sharing the calling thread's AS. Returns
  the child's tid in the parent.
* **`SYS_FUTEX_WAIT(uaddr, expected)`** — block on `uaddr`
  if `*uaddr == expected`, otherwise return -EAGAIN.
* **`SYS_FUTEX_WAKE(uaddr, n)`** — wake threads blocked on
  `uaddr`. Floor: `n` is treated as "wake all" today.
* **`userspace/libc/thread.h`** — atomic primitives,
  `mutex_t`, `thread_spawn`, `thread_join`.
* **`userspace/threadtest`** — 4 workers × 1000 iterations
  each, incrementing a shared counter under a mutex.
* **`scripts/test_threads.py`** — the regression sweep
  entry that boots the kernel and verifies the counter
  comes out to 4000.

## Prerequisites

* **Chapter 73** — fork. We don't reuse the fork
  machinery, but the AS lifecycle and per-thread state
  conventions all come from that chapter.
* **Chapter 86** — atomics. `atomic_cmpxchg32` is the
  kernel primitive the userspace mutex's fast path
  mirrors via the same instruction sequence.
* **Chapter 89** — SMP. Sets the chapter 91 floor:
  user threads stay on CPU 0. Clone children inherit
  that pinning automatically because `runq_push` always
  pushes to the *current* CPU's runq, and main runs on
  CPU 0.
* **Chapter 90** — mmap. The userspace `thread_spawn`
  helper allocates the worker's stack via
  `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`, which means
  chapter 91 doesn't need its own page-allocator path.

## Address-space refcounting

Pre-chapter 91, the AS was an exclusive resource: every
`thread` either owned a `struct address_space` outright
(user threads) or had `as == NULL` (kernel threads). The
exiting thread's AS was always destroyed by the reaper.

Two threads sharing one AS breaks that invariant. The
parent's exit must not free the child's page tables, and
the child's exit must not free the parent's. The fix is
the same one Linux uses: a reference count.

```c
/* kernel/arch/address_space.h */
struct address_space {
    /* ... existing fields ... */
    volatile uint32_t refcount;
};

void address_space_share(struct address_space *as);
```

```c
/* kernel/arch/address_space.c */
struct address_space *address_space_create(void) {
    /* ... existing setup ... */
    as->refcount = 1;          /* single owner at creation */
    return as;
}

void address_space_destroy(struct address_space *as) {
    if (!as) return;
    if (atomic_sub_return32(&as->refcount, 1) > 0) return;
    teardown_user_range(as);
    teardown_vmas(as);
    pmem_free_page(as->l2_pa);
    pmem_free_page(as->l1_pa);
    kfree(as);
}

void address_space_share(struct address_space *as) {
    if (!as) return;
    atomic_add_return32(&as->refcount, 1);
}
```

Refcount changes are atomic because chapter 89 lets
kernel threads run on CPU 1, and the reaper running on
the wrong CPU could in principle race a clone running on
CPU 0. The atomic ops are essentially free in the
uncontended case (one LL/SC pair).

The crucial insight: **every existing
`address_space_destroy` call site continues to do the
right thing without any changes.** They all match the
"I took the create-time reference, now I'm dropping it"
pattern. The new `address_space_share` is the only thing
that adds an extra reference, and it's matched by exactly
one extra `address_space_destroy` when the cloned thread
exits and is reaped.

## Spawning the cloned thread

The kernel side of `SYS_CLONE` is compact once the refcount
and trampoline are in place:

```c
static long sys_clone(long entry, long arg,
                      long stack_top, long tls) {
    struct thread *parent = thread_current();
    if (!parent || !parent->as) return -EINVAL;
    /* ...range checks on entry / stack_top... */

    struct thread *child = user_thread_create_shared(
        entry, stack_top, "clone", parent->as,
        (uint64_t)arg, (uint64_t)tls);
    if (!child) return -ENOMEM;
    return (long)child->id;
}
```

`user_thread_create_shared` mirrors the existing
`user_thread_create` body, with three deltas:

1. It calls `address_space_share(as)` instead of
   accepting ownership. The new thread holds an extra
   AS reference until it's reaped.
2. It pre-loads `x21 = arg` and `x22 = tls` into the
   launch frame's GPR slots, alongside the usual
   `x19 = entry, x20 = sp_top`.
3. It points the launch trampoline at
   `user_clone_trampoline` instead of `user_trampoline`.

The new trampoline is a verbatim copy of the original
with two changes — write `tpidr_el0` from `x22`, and
load `x0` from `x21` instead of zeroing it:

```asm
.global user_clone_trampoline
user_clone_trampoline:
    msr     sp_el0, x20
    msr     elr_el1, x19
    msr     tpidr_el0, x22
    mov     x16, #0x340         /* EL0t, IRQs on */
    msr     spsr_el1, x16
    mov     x0,  x21            /* arg → x0  */
    mov     x1,  xzr
    /* ...zero the rest of the GPRs... */
    eret
```

After the eret the child is at EL0 executing
`entry(arg)` with its own SP and (optionally) its own
TLS register. From the scheduler's perspective the
clone is just another `THREAD_READY` row in the runq.

## Futex

The userspace mutex's fast path is a single
compare-and-swap. The slow path needs the kernel for
exactly one job: park the failing thread until somebody
unlocks. Linux's solution is `futex` — a syscall pair
keyed on the user-mode address of the lock word itself.

The kernel side is reused infrastructure. We already
have `thread_block_on(token)` and `thread_wake_blocked
(token)` from chapter 30 (pipes). The wait token is
just the user VA cast to `void *`:

```c
static long sys_futex_wait(long uaddr, long expected) {
    /* ...range/alignment checks... */
    uint64_t flags = irq_save_disable();
    uint32_t cur;
    if (copy_from_user(&cur, uaddr, sizeof(cur)) < 0) {
        irq_restore(flags); return -EFAULT;
    }
    if (cur != (uint32_t)expected) {
        irq_restore(flags); return -EAGAIN;
    }
    thread_block_on((void *)(uintptr_t)uaddr);
    /* yield's eret restores IRQs-on; no irq_restore here */
    return 0;
}

static long sys_futex_wake(long uaddr, long n) {
    /* ...checks... */
    if (n == 0) return 0;
    thread_wake_blocked((void *)(uintptr_t)uaddr);
    return 1;
}
```

Two non-obvious details:

### IRQ discipline closes the wait/wake race

If we ran the predicate check and the
`state = THREAD_BLOCKED` set with IRQs on, this race
fires on every contended unlock:

1. T2 enters `sys_futex_wait`. Reads `*uaddr == 1`.
2. **Timer IRQ fires.** Switch to T1.
3. T1 unlocks: sets `*uaddr = 0`, calls `futex_wake`.
   The wake walks the BLOCKED list, finds nobody (T2
   isn't blocked yet), returns.
4. Switch back to T2.
5. T2 calls `thread_block_on`. State is now BLOCKED
   waiting for a wake that already happened. T2
   sleeps forever.

The fix is `irq_save_disable` around (predicate check,
state-set, `yield`). On a single-CPU-for-user-threads
machine (chapter 89's invariant), masking IRQs is enough
to make the whole sequence atomic with respect to any
unlocker. We deliberately do *not* `irq_restore` after
`thread_block_on` returns — `cswitch_to`'s eret restores
the saved SPSR for the resumed thread, which has IRQs
already on. Mixing `irq_save_disable` with `yield`
sounds scary, but `yield` itself manages DAIF
internally; the rule is just "don't yield with
unbalanced state."

### Why use the user VA as the wait token

Different threads in the same address space see the same
VA pointing at the same physical page, so hashing on the
VA gives correct same-AS rendezvous. Different processes
that happened to use the same VA would alias — but since
they have *different* physical pages behind that VA, the
post-wake predicate re-read still sees the local value,
mismatches the expected, and the woken thread goes back
to sleep. Spurious wakes are explicitly part of the
futex contract; aliasing here is a (rare) wakeup-then-
re-block dance, not a correctness bug.

A real kernel would walk the user PTE to find the
backing PA and use that as the token, eliminating
aliasing entirely. We can ship that later if cross-
process futex (which would also need shared memory,
not yet implemented) ever becomes a thing.

## Userspace mutex

`userspace/libc/thread.h` builds the mutex on top of
`atomic_cmpxchg32` and the futex syscalls:

```c
typedef struct { volatile int state; } mutex_t;
#define MUTEX_INIT  { 0 }

static inline void mutex_lock(mutex_t *m) {
    if (atomic_cmpxchg32_u(&m->state, 0, 1)) return;
    for (;;) {
        if (atomic_cmpxchg32_u(&m->state, 0, 1)) return;
        (void)futex_wait(&m->state, 1);
    }
}

static inline void mutex_unlock(mutex_t *m) {
    atomic_store32_u(&m->state, 0);
    (void)futex_wake(&m->state, 1);
}
```

This is the simplest possible correct futex mutex. It
has two known costs:

* **Always wakes on unlock**, even if no one was
  waiting. Drepper's 3-state mutex (`0=free`,
  `1=held-no-waiters`, `2=held-with-waiters`) skips the
  wake syscall on uncontended unlocks, but the
  bookkeeping is messy and the chapter 91 floor doesn't
  need the saving. (`futex_wake` with no waiters is
  itself a fast no-op.)

* **Thundering herd on contended unlocks**: with N
  blocked waiters, all N wake up, all but one fail the
  cmpxchg, and N-1 immediately re-`futex_wait`. For
  the four-thread test it's fine; with hundreds of
  waiters we'd want bounded wakes (which would require
  exposing `g_all_head` walking through a new helper —
  deferred).

## thread_spawn / thread_join

The pthread-shaped helpers fall out for free:

```c
#define THREAD_STACK_BYTES   (16 * 4096)   /* 64 KiB */

static inline int thread_spawn(clone_entry_t entry, void *arg) {
    void *stack = mmap(NULL, THREAD_STACK_BYTES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -12;
    void *sp_top = (uint8_t *)stack + THREAD_STACK_BYTES;
    int tid = clone(entry, arg, sp_top, NULL);
    if (tid < 0) { munmap(stack, THREAD_STACK_BYTES); return tid; }
    return tid;
}

static inline int thread_join(int tid) {
    int code = -1;
    int reaped = waitpid(tid, &code, 0);
    if (reaped < 0) return -1;
    return code;
}
```

A thread's last act must be `exit(0)` — `clone` does
*not* synthesize a return-to-exit trampoline at child
launch time, so a worker that returns from its entry
function falls into garbage. Convention is for entry
to end with `exit(0)`.

Stacks are deliberately not freed after join. Doing so
correctly would require the kernel to defer the unmap
until *after* the thread had fully exited (otherwise
the thread's exit is using the stack we're trying to
unmap). The chapter 91 floor is "create N threads,
join them all, exit" — a leaked stack region per
thread is acceptable until we need a thread pool that
churns workers.

## Smoke test

`userspace/threadtest`:

```c
static volatile uint32_t counter = 0;
static mutex_t lock = MUTEX_INIT;

static void worker(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&lock);
        counter++;
        mutex_unlock(&lock);
        if ((i & 0x3F) == 0) yield();
    }
    write(1, "[thread] worker ", 16);
    putdec_small(id);
    write(1, " done\n", 6);
    exit(0);
}

int main(void) {
    write(1, "[thread] start\n", 15);
    int tids[4];
    for (int i = 0; i < 4; i++)
        tids[i] = thread_spawn(worker, (void *)(long)i);
    for (int i = 0; i < 4; i++) thread_join(tids[i]);
    if (atomic_load32_u(&counter) != 4000) {
        write(1, "[thread] FAIL count\n", 20); return 1;
    }
    write(1, "[thread] OK\n", 12);
    return 0;
}
```

The voluntary yield every 64 iterations isn't required
for correctness — the timer preempts us on its own —
but it produces denser interleaving in a shorter
wall-clock window, increasing the odds that a missing
wake (if one slipped in) gets caught.

A sample passing run:

```
threadtest
[thread] start
[thread] worker 0 done
[sys_exit] thread 'clone' exited with code 0x0000000000000000
[thread] worker 1 done
[sys_exit] thread 'clone' exited with code 0x0000000000000000
[thread] worker 2 done
[sys_exit] thread 'clone' exited with code 0x0000000000000000
[thread] worker 3 done
[sys_exit] thread 'clone' exited with code 0x0000000000000000
[thread] OK
```

`scripts/test_threads.py` boots a kernel under
`-smp 2`, runs `threadtest`, and asserts both `[thread]
OK` *and* the four `worker N done` markers (the order
varies between runs because the scheduler interleaves
them). It joins the chapter 90 sweep at slot 26.

## Chapter 89 SMP interaction

The chapter 89 invariant says: user threads stay on
CPU 0 (sticky-by-creation). Clone children inherit
that pinning automatically — `user_thread_create_shared`
calls `runq_push`, which pushes onto the *current*
CPU's runq. The creating thread runs on CPU 0, so its
clones land on CPU 0 too. CPU 1 stays idle for user
work; we still get concurrency through CPU-0
timer-driven preemption.

Means there's no *real* parallelism across user
threads in chapter 91 — just interleaved execution.
That's enough to surface every concurrency bug we'd
see on real SMP (preemption is just as merciless as
another core), but nominal throughput on N CPUs is
limited to one CPU's worth of work. Lifting that
restriction is its own chapter (somewhere around 95,
when we want to put the browser's parser on CPU 1
while paint runs on CPU 0).

## What's not done

* **No TLS scaffolding.** `TPIDR_EL0` is set per
  thread but libc doesn't use it. There's no `__thread`
  storage class, no per-thread errno, no
  `pthread_self()` returning a unique handle. Adding
  TLS means choosing between (a) a libc-side TLS pool
  and (b) compiler-supported `__thread` (which means
  GCC's `-mtp=el0` flag and a TLS section per
  binary). Deferred to its own chapter.
* **FDs are not shared across clone siblings.** Each
  cloned thread starts with a fresh empty fd table.
  The chapter 91 test doesn't need shared fds; a real
  pthread library would either share by reference
  (with a refcounted fd-table struct) or copy at
  clone time. Both involve plumbing that doesn't pay
  off until we actually have multi-threaded I/O.
* **No `fork` after `clone`.** The fork code clones
  the AS via `address_space_clone_cow` but doesn't
  audit refcount semantics for "fork while siblings
  exist." The current floor pattern is "main thread
  forks before any clones exist" (e.g. shell pipelines)
  or "main thread spawns clones, never forks" (the
  chapter 91 test). Mixing both is undefined.
* **No `MAP_SHARED`.** Clone siblings share the *AS*
  (same page tables), so they trivially share every
  ordinary mapping — but two unrelated processes can't
  share memory. That's a separate feature (`shm_open`
  / `mmap(MAP_SHARED, fd)`) and a separate chapter.
* **No real `pthread` API.** The libc layer is
  `thread_spawn` / `thread_join` / `mutex_t`. Building
  a POSIX-shaped `pthread_t` / `pthread_attr_t` /
  `pthread_mutex_attr_t` skin on top is mechanical and
  deferred until we have a port that actually wants it.
* **Stacks leak on join.** See the `thread_spawn`
  banner above. The fix needs kernel-deferred munmap
  (similar to the existing `cpu->stack_to_free`
  pattern for kernel stacks).
* **`futex_wake` ignores `n`.** Currently treated as
  "wake all" for any `n >= 1`. Bounded wakes need a
  new helper that walks `g_all_head` in
  `kernel/core/thread.c` rather than using
  `thread_wake_blocked`.

## What this unlocks

* A path to multi-threaded servers (httpd from a
  future chapter, a render-thread for the browser,
  parallel I/O in tools like `wc -l` over a directory).
* A real C++ runtime (which assumes pthreads exist)
  if we ever want one.
* The first place where AS sharing exists at all —
  the same plumbing supports `shm_open` for inter-
  process shared memory once we want it.
* A practical proof that the chapter 89 SMP work was
  enough: the AS refcount + the futex queue both
  needed atomic operations that the chapter 86/87
  primitives provide off the shelf.

## Files added

* `userspace/threadtest/threadtest.c`
* `userspace/libc/thread.h`
* `scripts/test_threads.py`

## Files modified

* `kernel/arch/address_space.h` — refcount field, share proto
* `kernel/arch/address_space.c` — refcount-aware destroy, share
* `kernel/arch/context_switch.s` — `user_clone_trampoline`
* `kernel/core/thread.c` — `user_thread_create_shared`
* `kernel/core/thread.h` — `user_thread_create_shared` proto
* `kernel/core/syscall.c` — sys_clone / sys_futex_wait/wake
* `kernel/core/syscall.h` — SYS_CLONE / SYS_FUTEX_WAIT / WAKE
* `userspace/libc/syscall.h` — clone() / futex_*() wrappers
* `Makefile` — threadtest binary + disk wiring
* `book/INDEX.md` — chapter 91 row marked Done

## Build & test

```
make all
python3 scripts/test_threads.py     # chapter 91 smoke

# 26-test regression sweep
for t in test_directories test_journal test_osfs2 test_notepad \
         test_notepad_save_as test_notepad_save_as_nav test_wm \
         test_taskbar test_clock test_dns test_httpget test_dhcp \
         test_layout test_html_dom test_fork_exec test_cow \
         test_sigaction test_sigchld test_minimize test_launcher \
         test_html_tokenizer test_css test_arrow_keys test_gui_term \
         test_mmap test_threads; do
  timeout 240 python3 scripts/$t.py >/tmp/sweep_$t.log 2>&1 \
    && echo "PASS $t" || echo "FAIL $t"
done
```

Expected: 26/26 PASS.
