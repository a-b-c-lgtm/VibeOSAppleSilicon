/*
 * kernel/device/blk_cache.c — implementation.  See header for design.
 */
#include "blk_cache.h"
#include "virtio_blk.h"
#include "../core/serial.h"

#include <stdint.h>
#include <stddef.h>

#define SLOTS  64
#define SECTOR 512u

struct slot {
    uint64_t lba;          /* sector number; valid only when valid==1 */
    uint64_t last_used;    /* clock counter at most-recent access */
    uint8_t  valid;
    uint8_t  pad[7];
    uint8_t  data[SECTOR];
};

static struct slot g_slots[SLOTS];
static uint64_t    g_clock;
static uint64_t    g_hits;
static uint64_t    g_misses;
static uint64_t    g_evictions;

void blk_cache_init(void)
{
    for (int i = 0; i < SLOTS; i++) {
        g_slots[i].valid     = 0;
        g_slots[i].lba       = 0;
        g_slots[i].last_used = 0;
    }
    g_clock     = 0;
    g_hits      = 0;
    g_misses    = 0;
    g_evictions = 0;
    serial_puts("[blk_cache] ready, ");
    serial_puthex((uint64_t)SLOTS);
    serial_puts(" slots × 512B = 32 KiB\n");
}

/* Linear scan; 16 slots makes anything cleverer wasteful. */
static int find_slot(uint64_t lba)
{
    for (int i = 0; i < SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].lba == lba) return i;
    }
    return -1;
}

/* Pick a slot to evict: prefer any invalid slot; otherwise the
 * one with the oldest last_used timestamp. */
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

int blk_cache_read(uint64_t lba, void *buf)
{
    int idx = find_slot(lba);
    if (idx >= 0) {
        g_slots[idx].last_used = ++g_clock;
        uint8_t       *dst = (uint8_t *)buf;
        const uint8_t *src = g_slots[idx].data;
        for (uint32_t i = 0; i < SECTOR; i++) dst[i] = src[i];
        g_hits++;
        return 0;
    }

    /* Miss: read through, install in a victim slot. */
    int v = pick_victim();
    if (g_slots[v].valid) g_evictions++;
    if (virtio_blk_read(lba, g_slots[v].data) != 0) {
        /* Don't install garbage on failure. */
        g_slots[v].valid = 0;
        return -1;
    }
    g_slots[v].lba       = lba;
    g_slots[v].valid     = 1;
    g_slots[v].last_used = ++g_clock;

    uint8_t       *dst = (uint8_t *)buf;
    const uint8_t *src = g_slots[v].data;
    for (uint32_t i = 0; i < SECTOR; i++) dst[i] = src[i];
    g_misses++;
    return 0;
}

void blk_cache_invalidate(uint64_t lba)
{
    int idx = find_slot(lba);
    if (idx >= 0) g_slots[idx].valid = 0;
}

uint64_t blk_cache_hits(void)       { return g_hits; }
uint64_t blk_cache_misses(void)     { return g_misses; }
uint64_t blk_cache_evictions(void)  { return g_evictions; }

void blk_cache_dump_stats(const char *prefix)
{
    serial_puts(prefix ? prefix : "[blk_cache]");
    serial_puts(" hits=");
    serial_puthex(g_hits);
    serial_puts(" misses=");
    serial_puthex(g_misses);
    serial_puts(" evictions=");
    serial_puthex(g_evictions);
    serial_puts("\n");
}
