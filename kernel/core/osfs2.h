/*
 * kernel/core/osfs2.h — writable on-disk filesystem (format OSFS-2).
 *
 * OSFS-2 is the writable companion to OSFS-1.  Where OSFS-1 is a
 * single contiguous extent per file packed into a flat directory,
 * OSFS-2 introduces the three core ideas of every modern Unix
 * filesystem:
 *
 *   • Allocation in fixed-size *blocks*, tracked by a bitmap, so
 *     files can grow and shrink without compacting the whole disk.
 *   • Inodes — a separate metadata table that decouples a file's
 *     identity from its name (so unlink can free disk space without
 *     scanning the directory, and a future hard-link feature can
 *     point two names at the same data).
 *   • A directory is just a *file* whose contents are an array of
 *     name→inode dirents.
 *
 * Layout (all little-endian; block size = 4096 bytes = 8 virtio
 * sectors).  All on-disk offsets are in 4 KiB block units.
 *
 *   Block 0           superblock (magic "OSFS-002")
 *   Block 1           block bitmap (1 bit per block, LSB-first)
 *   Block 2           inode bitmap (1 bit per inode, LSB-first)
 *   Block 3..66       inode table (64 blocks × 32 inodes/block = 2048 inodes)
 *   Block 67          journal header                       (chapter 84)
 *   Block 68..99      journal data slots (32 × 4 KiB)      (chapter 84)
 *   Block 100..16383  data blocks (file contents + indirect blocks)
 *
 * 64 MiB image total (16384 × 4 KiB).  See scripts/mkosfs2.py for
 * the host-side formatter; it must agree byte-for-byte with the
 * structs and constants in this header.
 *
 * Inode 0 is reserved as a null sentinel.  Inode 1 is the root
 * directory.  Every other inode is allocated dynamically.
 *
 * Why ext2-shaped instead of LFS or CoW?  See chapter 81; we want
 * the simplest layout that exhibits the three ideas above without
 * needing a journal or a block-pointer translation layer.
 *
 * No journaling, no caching, no atomic-rename in chapter 82.  Every
 * write is synchronous: write the data block(s), then write the
 * inode block, then write the bitmap block(s).  A power loss
 * mid-write therefore leaves the FS in a self-consistent shape
 * iff the kernel crashed between two of those phases AND the
 * inode hadn't yet been linked into the directory; we accept the
 * small inconsistency window for chapter 82 and revisit in 82/83.
 *
 * Chapter 83 added a 32-slot write-back cache; chapter 84 added
 * the single-active-transaction physical-block journal whose on-
 * disk region is described above.  Every flush is now wrapped in
 * a journal commit, so a crash mid-flush either replays the entire
 * batch on next mount or has no effect at all.
 */
#ifndef OSFS2_H
#define OSFS2_H

#include <stdint.h>
#include <stddef.h>

/* On-disk geometry. */
#define OSFS2_BLOCK_SIZE          4096u
#define OSFS2_SECTORS_PER_BLOCK   8u                 /* 4096 / 512 */
#define OSFS2_INODE_SIZE          128u
#define OSFS2_INODES_PER_BLOCK    (OSFS2_BLOCK_SIZE / OSFS2_INODE_SIZE) /* 32 */
#define OSFS2_DIRENT_SIZE         64u
#define OSFS2_DIRENTS_PER_BLOCK   (OSFS2_BLOCK_SIZE / OSFS2_DIRENT_SIZE) /* 64 */

/* Per-inode block-pointer fanout. */
#define OSFS2_DIRECT_PTRS         16u
#define OSFS2_INDIRECT_PTRS       (OSFS2_BLOCK_SIZE / sizeof(uint32_t))   /* 1024 */
#define OSFS2_MAX_FILE_BLOCKS     (OSFS2_DIRECT_PTRS + OSFS2_INDIRECT_PTRS)
#define OSFS2_MAX_FILE_BYTES      (OSFS2_MAX_FILE_BLOCKS * OSFS2_BLOCK_SIZE)

/* Per-name budget. */
#define OSFS2_NAME_MAX            60u    /* 60 + the 4-byte ino = 64 byte ent */

/* Inode types. */
#define OSFS2_TYPE_FREE           0u
#define OSFS2_TYPE_FILE           1u
#define OSFS2_TYPE_DIR            2u

/* Reserved inode numbers. */
#define OSFS2_INODE_NULL          0u
#define OSFS2_INODE_ROOT          1u

/* Which virtio-blk device hosts OSFS-2.  hd0 is OSFS-1; hd1 is us. */
#define OSFS2_DEVICE              1

/* Chapter 84 — journal sizing.  The journal is one header block
 * plus N data slots, sized to match the cache so a single flush
 * can be journalled in one transaction.  Both must agree with
 * scripts/mkosfs2.py. */
#define OSFS2_JOURNAL_DATA_BLOCKS 32u
#define OSFS2_JOURNAL_TOTAL_BLOCKS (1u + OSFS2_JOURNAL_DATA_BLOCKS)

struct osfs2_superblock {
    uint8_t  magic[8];            /* "OSFS-002" */
    uint32_t block_size;          /* 4096 */
    uint32_t total_blocks;        /* 16384 */
    uint32_t inode_count;         /* 2048 */
    uint32_t block_bitmap_block;  /* 1 */
    uint32_t inode_bitmap_block;  /* 2 */
    uint32_t inode_table_block;   /* 3 */
    uint32_t inode_table_blocks;  /* 64 */
    uint32_t data_start_block;    /* 100 (post-chapter-83 layout) */
    uint32_t root_inode;          /* 1 */
    uint32_t journal_header_block; /* 67  (chapter 84) */
    uint32_t journal_data_blocks;  /* 32  (chapter 84) */
    uint32_t reserved;            /* 0; future second-journal head */
} __attribute__((packed));

struct osfs2_inode {
    uint32_t type;                                 /* OSFS2_TYPE_* */
    uint32_t size;                                 /* bytes */
    uint32_t nlink;                                /* hard-link count */
    uint32_t mode;                                 /* permission bits */
    uint32_t ctime_ms;                             /* monotonic ms */
    uint32_t mtime_ms;                             /* monotonic ms */
    uint32_t direct[OSFS2_DIRECT_PTRS];            /* 16 × 4 KiB */
    uint32_t indirect;                             /* 0 if unused */
    uint8_t  reserved[36];                         /* pad to 128 */
} __attribute__((packed));

struct osfs2_dirent {
    uint32_t ino;                                  /* 0 = empty slot */
    char     name[60];                             /* NUL-padded */
} __attribute__((packed));

/* Probe hd1 for the OSFS-2 magic, cache the superblock + bitmaps,
 * and return 0 on success / -1 if the device is absent or the
 * magic doesn't match. */
int osfs2_init(void);

int osfs2_present(void);

/* Look up `path` in the on-disk filesystem.  `path` is OSFS-2-
 * relative (no `/data/` prefix), uses `/` as a separator, and
 * may contain multiple components (e.g. "notes/personal/foo.txt").
 * On success returns the inode number (≥ 1).  Returns 0 on miss
 * or on a path that walks through a non-directory. */
uint32_t osfs2_lookup(const char *path);

/* Create a new regular file at `path` if it doesn't already
 * exist.  All path components except the last must already
 * exist as directories; the last is the new file's name.
 * Returns the new (or existing) inode number, 0 on failure
 * (out of inodes / blocks, name too long, parent missing,
 * parent is not a directory, leaf already exists as a
 * directory). */
uint32_t osfs2_create(const char *path);

/* Create a new directory at `path`.  Same parent-must-exist /
 * leaf-must-not-exist semantics as osfs2_create.  Returns the
 * new inode number, 0 on failure. */
uint32_t osfs2_mkdir(const char *path);

/* Remove the regular file at `path` and free its inode + data
 * blocks.  Returns 0 on success, -1 if the path doesn't exist,
 * if it walks through a non-directory, or if the leaf is itself
 * a directory (use osfs2_rmdir for that). */
int osfs2_unlink(const char *path);

/* Remove the (empty) directory at `path`.  Returns 0 on success,
 * -1 if the path doesn't exist, isn't a directory, or contains
 * any entries.  Will not remove the root. */
int osfs2_rmdir(const char *path);

/* Read up to `len` bytes starting at byte `offset` within the
 * file at `ino`.  Returns the number of bytes copied (may be
 * less at EOF), or -1 on error. */
long osfs2_read(uint32_t ino, uint64_t offset, void *buf, size_t len);

/* Write `len` bytes starting at byte `offset` within the file at
 * `ino`, growing the file if needed.  Returns the number of bytes
 * written, or -1 on error. */
long osfs2_write(uint32_t ino, uint64_t offset, const void *buf, size_t len);

/* Truncate the file at `ino` to exactly `size` bytes, freeing
 * any blocks that fall past the new end.  Returns 0 on success. */
int osfs2_truncate(uint32_t ino, uint32_t size);

/* Returns the current size in bytes (or 0 if the inode is free). */
uint32_t osfs2_size(uint32_t ino);

/* Iterate the root directory.  `idx` is the index of the next
 * dirent to inspect (0..N-1 where N = root size / dirent size).
 * On a hit, copies the leaf name into `name_out` (NUL-terminated,
 * up to cap-1 bytes), stores the file size in *size_out, and
 * returns 1.  On end-of-directory returns 0.  Empty slots are
 * skipped automatically.
 *
 * Kept as a thin wrapper around osfs2_listdir_at(ROOT, ...) so
 * the existing flat-namespace callers (vfs_listdir's `/data/`
 * branch in chapters 82–85) keep working unchanged. */
int osfs2_listdir(uint32_t *idx, char *name_out, size_t cap,
                  uint32_t *size_out);

/* Iterate any directory by inode number.  Same `idx`/`name_out`/
 * `size_out` semantics as osfs2_listdir, plus a `type_out`
 * parameter that receives OSFS2_TYPE_FILE / OSFS2_TYPE_DIR so
 * callers (e.g. the Save As dialog) can distinguish files from
 * subdirectories without a follow-up stat.  Returns 1 on a hit,
 * 0 on end-of-directory, -1 if `parent_ino` is not a directory. */
int osfs2_listdir_at(uint32_t parent_ino, uint32_t *idx,
                     char *name_out, size_t cap,
                     uint32_t *size_out, uint32_t *type_out);

/* Chapter 83 \u2014 force every dirty cache block to disk.
 *
 * `ino` is advisory: the implementation flushes the WHOLE
 * write-back cache because bitmap and inode-table blocks aren't
 * owned by a single file.  Pass 0 to skip the inode validity
 * check (used by the periodic background flusher); pass an inode
 * number to additionally assert it's allocated.
 *
 * Returns 0 on success, -1 if either the inode check or any of
 * the underlying virtio-blk writes failed.  Failed slots are
 * left dirty so the data isn't silently lost \u2014 a retry can
 * succeed. */
int osfs2_fsync(uint32_t ino);

/* Chapter 132 — vtable adapter for the mount table.  OSFS-2 is
 * writable, supports subdirectories, mkdir, and unlink.  Mounted
 * at `/data` with flags=0 (writable). */
struct fs_ops;
extern const struct fs_ops osfs2_fs_ops;
void osfs2_register_mount(void);

#endif /* OSFS2_H */
