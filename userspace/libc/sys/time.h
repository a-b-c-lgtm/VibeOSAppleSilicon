/* userspace/libc/sys/time.h — chapter 132f.
 *
 * Pure compatibility shim.  All the real machinery (struct
 * timeval, gettimeofday) lives in <syscall.h> alongside the
 * SYS_GETTIMEOFDAY wrapper.  This header just forwards so
 * upstream code that does `#include <sys/time.h>` (gcc-14's
 * libcpp/system.h, BearSSL, etc.) sees the same definitions
 * and doesn't double-declare them.
 *
 * `suseconds_t` and `struct timezone` are the two pieces POSIX
 * places here that syscall.h doesn't carry; they're tiny so
 * we add them in this TU. */
#ifndef USERSPACE_LIBC_SYS_TIME_H
#define USERSPACE_LIBC_SYS_TIME_H

#include "../syscall.h"   /* struct timeval, gettimeofday() */
#include "../time.h"      /* struct tm, gmtime/localtime/asctime */
#include "types.h"        /* time_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SUSECONDS_T_DEFINED
#define _SUSECONDS_T_DEFINED
typedef long suseconds_t;
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIBC_SYS_TIME_H */
