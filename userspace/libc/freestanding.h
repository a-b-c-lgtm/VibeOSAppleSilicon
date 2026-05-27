/*
 * userspace/libc/freestanding.h -- mem* shims for freestanding userspace.
 *
 * GCC silently emits calls to memcpy / memset / memmove when the
 * optimizer recognises a struct copy or zero-init that's bigger
 * than its inlining threshold.  Our userspace links without libc
 * and the kernel's mem* implementations aren't reachable, so
 * those calls become "undefined reference to memcpy" at link.
 *
 * Centralising the shim here means:
 *
 *   1. Any new libc header that needs to handle a big struct can
 *      just `#include "freestanding.h"` and stop worrying.
 *
 *   2. We don't get duplicate `static memcpy` definitions when
 *      two such headers are included by the same .c (which is
 *      what bit chapter 120 -- both layout.h and cookies.h had
 *      their own copies).
 *
 *   3. There's exactly one place to add memmove() the day a
 *      header needs it.
 *
 * All shims are `static __attribute__((used))` so each TU keeps
 * its own private copy (no link-time duplicate-symbol clashes
 * across binaries) and the unused-function warning never fires
 * when a particular .c doesn't actually trigger an implicit call.
 *
 * Background: /memories/freestanding-c-memset-trap.md
 */

#ifndef OSDEV_LIBC_FREESTANDING_H
#define OSDEV_LIBC_FREESTANDING_H

#include <stddef.h>

static __attribute__((used)) void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static __attribute__((used)) void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

static __attribute__((used)) void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

#endif /* OSDEV_LIBC_FREESTANDING_H */
