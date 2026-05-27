/* userspace/libc/sys/types.h — chapter 172.
 *
 * POSIX `<sys/types.h>`: typedefs for ssize_t, off_t, pid_t,
 * mode_t, uid_t, gid_t, etc.  This OS's other headers
 * (sys/stat.h, syscall.h) already define struct stat and the
 * mode_t macros, so we mostly forward to <stdint.h> and add
 * the typedefs nothing else has yet. */
#ifndef USERSPACE_LIBC_SYS_TYPES_H
#define USERSPACE_LIBC_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;       /* signed counterpart of size_t */
#endif

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef int64_t off_t;      /* 64-bit file offsets — POSIX-2008
                               (matches syscall.h's typedef so
                               both headers can coexist). */
#endif

#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int  pid_t;
#endif

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif

#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef int64_t time_t;     /* match syscall.h */
#endif

#ifndef _CLOCK_T_DEFINED
#define _CLOCK_T_DEFINED
typedef long clock_t;
#endif

/* chapter 186 — libcpp/include/cpplib.h:807,813 references
 * ino_t / dev_t to declare struct cpp_dir's stat-cache slots.
 * Match POSIX: ino_t is unsigned long, dev_t is unsigned long
 * long (Linux convention; opaque to libcpp which only compares
 * for equality). */
#ifndef _INO_T_DEFINED
#define _INO_T_DEFINED
typedef unsigned long      ino_t;
#endif

#ifndef _DEV_T_DEFINED
#define _DEV_T_DEFINED
typedef unsigned long long dev_t;
#endif

#endif /* USERSPACE_LIBC_SYS_TYPES_H */
