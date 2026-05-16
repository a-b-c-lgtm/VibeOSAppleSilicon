/*
 * kernel/device/blk_cache.h — tiny LRU block cache in front of virtio_blk.
 *
 * 64 × 512-byte slots = 32 KiB total.  Read-through, no write-back
 * (we have no write paths into OSFS yet).  Linear-scan LRU because
 * 16 slots makes any cleverer data structure pointless.
 *
 * The cache exists for two reasons:
 *   1. Each spawn() now does ~10 polled virtio reads to load an
 *      ELF off disk.  Re-spawning the same binary should be a
 *      memcpy, not 10 round-trips through the device.
 *   2. ELF-loading reads the same sector several times (the file
 *      header, the program-header table, and the first PT_LOAD
 *      file bytes are commonly all in sector 0 of the file's
 *      payload).  Even cold spawns benefit.
 *
 * Eviction policy: clock-counter LRU.  Each slot stamps its
 * `last_used` with ++g_clock on access; eviction picks the slot
 * with the smallest stamp.  Invalid slots (valid==0) are always
 * preferred over any valid slot.
 */
#ifndef BLK_CACHE_H
#define BLK_CACHE_H

#include <stdint.h>

/* One-time setup.  Call after virtio_blk_init() succeeds. */
void blk_cache_init(void);

/* Cached read of one 512-byte sector.  Same return contract as
 * virtio_blk_read: 0 on success, -1 on device error.  Misses
 * fall through to the underlying virtio driver. */
int blk_cache_read(uint64_t lba, void *buf);

/* Drop a sector from the cache (used after a write that didn't
 * go through the cache, or to force a re-read).  Idempotent. */
void blk_cache_invalidate(uint64_t lba);

/* Read-only stats accessors.  Useful for the chapter-23
 * verification trace and future telemetry. */
uint64_t blk_cache_hits(void);
uint64_t blk_cache_misses(void);
uint64_t blk_cache_evictions(void);

/* One-shot human-readable dump to the serial console. */
void blk_cache_dump_stats(const char *prefix);

#endif /* BLK_CACHE_H */
