/* userspace/libc/strings.h — chapter 130a.
 *
 * POSIX legacy header: case-insensitive string compare.  C99
 * moved these into <string.h>; <strings.h> still exists for
 * pre-C99 code that #include's it (DoomGeneric does, via
 * doomtype.h).
 *
 * Just re-export the strcasecmp/strncasecmp/bzero declarations
 * from string.h so callers compile.  No new code — we're a
 * forwarding header. */
#ifndef USERSPACE_LIBC_STRINGS_H
#define USERSPACE_LIBC_STRINGS_H

#include "string.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bzero — equivalent to memset(s, 0, n).  Used by a handful of
 * upstream codebases (Doom doesn't, but provide it anyway to
 * spare the next port). */
static inline void bzero(void *s, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = 0;
}

/* bcopy — like memmove(dst, src, n) but with arguments in
 * (src, dst) order.  Pre-POSIX.1-2001 portability. */
static inline void bcopy(const void *src, void *dst, size_t n)
{
    (void)memmove(dst, src, n);
}

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIBC_STRINGS_H */
