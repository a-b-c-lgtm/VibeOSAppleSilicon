/*
 * kernel/core/osfs2.c — writable OSFS-2 driver.
 *
 * The format is documented in osfs2.h.  This file implements the
 * minimal verb-set the VFS needs:
 *
 *   osfs2_init       probe hd1 for the OSFS-2 magic, cache the
 *                    superblock + bitmaps in memory.
 *   osfs2_lookup     walk the root directory, return ino or 0.
 *   osfs2_create     allocate an inode + add a dirent.
 *   osfs2_unlink     free the inode + clear the dirent.
 *   osfs2_read       byte-range read across the direct[]/indirect
 *                    block tree.
 *   osfs2_write      byte-range write, growing the file (and
 *                    allocating data blocks / the indirect block)
 *                    as needed.
 *   osfs2_truncate   shrink to N bytes, freeing trailing blocks.
 *   osfs2_listdir    iterate the root directory for `ls /data/`.
 *
 * I/O strategy: every block read is 8 sequential virtio_blk_dev_read
 * calls (one per 512-byte sector) into a 4 KiB scratch buffer.  No
 * caching in chapter 81 — chapter 82 will fold blk_cache in front
 * once we have unit tests that prove the bitmap/inode-table updates
 * make it to disk.  Ordering of writes is data-block first, then
 * inode, then bitmaps; see chapter 80 for the rationale.
 *
 * No directory hierarchy yet: only the root.  Subdirectories are
 * a one-page extension to osfs2_create that chapter 83 ships.
 */

#include "osfs2.h"
#include "osfs2_cache.h"
#include "osfs2_journal.h"
#include "serial.h"
#include "../device/virtio_blk.h"

#include <stdint.h>
#include <stddef.h>

/* ---------- module state ---------- */
static int                      g_present = 0;
static struct osfs2_superblock  g_sb;
static uint8_t                  g_block_bitmap[OSFS2_BLOCK_SIZE];
static uint8_t                  g_inode_bitmap[OSFS2_BLOCK_SIZE];

/* ---------- low-level helpers ---------- */

/* Compare two NUL-terminated strings.  Returns 1 if equal. */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Return the length of `s`, capped at `cap`. */
static size_t str_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

/* Read one 4 KiB block from disk into `buf`.
 *
 * Routed through the chapter-82 write-back cache.  A repeat read
 * of the same block (e.g. the inode-table block holding inode 1
 * during a single `osfs2_create` call) becomes a memcpy after
 * the first miss. */
static int read_block(uint32_t blk, void *buf)
{
    if (blk >= g_sb.total_blocks) return -1;
    return osfs2_cache_read(blk, buf);
}

/* Write one 4 KiB block from `buf` to disk.
 *
 * The cache marks the block dirty but does NOT issue a virtio-blk
 * write here — chapter 82's whole point.  Durability is achieved
 * via `osfs2_fsync()` (file granularity — actually flushes the
 * whole cache, see header) or the periodic background flusher
 * spawned in main.c.  Any caller that needs immediate durability
 * must follow up with `osfs2_cache_flush()`; the only such caller
 * inside this driver is the unlink path's bitmap update (see
 * `free_inode` / `free_block`). */
static int write_block(uint32_t blk, const void *buf)
{
    if (blk >= g_sb.total_blocks) return -1;
    return osfs2_cache_write(blk, buf);
}

/* Read a single inode by number into `out`. */
static int read_inode(uint32_t ino, struct osfs2_inode *out)
{
    if (ino == 0 || ino >= g_sb.inode_count) return -1;
    uint32_t blk_idx  = ino / OSFS2_INODES_PER_BLOCK;
    uint32_t in_block = ino % OSFS2_INODES_PER_BLOCK;
    static uint8_t scratch[OSFS2_BLOCK_SIZE];
    if (read_block(g_sb.inode_table_block + blk_idx, scratch) != 0) {
        return -1;
    }
    const uint8_t *src = scratch + in_block * OSFS2_INODE_SIZE;
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) dst[i] = src[i];
    return 0;
}

/* Write a single inode by number from `src`. */
static int write_inode(uint32_t ino, const struct osfs2_inode *src)
{
    if (ino == 0 || ino >= g_sb.inode_count) return -1;
    uint32_t blk_idx  = ino / OSFS2_INODES_PER_BLOCK;
    uint32_t in_block = ino % OSFS2_INODES_PER_BLOCK;
    static uint8_t scratch[OSFS2_BLOCK_SIZE];
    if (read_block(g_sb.inode_table_block + blk_idx, scratch) != 0) {
        return -1;
    }
    uint8_t *dst = scratch + in_block * OSFS2_INODE_SIZE;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < sizeof(*src); i++) dst[i] = s[i];
    return write_block(g_sb.inode_table_block + blk_idx, scratch);
}

/* Bit operations on the cached bitmaps (mirrored to disk by the
 * `flush_*_bitmap` helpers below). */
static int bitmap_get(const uint8_t *bm, uint32_t bit)
{
    return (bm[bit >> 3] >> (bit & 7)) & 1u;
}
static void bitmap_set(uint8_t *bm, uint32_t bit)
{
    bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}
static void bitmap_clear(uint8_t *bm, uint32_t bit)
{
    bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static int flush_block_bitmap(void)
{
    return write_block(g_sb.block_bitmap_block, g_block_bitmap);
}
static int flush_inode_bitmap(void)
{
    return write_block(g_sb.inode_bitmap_block, g_inode_bitmap);
}

/* Allocate a fresh data block and return its block number, or 0
 * on out-of-space.  The bitmap is updated both in memory and on
 * disk before returning. */
static uint32_t alloc_block(void)
{
    for (uint32_t b = g_sb.data_start_block; b < g_sb.total_blocks; b++) {
        if (!bitmap_get(g_block_bitmap, b)) {
            bitmap_set(g_block_bitmap, b);
            if (flush_block_bitmap() != 0) {
                bitmap_clear(g_block_bitmap, b);
                return 0;
            }
            /* Zero-initialize so callers see clean data. */
            static uint8_t zero[OSFS2_BLOCK_SIZE];
            for (uint32_t i = 0; i < OSFS2_BLOCK_SIZE; i++) zero[i] = 0;
            (void)write_block(b, zero);
            return b;
        }
    }
    return 0;
}

static int free_block(uint32_t b)
{
    if (b < g_sb.data_start_block || b >= g_sb.total_blocks) return -1;
    bitmap_clear(g_block_bitmap, b);
    return flush_block_bitmap();
}

/* Allocate a fresh inode and return its number, or 0 on
 * out-of-inodes. */
static uint32_t alloc_inode(void)
{
    for (uint32_t i = OSFS2_INODE_ROOT + 1; i < g_sb.inode_count; i++) {
        if (!bitmap_get(g_inode_bitmap, i)) {
            bitmap_set(g_inode_bitmap, i);
            if (flush_inode_bitmap() != 0) {
                bitmap_clear(g_inode_bitmap, i);
                return 0;
            }
            return i;
        }
    }
    return 0;
}

static int free_inode(uint32_t i)
{
    if (i < OSFS2_INODE_ROOT + 1 || i >= g_sb.inode_count) return -1;
    bitmap_clear(g_inode_bitmap, i);
    return flush_inode_bitmap();
}

/* Translate a logical block number (LBN) within an inode into a
 * physical block number on disk.  If `allocate` is non-zero and
 * the logical block has no backing yet, allocate one (and the
 * indirect block, if needed) and update *inode in memory; the
 * caller is responsible for write_inode'ing it back.  Returns 0
 * on a miss with allocate=0, or on out-of-space. */
static uint32_t resolve_block(struct osfs2_inode *ino, uint32_t lbn,
                              int allocate)
{
    if (lbn >= OSFS2_MAX_FILE_BLOCKS) return 0;
    if (lbn < OSFS2_DIRECT_PTRS) {
        if (ino->direct[lbn] != 0) return ino->direct[lbn];
        if (!allocate) return 0;
        uint32_t b = alloc_block();
        if (b == 0) return 0;
        ino->direct[lbn] = b;
        return b;
    }
    /* Single-indirect tier. */
    uint32_t idx = lbn - OSFS2_DIRECT_PTRS;
    if (ino->indirect == 0) {
        if (!allocate) return 0;
        uint32_t b = alloc_block();
        if (b == 0) return 0;
        ino->indirect = b;
    }
    static uint8_t ind[OSFS2_BLOCK_SIZE];
    if (read_block(ino->indirect, ind) != 0) return 0;
    uint32_t *table = (uint32_t *)ind;
    if (table[idx] != 0) return table[idx];
    if (!allocate) return 0;
    uint32_t b = alloc_block();
    if (b == 0) return 0;
    table[idx] = b;
    if (write_block(ino->indirect, ind) != 0) {
        free_block(b);
        return 0;
    }
    return b;
}

/* Walk a directory inode looking for `name`.  Returns the dirent
 * index on hit, or -1 on miss.  The first empty slot encountered
 * is recorded in *out_first_empty (or U32_MAX if none seen yet),
 * which create/unlink use to avoid a second pass. */
static int dir_find(const struct osfs2_inode *dir, const char *name,
                    uint32_t *out_first_empty)
{
    if (out_first_empty) *out_first_empty = 0xFFFFFFFFu;
    uint32_t total_dirents = dir->size / OSFS2_DIRENT_SIZE;
    static uint8_t buf[OSFS2_BLOCK_SIZE];

    for (uint32_t i = 0; i < OSFS2_DIRECT_PTRS; i++) {
        uint32_t blk = dir->direct[i];
        if (blk == 0) continue;
        if (read_block(blk, buf) != 0) return -1;
        for (uint32_t j = 0; j < OSFS2_DIRENTS_PER_BLOCK; j++) {
            uint32_t global = i * OSFS2_DIRENTS_PER_BLOCK + j;
            if (global >= total_dirents) return -1;
            const struct osfs2_dirent *d =
                (const struct osfs2_dirent *)(buf + j * OSFS2_DIRENT_SIZE);
            if (d->ino == 0) {
                if (out_first_empty &&
                    *out_first_empty == 0xFFFFFFFFu) {
                    *out_first_empty = global;
                }
                continue;
            }
            if (str_eq(d->name, name)) return (int)global;
        }
    }
    return -1;
}

/* Read the dirent at index `idx` (within the root) into *out.
 * Returns 0 on success, -1 if idx is past the directory. */
static int dir_read_at(const struct osfs2_inode *dir, uint32_t idx,
                       struct osfs2_dirent *out)
{
    uint32_t blk_idx = idx / OSFS2_DIRENTS_PER_BLOCK;
    uint32_t in_blk  = idx % OSFS2_DIRENTS_PER_BLOCK;
    if (blk_idx >= OSFS2_DIRECT_PTRS) return -1;
    uint32_t blk = dir->direct[blk_idx];
    if (blk == 0) return -1;
    static uint8_t buf[OSFS2_BLOCK_SIZE];
    if (read_block(blk, buf) != 0) return -1;
    const struct osfs2_dirent *d =
        (const struct osfs2_dirent *)(buf + in_blk * OSFS2_DIRENT_SIZE);
    *out = *d;
    return 0;
}

/* Write `ent` at dirent index `idx` within the root directory. */
static int dir_write_at(const struct osfs2_inode *dir, uint32_t idx,
                        const struct osfs2_dirent *ent)
{
    uint32_t blk_idx = idx / OSFS2_DIRENTS_PER_BLOCK;
    uint32_t in_blk  = idx % OSFS2_DIRENTS_PER_BLOCK;
    if (blk_idx >= OSFS2_DIRECT_PTRS) return -1;
    uint32_t blk = dir->direct[blk_idx];
    if (blk == 0) return -1;
    static uint8_t buf[OSFS2_BLOCK_SIZE];
    if (read_block(blk, buf) != 0) return -1;
    struct osfs2_dirent *d =
        (struct osfs2_dirent *)(buf + in_blk * OSFS2_DIRENT_SIZE);
    *d = *ent;
    return write_block(blk, buf);
}

/* ---------- public API ---------- */

int osfs2_init(void)
{
    g_present = 0;

    if (!virtio_blk_dev_present(OSFS2_DEVICE)) {
        serial_puts("[osfs2] hd1 absent, skipping\n");
        return -1;
    }

    static uint8_t sb_buf[OSFS2_BLOCK_SIZE];
    /* Read 8 sectors = 1 OSFS-2 block via the multi-device API. */
    for (uint32_t i = 0; i < OSFS2_SECTORS_PER_BLOCK; i++) {
        if (virtio_blk_dev_read(OSFS2_DEVICE, i, sb_buf + i * 512u) != 0) {
            serial_puts("[osfs2] superblock read failed\n");
            return -1;
        }
    }
    static const char magic[8] = { 'O','S','F','S','-','0','0','2' };
    for (int i = 0; i < 8; i++) {
        if (sb_buf[i] != (uint8_t)magic[i]) {
            serial_puts("[osfs2] no OSFS-2 magic on hd1, skipping\n");
            return -1;
        }
    }
    /* Copy the superblock fields out by hand (avoids relying on
     * structure layout at this point). */
    const struct osfs2_superblock *src =
        (const struct osfs2_superblock *)sb_buf;
    g_sb = *src;

    /* Sanity checks. */
    if (g_sb.block_size != OSFS2_BLOCK_SIZE) {
        serial_puts("[osfs2] block_size mismatch, refusing\n");
        return -1;
    }
    if (g_sb.inode_count == 0 || g_sb.inode_count > 65536) {
        serial_puts("[osfs2] inode_count out of range, refusing\n");
        return -1;
    }

    /* Chapter 83 — wire up the journal from the superblock and
     * replay any committed-but-not-checkpointed transaction
     * BEFORE we read the bitmaps.  The bitmap and inode-table
     * blocks are themselves potential replay targets, so we have
     * to apply replay first or we'd cache a stale snapshot. */
    osfs2_journal_init(g_sb.journal_header_block,
                       g_sb.journal_data_blocks);
    if (osfs2_journal_replay() != 0) {
        serial_puts("[osfs2] journal replay failed, refusing\n");
        return -1;
    }

    if (read_block(g_sb.block_bitmap_block, g_block_bitmap) != 0) {
        serial_puts("[osfs2] block bitmap read failed\n");
        return -1;
    }
    if (read_block(g_sb.inode_bitmap_block, g_inode_bitmap) != 0) {
        serial_puts("[osfs2] inode bitmap read failed\n");
        return -1;
    }

    g_present = 1;
    serial_puts("[osfs2] mounted hd1, ");
    serial_puthex(g_sb.total_blocks);
    serial_puts(" blocks, ");
    serial_puthex(g_sb.inode_count);
    serial_puts(" inodes\n");
    return 0;
}

int osfs2_present(void) { return g_present; }

/* ---------- path walking (chapter 85) ----------
 *
 * OSFS-2 is laid out as ino-1 = root directory, every other dirent
 * is either a file or another directory inode.  A path like
 * "notes/personal/foo.txt" is walked one component at a time:
 * start at root, dir_find("notes"), recurse into that ino,
 * dir_find("personal"), recurse, dir_find("foo.txt"), return.
 *
 * Pre-chapter-85 the public API hardcoded ROOT as the parent for
 * every operation \u2014 see git blame on this file for the older
 * "no directory hierarchy yet" comment.  The dirent helpers
 * (dir_find / dir_read_at / dir_write_at) were already directory-
 * generic; they just had no non-root caller.
 */

/* Copy one path component (everything up to the next '/' or '\\0')
 * into `out`.  Returns the source pointer advanced past the
 * component AND any trailing '/'s.  Returns NULL when the input
 * was empty.  Truncates silently if the component overruns
 * `cap` (the caller is expected to compare strlen(out) against
 * OSFS2_NAME_MAX). */
static const char *path_next_component(const char *p, char *out, size_t cap)
{
    if (!p || !*p) return NULL;
    while (*p == '/') p++;       /* skip leading slashes */
    if (!*p) return NULL;
    size_t i = 0;
    while (*p && *p != '/') {
        if (i + 1 < cap) out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    while (*p == '/') p++;
    return p;
}

/* Walk `path` from root and return the inode it names, or 0 if
 * any component is missing or non-directory traversal occurs.
 * An empty/NULL path returns ROOT (so osfs2_lookup("") gives the
 * root directory inode \u2014 useful for callers that want to
 * enumerate the mount root). */
static uint32_t walk(const char *path)
{
    if (!path || !*path || (path[0] == '/' && path[1] == '\0'))
        return OSFS2_INODE_ROOT;

    uint32_t cur = OSFS2_INODE_ROOT;
    char comp[OSFS2_NAME_MAX];
    const char *p = path;
    while ((p = path_next_component(p, comp, sizeof(comp))) || comp[0]) {
        struct osfs2_inode dir;
        if (read_inode(cur, &dir) != 0) return 0;
        if (dir.type != OSFS2_TYPE_DIR) return 0;
        int idx = dir_find(&dir, comp, NULL);
        if (idx < 0) return 0;
        struct osfs2_dirent ent;
        if (dir_read_at(&dir, (uint32_t)idx, &ent) != 0) return 0;
        cur = ent.ino;
        if (!p) break;
        comp[0] = '\0';
    }
    return cur;
}

/* Split `path` into (parent_path, leaf).  parent_out receives the
 * inode number of the parent directory; leaf_out receives the
 * final component (must be \u2264 OSFS2_NAME_MAX bytes including
 * the NUL).  Returns 0 on success, -1 if any parent component is\n * missing or non-directory, or if the leaf is empty / too long. */
static int walk_parent(const char *path, uint32_t *parent_out,
                       char *leaf_out, size_t leaf_cap)
{
    if (!path || !leaf_out || leaf_cap == 0) return -1;
    /* Find the last '/'.  Everything before it is the parent
     * path; everything after is the leaf.  A path with no slash
     * (or only leading slashes) has root as its parent. */
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return -1;          /* path was just "/" */

    const char *leaf_start = p;
    const char *scan       = p;
    while (*scan) {
        if (*scan == '/' && scan[1] && scan[1] != '/') {
            leaf_start = scan + 1;
        }
        scan++;
    }
    /* Strip trailing slashes from the path's view of the leaf. */
    const char *leaf_end = scan;
    while (leaf_end > leaf_start && leaf_end[-1] == '/') leaf_end--;
    size_t leaf_len = (size_t)(leaf_end - leaf_start);
    if (leaf_len == 0 || leaf_len >= leaf_cap ||
        leaf_len >= OSFS2_NAME_MAX) return -1;
    for (size_t i = 0; i < leaf_len; i++) leaf_out[i] = leaf_start[i];
    leaf_out[leaf_len] = '\0';
    /* Reject components containing the path separator (defence in\n     * depth \u2014 path_next_component already split on '/'). */
    for (size_t i = 0; i < leaf_len; i++) {
        if (leaf_out[i] == '/') return -1;
    }

    /* Walk the parent.  Build a NUL-terminated parent string in a\n     * local buffer rather than mutating the input. */
    char parent_buf[256];
    size_t parent_len = (size_t)(leaf_start - path);
    /* Trim trailing slashes from the parent portion. */
    while (parent_len > 0 && path[parent_len - 1] == '/') parent_len--;
    if (parent_len >= sizeof(parent_buf)) return -1;
    for (size_t i = 0; i < parent_len; i++) parent_buf[i] = path[i];
    parent_buf[parent_len] = '\0';

    uint32_t parent = walk(parent_buf);
    if (parent == 0) return -1;
    /* Make sure the parent is actually a directory. */
    struct osfs2_inode pi;
    if (read_inode(parent, &pi) != 0) return -1;
    if (pi.type != OSFS2_TYPE_DIR) return -1;
    *parent_out = parent;
    return 0;
}

/* Internal: allocate a new inode of `type`, link it into
 * `parent_ino` under `name`.  If `type == OSFS2_TYPE_DIR`, leaves
 * the directory empty (size = 0, no '.' / '..' \u2014 we only support
 * the relative-walk model where path traversal starts from the
 * root, so '.' / '..' aren't needed).
 *
 * Returns the new inode's number, or 0 on any failure (out of
 * inodes/blocks, name too long, name already exists, parent
 * full).  On failure leaves the FS as it was before the call,
 * modulo cleanup of any blocks we allocated and then had to
 * free \u2014 those are leaked from the bitmap's POV (this is OK
 * because the journal in chapter 83 either commits the whole
 * transaction or none of it; mid-call ENOSPC is a user-visible
 * error, not a corruption). */
static uint32_t dir_create_in(uint32_t parent_ino, const char *name,
                              uint32_t type)
{
    size_t name_len = str_len(name, OSFS2_NAME_MAX);
    if (name_len == 0 || name_len >= OSFS2_NAME_MAX) return 0;

    struct osfs2_inode parent;
    if (read_inode(parent_ino, &parent) != 0) return 0;
    if (parent.type != OSFS2_TYPE_DIR) return 0;

    uint32_t empty = 0xFFFFFFFFu;
    int hit = dir_find(&parent, name, &empty);
    if (hit >= 0) {
        /* Name exists.  Semantics differ by requested type:
         *   - FILE: behave as create-or-open (return the existing
         *     inode if it's also a file).  vfs_open() relies on
         *     this to implement O_CREAT-on-an-existing-file.
         *   - DIR:  always fail.  POSIX mkdir(2) returns EEXIST
         *     in this case; we mirror that to keep the
         *     `mkdir foo && mkdir foo` script error visible. */
        struct osfs2_dirent ent;
        if (dir_read_at(&parent, (uint32_t)hit, &ent) != 0) return 0;
        struct osfs2_inode existing;
        if (read_inode(ent.ino, &existing) != 0) return 0;
        if (type == OSFS2_TYPE_DIR) return 0;
        if (existing.type != type) return 0;
        return ent.ino;
    }

    uint32_t ino = alloc_inode();
    if (ino == 0) return 0;

    struct osfs2_inode ni;
    uint8_t *p = (uint8_t *)&ni;
    for (uint32_t i = 0; i < sizeof(ni); i++) p[i] = 0;
    ni.type  = type;
    ni.size  = 0;
    ni.nlink = 1;
    ni.mode  = (type == OSFS2_TYPE_DIR) ? 0755 : 0644;
    if (write_inode(ino, &ni) != 0) {
        free_inode(ino);
        return 0;
    }

    /* Find / make a slot in the parent directory. */
    uint32_t slot;
    if (empty != 0xFFFFFFFFu) {
        slot = empty;
    } else {
        slot = parent.size / OSFS2_DIRENT_SIZE;
        uint32_t blk_idx = slot / OSFS2_DIRENTS_PER_BLOCK;
        if (blk_idx >= OSFS2_DIRECT_PTRS) {
            free_inode(ino);
            return 0;
        }
        if (parent.direct[blk_idx] == 0) {
            uint32_t b = alloc_block();
            if (b == 0) { free_inode(ino); return 0; }
            parent.direct[blk_idx] = b;
        }
        parent.size = (slot + 1) * OSFS2_DIRENT_SIZE;
    }

    struct osfs2_dirent ent;
    ent.ino = ino;
    for (uint32_t i = 0; i < sizeof(ent.name); i++) ent.name[i] = 0;
    for (size_t i = 0; i < name_len; i++) ent.name[i] = name[i];
    if (dir_write_at(&parent, slot, &ent) != 0) {
        free_inode(ino);
        return 0;
    }
    if (write_inode(parent_ino, &parent) != 0) {
        free_inode(ino);
        return 0;
    }
    return ino;
}

uint32_t osfs2_lookup(const char *path)
{
    if (!g_present) return 0;
    uint32_t ino = walk(path);
    return (ino == OSFS2_INODE_ROOT) ? 0 : ino;
}

uint32_t osfs2_create(const char *path)
{
    if (!g_present) return 0;
    uint32_t parent;
    char leaf[OSFS2_NAME_MAX];
    if (walk_parent(path, &parent, leaf, sizeof(leaf)) != 0) return 0;
    return dir_create_in(parent, leaf, OSFS2_TYPE_FILE);
}

uint32_t osfs2_mkdir(const char *path)
{
    if (!g_present) return 0;
    uint32_t parent;
    char leaf[OSFS2_NAME_MAX];
    if (walk_parent(path, &parent, leaf, sizeof(leaf)) != 0) return 0;
    return dir_create_in(parent, leaf, OSFS2_TYPE_DIR);
}

/* Internal: clear the dirent at `idx` in `parent` and write it back. */
static int dir_unlink_slot(uint32_t parent_ino, uint32_t slot)
{
    struct osfs2_inode parent;
    if (read_inode(parent_ino, &parent) != 0) return -1;
    if (parent.type != OSFS2_TYPE_DIR) return -1;
    struct osfs2_dirent zero;
    zero.ino = 0;
    for (uint32_t i = 0; i < sizeof(zero.name); i++) zero.name[i] = 0;
    return dir_write_at(&parent, slot, &zero);
}

int osfs2_unlink(const char *path)
{
    if (!g_present) return -1;
    uint32_t parent;
    char leaf[OSFS2_NAME_MAX];
    if (walk_parent(path, &parent, leaf, sizeof(leaf)) != 0) return -1;
    struct osfs2_inode pi;
    if (read_inode(parent, &pi) != 0) return -1;
    int idx = dir_find(&pi, leaf, NULL);
    if (idx < 0) return -1;
    struct osfs2_dirent ent;
    if (dir_read_at(&pi, (uint32_t)idx, &ent) != 0) return -1;
    uint32_t ino = ent.ino;

    struct osfs2_inode fi;
    if (read_inode(ino, &fi) != 0) return -1;
    /* Refuse to unlink directories \u2014 callers must use rmdir.\n     * This mirrors POSIX (unlink(2) returns -EISDIR on a dir)\n     * and prevents accidentally orphaning a populated subtree. */
    if (fi.type == OSFS2_TYPE_DIR) return -1;
    if (osfs2_truncate(ino, 0) != 0) return -1;
    if (read_inode(ino, &fi) != 0) return -1;

    if (dir_unlink_slot(parent, (uint32_t)idx) != 0) return -1;

    fi.type = OSFS2_TYPE_FREE;
    fi.size = 0;
    fi.nlink = 0;
    if (write_inode(ino, &fi) != 0) return -1;
    if (free_inode(ino) != 0) return -1;
    return 0;
}

int osfs2_rmdir(const char *path)
{
    if (!g_present) return -1;
    uint32_t parent;
    char leaf[OSFS2_NAME_MAX];
    if (walk_parent(path, &parent, leaf, sizeof(leaf)) != 0) return -1;
    struct osfs2_inode pi;
    if (read_inode(parent, &pi) != 0) return -1;
    int idx = dir_find(&pi, leaf, NULL);
    if (idx < 0) return -1;
    struct osfs2_dirent ent;
    if (dir_read_at(&pi, (uint32_t)idx, &ent) != 0) return -1;
    uint32_t ino = ent.ino;
    if (ino == OSFS2_INODE_ROOT) return -1;        /* never remove root */

    struct osfs2_inode di;
    if (read_inode(ino, &di) != 0) return -1;
    if (di.type != OSFS2_TYPE_DIR) return -1;
    /* Empty-directory check: scan every dirent and require ino==0\n     * for all of them.  We only need to look as far as di.size/64,\n     * the same bound dir_find uses. */
    uint32_t total = di.size / OSFS2_DIRENT_SIZE;
    for (uint32_t i = 0; i < total; i++) {
        struct osfs2_dirent e2;
        if (dir_read_at(&di, i, &e2) != 0) return -1;
        if (e2.ino != 0) return -1;                /* not empty */
    }
    /* Free the dirent blocks held by the directory itself, then\n     * free the inode. */
    if (osfs2_truncate(ino, 0) != 0) return -1;
    if (read_inode(ino, &di) != 0) return -1;

    if (dir_unlink_slot(parent, (uint32_t)idx) != 0) return -1;

    di.type = OSFS2_TYPE_FREE;
    di.size = 0;
    di.nlink = 0;
    if (write_inode(ino, &di) != 0) return -1;
    if (free_inode(ino) != 0) return -1;
    return 0;
}

long osfs2_read(uint32_t ino, uint64_t offset, void *buf, size_t len)
{
    if (!g_present) return -1;
    struct osfs2_inode fi;
    if (read_inode(ino, &fi) != 0) return -1;
    if (fi.type != OSFS2_TYPE_FILE && fi.type != OSFS2_TYPE_DIR) return -1;
    if (offset >= fi.size) return 0;

    uint64_t remaining = (uint64_t)fi.size - offset;
    if ((uint64_t)len > remaining) len = (size_t)remaining;
    if (len == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;
    static uint8_t blk[OSFS2_BLOCK_SIZE];

    size_t produced = 0;
    while (produced < len) {
        uint64_t pos     = offset + produced;
        uint32_t lbn     = (uint32_t)(pos / OSFS2_BLOCK_SIZE);
        uint32_t in_blk  = (uint32_t)(pos % OSFS2_BLOCK_SIZE);
        uint32_t can     = OSFS2_BLOCK_SIZE - in_blk;
        size_t   want    = len - produced;
        if ((size_t)can > want) can = (uint32_t)want;

        uint32_t pb = resolve_block(&fi, lbn, /*allocate=*/0);
        if (pb == 0) {
            /* Sparse-hole read: the bytes are all zeros. */
            for (uint32_t i = 0; i < can; i++) dst[produced + i] = 0;
        } else {
            if (read_block(pb, blk) != 0) {
                return produced > 0 ? (long)produced : -1;
            }
            for (uint32_t i = 0; i < can; i++) {
                dst[produced + i] = blk[in_blk + i];
            }
        }
        produced += can;
    }
    return (long)produced;
}

long osfs2_write(uint32_t ino, uint64_t offset, const void *buf, size_t len)
{
    if (!g_present) return -1;
    if (len == 0) return 0;
    if (offset + len > OSFS2_MAX_FILE_BYTES) return -1;

    struct osfs2_inode fi;
    if (read_inode(ino, &fi) != 0) return -1;
    if (fi.type != OSFS2_TYPE_FILE) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    static uint8_t blk[OSFS2_BLOCK_SIZE];

    size_t consumed = 0;
    while (consumed < len) {
        uint64_t pos    = offset + consumed;
        uint32_t lbn    = (uint32_t)(pos / OSFS2_BLOCK_SIZE);
        uint32_t in_blk = (uint32_t)(pos % OSFS2_BLOCK_SIZE);
        uint32_t can    = OSFS2_BLOCK_SIZE - in_blk;
        size_t   want   = len - consumed;
        if ((size_t)can > want) can = (uint32_t)want;

        uint32_t pb = resolve_block(&fi, lbn, /*allocate=*/1);
        if (pb == 0) {
            /* Out of space — return what we managed (or -1). */
            if (consumed > 0) goto done;
            return -1;
        }

        /* Read-modify-write only when the write doesn't cover the
         * entire block; full-block writes can skip the read. */
        int full_block = (in_blk == 0 && can == OSFS2_BLOCK_SIZE);
        if (!full_block) {
            if (read_block(pb, blk) != 0) {
                if (consumed > 0) goto done;
                return -1;
            }
        }
        for (uint32_t i = 0; i < can; i++) blk[in_blk + i] = src[consumed + i];
        if (write_block(pb, blk) != 0) {
            if (consumed > 0) goto done;
            return -1;
        }
        consumed += can;
    }

done:
    /* Grow the recorded size if we wrote past the previous end. */
    uint64_t end = offset + consumed;
    if (end > fi.size) fi.size = (uint32_t)end;
    if (write_inode(ino, &fi) != 0) return -1;
    return (long)consumed;
}

int osfs2_truncate(uint32_t ino, uint32_t size)
{
    if (!g_present) return -1;
    struct osfs2_inode fi;
    if (read_inode(ino, &fi) != 0) return -1;
    if (fi.type != OSFS2_TYPE_FILE && fi.type != OSFS2_TYPE_DIR) return -1;
    if (size > OSFS2_MAX_FILE_BYTES) return -1;

    /* Free blocks past the new EOF. */
    uint32_t last_kept_block = (size == 0) ? 0 :
        ((size + OSFS2_BLOCK_SIZE - 1) / OSFS2_BLOCK_SIZE);

    /* Direct blocks. */
    for (uint32_t i = 0; i < OSFS2_DIRECT_PTRS; i++) {
        if (i >= last_kept_block && fi.direct[i] != 0) {
            free_block(fi.direct[i]);
            fi.direct[i] = 0;
        }
    }
    /* Indirect block. */
    if (fi.indirect != 0) {
        static uint8_t ind[OSFS2_BLOCK_SIZE];
        if (read_block(fi.indirect, ind) != 0) return -1;
        uint32_t *table = (uint32_t *)ind;
        int dirty = 0;
        int any_left = 0;
        for (uint32_t j = 0; j < OSFS2_INDIRECT_PTRS; j++) {
            uint32_t global_lbn = OSFS2_DIRECT_PTRS + j;
            if (global_lbn >= last_kept_block && table[j] != 0) {
                free_block(table[j]);
                table[j] = 0;
                dirty = 1;
            } else if (table[j] != 0) {
                any_left = 1;
            }
        }
        if (!any_left) {
            free_block(fi.indirect);
            fi.indirect = 0;
        } else if (dirty) {
            if (write_block(fi.indirect, ind) != 0) return -1;
        }
    }

    fi.size = size;
    return write_inode(ino, &fi);
}

uint32_t osfs2_size(uint32_t ino)
{
    struct osfs2_inode fi;
    if (read_inode(ino, &fi) != 0) return 0;
    if (fi.type == OSFS2_TYPE_FREE) return 0;
    return fi.size;
}

int osfs2_listdir_at(uint32_t parent_ino, uint32_t *idx,
                     char *name_out, size_t cap,
                     uint32_t *size_out, uint32_t *type_out)
{
    if (!g_present || !idx || !name_out || cap == 0) return 0;
    struct osfs2_inode dir;
    if (read_inode(parent_ino, &dir) != 0) return -1;
    if (dir.type != OSFS2_TYPE_DIR) return -1;
    uint32_t total = dir.size / OSFS2_DIRENT_SIZE;

    while (*idx < total) {
        struct osfs2_dirent ent;
        if (dir_read_at(&dir, *idx, &ent) != 0) return 0;
        (*idx)++;
        if (ent.ino == 0) continue;
        size_t n = 0;
        while (n + 1 < cap && n < sizeof(ent.name) && ent.name[n]) {
            name_out[n] = ent.name[n];
            n++;
        }
        name_out[n] = '\0';
        struct osfs2_inode child;
        if (read_inode(ent.ino, &child) != 0) {
            if (size_out) *size_out = 0;
            if (type_out) *type_out = OSFS2_TYPE_FREE;
        } else {
            if (size_out) *size_out = child.size;
            if (type_out) *type_out = child.type;
        }
        return 1;
    }
    return 0;
}

int osfs2_listdir(uint32_t *idx, char *name_out, size_t cap,
                  uint32_t *size_out)
{
    /* Backward-compatible root-only wrapper.  Pre-chapter-85 the
     * VFS only enumerated the root of /data/ \u2014 the dialog and
     * anything else that wants a non-root listing now goes through
     * osfs2_listdir_at directly. */
    int rc = osfs2_listdir_at(OSFS2_INODE_ROOT, idx, name_out, cap,
                              size_out, NULL);
    return (rc == 1) ? 1 : 0;
}

/* Chapter 82 — flush every dirty cache slot to disk.
 *
 * The current implementation is whole-cache (not per-inode).  See
 * the design note in the chapter for why: a per-inode flush would
 * need a back-pointer from each cache slot to the owning inode,
 * but bitmap and inode-table blocks aren't owned by any single
 * inode \u2014 so any "flush only this file's blocks" still has to
 * flush those out-of-band.  Whole-cache flush is correct, simple,
 * and \u2264 32 disk writes by construction.  We can refine if a real
 * workload pushes back. */
int osfs2_fsync(uint32_t ino)
{
    /* `ino` is currently advisory \u2014 see comment above.  Validating\n     * it lets us return -1 for callers that fsync a fresh open\n     * before any write (which is fine but suggests they're\n     * confused). */
    if (!g_present) return -1;
    if (ino != 0) {
        struct osfs2_inode fi;
        if (read_inode(ino, &fi) != 0) return -1;
        if (fi.type == OSFS2_TYPE_FREE) return -1;
    }
    return osfs2_cache_flush();
}

/* ------------------------------------------------------------------
 * Chapter 113 — struct fs_ops adapter.
 *
 * OSFS-2 is the writable on-disk filesystem, mounted at `/data`.
 * Unlike OSFS-1 / tmpfs it has subdirectories, so the adapter
 * has to walk path components in `is_dir`, `listdir`, and `load`.
 *
 * Path conventions inside the rel string passed by vfs_resolve:
 *   "/data"        -> rel = ""        (mount root)
 *   "/data/"       -> rel = "/"       (mount root, trailing slash)
 *   "/data/foo"    -> rel = "/foo"    (file or subdir at root)
 *   "/data/sub/x"  -> rel = "/sub/x"  (file under subdir)
 *
 * osfs2_lookup accepts the bare path WITHOUT a leading slash
 * and returns 0 for "" (= the root inode), so we strip the
 * leading '/' once at entry.
 * ------------------------------------------------------------------ */

#include "vfs.h"
#include "heap.h"

static const char *osfs2_strip_slash(const char *rel)
{
    if (!rel) return "";
    if (rel[0] == '/') return rel + 1;
    return rel;
}

static long osfs2_op_open(void *cookie, const char *rel, int flags,
                          struct fd_entry *out)
{
    (void)cookie;
    if (!out) return -EINVAL_VFS;
    const char *bare = osfs2_strip_slash(rel);
    if (!*bare) return -EINVAL_VFS;
    if (!osfs2_present()) return -ENOENT_VFS;
    uint32_t ino = osfs2_lookup(bare);
    if (ino == 0) {
        if (!(flags & O_CREAT)) return -ENOENT_VFS;
        ino = osfs2_create(bare);
        if (ino == 0) return -ENOMEM_VFS;
    } else if ((flags & O_TRUNC) && !(flags & O_APPEND)) {
        if (osfs2_truncate(ino, 0) != 0) return -EIO;
    }
    out->kind        = FD_OSFS2_FILE;
    out->offset      = 0;
    out->ramfs_index = -1;
    out->osfs_start  = 0;
    out->osfs_size   = 0;
    out->pipe        = NULL;
    out->socket_cid  = -1;
    out->pty         = NULL;
    out->osfs2_ino   = ino;
    out->srv_l       = NULL;
    out->srv_c       = NULL;
    out->srv_is_service = 0;
    return 0;
}

static long osfs2_op_read(void *cookie, struct fd_entry *e,
                          void *buf, size_t n)
{
    (void)cookie;
    if (!e || !buf) return -EINVAL_VFS;
    long got = osfs2_read(e->osfs2_ino, e->offset, buf, n);
    if (got > 0) e->offset += (uint64_t)got;
    return got;
}

static long osfs2_op_write(void *cookie, struct fd_entry *e,
                           const void *buf, size_t n)
{
    (void)cookie;
    if (!e || !buf) return -EINVAL_VFS;
    long wr = osfs2_write(e->osfs2_ino, e->offset, buf, n);
    if (wr > 0) e->offset += (uint64_t)wr;
    return wr;
}

static long osfs2_op_close(void *cookie, struct fd_entry *e)
{
    (void)cookie; (void)e;
    return 0;
}

static int osfs2_op_listdir(void *cookie, const char *rel, int idx,
                            char *name, size_t cap, uint32_t *type)
{
    (void)cookie;
    if (!osfs2_present()) return -1;
    const char *sub = osfs2_strip_slash(rel);
    uint32_t parent_ino;
    if (!*sub) {
        parent_ino = OSFS2_INODE_ROOT;
    } else {
        parent_ino = osfs2_lookup(sub);
        if (parent_ino == 0) return -1;
    }
    /* Walk dirent-by-dirent skipping holes until the `idx`-th
     * non-empty entry.  Cheap: directories are tiny. */
    uint32_t walk = 0;
    char     n2[OSFS2_NAME_MAX];
    uint32_t size = 0;
    uint32_t t = 0;
    int      cur = 0;
    while (1) {
        int rc = osfs2_listdir_at(parent_ino, &walk, n2, sizeof(n2),
                                  &size, &t);
        if (rc <= 0) return -1;
        if (cur == idx) break;
        cur++;
    }
    size_t i = 0;
    for (; i + 1 < cap && n2[i]; i++) name[i] = n2[i];
    name[i] = '\0';
    if (type) *type = t;
    return (int)i;
}

static int osfs2_op_unlink(void *cookie, const char *rel)
{
    (void)cookie;
    const char *bare = osfs2_strip_slash(rel);
    if (!*bare) return -EINVAL_VFS;
    if (!osfs2_present()) return -ENOENT_VFS;
    if (osfs2_unlink(bare) != 0) return -ENOENT_VFS;
    return 0;
}

static int osfs2_op_mkdir(void *cookie, const char *rel)
{
    (void)cookie;
    const char *bare = osfs2_strip_slash(rel);
    if (!*bare) return -EINVAL_VFS;
    if (!osfs2_present()) return -ENOENT_VFS;
    /* osfs2_mkdir returns the new inode on success, 0 on failure.
     * Matches the pre-113 syscall behaviour, which returned
     * -ENOENT_VFS on rc==0. */
    if (osfs2_mkdir(bare) == 0) return -ENOENT_VFS;
    return 0;
}

static int osfs2_op_is_dir(void *cookie, const char *rel)
{
    (void)cookie;
    const char *bare = osfs2_strip_slash(rel);
    if (!*bare) return 1;   /* mount root */
    if (!osfs2_present()) return 0;
    uint32_t ino = osfs2_lookup(bare);
    if (ino == 0) return 0;
    /* osfs2 has no public stat() yet, but we can probe by
     * attempting a 1-entry listdir: a non-directory returns -1
     * (osfs2_listdir_at refuses non-DIR inodes), a directory
     * returns >=0. */
    uint32_t walk = 0;
    char     n2[OSFS2_NAME_MAX];
    uint32_t size = 0, t = 0;
    int      rc = osfs2_listdir_at(ino, &walk, n2, sizeof(n2), &size, &t);
    return rc >= 0 ? 1 : 0;
}

static long osfs2_op_load(void *cookie, const char *rel,
                          uint8_t **out_buf, size_t *out_size)
{
    (void)cookie;
    if (!out_buf || !out_size) return -EINVAL_VFS;
    *out_buf = NULL; *out_size = 0;
    if (!osfs2_present()) return -ENOENT_VFS;
    const char *bare = osfs2_strip_slash(rel);
    if (!*bare) return -ENOENT_VFS;
    uint32_t ino = osfs2_lookup(bare);
    if (ino == 0) return -ENOENT_VFS;
    uint32_t sz = osfs2_size(ino);
    uint8_t *buf = (uint8_t *)kmalloc((size_t)sz);
    if (!buf && sz > 0) return -ENOMEM_VFS;
    if (sz > 0) {
        long got = osfs2_read(ino, 0, buf, (size_t)sz);
        if (got < 0 || (uint32_t)got != sz) { kfree(buf); return -EIO; }
    }
    *out_buf = buf;
    *out_size = (size_t)sz;
    return 0;
}

const struct fs_ops osfs2_fs_ops = {
    .open    = osfs2_op_open,
    .read    = osfs2_op_read,
    .write   = osfs2_op_write,
    .close   = osfs2_op_close,
    .lseek   = NULL,
    .listdir = osfs2_op_listdir,
    .unlink  = osfs2_op_unlink,
    .mkdir   = osfs2_op_mkdir,
    .is_dir  = osfs2_op_is_dir,
    .load    = osfs2_op_load,
};

void osfs2_register_mount(void)
{
    (void)vfs_mount_register("/data", &osfs2_fs_ops, NULL, 0u);
}

