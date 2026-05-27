/* userspace/libc/sys/param.h — chapter 179 minimal BSD-isms.
 *
 * libctf/ctf-impl.h and ctf-create.c unconditionally include
 * <sys/param.h>.  ctf-decls.h then `#undef`s and redefines
 * MIN / MAX itself, so all we need is a header that exists and
 * provides a reasonable MAXPATHLEN for the few callers that
 * still reach for it.
 */
#ifndef USER_SYS_PARAM_H
#define USER_SYS_PARAM_H 1

#include "types.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif

#endif /* USER_SYS_PARAM_H */
