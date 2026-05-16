/* userspace/libc/signal.h — Catchable signals (chapter 77).
 *
 * Header-only wrapper around SYS_SIGACTION / SYS_SIGRETURN.
 * The kernel-side delivery contract is documented in
 * kernel/core/syscall.c near `deliver_signal` and the matching
 * `struct sigframe_k`; if you change one, change the other.
 *
 * Quick guide for users of this header:
 *
 *   void on_int(int sig) { ... }
 *   signal(SIGINT, on_int);
 *
 * Use SIG_DFL / SIG_IGN like POSIX:
 *
 *   signal(SIGINT, SIG_IGN);   // ignore Ctrl-C
 *   signal(SIGINT, SIG_DFL);   // back to "terminate w/ 130"
 *
 * Caveats vs real POSIX:
 *   - No sigmask, no sigprocmask, no SA_RESTART.  Slow syscalls
 *     are NOT yet interrupted by signals (no -EINTR returns); a
 *     handler runs only at the next syscall return.
 *   - SIGKILL is uncatchable (the kernel coerces requests back
 *     to SIG_DFL silently).
 *   - Handler nesting is allowed (no automatic mask-during-
 *     delivery); avoid recursing into the same signal.
 *   - Only the synchronous `signal()` shape is provided; if you
 *     need the old/new-action exchange you can call sigaction()
 *     directly.
 */
#ifndef USERSPACE_LIBC_SIGNAL_H
#define USERSPACE_LIBC_SIGNAL_H

#include <stdint.h>
#include "syscall.h"

typedef void (*sig_handler_t)(int signo);

/* Default disposition: terminate with exit code 128 + signum. */
#define SIG_DFL   ((sig_handler_t)0)
/* Drop the signal without touching user state. */
#define SIG_IGN   ((sig_handler_t)1)

/* Trampoline supplied by crt0.S.  The first sigaction() call
 * passes its address to the kernel; subsequent calls reuse it. */
extern void __sigreturn_trampoline(void);

/*
 * sigaction(sig, handler) -> previous handler.
 *
 * Pass SIG_DFL or SIG_IGN to (re)set the disposition; pass any
 * other function pointer to install a catcher.  Returns the
 * previous handler so callers can restore it.  On error
 * (out-of-range signum) returns (sig_handler_t)-1 and leaves
 * the disposition unchanged.
 *
 * The first call also registers crt0's __sigreturn_trampoline
 * with the kernel; subsequent calls pass restorer=0 (= "keep
 * existing").  This means a program that catches its first
 * signal pays a one-time cost of an extra syscall arg; calls
 * after that are normal three-arg.
 */
static inline sig_handler_t sigaction(int sig, sig_handler_t handler)
{
    long old = _svc3(SYS_SIGACTION,
                     (long)sig,
                     (long)(uintptr_t)handler,
                     (long)(uintptr_t)__sigreturn_trampoline);
    /* Out-of-range signum: kernel returns a large unsigned value
     * derived from a negative errno.  We can't compare against
     * -EINVAL because libc has no errno.h yet — recognise the
     * "very large" case as failure. */
    if ((uint64_t)old > (uint64_t)0xFFFFFFFF00000000ULL)
        return (sig_handler_t)-1;
    return (sig_handler_t)(uintptr_t)old;
}

/* signal() is a convenience wrapper.  Kept for ergonomics; some
 * programs (e.g. sh, browser) prefer the simpler shape. */
static inline sig_handler_t signal(int sig, sig_handler_t handler)
{
    return sigaction(sig, handler);
}

/* sigreturn() is normally invoked via the trampoline; callers
 * shouldn't need to issue it themselves.  Exposed for parity. */
struct sigframe {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t elr;
    uint64_t spsr;
    uint32_t signum;
    uint32_t pad;
    uint64_t pad2;     /* round to 288 — must match kernel sigframe_k */
};

#endif /* USERSPACE_LIBC_SIGNAL_H */
