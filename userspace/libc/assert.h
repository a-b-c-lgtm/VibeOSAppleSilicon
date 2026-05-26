/* userspace/libc/assert.h — chapter 128c.
 *
 * C99 7.2 <assert.h>.  Two macros:
 *
 *   assert(expr)    -- if NDEBUG is defined, expand to (void)0.
 *                      Otherwise: if !expr, write a diagnostic to
 *                      stderr (fd 2) and abort().
 *   static_assert(cond, msg)
 *                   -- compile-time, via _Static_assert (C11).
 *
 * We deliberately do not pull in stdio.h's fprintf machinery
 * here -- it would force every TU that includes <assert.h> to
 * include the heavy printf state too.  Instead the diagnostic
 * is a fixed-shape "file:line: function: expression" message
 * built with the existing write() syscall and the tiny
 * decimal-int formatter from syscall.h.
 *
 * Real abort() lives in signal.h (chapter 128b).  We forward-
 * declare it as `extern` to avoid a circular include.
 */
#ifndef USERSPACE_LIBC_ASSERT_H
#define USERSPACE_LIBC_ASSERT_H

#ifdef NDEBUG
# define assert(expr) ((void)0)
#else

/* Defined in userspace/libc/cstring.c so every binary that links
 * cstring.o (and a few that don't -- we add a small extern shim
 * if needed) sees one definition. */
extern __attribute__((noreturn))
void __assert_fail(const char *expr,
                   const char *file,
                   int line,
                   const char *func);

# define assert(expr)                                              \
    ((expr)                                                        \
        ? (void)0                                                  \
        : __assert_fail(#expr, __FILE__, __LINE__, __func__))

#endif /* NDEBUG */

/* C11 _Static_assert spelled the C99 way for older translation
 * environments (none of ours are < C11 in practice, but the spec
 * spells the user-facing name `static_assert` and points at
 * _Static_assert as the keyword). */
#ifndef static_assert
# define static_assert(cond, msg) _Static_assert(cond, msg)
#endif

#endif /* USERSPACE_LIBC_ASSERT_H */
