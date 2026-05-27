/* userspace/libc/string.h — chapter 167.
 *
 * Header-only str* family that real upstream code expects.  Most
 * are short loops; the longer ones (memmem, strdup) get one
 * extern definition each in cstring.c to keep code size down
 * when several binaries pull them in.
 *
 * mem* functions (memcpy/memset/memmove/memcmp) already exist
 * in two flavours:
 *   - extern definitions in userspace/libc/cstring.c -- linked
 *     into every binary that wants to use BearSSL or any other
 *     module that issues calls to them by name.
 *   - a `static inline strlen` shim in userspace/libc/syscall.h,
 *     guarded by OSDEV_STRLEN_PROVIDED.
 *
 * We deliberately do NOT redefine memcpy / memset / memmove /
 * memcmp here -- including both this header and cstring.o would
 * produce duplicate-symbol errors (the extern is non-inline).
 * Bring them in by declaration only, and trust cstring.o or the
 * BearSSL shim's prototypes when present.
 */
#ifndef USERSPACE_LIBC_STRING_H
#define USERSPACE_LIBC_STRING_H

#include <stddef.h>

/* Chapter 179 — POSIX places `char *strerror(int)` in <string.h>.
 * Pull our static-inline definition in by including errno.h here.
 * libsframe/sframe-error.c only includes <string.h> and expects
 * strerror's prototype to come along for the ride. */
#include "errno.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Re-declare cstring.o's externs.  Repeated `extern` of the same
 * signature is fine. */
void  *memcpy (void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset (void *dst, int c, size_t n);
int    memcmp (const void *a, const void *b, size_t n);

/* strlen — declared here as a non-static extern, satisfied by
 * cstring.c.  Set OSDEV_STRLEN_PROVIDED so syscall.h's static
 * inline shim doesn't also try to define it in the same TU
 * (which would error out as "static declaration follows
 * non-static").  Chapter 172 hit this when DoomGeneric's
 * f_wipe.c pulled both string.h and (via z_zone.h → stdlib.h)
 * syscall.h. */
#ifndef OSDEV_STRLEN_PROVIDED
#define OSDEV_STRLEN_PROVIDED
size_t strlen (const char *s);
#endif

/* memchr — find first byte equal to c (cast to unsigned char) in
 * the first n bytes of s.  Returns NULL if not present. */
static inline void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char target = (unsigned char)c;
    while (n--) {
        if (*p == target) return (void *)p;
        p++;
    }
    return (void *)0;
}

/* strchr / strrchr — find first/last byte equal to c.  POSIX:
 * c == '\0' matches the trailing NUL. */
static inline char *strchr(const char *s, int c)
{
    char target = (char)c;
    for (;;) {
        if (*s == target) return (char *)s;
        if (*s == '\0')   return (char *)0;
        s++;
    }
}

static inline char *strrchr(const char *s, int c)
{
    char target = (char)c;
    const char *last = (const char *)0;
    for (;;) {
        if (*s == target) last = s;
        if (*s == '\0')   return (char *)last;
        s++;
    }
}

/* strcmp / strncmp — byte-wise unsigned comparison.  Returns
 * negative / zero / positive following the standard convention. */
static inline int strcmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (*p && *p == *q) { p++; q++; }
    return (int)*p - (int)*q;
}

static inline int strncmp(const char *a, const char *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q)        return (int)*p - (int)*q;
        if (*p == 0)         return 0;
        p++; q++;
    }
    return 0;
}

/* strcpy / strncpy — classic POSIX shapes.  strncpy zero-pads
 * (per C99 7.21.2.4) if the source is shorter than n; if longer,
 * the result is NOT NUL-terminated.  Yes, this is the broken
 * semantics; we match it because that's what real upstream code
 * expects.  Use strlcpy (provided below) for sane behaviour. */
static inline char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

static inline char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d = *src) != '\0') { d++; src++; n--; }
    while (n--) *d++ = '\0';
    return dst;
}

/* strcat / strncat — append.  strncat always NUL-terminates
 * (unlike strncpy, because POSIX is inconsistent that way). */
static inline char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

static inline char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d) d++;
    while (n && (*d = *src) != '\0') { d++; src++; n--; }
    *d = '\0';
    return dst;
}

/* strspn / strcspn — span of bytes in / not in `accept`. */
static inline size_t strspn(const char *s, const char *accept)
{
    const char *p = s;
    while (*p) {
        const char *a = accept;
        while (*a && *a != *p) a++;
        if (!*a) break;
        p++;
    }
    return (size_t)(p - s);
}

static inline size_t strcspn(const char *s, const char *reject)
{
    const char *p = s;
    while (*p) {
        const char *r = reject;
        while (*r && *r != *p) r++;
        if (*r) break;
        p++;
    }
    return (size_t)(p - s);
}

/* strpbrk — first byte in s that's also in accept. */
static inline char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;
        while (*a) if (*a++ == *s) return (char *)s;
        s++;
    }
    return (char *)0;
}

/* strstr — first occurrence of needle in haystack.  Simple
 * O(n*m) scan; good enough for everything we link today. */
static inline char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return (char *)0;
}

/* strlcpy / strlcat — BSD-flavoured safe-by-construction copy.
 * Always NUL-terminate (unless size == 0).  Return the total
 * length of the source string -- callers can detect truncation
 * with `if (strlcpy(buf, src, sizeof buf) >= sizeof buf)`.
 *
 * Not in C99/POSIX but widely used; Doom doesn't need it, but
 * notepad and shell ports do.  Adding now to avoid the per-app
 * re-implementation tax. */
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = 0;
    while (src[srclen]) srclen++;
    if (size > 0) {
        size_t copy = (srclen < size - 1) ? srclen : size - 1;
        for (size_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = '\0';
    }
    return srclen;
}

static inline size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = 0;
    while (dlen < size && dst[dlen]) dlen++;
    size_t srclen = 0;
    while (src[srclen]) srclen++;
    if (dlen == size) return size + srclen;   /* dst not NUL-terminated */
    size_t avail = size - dlen - 1;
    size_t copy  = (srclen < avail) ? srclen : avail;
    for (size_t i = 0; i < copy; i++) dst[dlen + i] = src[i];
    dst[dlen + copy] = '\0';
    return dlen + srclen;
}

/* strdup — malloc a copy of s.  Chapter 172 (Doom port):
 * DoomGeneric uses strdup heavily for parsing -iwad / -file
 * paths and for assembling savegame filenames.  Returns NULL
 * on alloc failure.  Caller frees with free().
 *
 * Not a `static inline` because (a) it pulls in malloc.h,
 * which we don't want every TU that includes string.h to
 * have to declare, and (b) several .o files in Doom would
 * each emit their own copy.  Definition lives in cstring.c. */
char *strdup(const char *s);

/* strcasecmp / strncasecmp — POSIX byte-wise compare with
 * ASCII-only case folding.  Chapter 172: Doom's WAD code
 * compares lump names case-insensitively (sometimes the
 * IWAD has FLOOR0_1, sometimes floor0_1). */
static inline int strcasecmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (;;) {
        unsigned char ca = *p, cb = *q;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca)      return 0;
        p++; q++;
    }
}

static inline int strncasecmp(const char *a, const char *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        unsigned char ca = *p, cb = *q;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca)      return 0;
        p++; q++;
    }
    return 0;
}

/* atoi — convert decimal ASCII to int.  Stops at the first
 * non-digit.  No error reporting (use strtol from chapter 169
 * when you need it).  Tolerates leading whitespace and an
 * optional sign. */
static inline int atoi(const char *s)
{
    int sign = 1;
    int v    = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}

/* strtok — POSIX tokenizer.  Not thread-safe (uses an internal
 * static cursor); chapter 186 adds it for gcc's attribs.cc
 * which uses it to split target-attribute strings.
 *
 * Mutates *s* by writing NUL over the first separator byte
 * after each token.  Call with a non-NULL str on the first
 * call, then NULL to continue. */
static inline char *strtok(char *s, const char *sep)
{
    static char *_strtok_state;
    if (s) _strtok_state = s;
    if (!_strtok_state) return 0;

    /* skip leading separators */
    char *p = _strtok_state;
    while (*p) {
        const char *q = sep;
        int is_sep = 0;
        while (*q) { if (*q++ == *p) { is_sep = 1; break; } }
        if (!is_sep) break;
        p++;
    }
    if (!*p) { _strtok_state = 0; return 0; }

    char *tok = p;
    /* scan to end of token */
    while (*p) {
        const char *q = sep;
        int is_sep = 0;
        while (*q) { if (*q++ == *p) { is_sep = 1; break; } }
        if (is_sep) { *p = 0; _strtok_state = p + 1; return tok; }
        p++;
    }
    _strtok_state = 0;
    return tok;
}

/* strtok_r — reentrant POSIX tokenizer.  Caller owns the saveptr.
 * Required by gcc/config/aarch64/aarch64.cc which uses it to parse
 * -mcpu / -march extension strings. */
static inline char *strtok_r(char *s, const char *sep, char **saveptr)
{
    if (!saveptr) return 0;
    if (s) *saveptr = s;
    if (!*saveptr) return 0;

    char *p = *saveptr;
    while (*p) {
        const char *q = sep;
        int is_sep = 0;
        while (*q) { if (*q++ == *p) { is_sep = 1; break; } }
        if (!is_sep) break;
        p++;
    }
    if (!*p) { *saveptr = 0; return 0; }

    char *tok = p;
    while (*p) {
        const char *q = sep;
        int is_sep = 0;
        while (*q) { if (*q++ == *p) { is_sep = 1; break; } }
        if (is_sep) { *p = 0; *saveptr = p + 1; return tok; }
        p++;
    }
    *saveptr = 0;
    return tok;
}

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIBC_STRING_H */
