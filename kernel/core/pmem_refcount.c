/*
 * kernel/core/pmem_refcount.c \u2014 chapter 74 COW refcount table.
 *
 * One uint16_t per 4 KiB DRAM frame.  Index = (pa - dram_base) /
 * PAGE_SIZE.  Sized at boot; storage comes from kheap.
 *
 * Refcount semantics
 * ------------------
 *   rc == 0   "untracked sole owner": exactly one holder, never
 *             shared.  This is the default state of every frame
 *             after pmem_init, and the state pmem_alloc_page
 *             implicitly hands back.  pmem_dec_and_free on a
 *             rc==0 page frees it.
 *   rc == 1   transient "one tracked holder remaining" state
 *             reached just before the last sharer drops.
 *             pmem_dec_and_free on rc==1 frees the page (and
 *             resets rc back to 0).
 *   rc >= 2   exactly that many holders.  pmem_dec_and_free
 *             decrements; the page is NOT freed until the count
 *             eventually reaches 0.
 *
 * The first share via pmem_refcount_share takes 0 -> 2 (NOT 0
 * -> 1), because what was previously "1 implicit holder,
 * untracked" is now "2 holders, tracked."  Subsequent shares
 * increment by 1.
 *
 * Why the asymmetry?  Because every existing call site of
 * pmem_free_page predates COW and would over-decrement if we
 * forced rc to 1 on every fresh allocation.  Keeping rc == 0 as
 * the default + jumping the share path to 2 lets us retrofit
 * COW without auditing every existing pmem caller.
 *
 * 16 bits is plenty: the worst-case sharer count is the number
 * of live processes, and even a long-running shell with hundreds
 * of fork+exec children will have at most a few dozen processes
 * sharing any one page.
 */

#include "pmem_refcount.h"
#include "pmem.h"
#include "heap.h"
#include "serial.h"

static uint16_t *g_rc      = NULL;
static uint64_t  g_base    = 0;
static size_t    g_npages  = 0;

static inline int pa_in_range(uint64_t pa)
{
    return g_rc != NULL && pa >= g_base &&
           ((pa - g_base) >> 12) < g_npages;
}

static inline size_t pa_to_idx(uint64_t pa)
{
    return (size_t)((pa - g_base) >> 12);
}

size_t pmem_refcount_init(uint64_t dram_base, uint64_t dram_size)
{
    g_base   = dram_base;
    g_npages = (size_t)(dram_size >> 12);

    /* Allocate ~2 bytes per frame.  For 8 GiB DRAM that's 4 MiB
     * \u2014 small enough for the boot kheap, large enough that we
     * don't want to bury it in .bss. */
    size_t bytes = g_npages * sizeof(uint16_t);
    g_rc = (uint16_t *)kmalloc(bytes);
    if (!g_rc) {
        serial_puts("[pmem_rc] FATAL: refcount table alloc failed\n");
        for (;;) __asm__ volatile("wfe");
    }
    /* Default refcount = 0 ("untracked sole owner"). */
    for (size_t i = 0; i < g_npages; i++) g_rc[i] = 0;

    serial_puts("[pmem_rc] tracking ");
    serial_puthex((uint64_t)g_npages);
    serial_puts(" frames (");
    serial_puthex((uint64_t)bytes);
    serial_puts(" bytes)\n");
    return g_npages;
}

void pmem_refcount_set(uint64_t pa, uint16_t v)
{
    if (!pa_in_range(pa)) return;
    g_rc[pa_to_idx(pa)] = v;
}

uint16_t pmem_refcount_get(uint64_t pa)
{
    if (!pa_in_range(pa)) return 0;
    return g_rc[pa_to_idx(pa)];
}

void pmem_refcount_share(uint64_t pa)
{
    if (!pa_in_range(pa)) return;
    size_t i = pa_to_idx(pa);
    if (g_rc[i] == 0) {
        /* First share of a previously untracked page.  The page
         * had 1 implicit holder; now it has 2 explicit holders. */
        g_rc[i] = 2;
    } else {
        /* Already shared between N holders — add one more. */
        g_rc[i]++;
    }
}

uint16_t pmem_refcount_dec(uint64_t pa)
{
    if (!pa_in_range(pa)) return 0;
    size_t i = pa_to_idx(pa);
    if (g_rc[i] == 0) return 0;     /* untracked: nothing to dec */
    g_rc[i]--;
    return g_rc[i];
}

void pmem_dec_and_free(uint64_t pa)
{
    if (!pa) return;
    uint16_t cur = pmem_refcount_get(pa);
    if (cur >= 2) {
        /* Multi-holder — some other AS still maps this page.
         * Just dec; do NOT return the page to pmem. */
        pmem_refcount_dec(pa);
        return;
    }
    /* cur == 0 (untracked sole owner) or cur == 1 (last
     * remaining tracked holder).  Either way we are dropping
     * the LAST reference and the page must go back to pmem.
     * Reset rc to 0 so the next pmem_alloc_page that hands this
     * page out starts from a clean "untracked sole owner" state. */
    if (cur == 1) pmem_refcount_set(pa, 0);
    pmem_free_page(pa);
}
