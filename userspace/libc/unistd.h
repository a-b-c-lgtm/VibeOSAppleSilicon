/* userspace/libc/unistd.h — chapter 130a, extended in 131d.
 *
 * POSIX `<unistd.h>` shim: a forwarding header that pulls in
 * the syscall wrappers DoomGeneric (and any future POSIX-ish
 * port) expects under angle-bracket names.
 *
 * Upstream DoomGeneric's i_system.c uses unistd.h for
 * getpid / sleep / usleep / read / write / close / isatty.
 * All of those already live in syscall.h; just re-export.
 *
 * Chapter 131d adds an inline `execvp` so libiberty's
 * pex-unix.c compiles.  We don't do real $PATH lookup yet —
 * that arrives with the libc.a refactor when env.h becomes
 * safe to include from a forwarding header.  The lookup we
 * do today (path-with-slash → execv direct; otherwise
 * /bin/<name>) covers everything binutils-gas actually
 * exec()s in our tree.
 */
#ifndef USERSPACE_LIBC_UNISTD_H
#define USERSPACE_LIBC_UNISTD_H

#include "syscall.h"
#include "sys/stat.h"   /* for access() + F_OK/R_OK/W_OK/X_OK */

static inline int execvp(const char *file, char *const argv[])
{
    if (!file || !file[0]) { errno = ENOENT; return -1; }

    /* Absolute / relative path with a slash: skip lookup. */
    for (const char *p = file; *p; p++) {
        if (*p == '/') return execv(file, argv);
    }

    /* No slash: try /bin/<file>.  Future chapter will walk
     * the real $PATH when env.h is safe to include here. */
    char buf[256];
    const char *prefix = "/bin/";
    int i = 0;
    while (prefix[i] && i < (int)sizeof(buf) - 1) { buf[i] = prefix[i]; i++; }
    int j = 0;
    while (file[j] && i < (int)sizeof(buf) - 1) { buf[i++] = file[j++]; }
    buf[i] = '\0';
    return execv(buf, argv);
}

/* Chapter 131d — POSIX `link(old, new)`.  Hard links are not a
 * thing on our filesystems (no inode refcount machinery in
 * OSFS-2; tmpfs is per-process).  Always returns -1/EPERM so
 * callers fall back to "copy bytes".  libiberty/rename.c is the
 * only consumer in the binutils source tree we've audited and
 * it has exactly that fallback. */
static inline int link(const char *oldpath, const char *newpath)
{
    (void)oldpath; (void)newpath;
    errno = EPERM;
    return -1;
}

/* Chapter 131e — POSIX `getuid` / `getgid` / `geteuid` / `getegid`.
 * No user system; always returns 0 (root).  bfd/archive.c needs
 * these when writing `ar` archive member headers (uid/gid bytes
 * in the ar_hdr). */
#include "sys/types.h"

static inline uid_t getuid(void)  { return 0; }
static inline uid_t geteuid(void) { return 0; }
static inline gid_t getgid(void)  { return 0; }
static inline gid_t getegid(void) { return 0; }

/* Chapter 132f — POSIX `alarm(seconds)`.  We don't yet ship a
 * SIGALRM scheduler; fixincludes/server.c calls alarm() as a
 * watchdog around its waitpid(), so we satisfy the declaration
 * and silently no-op.  Returning 0 (no previous alarm pending)
 * matches a fresh process. */
static inline unsigned int alarm(unsigned int seconds)
{
    (void)seconds;
    return 0;
}

#endif /* USERSPACE_LIBC_UNISTD_H */
