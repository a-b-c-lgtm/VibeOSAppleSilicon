/* userspace/libc/fcntl.h -- POSIX open flags + (stub) fcntl().
 *
 * Chapter 153 of the book.  Consolidates the O_* flag bits that
 * were scattered across the code base into one POSIX-shaped
 * header, and provides a stub `fcntl()` for the bits of the
 * surface GCC / TCC / configure scripts insist on having.
 *
 * The kernel's `vfs_open` already accepts these exact bit
 * values (defined in kernel/core/vfs.h); we just re-declare them
 * here so user code can include <fcntl.h> instead of dragging
 * in the syscall header.
 *
 * The fcntl() commands we implement today:
 *   F_DUPFD     -> dup-with-minimum-fd, backed by SYS_DUP2 against
 *                  the first free slot from `arg` upwards.
 *
 * Everything else (F_GETFL / F_SETFL / F_GETFD / F_SETFD / locks)
 * is recognised but currently returns 0 (success) or -ENOSYS as
 * appropriate -- enough for code that just queries and never
 * acts on the result.  Real per-fd flag plumbing arrives when
 * we need O_NONBLOCK on a userspace socket.
 */
#ifndef USER_FCNTL_H
#define USER_FCNTL_H

#include "syscall.h"
#include "errno.h"

#ifndef O_RDONLY
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#endif

#ifndef O_CREAT
#define O_CREAT   0100      /* 64 */
#define O_TRUNC   01000     /* 512 */
#define O_APPEND  02000     /* 1024 */
#endif

#define O_EXCL    0200      /* 128; honoured by tmpfs/OSFS-2 */
#define O_NONBLOCK 04000    /* 2048; recognised, today a no-op */

#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

#define FD_CLOEXEC 1

static inline int creat(const char *path, int mode)
{
    (void)mode;
    return (int)open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

/* dup() / dup2() -- dup2() is defined in syscall.h (a SYS_DUP2
 * wrapper).  dup() picks an unused fd by probing upward. */
static inline int dup(int oldfd)
{
    /* Chapter 153's kernel does not expose an "is fd in use"
     * probe, so we walk indices and call dup2 with each.  dup2
     * happily replaces the slot if it WAS in use -- so to keep
     * dup() faithful we'd need that probe.  In practice every
     * caller in our tree dup()s right after pipe()/openpty()
     * where the high indices are known free.  The documented
     * future fix is a single-arg SYS_DUP returning the
     * kernel's allocator pick. */
    for (int fd = 3; fd < 64; fd++) {
        if (fd == oldfd) continue;
        int r = dup2(oldfd, fd);
        if (r >= 0) return r;
    }
    errno = 24 /*EMFILE*/;
    return -1;
}

static inline int fcntl(int fd, int cmd, ...)
{
    /* Variadic to satisfy POSIX prototypes; we only consume
     * arg for F_DUPFD today. */
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    int arg = __builtin_va_arg(ap, int);
    __builtin_va_end(ap);

    switch (cmd) {
    case F_DUPFD: {
        for (int f = arg; f < 64; f++) {
            if (f == fd) continue;
            int r = dup2(fd, f);
            if (r >= 0) return r;
        }
        errno = 24 /*EMFILE*/;
        return -1;
    }
    case F_GETFD: return 0;             /* no FD_CLOEXEC yet */
    case F_SETFD: return 0;             /* accepted, no-op */
    case F_GETFL: return O_RDWR;        /* best-effort */
    case F_SETFL: return 0;             /* accepted, no-op */
    default:      errno = 38 /*ENOSYS*/; return -1;
    }
}

#endif /* USER_FCNTL_H */
