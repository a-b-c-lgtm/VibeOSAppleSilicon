/* userspace/libc/sys/times.h — chapter 186
 *
 * Minimal <sys/times.h> for the in-guest gcc cross-build.  We
 * don't actually track per-process CPU time on this OS, so
 * times() reports wall-clock ticks in tms_utime and zeros for
 * the rest.  That's enough for `time.cc`, `timevar.cc`, and
 * any "elapsed seconds" reporting in gcc.
 */
#ifndef _OSDEV_SYS_TIMES_H
#define _OSDEV_SYS_TIMES_H 1

#include "../time.h"
#include "../syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

static inline clock_t times(struct tms *buf)
{
    clock_t now = clock();
    if (buf) {
        buf->tms_utime  = now;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return now;
}

#ifdef __cplusplus
}
#endif

#endif /* _OSDEV_SYS_TIMES_H */
