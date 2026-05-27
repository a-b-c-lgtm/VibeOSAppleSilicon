/*
 * kernel/core/strace.h — chapter 102 per-thread syscall tracer.
 *
 * Design summary
 * --------------
 * A traced thread carries one `struct strace_ring` allocated on
 * demand from kheap.  Every SVC entered through svc_dispatch
 * checks `t->strace` (one branch — the "free when not attached"
 * property): if non-NULL, the dispatcher records a fresh entry
 * BEFORE the handler runs, then back-fills the return value
 * AFTER the handler returns.
 *
 * The ring is overwrite-on-full: a slow reader will see "lost
 * entries" rather than back-pressure on the traced thread.  The
 * traced thread never blocks waiting for /proc/<pid>/trace to
 * be drained.
 *
 * The ring is exposed read-only as `/proc/<pid>/trace`.  Each
 * open snapshots the current contents into a procfs buffer
 * (chapter 101's snapshot-at-open shape) AND drains them — the
 * next open sees only entries added in between.  This makes
 * the natural `cat /proc/<pid>/trace` loop work as a poll.
 *
 * Why per-thread, not per-process: we don't have process groups,
 * and fork is rare relative to threads-via-clone (chapter 92).
 * One ring per thread keeps the lock per-ring (cheap) and
 * matches POSIX strace's default "don't follow fork" behaviour.
 *
 * Ownership: the ring is freed in the two reap sites (the same
 * places kfree(stack_base) is called) via strace_release().
 * Allocation happens in sys_trace_me; userspace cannot allocate
 * for another thread (no ptrace_attach yet).
 */
#ifndef STRACE_H
#define STRACE_H

#include <stddef.h>
#include <stdint.h>

struct thread;

/* Ring capacity in entries.  Power of two so head & (cap-1) is
 * cheap modulo.  Sized small (64) so the textual rendering fits
 * inside one PROCFS_MAX_FILE buffer (8 KiB) even when every
 * recorded syscall is the widest case (6 args, 19-digit hex
 * each + name).  At 64 entries × ~120 bytes worst case we land
 * around 8 KiB. */
#define STRACE_RING_CAP   64

/* One recorded syscall.  Filled in two phases:
 *   1. trace_enter (before dispatch) fills syscall_no, args[],
 *      ts_ms, and sets completed = 0.
 *   2. trace_exit (after dispatch) writes ret and sets
 *      completed = 1.
 *
 * Renderers see incomplete entries when the thread is mid-
 * syscall (typical case: the read() of /proc/<pid>/trace
 * itself).  We render them with `= ?` so the trace remains
 * self-consistent without needing to wait. */
struct strace_entry {
    uint64_t ts_ms;          /* timer_ticks()*TICK_INTERVAL_MS at enter */
    uint64_t args[6];        /* x0..x5 as observed at enter */
    int64_t  ret;            /* return value once completed */
    uint32_t syscall_no;     /* x8 */
    uint32_t completed;      /* 0 until the syscall returns */
};

struct strace_ring {
    struct strace_entry  entries[STRACE_RING_CAP];
    /* Monotonic indices.  Reader: tail..head-1 are valid.
     * On overwrite, tail is advanced and `lost` is bumped so
     * the renderer can show "(N entries lost)" once. */
    uint32_t             head;
    uint32_t             tail;
    uint32_t             lost;
};

/* ------------------------------------------------------------------
 * Kernel-internal API (used by svc_dispatch and procfs).
 * ------------------------------------------------------------------ */

/* Allocate a fresh ring for `t` if it doesn't have one.  Returns
 * 0 on success or already-attached, -1 on OOM.  Idempotent. */
int strace_enable(struct thread *t);

/* Free the per-thread ring (if any) and clear the pointer.
 * Safe to call on a thread that was never traced.  Called from
 * the two thread-reap sites (waitpid reap + exit's orphan
 * cleanup) alongside the stack/AS free. */
void strace_release(struct thread *t);

/* Record the start of a syscall.  Returns a pointer to the
 * reserved entry so the dispatcher can later stamp `ret` +
 * `completed`, or NULL if the thread isn't traced.  Lock-free
 * because the only writer to `head` is the traced thread
 * itself, which is mid-dispatch and cannot be re-entered.
 *
 * The renderer (running in another thread) reads `head` to
 * decide how far to walk; a torn read at worst causes it to
 * miss the most recent entry, which is fine. */
struct strace_entry *strace_enter(struct thread *t,
                                  uint32_t syscall_no,
                                  uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5);

/* Render the contents of `t`'s ring as text into `out[cap]` and
 * drain the ring (tail := head).  Returns the byte length
 * written (excluding NUL).  Safe to call from any thread; the
 * traced thread is not blocked.  No-op (returns "(not traced)
 * " banner) if `t->strace` is NULL.
 *
 * Used by procfs.c::render_pid_trace; takes t as input rather
 * than a pid so the caller can avoid a second snapshot walk. */
long strace_render_and_drain(struct thread *t, char *out, size_t cap);

#endif /* STRACE_H */
