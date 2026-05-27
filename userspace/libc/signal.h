/* userspace/libc/signal.h — Catchable signals (chapter 76).
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

#ifdef __cplusplus
extern "C" {
#endif

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

/* ── Chapter 166 — raise() / abort() ────────────────────────
 *
 * Two small C99 conveniences built on top of the existing
 * kill() + getpid() syscalls.  Real upstream code (Doom's
 * I_Quit, BearSSL's selftests, every libc-using test harness)
 * calls these by name; without them every port has to write a
 * three-line shim.  With them, the port just compiles.
 */

/* raise(sig) -> int
 *
 * Send `sig` to the calling thread.  Returns 0 on success or
 * -1 on error (mirrors C99 7.14.2.1).  Implemented as a
 * straight-through kill(getpid(), sig); the kernel does the
 * actual queueing and delivery on the next syscall return.
 */
static inline int raise(int sig)
{
    return (kill(getpid(), sig) == 0) ? 0 : -1;
}

/* abort(void) -> __attribute__((noreturn))
 *
 * Raise SIGABRT against the calling thread, then -- in case the
 * caller had SIGABRT ignored or caught with a returning handler
 * -- restore SIG_DFL and raise SIGABRT again, then call exit().
 * C99 7.20.4.1 requires abort() never to return; this three-
 * step dance is the standard implementation idiom (also used
 * by glibc, musl, and BSD libc).
 *
 * The exit() at the end is the belt-and-braces backstop: even
 * if SIG_DFL somehow doesn't terminate (it does — kernel
 * default action is "exit with 128+sig"), we still leave with
 * 128 + SIGABRT so a waiting parent's waitpid() sees something
 * sensible.
 */
__attribute__((noreturn))
static inline void abort(void)
{
    raise(SIGABRT);
    sigaction(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    exit(128 + SIGABRT);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* USERSPACE_LIBC_SIGNAL_H */
