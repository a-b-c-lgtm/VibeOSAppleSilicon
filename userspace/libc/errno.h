/* userspace/libc/errno.h -- POSIX errno for osdev userspace
 * (chapter 149, convention flipped in chapter 152).
 *
 * Today: a process-global `int __errno_value` exposed via the
 * standard `errno` macro.  Set automatically by every syscall
 * wrapper that returns a negative kernel errno (see
 * `__svc_check` in syscall.h).  As of chapter 152, syscall
 * wrappers follow the POSIX shape: on failure they return -1
 * and set `errno`.  Callers must read `errno` to recover the
 * specific error; the old `-rc` trick is gone.
 *
 * Per-thread errno is deferred until userspace threading grows
 * a real TLS layout.  For now every thread in a multi-threaded
 * process shares the same `__errno_value` slot.  Single-threaded
 * programs (everything we ship today, and everything GCC's
 * bring-up forks) are correct.  The browser parser thread does
 * not inspect errno.
 *
 * The E* constants match the kernel's vfs.h / syscall.h values
 * so callers doing `if (errno == ENOENT)` after a kernel-issued
 * failure see the right number.  When in doubt, the kernel
 * names are authoritative; this file is just the userspace
 * mirror.
 *
 * `strerror(e)` returns a static, thread-safe descriptive
 * string.  See the table at the bottom of this file.
 */
#ifndef OSDEV_LIBC_ERRNO_H
#define OSDEV_LIBC_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Process-global errno slot.  Defined once in crt0.S (which
 * every userspace binary links) so apps don't have to thread
 * an extra ERRNO_OBJ through their *_OBJS lists. */
extern int __errno_value;

/* POSIX-shaped errno macro.  Expressions that need to take the
 * address of errno (e.g. `*__errno_location()`) can do so via
 * `&__errno_value`. */
#define errno (__errno_value)

/* The subset we set today.  Values match Linux's <errno.h>
 * AND the kernel's vfs.h / syscall.h.  Add new ones in lockstep
 * with the kernel when the chapter that introduces them lands. */
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD         10
#define EAGAIN         11
#define ENOMEM         12
#define EACCES         13
#define EFAULT         14
#define EBUSY          16
#define EEXIST         17
#define EXDEV          18
#define ENODEV         19
#define ENOTDIR        20
#define EISDIR         21
#define EINVAL         22
#define ENFILE         23
#define EMFILE         24
#define ENOTTY         25
#define ETXTBSY        26
#define EFBIG          27
#define ENOSPC         28
#define ESPIPE         29
#define EROFS          30
#define EMLINK         31
#define EPIPE          32
#define EDOM           33
#define ERANGE         34
#define EDEADLK        35
#define ENOSYS         38
#define ENOTEMPTY      39
#define ENAMETOOLONG   36
#define ELOOP          40
#define EADDRINUSE     98
#define ECONNRESET    104
#define ETIMEDOUT     110
#define ECONNREFUSED  111

/* ── Chapter 152 / 131d — strerror ───────────────────────────
 *
 * Returns a static string for each errno we set anywhere in the
 * tree.  Unknown numbers come back as "Unknown error" rather
 * than a per-call buffer ala glibc — keeps the inline tiny and
 * thread-safe.  Real glibc uses sys_errlist[]; we don't need
 * the array indirection because there are only ~30 codes.
 *
 * Returns `char *` (not `const char *`) to match POSIX and
 * libiberty/xstrerror.c's extern declaration; we never write
 * through the pointer, but the standard signature is mandatory
 * — chapter 178 hit a conflicting-types error compiling
 * libiberty/xstrerror.c against our libc until this was fixed.  */
static inline char *strerror(int e)
{
    const char *s;
    switch (e) {
    case 0:             s = "Success"; break;
    case EPERM:         s = "Operation not permitted"; break;
    case ENOENT:        s = "No such file or directory"; break;
    case ESRCH:         s = "No such process"; break;
    case EINTR:         s = "Interrupted system call"; break;
    case EIO:           s = "I/O error"; break;
    case ENXIO:         s = "No such device or address"; break;
    case E2BIG:         s = "Argument list too long"; break;
    case ENOEXEC:       s = "Exec format error"; break;
    case EBADF:         s = "Bad file descriptor"; break;
    case ECHILD:        s = "No child processes"; break;
    case EAGAIN:        s = "Try again"; break;
    case ENOMEM:        s = "Out of memory"; break;
    case EACCES:        s = "Permission denied"; break;
    case EFAULT:        s = "Bad address"; break;
    case EBUSY:         s = "Device or resource busy"; break;
    case EEXIST:        s = "File exists"; break;
    case EXDEV:         s = "Cross-device link"; break;
    case ENODEV:        s = "No such device"; break;
    case ENOTDIR:       s = "Not a directory"; break;
    case EISDIR:        s = "Is a directory"; break;
    case EINVAL:        s = "Invalid argument"; break;
    case ENFILE:        s = "Too many open files in system"; break;
    case EMFILE:        s = "Too many open files"; break;
    case ENOTTY:        s = "Not a terminal"; break;
    case ETXTBSY:       s = "Text file busy"; break;
    case EFBIG:         s = "File too large"; break;
    case ENOSPC:        s = "No space left on device"; break;
    case ESPIPE:        s = "Illegal seek"; break;
    case EROFS:         s = "Read-only file system"; break;
    case EMLINK:        s = "Too many links"; break;
    case EPIPE:         s = "Broken pipe"; break;
    case EDOM:          s = "Math domain error"; break;
    case ERANGE:        s = "Math result out of range"; break;
    case EDEADLK:       s = "Resource deadlock"; break;
    case ENAMETOOLONG:  s = "File name too long"; break;
    case ENOSYS:        s = "Function not implemented"; break;
    case ENOTEMPTY:     s = "Directory not empty"; break;
    case ELOOP:         s = "Symlink loop"; break;
    case EADDRINUSE:    s = "Address already in use"; break;
    case ECONNRESET:    s = "Connection reset by peer"; break;
    case ETIMEDOUT:     s = "Connection timed out"; break;
    case ECONNREFUSED:  s = "Connection refused"; break;
    default:            s = "Unknown error"; break;
    }
    /* Cast through unsigned long to drop const without a warning;
       POSIX strerror is char*, but our table is read-only.  */
    return (char *)(unsigned long)s;
}

#ifdef __cplusplus
}
#endif

#endif /* OSDEV_LIBC_ERRNO_H */
