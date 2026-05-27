/* userspace/libc/wchar.h — chapter 180 stub.
 *
 * Two callers in the binutils-2.44 cross build:
 *
 *   1. gas/read.c (around line 43) unconditionally `#include
 *      "wchar.h"`.
 *   2. gas/read.c::read_symbol_name calls
 *          mbstowcs (NULL, name, 0) == (size_t) -1
 *      as a validity probe on user-supplied symbol names.
 *
 * Our source language is ASCII-only, so the multi-byte/wide-char
 * machinery degenerates to a passthrough.  We provide just enough:
 *
 *   - rely on GCC's <stddef.h> for `wchar_t` (do NOT re-typedef
 *     here: GCC defines it as `__WCHAR_TYPE__` which expands to
 *     `unsigned int` for aarch64-elf, and a duplicate `typedef int
 *     wchar_t` triggers a hard error)
 *   - `wint_t`, `WEOF`, `WCHAR_MIN/MAX` constants
 *   - a static-inline `mbstowcs` that treats the input as plain
 *     ASCII (one byte == one wide char, no multibyte sequences)
 *
 * If a vendor build needs real multi-byte parsing, replace this
 * stub with a proper UTF-8 → UTF-32 decoder.  For now every input
 * gas, ld, or gcc sees is plain ASCII.
 */
#ifndef _OSDEV_WCHAR_H
#define _OSDEV_WCHAR_H

#include "stddef.h"   /* size_t, wchar_t (via GCC builtin) */

typedef int wint_t;

#define WEOF       ((wint_t)-1)
#define WCHAR_MIN  (-2147483647 - 1)
#define WCHAR_MAX  2147483647

/* ASCII passthrough.  Per POSIX:
 *   - if dst is NULL, return the number of wide chars the string
 *     would convert to (excluding the terminator);
 *   - otherwise convert up to n wide chars and return the count;
 *   - return (size_t)-1 on conversion error.
 * For 7-bit ASCII input there is no error path — one byte one
 * wide char.  We probe each byte with `(unsigned char)c < 128` so
 * a stray high-bit byte returns (size_t)-1 (preserves gas's
 * validity check semantics). */
static inline size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (!dst) {
        while (src[i]) {
            if ((unsigned char)src[i] >= 128) return (size_t)-1;
            i++;
        }
        return i;
    }
    while (i < n && src[i]) {
        if ((unsigned char)src[i] >= 128) return (size_t)-1;
        dst[i] = (wchar_t)(unsigned char)src[i];
        i++;
    }
    if (i < n) dst[i] = 0;
    return i;
}

#endif
