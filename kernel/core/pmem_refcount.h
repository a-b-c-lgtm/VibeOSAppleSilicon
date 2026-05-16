/*
 * kernel/core/pmem_refcount.h \u2014 per-frame refcount table for COW.
 *
 * Backs the chapter-75 copy-on-write fork.  Each 4 KiB DRAM frame
 * has a single `uint16_t` refcount stored in a flat array indexed
 * by `(pa - dram_base) >> 12`.
 *
 * Semantics:
 *   - A freshly `pmem_alloc_page()`d frame has refcount 1 (the
 *     caller is the sole holder).
 *   - On COW share (clone), the parent's refcount becomes 2; both
 *     parent and child point at the same physical page, both as
 *     read-only.
 *   - On a write fault, `pmem_share_break()` is called: it dec's
 *     the count; if the count was > 1 the caller must allocate a
 *     fresh page and copy.  If the count was already 1 (sole
 *     holder), the page just gets its descriptor flipped back to
 *     RW \u2014 no copy needed.
 *   - On AS destroy, every owned page is `pmem_dec_and_free()`d:
 *     decrement the count and only return the page to pmem if
 *     it hits 0.
 *
 * Out-of-range PAs (pmem we never indexed: device MMIO, kernel
 * image carveouts, etc.) are silently no-op'd.  COW only ever
 * shares pages that came out of `pmem_alloc_page`, so they are
 * always in-range by construction.
 */
#ifndef PMEM_REFCOUNT_H
#define PMEM_REFCOUNT_H

#include <stdint.h>
#include <stddef.h>

/* Initialise the table.  Must be called AFTER pmem_init.  Pulls
 * the DRAM extent (lowest..highest mapped page) from a slice of
 * the carveout used during pmem_init; in practice we hand it the
 * known-DRAM range.  Returns the number of frame slots covered.
 *
 * Storage comes out of kheap, so this MUST be called after the
 * heap is up. */
size_t pmem_refcount_init(uint64_t dram_base, uint64_t dram_size);

/* Refcount accessors.  Out-of-range PAs are silently ignored on
 * inc/dec (return value defined below), and read as 0. */
void     pmem_refcount_set(uint64_t pa, uint16_t v);
uint16_t pmem_refcount_get(uint64_t pa);

/* Add one more sharer to `pa`.  If the page was previously
 * untracked (rc==0, meaning "1 implicit holder"), the rc jumps
 * to 2 because there are now 2 holders, not 1.  Otherwise the
 * rc just increments by 1. */
void     pmem_refcount_share(uint64_t pa);

/* Decrement and return the new value.  If the page is out of
 * range, returns 0 (caller treats that as \"someone else owns
 * it\", i.e. don't free).  If the new value is 0, the caller
 * MUST free the page (we don't free here so the caller can
 * batch other cleanup). */
uint16_t pmem_refcount_dec(uint64_t pa);

/* Convenience: dec + free if we held the last ref.  Use this in
 * AS teardown / brk shrink / generally any \"I'm releasing my
 * mapping of this page\" path. */
void pmem_dec_and_free(uint64_t pa);

#endif /* PMEM_REFCOUNT_H */
