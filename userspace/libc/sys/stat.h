/* userspace/libc/sys/stat.h -- POSIX stat() + struct stat.
 *
 * Chapter 153 of the book.  Wraps the raw `__sys_stat` /
 * `__sys_fstat` defined in `../syscall.h` so apps that include
 * `<sys/stat.h>` get the familiar POSIX shape:
 *
 *     #include "libc/sys/stat.h"
 *
 *     struct stat st;
 *     if (stat("/data/notes.txt", &st) == 0 &&
 *         S_ISREG(st.st_mode))
 *         printf("size = %llu\n", (unsigned long long)st.st_size);
 *
 * The on-the-wire `struct kstat` (kernel) has the same byte
 * layout as `struct stat` (user) -- the same 4 fields, in the
 * same order.  Adding new fields means extending both sides
 * together; appending is safe.
 *
 * Header-only, single-TU.  Pull in once per binary alongside
 * `errno.h`.
 */
#ifndef USER_SYS_STAT_H
#define USER_SYS_STAT_H

#include <stdint.h>
#include <stddef.h>
#include "../syscall.h"
#include "../errno.h"

/* POSIX file-mode bits.  Match the kernel's S_IFREG_K etc.
 * exactly so we can cast through `struct __kstat_raw` without
 * a translation step. */
#define S_IFMT     0xF000u
#define S_IFREG    0x8000u
#define S_IFDIR    0x4000u
#define S_IFCHR    0x2000u
#define S_IFIFO    0x1000u
#define S_IFSOCK   0xC000u
/* Block-special bit, added chapter 186 for libcpp/system.h:341
 * which references S_IFBLK via the libcpp fallback S_ISBLK
 * macro.  This OS has no block-special inodes (the only block
 * device is virtio-blk, mounted as a filesystem, never opened
 * by path), so the value is reserved but never matches. */
#define S_IFBLK    0x6000u

#define S_ISREG(m)   (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)   (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)   (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m)  (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)  (((m) & S_IFMT) == S_IFSOCK)
#define S_ISBLK(m)   (((m) & S_IFMT) == S_IFBLK)

/* Standard permission triplets, useful for new files. */
#define S_IRWXU    0700
#define S_IRUSR    0400
#define S_IWUSR    0200
#define S_IXUSR    0100
#define S_IRWXG    0070
#define S_IRGRP    0040
#define S_IWGRP    0020
#define S_IXGRP    0010
#define S_IRWXO    0007
#define S_IROTH    0004
#define S_IWOTH    0002
#define S_IXOTH    0001

/* User-facing stat shape.  Layout matches `struct __kstat_raw`
 * (and the kernel's `struct kstat`) byte-for-byte; do not
 * reorder.
 *
 * Chapter 178 added `st_dev` / `st_ino` at the tail (POSIX
 * shape) so libiberty's "is this the same file?" comparisons
 * (fdmatch.c, getpwd.c) compile and produce sane results.
 * Today the kernel assigns one device number per FS kind
 * (OSFS-1 = 1, OSFS-2 = 2, tmpfs = 3, userfs = 4) and the
 * inode is the FS's own per-file index — directory-entry
 * sector for OSFS-1, inode number for OSFS-2, table index
 * for tmpfs.  Stable for the lifetime of a boot.
 *
 * Chapter 179 renamed `st_mtime_ms` -> `st_mtime` (POSIX
 * seconds; we still write 0 — kernel doesn't track wall-clock
 * mtime yet — but bfd / ar etc. assign through it so the name
 * matters) and appended `st_uid` / `st_gid` (always 0, no user
 * system) so binutils' archive.c builds. */
struct stat {
    uint32_t st_mode;
    uint32_t _pad;
    /* chapter 186 — POSIX requires st_size to be off_t (signed
     * 64-bit on this platform).  libcpp/files.cc passes &st_size
     * to a function expecting off_t* and the gcc++ rejects the
     * type mismatch.  We switched from uint64_t to int64_t; all
     * existing users cast through (size_t) or (unsigned), neither
     * of which cares about the change. */
    int64_t  st_size;
    /* chapter 186 — st_mtime must be time_t (signed 64-bit) for
     * the same -fpermissive reason: libcpp/macro.cc:525 passes
     * &st->st_mtime to localtime(const time_t*). */
    int64_t  st_mtime;
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
};

static inline int stat(const char *path, struct stat *out)
{
    return (int)__sys_stat(path, (struct __kstat_raw *)out);
}

static inline int fstat(int fd, struct stat *out)
{
    return (int)__sys_fstat(fd, (struct __kstat_raw *)out);
}

/* access(path, mode) -- POSIX-shaped "can I touch this file?".
 *
 * We don't have users/groups yet, so the mode bit set is read
 * directly from the inode's permission triplets.  Modes:
 *   F_OK = 0 -> existence check only.
 *   R_OK = 4 -> at least one read bit set.
 *   W_OK = 2 -> at least one write bit set.
 *   X_OK = 1 -> at least one exec bit set.
 *
 * Implementation is intentionally a libc concern: stat() the
 * path, then test the mode bits.  Returns 0 on success or -1 +
 * errno=EACCES/ENOENT on failure.
 */
#define F_OK  0
#define R_OK  4
#define W_OK  2
#define X_OK  1

static inline int access(const char *path, int mode)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (mode == F_OK) return 0;
    uint32_t want = 0;
    if (mode & R_OK) want |= (S_IRUSR | S_IRGRP | S_IROTH);
    if (mode & W_OK) want |= (S_IWUSR | S_IWGRP | S_IWOTH);
    if (mode & X_OK) want |= (S_IXUSR | S_IXGRP | S_IXOTH);
    if ((st.st_mode & want) == 0) {
        errno = 13; /* EACCES */
        return -1;
    }
    return 0;
}

/* Chapter 179 — POSIX umask / chmod.  We don't enforce file-
 * mode bits; both are no-ops.  bfd/opncls.c calls these after
 * writing output so the file ends up world-readable on real
 * systems; on us, every file is effectively 0666 anyway. */
#include "types.h"
static inline mode_t umask(mode_t cmask) { (void)cmask; return 0; }
static inline int    chmod(const char *p, mode_t m) { (void)p; (void)m; return 0; }

#endif /* USER_SYS_STAT_H */
