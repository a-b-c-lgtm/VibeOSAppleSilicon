/*
 * userspace/libc/thread.h — chapter 91 user-space threading.
 *
 * Three layers, smallest first:
 *
 *   1. Atomic primitives on a 32-bit slot (load / store / cmpxchg).
 *      Built directly on AArch64 LL/SC.  Used by user-space
 *      mutexes and lock-free counters.
 *
 *   2. mutex_t.  A 32-bit word with two states (0=free, 1=held)
 *      and a 1-instruction fast path (cmpxchg).  On contention,
 *      blocks via futex_wait; on unlock, wakes via futex_wake.
 *      Spec-equivalent to a Drepper 2-state futex mutex; the
 *      3-state "no waiters" variant would shave a futex_wake
 *      syscall off the uncontended unlock path but is not
 *      worth the complexity for chapter 91.
 *
 *   3. thread_spawn(entry, arg) / thread_join(tid).  These
 *      build on top of clone() + waitpid() to provide a tiny
 *      pthread_create / pthread_join shape.  The stack region
 *      is mmap'd anonymously and never freed (see the floor
 *      caveat below).
 *
 * Floor caveats (chapter 91):
 *   - No TLS scaffolding.  TPIDR_EL0 is set to 0 by default;
 *     callers can pass a value via clone() if they want, but
 *     libc itself does not use it (no per-thread errno yet).
 *   - thread_spawn() never frees the worker's stack.  A real
 *     thread library waits for the join, then unmaps; doing so
 *     here would require the kernel to defer the unmap until
 *     after the user thread has fully exited (otherwise the
 *     thread itself is using the stack at exit time).  Not
 *     worth the floor cost — the test creates a fixed number
 *     of threads and exits.
 *   - File descriptors are NOT shared across thread_spawn'd
 *     threads.  Each gets a fresh empty fd table.  A real
 *     pthread library would share fds; deferred.
 *
 * All inline so the binary stays a single object.  No heap
 * allocations; the worker_stack mmap is the only kernel
 * allocation per thread.
 */
#ifndef USER_THREAD_H
#define USER_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"

/* ── Atomic primitives ──────────────────────────────────────────
 *
 * AArch64 LL/SC built-ins.  The kernel uses the same instruction
 * shapes (see kernel/arch/atomic.h) so the user-space view of
 * the futex word matches what the kernel sees on a copy_from_user.
 * ldaxr / stlxr give us release/acquire semantics — sufficient
 * for mutex hand-off without a full DMB. */

static inline uint32_t atomic_load32_u(const volatile uint32_t *p)
{
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]"
                     : "=r"(v)
                     : "r"(p)
                     : "memory");
    return v;
}

static inline void atomic_store32_u(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("stlr %w1, [%0]"
                     :
                     : "r"(p), "r"(v)
                     : "memory");
}

/* Compare-and-swap: if *p == expected, store new and return 1;
 * otherwise leave *p alone and return 0.  Mirrors Linux's
 * atomic_cmpxchg "did the swap happen?" Boolean shape. */
static inline int atomic_cmpxchg32_u(volatile uint32_t *p,
                                     uint32_t expected,
                                     uint32_t new_)
{
    uint32_t cur, fail;
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]            \n"
        "   cmp     %w0, %w4             \n"
        "   b.ne    2f                   \n"
        "   stlxr   %w1, %w5, [%3]       \n"
        "   cbnz    %w1, 1b              \n"
        "   mov     %w1, #0              \n"   /* success */
        "   b       3f                   \n"
        "2: clrex                        \n"
        "   mov     %w1, #1              \n"   /* mismatch */
        "3:                              \n"
        : "=&r"(cur), "=&r"(fail)
        : "r"(p), "r"(p), "r"(expected), "r"(new_)
        : "cc", "memory");
    return fail == 0;
}

/* Atomic add-and-return-new.  Useful for a thread-safe counter
 * that doesn't need a mutex around it (e.g. completion
 * counters in tests, refcounts). */
static inline uint32_t atomic_add_return32_u(volatile uint32_t *p, uint32_t delta)
{
    uint32_t old, new_, fail;
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]            \n"
        "   add     %w1, %w0, %w4        \n"
        "   stlxr   %w2, %w1, [%3]       \n"
        "   cbnz    %w2, 1b              \n"
        : "=&r"(old), "=&r"(new_), "=&r"(fail)
        : "r"(p), "r"(delta)
        : "memory");
    return new_;
}

/* ── Mutex ──────────────────────────────────────────────────────
 *
 * 32-bit word.  Two states: 0 = free, 1 = held.
 *
 * mutex_lock fast path: one cmpxchg.  If it fails (lock held),
 * fall through to a futex_wait loop: re-check the lock with
 * cmpxchg between every futex_wait so spurious wakes / wake-
 * while-holding races resolve correctly.
 *
 * mutex_unlock: store 0; futex_wake one waiter.  Always issues
 * the wake — slightly wasteful on uncontended unlocks but
 * means we never have to track waiter count.
 *
 * The kernel-side wait token is the address of the lock word
 * itself, so two threads in the same address space see the
 * same token and the wake finds the right waiters.  Different
 * processes using the same VA for different mutex words would
 * cause spurious wakes at most (re-check the predicate, go
 * back to sleep) — see kernel banner in syscall.c. */

typedef struct {
    volatile int state;
} mutex_t;

#define MUTEX_INIT  { 0 }

static inline void mutex_init(mutex_t *m)
{
    atomic_store32_u((volatile uint32_t *)&m->state, 0);
}

static inline void mutex_lock(mutex_t *m)
{
    /* Fast path: take an uncontended lock with one cmpxchg. */
    if (atomic_cmpxchg32_u((volatile uint32_t *)&m->state, 0, 1)) return;

    /* Slow path: contend.  Loop until we're the lucky cmpxchg
     * winner.  futex_wait can return for any of three reasons:
     *   - normal wake (somebody unlocked → state == 0 now)
     *   - -EAGAIN (state changed under us; the predicate read
     *     in the kernel saw != expected.  This happens if the
     *     lock was just released between our cmpxchg attempt
     *     and the kernel-side check)
     *   - spurious wake (no real wake but we got returned anyway)
     * In all three cases, the right thing to do is re-cmpxchg. */
    for (;;) {
        if (atomic_cmpxchg32_u((volatile uint32_t *)&m->state, 0, 1)) return;
        /* Sleep until somebody unlocks.  We pass `1` as
         * expected because that's what we just observed (lock
         * held).  If by the time the kernel reads the word it
         * has become 0 (the unlocker beat us in), futex_wait
         * returns -EAGAIN and we immediately retry the
         * cmpxchg above. */
        (void)futex_wait((volatile int *)&m->state, 1);
    }
}

static inline void mutex_unlock(mutex_t *m)
{
    atomic_store32_u((volatile uint32_t *)&m->state, 0);
    /* Wake one (or more — the kernel currently treats `n>=1`
     * as "wake all").  Multiple wakes risk a thundering herd
     * but the chapter 91 floor doesn't care; we'd either need
     * a 3-state mutex or per-mutex waiter counting to avoid
     * the wasteful wake-then-block dance. */
    (void)futex_wake((volatile int *)&m->state, 1);
}

/* ── thread_spawn / thread_join ─────────────────────────────────
 *
 * thread_spawn:
 *   - mmap a page-aligned worker stack (THREAD_STACK_BYTES total).
 *   - call clone() with stack_top = end of region (stacks grow
 *     down on AArch64).
 *   - Returns the child's tid on success or -errno on failure.
 *
 *   The worker's first user-mode instruction is `entry(arg)`.
 *   When entry returns, the worker MUST call exit() — otherwise
 *   it will return into garbage (the kernel doesn't synthesise
 *   a return-to-exit trampoline at clone time).  Convention is
 *   for entry to end with `exit(0)`.
 *
 * thread_join:
 *   - waitpid(tid, &code, 0).  Returns the worker's exit code
 *     on success or -errno on failure (e.g. if `tid` isn't a
 *     child of the calling thread).
 *
 *   Note: the worker's stack region (mmap'd in thread_spawn)
 *   is NOT unmapped after join.  See banner-level floor caveat. */

#define THREAD_STACK_BYTES   (64 * 4096)   /* 256 KiB.  Sized to fit
                                              the browser parser
                                              thread's layout
                                              recursion (~200 frames
                                              deep on a 35-comment
                                              Hacker News thread,
                                              each frame ~600 B
                                              between malloc'd
                                              inline_buf, css resolve,
                                              and emit_children).
                                              The mmap is lazy-faulted
                                              so the cost in physical
                                              memory is what's
                                              actually touched.  Same
                                              spirit as M64's main-
                                              thread bump (16→64 KiB)
                                              when the GUI thread
                                              first hit the same
                                              symptom on deep DOMs. */

static inline int thread_spawn(clone_entry_t entry, void *arg)
{
    void *stack = mmap(NULL, THREAD_STACK_BYTES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -12;       /* -ENOMEM */

    /* Stacks grow DOWN.  AAPCS requires SP be 16-byte aligned
     * at every function call boundary.  THREAD_STACK_BYTES is
     * a multiple of 16, and mmap guarantees page alignment, so
     * `stack + THREAD_STACK_BYTES` is already 16-aligned. */
    void *sp_top = (uint8_t *)stack + THREAD_STACK_BYTES;

    int tid = clone(entry, arg, sp_top, NULL);
    if (tid < 0) {
        /* Clone failed; reclaim the stack we just allocated.
         * We get the lazy-fault optimisation for free here:
         * if the kernel hadn't yet faulted any pages in, the
         * munmap is essentially just dropping the vma. */
        (void)munmap(stack, THREAD_STACK_BYTES);
        return tid;
    }
    return tid;
}

/* Wait for a specific clone() child to exit.  Returns the
 * child's exit code on success, or -1 if `tid` isn't a child
 * of the calling thread (or has already been reaped by another
 * caller).  This is a thin wrapper over waitpid(tid, ...). */
static inline int thread_join(int tid)
{
    int code = -1;
    int reaped = waitpid(tid, &code, 0);
    if (reaped < 0) return -1;
    return code;
}

/* ── Chapter 92 — thread_spawn_on (CPU-pinned) ─────────────────
 *
 * Same as thread_spawn but pins the new thread to absolute CPU
 * `cpu_id` for its lifetime.  cpu_id == -1 inherits the calling
 * thread's CPU (identical to thread_spawn).  cpu_id in
 * [0, SMP_MAX_CPUS) pins to that CPU.
 *
 * Useful for parking workers on the secondary core while the
 * main thread keeps a UI responsive on CPU 0.  Example:
 *
 *   int parser_tid = thread_spawn_on(parse_html_fn, &ctx, 1);
 *
 * Threads do not migrate after creation, so the CPU you pick
 * here is the one the worker runs on for its full lifetime.
 * Returns the child's tid on success or -errno (mostly -ENOMEM
 * for stack OOM, -EINVAL for an out-of-range cpu_id). */
static inline int thread_spawn_on(clone_entry_t entry, void *arg,
                                  int cpu_id)
{
    void *stack = mmap(NULL, THREAD_STACK_BYTES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -12;

    void *sp_top = (uint8_t *)stack + THREAD_STACK_BYTES;

    int tid = clone2(entry, arg, sp_top, NULL, cpu_id);
    if (tid < 0) {
        (void)munmap(stack, THREAD_STACK_BYTES);
        return tid;
    }
    return tid;
}

/* ── Chapter 93 — thread_spawn_files (shared fd table) ─────────
 *
 * Same as thread_spawn_on but the new thread *shares* the
 * calling thread's fd_table (CLONE_FILES).  Both threads see
 * the same fd numbers; an open() in either is visible to the
 * other; a close() in either truly closes the descriptor.
 * The fd_table is freed only when the LAST referencing thread
 * exits.
 *
 * This is the right tool whenever a worker thread needs to
 * share I/O state with its parent — e.g. the chapter-94
 * browser parser thread reading from a TCP socket the GUI
 * thread opened, or a logger thread writing to a log fd the
 * caller already opened.
 *
 * Returns the child's tid on success or -errno (-ENOMEM for
 * stack OOM, -EINVAL for an out-of-range cpu_id, -EFAULT for
 * an unreachable struct, etc).  cpu_id semantics are identical
 * to thread_spawn_on. */
static inline int thread_spawn_files(clone_entry_t entry, void *arg,
                                     int cpu_id)
{
    void *stack = mmap(NULL, THREAD_STACK_BYTES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -12;

    void *sp_top = (uint8_t *)stack + THREAD_STACK_BYTES;

    struct clone_args a;
    a.flags     = CLONE_FILES;
    a.entry     = (uint64_t)(uintptr_t)entry;
    a.arg       = (uint64_t)(uintptr_t)arg;
    a.stack_top = (uint64_t)(uintptr_t)sp_top;
    a.tls       = 0;
    a.cpu_id    = (int32_t)cpu_id;
    a._pad      = 0;

    int tid = clone3(&a);
    if (tid < 0) {
        (void)munmap(stack, THREAD_STACK_BYTES);
        return tid;
    }
    return tid;
}

#endif /* USER_THREAD_H */
