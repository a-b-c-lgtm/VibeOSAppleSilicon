#ifndef _USERSPACE_LIBC_ATEXIT_H
#define _USERSPACE_LIBC_ATEXIT_H

/*
 * atexit() and __cxa_finalize() — userspace/libc/atexit.h
 *
 * Header-only.  Including this header in any TU defines a
 * strong `__cxa_finalize` symbol that overrides the weak
 * no-op default that crt0.S provides.  The override walks
 * the atexit slot table in LIFO order, exactly like glibc
 * and musl.
 *
 * Capacity is fixed at 32 slots.  Real libc has no fixed
 * cap (it heap-allocates), but no plausible userspace app
 * we ship registers more than two or three, so 32 is far
 * more than we need.  Overflow returns -1 from atexit and
 * is silently dropped from the finalize walk; behaviour
 * matches POSIX which says atexit MAY fail past
 * ATEXIT_MAX (32 here).
 *
 * Storage is plain .bss — the kernel hands out zeroed
 * pages, so g_atexit_n starts at 0 implicitly.  No
 * constructor required.
 *
 * Threading: not safe.  Our libc has no global locks and
 * userspace threads share .bss.  If two threads both call
 * atexit concurrently the slot count is racy.  We accept
 * this — same trade-off newlib makes.
 */

#define ATEXIT_MAX 32

static void (*g_atexit_fns[ATEXIT_MAX])(void);
static int   g_atexit_n;
static int   g_atexit_in_finalize;   /* re-entry guard */

static inline int atexit(void (*fn)(void))
{
    if (!fn) return -1;
    if (g_atexit_n >= ATEXIT_MAX) return -1;
    g_atexit_fns[g_atexit_n++] = fn;
    return 0;
}

/* Override of crt0.S's weak default.  crt0 calls this with
 * arg=NULL after main returns; passing a non-null dso_handle
 * (real C++ semantics) is not modelled — every registered
 * handler runs unconditionally.
 *
 * Must be a STRONG global to win over crt0.S's `.weak`
 * default no-op.  Marking it weak here would put two weak
 * symbols in scope; the linker may then pick crt0's no-op
 * and silently skip the atexit chain (see test_atexit.py
 * regression observed end of chapter 179).
 *
 * Multi-TU vendor builds (binutils ld: ~150 .o files all
 * carrying their own copy of this header via <stdlib.h>)
 * suppress the body via OSDEV_LIBC_NO_GLOBAL_DEFS — the
 * same guard chapter 172's Doom shim uses.  The vendor
 * build's CFLAGS sets -DOSDEV_LIBC_NO_GLOBAL_DEFS so no
 * vendor TU emits __cxa_finalize; crt0's weak no-op then
 * satisfies the call (vendor code doesn't atexit anyway). */
#ifndef OSDEV_LIBC_NO_GLOBAL_DEFS
void __cxa_finalize(void *dso_handle)
{
    (void)dso_handle;
    /* Re-entry: if a handler itself calls _exit() (which
     * re-enters crt0's tail), we mustn't loop forever. */
    if (g_atexit_in_finalize) return;
    g_atexit_in_finalize = 1;
    while (g_atexit_n > 0) {
        void (*fn)(void) = g_atexit_fns[--g_atexit_n];
        if (fn) fn();
    }
    /* Also walk .fini_array — symbols provided by linker_user.ld.
     * Declared here as extern weak so a binary with no .fini_array
     * still links (start == end == NULL → loop is empty). */
    extern void (*__fini_array_start[])(void) __attribute__((weak));
    extern void (*__fini_array_end[])(void)   __attribute__((weak));
    if (__fini_array_start && __fini_array_end) {
        /* destructors run in reverse registration order */
        void (**p)(void) = __fini_array_end;
        while (p > __fini_array_start) { p--; if (*p) (*p)(); }
    }
}
#endif /* OSDEV_LIBC_NO_GLOBAL_DEFS */

#endif /* _USERSPACE_LIBC_ATEXIT_H */
