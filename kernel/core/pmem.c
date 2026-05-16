/*
 * kernel/core/pmem.c — 4 KiB physical-frame allocator.
 *
 * The freelist is stored in-band: each free page's first 8 bytes
 * contain the physical address of the next free page (or 0 for the
 * tail).  Allocating peels off the head; freeing pushes onto the
 * head.
 *
 * Critical assumption: the allocator only hands out pages whose
 * physical address ALSO happens to be its current usable virtual
 * address.  In our identity-mapped layout (TTBR0 covers
 * [0..2 GiB) one-to-one) this is automatically true.  When we
 * grow into a higher-half kernel (chapter 8) we will have to
 * either keep a permanent identity window for pmem, or convert
 * physical addresses to a `phys_to_virt` offset before
 * dereferencing — that change is local to this file.
 */

#include "pmem.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

static uint64_t g_free_head    = 0;   /* phys addr of head, 0 = empty */
static size_t   g_total_pages  = 0;
static size_t   g_free_pages   = 0;

static int range_overlaps(uint64_t a_base, uint64_t a_size,
                          uint64_t b_base, uint64_t b_size)
{
    uint64_t a_end = a_base + a_size;
    uint64_t b_end = b_base + b_size;
    return !(a_end <= b_base || b_end <= a_base);
}

static int page_excluded(uint64_t pa,
                         const struct pmem_carveout *carve,
                         size_t carve_count)
{
    for (size_t i = 0; i < carve_count; i++) {
        if (range_overlaps(pa, PAGE_SIZE, carve[i].base, carve[i].size))
            return 1;
    }
    return 0;
}

static void push_free(uint64_t pa)
{
    /* Identity-mapped: deref the page directly via its phys addr. */
    *(uint64_t *)(uintptr_t)pa = g_free_head;
    g_free_head = pa;
    g_free_pages++;
}

void pmem_init(const struct fdt_memory_map *mem,
               const struct pmem_carveout *carve, size_t carve_count,
               size_t *pages_out)
{
    g_free_head   = 0;
    g_total_pages = 0;
    g_free_pages  = 0;

    /* Iterate pages from HIGHEST physical address DOWN to LOWEST,
     * pushing each onto the freelist.  Because push_free is LIFO,
     * the LAST pushed page (lowest physical) becomes the head, so
     * pmem_alloc_page hands out LOW addresses first.
     *
     * Why low-first matters on aarch64 + HVF: kernel stacks come
     * out of kheap, and a 16 KiB stack whose top sits at the very
     * end of mapped RAM (e.g. 0x240000000 with -m 8G) lets
     * save_context's stp/ldp straddle the unmapped boundary.  The
     * resulting partial reads behave bizarrely on Apple silicon —
     * some accesses succeed (cached) while others fault.  Keeping
     * the heap pinned to the LOW end of RAM ensures every
     * subsequent kmalloc lives many GiB away from the boundary,
     * which is structurally safer than relying on a guard page
     * and matches what most real kernels do anyway. */
    for (size_t r = mem->count; r-- > 0; ) {
        uint64_t base = mem->regions[r].base;
        uint64_t size = mem->regions[r].size;

        /* Round to page boundaries inwards. */
        uint64_t start = (base + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
        uint64_t end   = (base + size) & ~((uint64_t)PAGE_SIZE - 1);

        /* Walk pages from end-1 down to start so the lowest page
         * is pushed last and ends up at the freelist head. */
        for (uint64_t pa = end; pa > start; ) {
            pa -= PAGE_SIZE;
            g_total_pages++;
            if (page_excluded(pa, carve, carve_count))
                continue;
            push_free(pa);
        }
    }

    if (pages_out) *pages_out = g_free_pages;
}

uint64_t pmem_alloc_page(void)
{
    if (!g_free_head) return 0;
    uint64_t pa  = g_free_head;
    g_free_head  = *(uint64_t *)(uintptr_t)pa;
    g_free_pages--;
    /* Zero on hand-out so callers don't see freelist links. */
    uint64_t *p = (uint64_t *)(uintptr_t)pa;
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
        p[i] = 0;
    return pa;
}

uint64_t pmem_alloc_contig(size_t npages)
{
    if (npages == 0) return 0;

    /* Now that pmem_init seeds the freelist low-first, the head
     * is the LOWEST free page and successive pops give us the
     * next-higher pages.  We therefore expect each subsequent
     * page to be `prev + PAGE_SIZE`, and the LOW base of the run
     * is just `first` itself. */
    uint64_t first = pmem_alloc_page();
    if (!first) return 0;
    uint64_t expected = first + PAGE_SIZE;
    for (size_t i = 1; i < npages; i++) {
        uint64_t got = pmem_alloc_page();
        if (got == 0 || got != expected) {
            /* Non-contiguous freelist or OOM. We deliberately do
             * not try to back the partial run out: the only callers
             * are early-boot one-shots that treat failure as fatal,
             * and pushing the partial run back through pmem_free_page
             * would just relink in reverse and confuse the next
             * caller's contiguity assumption. Return 0 and let the
             * caller panic with context. */
            serial_puts("[pmem] alloc_contig: gap at page ");
            serial_puthex((uint64_t)i);
            serial_puts(" (got ");
            serial_puthex(got);
            serial_puts(", expected ");
            serial_puthex(expected);
            serial_puts(")\n");
            return 0;
        }
        expected += PAGE_SIZE;
    }
    return first;
}

void pmem_free_page(uint64_t pa)
{
    if (!pa || (pa & (PAGE_SIZE - 1))) {
        serial_puts("[pmem] free of bad page ");
        serial_puthex(pa);
        serial_puts("\n");
        return;
    }
    push_free(pa);
}

size_t pmem_total_pages(void) { return g_total_pages; }
size_t pmem_free_pages(void)  { return g_free_pages;  }
