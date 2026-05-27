/*
 * kernel/core/osfs2_cache.c — write-back block cache for OSFS-2.
 *
 * See osfs2_cache.h for the design rationale.  This file is
 * intentionally small: 32 slots, linear scan for find/evict, and
 * one global clock counter for LRU.  Anything cleverer (radix
 * trees, separate clean/dirty lists, B+-tree of slots) is a perf
 * win that isn't worth the complexity at this scale.
 */
#include "osfs2_cache.h"
#include "osfs2.h"                 /* OSFS2_BLOCK_SIZE / SECTORS_PER_BLOCK / DEVICE */
#include "osfs2_journal.h"         /* chapter 84 — every flush goes through the journal */
#include "serial.h"
#include "../device/virtio_blk.h"

#include <stdint.h>
#include <stddef.h>

#define SLOTS  32

struct slot {
    uint32_t blk;                              /* disk block number  */
    uint8_t  valid;                            /* 1 = holds a block  */
    uint8_t  dirty;                            /* needs writeback    */
    uint8_t  pad[2];
    uint64_t last_used;                        /* clock at last touch */
    uint8_t  data[OSFS2_BLOCK_SIZE];           /* 4 KiB              */
};

static struct slot g_slots[SLOTS];
static uint64_t    g_clock;
static uint64_t    g_hits;
static uint64_t    g_misses;
static uint64_t    g_writebacks;
static uint64_t    g_evictions;

/* ---------- raw disk I/O (bypasses the cache) ---------- */

/* One OSFS-2 block = OSFS2_SECTORS_PER_BLOCK virtio-blk sectors,
 * issued sequentially.  Returns 0 on success, -1 on any failure.
 * Used only by the cache-fill (read) path; chapter 84 routed the
 * write path through osfs2_journal_commit() so the cache itself
 * no longer talks to virtio-blk for writes. */
static int raw_read(uint32_t blk, uint8_t *dst)
{
    uint64_t lba = (uint64_t)blk * OSFS2_SECTORS_PER_BLOCK;
    for (uint32_t i = 0; i < OSFS2_SECTORS_PER_BLOCK; i++) {
        if (virtio_blk_dev_read(OSFS2_DEVICE, lba + i,
                                dst + i * 512u) != 0)
            return -1;
    }
    return 0;
}

/* ---------- slot management ---------- */

/* Linear scan; 32 slots makes anything cleverer wasteful.  Returns
 * the slot index or -1 if `blk` isn't currently cached. */
static int find_slot(uint32_t blk)
{
    for (int i = 0; i < SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].blk == blk) return i;
    }
    return -1;
}

/* Pick a victim slot.  Prefers any invalid slot.  Falls back to
 * the slot with the smallest last_used.  Returns the index; the
 * caller is responsible for flushing the slot if it was dirty
 * before reusing it.  Never returns -1: the table is bounded. */
static int pick_victim(void)
{
    int      victim = 0;
    uint64_t oldest = (uint64_t)-1;
    for (int i = 0; i < SLOTS; i++) {
        if (!g_slots[i].valid) return i;
        if (g_slots[i].last_used < oldest) {
            oldest = g_slots[i].last_used;
            victim = i;
        }
    }
    return victim;
}

/* Flush a single slot.  No-op for clean slots.  On success the
 * slot is left valid + clean and we return 0.  On error we leave
 * the slot dirty so the data isn't silently lost.
 *
 * Chapter 84: the actual disk write is delegated to
 * osfs2_journal_commit() with count=1, so every eviction is
 * itself crash-atomic (the journal will replay it on recovery if
 * the kernel died after the journal commit landed but before the
 * destination was updated).  This makes single-block evictions
 * during a multi-step metadata operation safer than they would be
 * with a bare raw_write — but multi-block atomicity across an
 * entire metadata op still requires the caller to call fsync at
 * the end (only fsync flushes ALL dirty slots in one transaction). */
static int flush_slot(int idx)
{
    if (!g_slots[idx].valid || !g_slots[idx].dirty) return 0;
    const uint8_t *batch[1] = { g_slots[idx].data };
    uint32_t       dest[1]  = { g_slots[idx].blk };
    if (osfs2_journal_commit(dest, batch, 1) != 0) {
        return -1;
    }
    g_slots[idx].dirty = 0;
    g_writebacks++;
    return 0;
}

/* ---------- public API ---------- */

void osfs2_cache_init(void)
{
    for (int i = 0; i < SLOTS; i++) {
        g_slots[i].valid     = 0;
        g_slots[i].dirty     = 0;
        g_slots[i].blk       = 0;
        g_slots[i].last_used = 0;
    }
    g_clock      = 0;
    g_hits       = 0;
    g_misses     = 0;
    g_writebacks = 0;
    g_evictions  = 0;
    serial_puts("[osfs2_cache] ready, ");
    serial_puthex((uint64_t)SLOTS);
    serial_puts(" slots × 4 KiB = 128 KiB\n");
}

int osfs2_cache_read(uint32_t blk, void *buf)
{
    int idx = find_slot(blk);
    if (idx >= 0) {
        g_slots[idx].last_used = ++g_clock;
        uint8_t       *dst = (uint8_t *)buf;
        const uint8_t *src = g_slots[idx].data;
        for (uint32_t i = 0; i < OSFS2_BLOCK_SIZE; i++) dst[i] = src[i];
        g_hits++;
        return 0;
    }

    /* Miss: pick a victim, flush it if dirty, then fill from disk. */
    int v = pick_victim();
    if (g_slots[v].valid) {
        if (g_slots[v].dirty) {
            if (flush_slot(v) != 0) return -1;
        }
        g_evictions++;
    }
    if (raw_read(blk, g_slots[v].data) != 0) {
        g_slots[v].valid = 0;
        return -1;
    }
    g_slots[v].blk       = blk;
    g_slots[v].valid     = 1;
    g_slots[v].dirty     = 0;
    g_slots[v].last_used = ++g_clock;

    uint8_t       *dst = (uint8_t *)buf;
    const uint8_t *src = g_slots[v].data;
    for (uint32_t i = 0; i < OSFS2_BLOCK_SIZE; i++) dst[i] = src[i];
    g_misses++;
    return 0;
}

int osfs2_cache_write(uint32_t blk, const void *buf)
{
    int idx = find_slot(blk);
    if (idx < 0) {
        /* Allocate a slot but DON'T pre-fill from disk: the caller
         * is overwriting the entire block.  Flush any dirty
         * victim first. */
        idx = pick_victim();
        if (g_slots[idx].valid) {
            if (g_slots[idx].dirty) {
                if (flush_slot(idx) != 0) return -1;
            }
            g_evictions++;
        }
        g_slots[idx].blk   = blk;
        g_slots[idx].valid = 1;
    }

    /* Copy in.  Mark dirty.  Stamp clock.  Disk untouched. */
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t       *dst = g_slots[idx].data;
    for (uint32_t i = 0; i < OSFS2_BLOCK_SIZE; i++) dst[i] = src[i];
    g_slots[idx].dirty     = 1;
    g_slots[idx].last_used = ++g_clock;
    return 0;
}

int osfs2_cache_flush(void)
{
    /* Chapter 84 — batch every dirty slot into one journal
     * transaction.  This is the moment the journal earns its
     * keep: bitmap, inode, and dirent updates from a single
     * metadata operation that all sit in the cache become a
     * single all-or-nothing commit on disk.
     *
     * SLOTS == 32 == OSFS2_JOURNAL_DATA_BLOCKS, so the worst
     * case fits in one transaction by construction.  If anyone
     * bumps SLOTS in the future, raise the journal size to match
     * (the static_assert in osfs2_journal.c checks the inverse). */
    uint32_t       dest[SLOTS];
    const uint8_t *data[SLOTS];
    int            map[SLOTS];
    uint32_t n = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].dirty) {
            dest[n] = g_slots[i].blk;
            data[n] = g_slots[i].data;
            map[n]  = i;
            n++;
        }
    }
    if (n == 0) return 0;
    if (osfs2_journal_commit(dest, data, n) != 0) {
        return -1;
    }
    for (uint32_t k = 0; k < n; k++) {
        g_slots[map[k]].dirty = 0;
        g_writebacks++;
    }
    return 0;
}

void osfs2_cache_invalidate(uint32_t blk)
{
    int idx = find_slot(blk);
    if (idx < 0) return;
    if (g_slots[idx].dirty) {
        /* Refuse to drop dirty data silently.  Surface this on
         * the serial console so a misuse is caught loudly during
         * development. */
        serial_puts("[osfs2_cache] WARN: invalidate of dirty block ");
        serial_puthex((uint64_t)blk);
        serial_puts(" — flushing first\n");
        (void)flush_slot(idx);
    }
    g_slots[idx].valid = 0;
}

uint64_t osfs2_cache_hits(void)        { return g_hits; }
uint64_t osfs2_cache_misses(void)      { return g_misses; }
uint64_t osfs2_cache_writebacks(void)  { return g_writebacks; }
uint64_t osfs2_cache_evictions(void)   { return g_evictions; }

uint32_t osfs2_cache_dirty_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].dirty) n++;
    }
    return n;
}

void osfs2_cache_dump_stats(const char *prefix)
{
    serial_puts(prefix ? prefix : "[osfs2_cache]");
    serial_puts(" hits=");
    serial_puthex(g_hits);
    serial_puts(" misses=");
    serial_puthex(g_misses);
    serial_puts(" evictions=");
    serial_puthex(g_evictions);
    serial_puts(" writebacks=");
    serial_puthex(g_writebacks);
    serial_puts("\n");
}
