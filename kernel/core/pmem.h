/*
 * kernel/core/pmem.h — physical-frame allocator.
 *
 * 4 KiB pages, freelist style.  pmem_init() takes a list of usable
 * physical RAM regions (typically straight from fdt_read_memory)
 * plus any number of "carve-out" ranges to exclude (kernel image,
 * page tables, DTB itself, MMIO, etc.).  After init,
 * pmem_alloc_page() / pmem_free_page() hand out / return individual
 * frame addresses.
 *
 * The implementation is deliberately simple: we maintain a single
 * intrusive freelist where each free page stores a pointer to the
 * next free page in its first 8 bytes.  No bitmap, no allocator
 * metadata, no per-region accounting beyond a global counter.
 * Adding nicer bookkeeping (largest contiguous run, per-zone
 * tracking, etc.) is a milestone-6+ refinement.
 */
#ifndef PMEM_H
#define PMEM_H

#include <stdint.h>
#include <stddef.h>

#include "fdt.h"

#define PAGE_SIZE 4096u

struct pmem_carveout {
    uint64_t base;
    uint64_t size;
};

/* Initialise the allocator.  Adds every page in `mem` that is fully
 * outside every carveout in `carve` to the freelist.  `pages_out`,
 * if non-NULL, receives the total count of free frames. */
void pmem_init(const struct fdt_memory_map *mem,
               const struct pmem_carveout *carve, size_t carve_count,
               size_t *pages_out);

/* Allocate one 4 KiB page.  Returns 0 if out of memory. */
uint64_t pmem_alloc_page(void);

/* Allocate `npages` *physically contiguous* 4 KiB pages.  Returns
 * the physical address of the LOWEST page in the run on success
 * (so the run covers [base, base + npages * PAGE_SIZE)), or 0 on
 * failure (out of memory or insufficiently contiguous freelist).
 *
 * The current implementation drains pages off the freelist and
 * checks each one is exactly one page below the previous; if a
 * gap appears it gives up and panics rather than leaking the
 * partially-pulled run.  This is sound when called early in boot
 * before any fragmentation, which is when the kernel needs all
 * its big contiguous regions (heap, framebuffer, virtio queues).
 */
uint64_t pmem_alloc_contig(size_t npages);

/* Return a page previously obtained from pmem_alloc_page. */
void pmem_free_page(uint64_t pa);

/* Statistics accessors. */
size_t pmem_total_pages(void);
size_t pmem_free_pages(void);

#endif
