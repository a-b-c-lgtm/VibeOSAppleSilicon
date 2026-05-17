/*
 * kernel/core/osfs.h — read-only on-disk filesystem (format OSFS-1).
 *
 * Layout (all little-endian):
 *
 *   sector 0  superblock (512 bytes)
 *       0x00  magic[8]   = "OSFS-001"
 *       0x08  file_count : u32
 *       0x0C  reserved...
 *
 *   sector 1..8  directory: array of 32-byte entries, eight
 *             sectors gives room for OSFS_MAX_FILES (= 128)
 *             entries.  Was 2 sectors / 32 entries through M59;
 *             bumped to 4 / 64 in M60 when /bin/htmldom pushed
 *             us over the cap, and again to 8 / 128 in chapter
 *             106b when /bin/proxytest did the same.  The OSFS-1
 *             format will be retired wholesale in chapter
 *             113-114's VFS refactor; until then we just keep
 *             doubling.
 *
 *       struct osfs_dirent {           // 32 bytes
 *           char     name[20];          // NUL-padded, no path
 *           uint32_t start_sector;      // absolute LBA
 *           uint32_t size_bytes;
 *           uint32_t reserved;
 *       };
 *
 *   sectors 9..N  file data, sector-aligned, packed end-to-end.
 *
 *   sectors N..   file data, sector-aligned, packed end-to-end.
 *
 *   (FIRST_DATA_SECTOR = 1 + DIR_SECTORS = 9 with chapter-106b's
 *   8-sector directory; was 5 from M60 through chapter 106a, 3
 *   pre-M60.)
 *
 * Why custom instead of FAT12?
 *   FAT12 has 12-bit packed entries spanning byte boundaries, three
 *   reserved/EOC value ranges, a quirky "first entry is FAT_ID" rule,
 *   short-name 8.3 padding and case bits, and historically variable
 *   cluster sizes.  None of that is the lesson of this milestone.
 *   OSFS-1 lets us focus on the *layering* (block I/O -> on-disk
 *   parsing -> VFS mount -> syscall) and skip the legacy.  A real
 *   FAT12 / FAT32 reader will land in a later chapter once the rest
 *   of the FS plumbing has settled.
 */
#ifndef OSFS_H
#define OSFS_H

#include <stdint.h>
#include <stddef.h>

#define OSFS_MAX_FILES        128
#define OSFS_NAME_MAX         20
#define OSFS_SECTOR_SIZE      512u
#define OSFS_DIR_SECTOR       1u
#define OSFS_DIR_SECTORS      8u
#define OSFS_FIRST_DATA_SECTOR 9u

struct osfs_dirent {
    char     name[OSFS_NAME_MAX];
    uint32_t start_sector;
    uint32_t size_bytes;
    uint32_t reserved;
} __attribute__((packed));

/* Probe sector 0 for the OSFS-1 magic, read the directory, and
 * cache it.  Returns 0 on success, -1 if no OSFS / blk gone. */
int osfs_init(void);

int osfs_present(void);

/* Look up `name` (e.g. "hello.txt", with no leading slash).
 * On success returns 0 and fills *out_start / *out_size. */
int osfs_lookup(const char *name, uint32_t *out_start, uint32_t *out_size);

/* Read `len` bytes starting at byte `offset` within the file
 * whose first sector is `start_sector`, total size `size_bytes`.
 * Returns the number of bytes copied (may be < len at EOF). */
long osfs_read(uint32_t start_sector, uint32_t size_bytes,
               uint64_t offset, void *buf, size_t len);

/* Number of cached directory entries (for diagnostic listing). */
size_t osfs_file_count(void);
const struct osfs_dirent *osfs_dirent_at(size_t i);

#endif /* OSFS_H */
