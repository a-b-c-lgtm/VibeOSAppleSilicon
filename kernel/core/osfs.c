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
