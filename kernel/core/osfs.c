/*
 * kernel/core/osfs.c — read-only OSFS-1 driver.
 *
 * Reads the on-disk superblock at sector 0, caches up to 16
 * directory entries from sector 1, and serves byte-range reads
 * by translating into 512-byte sector reads via virtio_blk_read.
 *
 * No block cache yet.  Every osfs_read does (size + 511) / 512
 * blk reads, each a polled virtio request.  For files under a
 * dozen sectors this is fine; once we have larger files, a tiny
 * LRU page cache lands in front of virtio_blk.
 *
 * No write support: writing would mean updating the directory
 * sector and (eventually) extending the data area, neither of
 * which the current OSFS-1 layout supports without a free-space
 * map.  A later iteration of the format adds those.
 */

#include "osfs.h"
#include "serial.h"
#include "../device/virtio_blk.h"
#include "../device/blk_cache.h"

#include <stdint.h>
#include <stddef.h>

#define SECTOR OSFS_SECTOR_SIZE

static int                g_osfs_present = 0;
static uint32_t           g_file_count   = 0;
static struct osfs_dirent g_dir[OSFS_MAX_FILES];

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int osfs_init(void)
{
    g_osfs_present = 0;
    g_file_count   = 0;

    if (!virtio_blk_present()) {
        serial_puts("[osfs] no block device, skipping\n");
        return -1;
    }

    static uint8_t sb[SECTOR];
    if (blk_cache_read(0, sb) != 0) {
        serial_puts("[osfs] superblock read failed\n");
        return -1;
    }

    static const char magic[8] = { 'O','S','F','S','-','0','0','1' };
    for (int i = 0; i < 8; i++) {
        if (sb[i] != (uint8_t)magic[i]) {
            serial_puts("[osfs] no OSFS-1 magic on sector 0\n");
            return -1;
        }
    }
    uint32_t fc = (uint32_t)sb[8] | ((uint32_t)sb[9] << 8) |
                  ((uint32_t)sb[10] << 16) | ((uint32_t)sb[11] << 24);
    if (fc > OSFS_MAX_FILES) {
        serial_puts("[osfs] file_count exceeds OSFS_MAX_FILES, refusing\n");
        return -1;
    }

    static uint8_t dir[SECTOR * OSFS_DIR_SECTORS];
    for (uint32_t s = 0; s < OSFS_DIR_SECTORS; s++) {
        if (blk_cache_read(OSFS_DIR_SECTOR + s, dir + s * SECTOR) != 0) {
            serial_puts("[osfs] directory read failed\n");
            return -1;
        }
    }

    for (uint32_t i = 0; i < fc; i++) {
        const struct osfs_dirent *src =
            (const struct osfs_dirent *)(dir + i * sizeof(struct osfs_dirent));
        g_dir[i] = *src;
    }
    g_file_count = fc;
    g_osfs_present = 1;

    serial_puts("[osfs] mounted, ");
    serial_puthex(fc);
    serial_puts(" files:\n");
    for (uint32_t i = 0; i < fc; i++) {
        serial_puts("       /mnt/");
        serial_puts(g_dir[i].name);
        serial_puts(" (");
        serial_puthex(g_dir[i].size_bytes);
        serial_puts(" bytes @ sector ");
        serial_puthex(g_dir[i].start_sector);
        serial_puts(")\n");
    }
    return 0;
}

int osfs_present(void) { return g_osfs_present; }
size_t osfs_file_count(void) { return g_file_count; }
const struct osfs_dirent *osfs_dirent_at(size_t i)
{
    if (i >= g_file_count) return NULL;
    return &g_dir[i];
}

/* ------------------------------------------------------------------
 * Chapter 132 — struct fs_ops adapter.
 *
 * OSFS-1 is read-only, flat-namespace, and historically mounted
 * at BOTH `/mnt` and `/bin` (binaries live on disk since
 * the on-disk binaries chapter).  The vtable adapter is one set of methods plus
 * two mount registrations; the cookie is unused (NULL).
 *
 * Read-only is enforced two ways:
 *   1. MOUNT_RO flag on the mount entry (vfs_open returns
 *      -EROFS_VFS for any write flag);
 *   2. write / unlink / mkdir / lseek slots in `fs_ops` left
 *      NULL.  The dispatcher then either skips OSFS-1 (listdir-
 *      ladder fallthrough) or returns -EROFS_VFS at the syscall
 *      layer.
 * ------------------------------------------------------------------ */

#include "vfs.h"
#include "heap.h"

static const char *osfs_strip_slash(const char *rel)
{
    if (!rel) return "";
    if (rel[0] == '/') return rel + 1;
    return rel;
}

static long osfs_op_open(void *cookie, const char *rel, int flags,
                         struct fd_entry *out)
{
    (void)cookie; (void)flags;
    if (!out) return -EINVAL_VFS;
    const char *bare = osfs_strip_slash(rel);
    if (!*bare) return -EINVAL_VFS;
    uint32_t start = 0, size = 0;
    if (osfs_lookup(bare, &start, &size) != 0) return -ENOENT_VFS;
    out->kind        = FD_FILE;
    out->offset      = 0;
    out->ramfs_index = -1;
    out->osfs_start  = start;
    out->osfs_size   = size;
    out->pipe        = NULL;
    out->socket_cid  = -1;
    out->pty         = NULL;
    out->osfs2_ino   = 0;
    out->srv_l       = NULL;
    out->srv_c       = NULL;
    out->srv_is_service = 0;
    return 0;
}

static long osfs_op_read(void *cookie, struct fd_entry *e,
                         void *buf, size_t n)
{
    (void)cookie;
    if (!e || !buf) return -EINVAL_VFS;
    long got = osfs_read(e->osfs_start, e->osfs_size, e->offset, buf, n);
    if (got > 0) e->offset += (uint64_t)got;
    return got;
}

static long osfs_op_close(void *cookie, struct fd_entry *e)
{
    (void)cookie; (void)e;
    return 0;
}

static int osfs_op_listdir(void *cookie, const char *rel, int idx,
                           char *name, size_t cap, uint32_t *type)
{
    (void)cookie;
    const char *sub = osfs_strip_slash(rel);
    if (*sub) return -1;   /* flat namespace: only the mount root */
    if (!osfs_present()) return -1;
    if (idx < 0 || (size_t)idx >= osfs_file_count()) return -1;
    const struct osfs_dirent *e = osfs_dirent_at((size_t)idx);
    if (!e) return -1;
    size_t i = 0;
    for (; i + 1 < cap && i < OSFS_NAME_MAX && e->name[i]; i++)
        name[i] = e->name[i];
    name[i] = '\0';
    if (type) *type = 1u;   /* OSFS-1 is flat: every entry is a file (DT_REG) */
    return (int)i;
}

static int osfs_op_is_dir(void *cookie, const char *rel)
{
    (void)cookie;
    const char *bare = osfs_strip_slash(rel);
    if (!*bare) return 1;   /* mount root */
    return 0;                /* flat namespace */
}

static long osfs_op_load(void *cookie, const char *rel,
                         uint8_t **out_buf, size_t *out_size)
{
    (void)cookie;
    if (!out_buf || !out_size) return -EINVAL_VFS;
    *out_buf = NULL; *out_size = 0;
    const char *bare = osfs_strip_slash(rel);
    if (!*bare) return -ENOENT_VFS;
    uint32_t start = 0, sz = 0;
    if (osfs_lookup(bare, &start, &sz) != 0) return -ENOENT_VFS;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)sz);
    if (!buf && sz > 0) return -ENOMEM_VFS;
    if (sz > 0) {
        long got = osfs_read(start, sz, 0, buf, (size_t)sz);
        if (got < 0 || (uint32_t)got != sz) { kfree(buf); return -EIO; }
    }
    *out_buf = buf;
    *out_size = (size_t)sz;
    return 0;
}

const struct fs_ops osfs1_fs_ops = {
    .open    = osfs_op_open,
    .read    = osfs_op_read,
    .write   = NULL,
    .close   = osfs_op_close,
    .lseek   = NULL,
    .listdir = osfs_op_listdir,
    .unlink  = NULL,
    .mkdir   = NULL,
    .is_dir  = osfs_op_is_dir,
    .load    = osfs_op_load,
};

void osfs1_register_mount(void)
{
    /* OSFS-1 is the disk-backed mount that holds both user data
     * (chapter 11 — /mnt) and binaries (/bin).
     * Same on-disk file table, two different mount points; the
     * read-only flag is set on both so EROFS_VFS comes back
     * cleanly from any write attempt. */
    (void)vfs_mount_register("/mnt", &osfs1_fs_ops, NULL, MOUNT_RO);
    (void)vfs_mount_register("/bin", &osfs1_fs_ops, NULL, MOUNT_RO);
}

int osfs_lookup(const char *name, uint32_t *out_start, uint32_t *out_size)
{
    if (!g_osfs_present) return -1;
    if (!name) return -1;
    for (uint32_t i = 0; i < g_file_count; i++) {
        if (name_eq(name, g_dir[i].name)) {
            if (out_start) *out_start = g_dir[i].start_sector;
            if (out_size)  *out_size  = g_dir[i].size_bytes;
            return 0;
        }
    }
    return -1;
}

long osfs_read(uint32_t start_sector, uint32_t size_bytes,
               uint64_t offset, void *buf, size_t len)
{
    if (!g_osfs_present) return -1;
    if (offset >= size_bytes) return 0;

    uint64_t remaining = (uint64_t)size_bytes - offset;
    if ((uint64_t)len > remaining) len = (size_t)remaining;
    if (len == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;
    static uint8_t sect[SECTOR];

    size_t produced = 0;
    while (produced < len) {
        uint64_t pos     = offset + produced;
        uint32_t lba     = start_sector + (uint32_t)(pos / SECTOR);
        uint32_t in_sect = (uint32_t)(pos % SECTOR);
        uint32_t can     = SECTOR - in_sect;
        size_t   want    = len - produced;
        if ((size_t)can > want) can = (uint32_t)want;

        if (blk_cache_read(lba, sect) != 0) {
            serial_puts("[osfs] blk read failed at lba ");
            serial_puthex(lba);
            serial_puts("\n");
            return produced > 0 ? (long)produced : -1;
        }
        for (uint32_t i = 0; i < can; i++) dst[produced + i] = sect[in_sect + i];
        produced += can;
    }
    return (long)produced;
}
