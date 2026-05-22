/*
 * userspace/libfs/userfs.h — chapter 114 user-space filesystem
 * library.  A daemon that wants to mount a path implements a
 * handful of callbacks, fills in `struct userfs_handler`, and
 * calls `userfs_serve(prefix, &ops)`.  The library:
 *
 *   1. Allocates a pair of pipes via sys_mount.
 *   2. Loops reading 32-byte p9_msg request headers off the
 *      request pipe, dispatches to the matching callback, and
 *      writes a reply header (+ optional payload) back.
 *   3. Returns when the request pipe hits EOF (kernel umount),
 *      or when a callback returns a fatal error.
 *
 * The wire layout MUST match kernel/core/userfs.h byte-for-byte.
 * P9_OP_*, P9_REPLY, struct p9_msg, P9_MAX_PAYLOAD are duplicated
 * here so userspace doesn't have to include any kernel header.
 */
#ifndef LIBFS_USERFS_H
#define LIBFS_USERFS_H

#include <stdint.h>
#include <stddef.h>

#define P9_OP_OPEN    1u
#define P9_OP_READ    2u
#define P9_OP_WRITE   3u
#define P9_OP_CLOSE   4u
#define P9_OP_LISTDIR 5u
#define P9_OP_UNLINK  6u
#define P9_OP_MKDIR   7u
#define P9_OP_IS_DIR  8u
#define P9_OP_LOAD    9u
#define P9_REPLY      0x80000000u

#define P9_MAX_PAYLOAD 2048u

struct p9_msg {
    uint32_t op;
    uint32_t tag;
    uint32_t handle;
    uint32_t flags;
    uint64_t offset;
    uint32_t length;
    int32_t  status;
};

/* Per-handler callbacks.  Returning a negative errno sends the
 * negative value back as `status` in the reply; positive return
 * values are interpreted op-by-op.  Any callback may be NULL —
 * the library will reply with -ENOSYS (38) for unhandled ops.
 *
 * All `path` pointers are NUL-terminated; the library NULs the
 * byte past the request payload before invoking the callback. */
struct userfs_handler {
    /* Open `path` (already trimmed of the mount prefix).  On
     * success returns 0 and writes the daemon-local handle id
     * into *handle_out; the kernel uses this id as the `handle`
     * field in subsequent read/write/close requests. */
    int  (*on_open)(void *ud, const char *path, int flags,
                    uint32_t *handle_out);

    /* Read up to `cap` bytes starting at `offset` from `handle`.
     * Returns the byte count written to `buf` (0..cap) or a
     * negative errno.  Returning 0 signals EOF. */
    int  (*on_read)(void *ud, uint32_t handle, uint64_t offset,
                    void *buf, uint32_t cap);

    /* Write `n` bytes at `offset` to `handle`.  Returns the byte
     * count accepted (0..n) or a negative errno.  A short write
     * tells the kernel to stop the current sys_write loop. */
    int  (*on_write)(void *ud, uint32_t handle, uint64_t offset,
                     const void *buf, uint32_t n);

    /* Close `handle`.  Always succeeds from the kernel's POV;
     * any error is logged and dropped. */
    int  (*on_close)(void *ud, uint32_t handle);

    /* Directory iteration.  Walks 0..N-1 over the children of
     * `path`; on success returns the byte length written to
     * `name` (NUL terminator not required), sets *type to
     * 1 (file) or 2 (directory) — matches the LISTDIR_TYPE_*
     * encoding userspace gets back from sys_listdir_at, so
     * `ls`, `ps`, and friends see the same tags as for an
     * in-kernel filesystem.  Returns -ENOENT (2) past the end. */
    int  (*on_listdir)(void *ud, const char *path, int idx,
                       char *name, uint32_t cap, uint32_t *type);

    /* Mutating ops.  May be NULL on read-only filesystems. */
    int  (*on_unlink)(void *ud, const char *path);
    int  (*on_mkdir) (void *ud, const char *path);

    /* Predicate.  Returns 1 if directory, 0 if file, negative
     * errno on missing path. */
    int  (*on_is_dir)(void *ud, const char *path);

    /* User cookie passed back to every callback. */
    void *userdata;
};

/* Mount-flag bits exported to userspace.  Mirror of
 * kernel/core/vfs.h::MOUNT_RO so daemons can request a
 * read-only mount through userfs_serve_flags. */
#define USERFS_MOUNT_RO 0x1u

/* Mount `prefix` and serve requests until the channel closes.
 * Returns 0 on clean umount, or a negative errno if sys_mount
 * failed.  Never returns while the channel is alive.
 *
 * userfs_serve mounts read-write; userfs_serve_flags lets a
 * daemon request USERFS_MOUNT_RO so the kernel rejects writes
 * to its files with -EROFS. */
int userfs_serve(const char *prefix, const struct userfs_handler *h);
int userfs_serve_flags(const char *prefix,
                       const struct userfs_handler *h,
                       unsigned long flags);

#endif /* LIBFS_USERFS_H */
