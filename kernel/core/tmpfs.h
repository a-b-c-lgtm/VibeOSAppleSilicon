/*
 * kernel/core/tmpfs.h — in-memory writable filesystem mounted at
 * /tmp/.  Designed to mirror the OSFS-1 surface (lookup/read by
 * (start_sector, size, offset)) but with `start_sector` reused as
 * an opaque index into the tmpfs file table, and `osfs_size`
 * reused to mark "this is a tmpfs file" (we set it to the magic
 * sentinel TMPFS_MAGIC_SIZE).  This keeps the existing fd_entry
 * layout unchanged while letting vfs_read / vfs_close dispatch
 * generically.
 *
 * Files are kmalloc'd byte buffers that grow on demand up to
 * TMPFS_MAX_FILE_SIZE.  Up to TMPFS_MAX_FILES files exist in
 * the table.  No subdirectories, no permissions, no inodes.
 *
 * Lifetime: files persist until explicitly removed (no `rm` yet)
 * or until the kernel exits.  No periodic flush — there's
 * nothing on disk to flush to.
 */

#ifndef TMPFS_H
#define TMPFS_H

#include <stddef.h>
#include <stdint.h>

#define TMPFS_MAX_FILES        16
#define TMPFS_MAX_NAME         32
#define TMPFS_MAX_FILE_SIZE    (256u * 1024u)   /* 256 KiB ceiling per file */
#define TMPFS_INITIAL_CAP      4096u            /* first kmalloc */

/* Initialise the tmpfs file table.  Idempotent.  Called from
 * vfs_init alongside ramfs / OSFS setup. */
void tmpfs_init(void);

/* Look up `name` (without the /tmp/ prefix) in the table.
 * Returns the slot index >= 0 on hit, -1 on miss. */
int tmpfs_lookup(const char *name);

/* Create or truncate `name`.  If it already exists, truncate to
 * zero length and return its index.  If not, allocate a new slot
 * and return that.  Returns -1 on out-of-slots / OOM / name too
 * long. */
int tmpfs_create_or_truncate(const char *name);

/* Read up to `len` bytes from file `idx` starting at byte
 * `offset` into `buf`.  Returns bytes read (0 at EOF), or a
 * negative errno. */
long tmpfs_read(int idx, uint64_t offset, void *buf, size_t len);

/* Append `len` bytes from `buf` to file `idx`.  Returns bytes
 * written (always == len on success), or a negative errno on
 * OOM / size cap. */
long tmpfs_write(int idx, uint32_t offset, const void *buf, size_t len);

/* Iterate: get the idx-th file's name (NUL-terminated, copied
 * into `out`) and size (in `*size_out`).  Returns the length of
 * the name on success, -1 on out-of-range. */
int tmpfs_listdir(int idx, char *out, size_t cap, uint32_t *size_out);

/* Number of files currently in tmpfs.  Used by vfs_listdir to
 * stitch tmpfs into the unified directory. */
int tmpfs_count(void);

/* Total file count getter for sys_listdir to span. */
uint32_t tmpfs_size_of(int idx);

/* Position the (size cursor of the) idx-th file at end-of-file
 * for `O_APPEND`-style opens.  Tmpfs `_write` always appends so
 * this is a no-op today, but kept as an explicit hook for future
 * non-append write paths. */
void tmpfs_seek_end(int idx);

/* Remove file `idx` from the table, freeing its buffer.  Safe
 * to call on an in-use slot; existing fds referencing the slot
 * will start returning -EBADF on next read/write because in_use
 * goes false. */
int tmpfs_unlink(const char *name);

/* Chapter 132 — vtable adapter for the mount table.  Forwards
 * every fs_ops method to the tmpfs_* functions above so the
 * dispatcher in vfs.c / syscall.c can route /tmp through
 * `vfs_resolve`.  `cookie` is always NULL (tmpfs is a singleton). */
struct fs_ops;
extern const struct fs_ops tmpfs_fs_ops;

/* Idempotent: registers /tmp with `tmpfs_fs_ops`.  Called from
 * vfs_init after tmpfs_init. */
void tmpfs_register_mount(void);

#endif /* TMPFS_H */
