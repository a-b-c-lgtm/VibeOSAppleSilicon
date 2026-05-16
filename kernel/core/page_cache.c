/*
 * kernel/core/page_cache.c — chapter 90 unified page cache.
 *
 * See page_cache.h for the contract.  Implementation is the
 * smallest possible thing that does the job:
 *
 *   - 32 fixed slots in a flat array.
 *   - Linear lookup (32 entries; any cleverer data structure
 *     would itself dominate the cache size).
 *   - Clock-counter LRU for eviction: each touch stamps
 *     `last_used` with ++g_clock; eviction picks the live
 *     refcount==0 entry with the smallest stamp.
 *   - "In use" entries (refcount > 0) are never evicted; if
 *     every slot is in use we return 0 to the caller (mmap
 *     will then fail with -ENOMEM and userspace can decide
 *     what to do).
 *
 * Concurrency: protected by a single spinlock, IRQ-saved.  All
 * mutation paths (get_or_load, release) take it.  The loader
 * callback runs WITHOUT the lock held — it may sleep or fault
 * (e.g. blk_cache_read does polled I/O); we re-validate the
 * slot afterwards in case another CPU populated the same key
 * in the meantime.
 */

#include "page_cache.h"
#include "pmem.h"
#include "serial.h"
#include "../arch/spinlock.h"

#define PAGE_CACHE_SLOTS 32

struct slot {
    uint8_t   used;          /* 0 = free, 1 = populated                  */
    uint8_t   _pad[3];
    uint32_t  cache_id;      /* opaque caller-assigned key                */
    uint64_t  offset;        /* page-aligned offset within (cache_id)     */
    uint64_t  pa;            /* pmem page backing this slot               */
    uint32_t  refcount;      /* live mappers / pinned readers             */
    uint32_t  last_used;     /* g_clock at most recent touch              */
};

static struct slot g_slots[PAGE_CACHE_SLOTS];
static uint32_t    g_clock;
static int         g_inited;

static uint64_t g_hits;
static uint64_t g_misses;
static uint64_t g_evicts;

static spinlock_t g_lock = SPINLOCK_INIT;

void page_cache_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        g_slots[i].used     = 0;
        g_slots[i].cache_id = 0;
        g_slots[i].offset   = 0;
        g_slots[i].pa       = 0;
        g_slots[i].refcount = 0;
        g_slots[i].last_used= 0;
    }
    g_clock = 1;
    serial_puts("[page-cache] init: ");
    serial_puthex((uint64_t)PAGE_CACHE_SLOTS);
    serial_puts(" slots\n");
}

/* Lookup by key.  Caller MUST hold g_lock.  Returns slot index
 * or -1. */
static int find_slot_locked(uint32_t cache_id, uint64_t offset)
{
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!g_slots[i].used) continue;
        if (g_slots[i].cache_id == cache_id && g_slots[i].offset == offset)
            return i;
    }
    return -1;
}

/* Lookup by PA.  Caller MUST hold g_lock.  Returns slot index
 * or -1.  Used by release(). */
static int find_slot_by_pa_locked(uint64_t pa)
{
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!g_slots[i].used) continue;
        if (g_slots[i].pa == pa) return i;
    }
    return -1;
}

/* Pick a victim slot for eviction.  Caller MUST hold g_lock.
 * Returns -1 if no slot is eligible (every slot in use AND
 * every populated one has refcount > 0). */
static int pick_victim_locked(void)
{
    int      best     = -1;
    uint32_t best_clk = 0xFFFFFFFFu;
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!g_slots[i].used) return i;     /* free slot beats any victim */
        if (g_slots[i].refcount != 0) continue;
        if (g_slots[i].last_used < best_clk) {
            best_clk = g_slots[i].last_used;
            best     = i;
        }
    }
    return best;
}

uint64_t page_cache_get_or_load(uint32_t cache_id, uint64_t offset_bytes,
                                page_cache_loader_fn loader, void *ctx)
{
    if (offset_bytes & (PAGE_SIZE - 1)) return 0;

    /* Phase 1: did we already cache this? */
    uint64_t flags = spin_lock_irqsave(&g_lock);
    int idx = find_slot_locked(cache_id, offset_bytes);
    if (idx >= 0) {
        g_slots[idx].refcount++;
        g_slots[idx].last_used = ++g_clock;
        uint64_t pa = g_slots[idx].pa;
        g_hits++;
        spin_unlock_irqrestore(&g_lock, flags);
        return pa;
    }

    /* Phase 2: pick a victim, allocate the page, drop the lock
     * for the loader call.  We mark the slot as in-progress by
     * setting refcount=1 BEFORE dropping the lock; that way no
     * other CPU evicts it while we're loading. */
    int victim = pick_victim_locked();
    if (victim < 0) {
        spin_unlock_irqrestore(&g_lock, flags);
        serial_puts("[page-cache] full, no idle slot\n");
        return 0;
    }

    /* Free the victim's old PA (if any).  pmem_alloc_page below
     * gives us a fresh one. */
    if (g_slots[victim].used) {
        uint64_t old_pa = g_slots[victim].pa;
        spin_unlock_irqrestore(&g_lock, flags);
        pmem_free_page(old_pa);
        flags = spin_lock_irqsave(&g_lock);
        g_evicts++;
    }

    /* Re-pick: another CPU may have raced ahead and populated
     * our key, or evicted our chosen victim.  Re-check both. */
    idx = find_slot_locked(cache_id, offset_bytes);
    if (idx >= 0) {
        /* Someone else already loaded this key.  Use their slot. */
        g_slots[idx].refcount++;
        g_slots[idx].last_used = ++g_clock;
        uint64_t pa = g_slots[idx].pa;
        g_hits++;
        spin_unlock_irqrestore(&g_lock, flags);
        return pa;
    }
    /* Re-pick victim — the previous one may now be re-used. */
    victim = pick_victim_locked();
    if (victim < 0) {
        spin_unlock_irqrestore(&g_lock, flags);
        return 0;
    }

    /* Reserve the slot before dropping the lock so no one
     * else thinks it's idle. */
    g_slots[victim].used      = 1;
    g_slots[victim].cache_id  = cache_id;
    g_slots[victim].offset    = offset_bytes;
    g_slots[victim].pa        = 0;     /* will be filled in below */
    g_slots[victim].refcount  = 1;
    g_slots[victim].last_used = ++g_clock;
    g_misses++;
    spin_unlock_irqrestore(&g_lock, flags);

    /* Phase 3: allocate physical page, run loader. */
    uint64_t pa = pmem_alloc_page();
    if (!pa) {
        flags = spin_lock_irqsave(&g_lock);
        g_slots[victim].used = 0;
        g_slots[victim].refcount = 0;
        spin_unlock_irqrestore(&g_lock, flags);
        return 0;
    }

    if (loader) {
        if (loader(pa, offset_bytes, ctx) != 0) {
            pmem_free_page(pa);
            flags = spin_lock_irqsave(&g_lock);
            g_slots[victim].used = 0;
            g_slots[victim].refcount = 0;
            spin_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    } else {
        /* No loader = anonymous zero page.  pmem_alloc_page
         * zero-fills already, so nothing to do. */
    }

    /* Phase 4: install the PA. */
    flags = spin_lock_irqsave(&g_lock);
    g_slots[victim].pa = pa;
    spin_unlock_irqrestore(&g_lock, flags);
    return pa;
}

void page_cache_release(uint64_t pa)
{
    if (!pa) return;
    uint64_t flags = spin_lock_irqsave(&g_lock);
    int idx = find_slot_by_pa_locked(pa);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_lock, flags);
        return;
    }
    if (g_slots[idx].refcount > 0) g_slots[idx].refcount--;
    spin_unlock_irqrestore(&g_lock, flags);
}

uint64_t page_cache_hits(void)       { return g_hits; }
uint64_t page_cache_misses(void)     { return g_misses; }
uint64_t page_cache_evictions(void)  { return g_evicts; }

uint64_t page_cache_in_use(void)
{
    uint64_t n = 0;
    uint64_t flags = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++)
        if (g_slots[i].used) n++;
    spin_unlock_irqrestore(&g_lock, flags);
    return n;
}
