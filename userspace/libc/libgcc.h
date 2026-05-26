#ifndef _USERSPACE_LIBC_LIBGCC_H
#define _USERSPACE_LIBC_LIBGCC_H

/*
 * libgcc.h — header-only stand-in for the helpers GCC's code
 * generator can emit calls to on aarch64.
 *
 * On aarch64 the helper list is short: most arithmetic the
 * hardware does in a single instruction.  These are the ones
 * GCC at -Os can still reach for:
 *
 *   __popcountdi2(int64_t x)  — popcount of low 64 bits
 *   __clzdi2(int64_t x)       — count leading zeros
 *   __ctzdi2(int64_t x)       — count trailing zeros
 *   __stack_chk_fail()        — SSP trap; aborts the process
 *
 * 128-bit divides (__udivti3, __divti3) are not provided yet
 * — none of our currently-shipped C source triggers them.
 * They land in chapter 121 when TCC starts pulling at libgcc
 * for code it can't lower in a single instruction.
 *
 * Including this header in any TU pulls all four definitions
 * in.  They're `static` so multiple TUs each get their own
 * copy without colliding — same convention as the rest of our
 * header-only libc.
 *
 * For chapter 121 onward we ALSO build these into a real
 * `/lib/libgcc.a` archive (via the host `aarch64-elf-ar` at
 * build time) so the in-guest /bin/cc driver has a file to
 * point at when it invokes /bin/ld -lgcc.
 */

#include "syscall.h"   /* for _exit */

static int __popcountdi2(long x)
{
    unsigned long u = (unsigned long)x;
    int c = 0;
    while (u) { c += (int)(u & 1u); u >>= 1; }
    return c;
}

static int __clzdi2(long x)
{
    unsigned long u = (unsigned long)x;
    if (!u) return 64;
    int c = 0;
    while (!(u & (1ULL << 63))) { c++; u <<= 1; }
    return c;
}

static int __ctzdi2(long x)
{
    unsigned long u = (unsigned long)x;
    if (!u) return 64;
    int c = 0;
    while (!(u & 1u)) { c++; u >>= 1; }
    return c;
}

static void __stack_chk_fail(void)
{
    /* SSP trap: write a one-line marker to stderr (fd 2) and
     * exit with 127.  Real glibc raises SIGABRT; we have no
     * sigabrt-from-libc story yet, so the SYS_EXIT is the
     * least-surprising approximation. */
    static const char msg[] = "*** stack smashing detected ***\n";
    (void)write(2, msg, sizeof(msg) - 1);
    _exit(127);
}

/* Reference the helpers so the compiler doesn't warn
 * "defined but not used" on -Werror=unused-function when a
 * TU includes the header purely to enable libgcc symbol
 * resolution.  Pattern matches the other header-only libc
 * files in this repo. */
static inline void __libgcc_h_touch(void)
{
    (void)__popcountdi2;
    (void)__clzdi2;
    (void)__ctzdi2;
    (void)__stack_chk_fail;
}

#endif /* _USERSPACE_LIBC_LIBGCC_H */
