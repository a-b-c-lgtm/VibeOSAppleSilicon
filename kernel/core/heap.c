/*
 * kernel/core/heap.c — first-fit kernel heap with bidirectional
 * coalescing.  Single-region, single-thread, no locking yet.
 *
 * Layout
 * ------
 * The heap region is a contiguous span of memory carved out by the
 * linker script as `.heap` between `heap_start` and `heap_end`.
 * We treat the entire span as a sequence of variable-sized blocks
 * laid out back-to-back.  Each block starts with a 16-byte header:
 *
 *   struct block {
 *       size_t total_size;     // header + payload, low bit = in_use
 *       size_t prev_size;      // total_size of preceding block, 0 at start
 *   };
 *
 * The "implicit list" walks every block in order by adding
 * total_size to a pointer.  Bidirectional coalescing on free uses
 * prev_size to find the previous block in O(1).
 *
 * Allocations are 16-byte aligned by construction: total_size is
 * always rounded up to a multiple of 16 in kmalloc, so every
 * header sits on a 16-byte boundary, and every payload (which
 * starts immediately after the 16-byte header) does too.
 *
 * Why no free list
 * ----------------
 * The implicit-list walk is O(N) per allocation, which is fine for
 * a teaching kernel where N is in the thousands.  Once N grows
 * past that — chapter 12's threading work pushes it up — we will
 * upgrade to an explicit segregated free list.  Keeping the
 * implementation linear lets the book chapter focus
 * on the *invariants* (alignment, coalescing, the in-use bit)
 * rather than the data structure.
 */

#include "heap.h"
#include "serial.h"
#include <stddef.h>
#include <stdint.h>

/* Heap region — set by kheap_init().  Earlier in the book these
 * came from linker-script symbols.  Now the page allocator hands
 * us a chunk and we manage it from there. */
static uint8_t *heap_start = NULL;
static uint8_t *heap_end   = NULL;

#define ALIGN_UP(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#define HEADER_SIZE     16
#define MIN_BLOCK_SIZE  32      /* header + 16-byte minimum payload */
#define INUSE_BIT       1ULL

struct block {
    size_t total_size;          /* full block including header; low bit = in-use */
    size_t prev_size;           /* total_size of preceding block, 0 if none */
};

static inline int blk_in_use(const struct block *b)
{
    return (int)(b->total_size & INUSE_BIT);
}

static inline size_t blk_size(const struct block *b)
{
    return b->total_size & ~INUSE_BIT;
}

static inline void blk_set(struct block *b, size_t total_size, int in_use)
{
    b->total_size = total_size | (in_use ? INUSE_BIT : 0);
}

static inline struct block *blk_next(struct block *b)
{
    uint8_t *p = (uint8_t *)b + blk_size(b);
    if (p >= heap_end)
        return NULL;
    return (struct block *)p;
}

static inline struct block *blk_prev(struct block *b)
{
    if (b->prev_size == 0)
        return NULL;
    return (struct block *)((uint8_t *)b - b->prev_size);
}

static inline void *blk_payload(struct block *b)
{
    return (void *)((uint8_t *)b + HEADER_SIZE);
}

static inline struct block *blk_from_payload(void *p)
{
    return (struct block *)((uint8_t *)p - HEADER_SIZE);
}

void kheap_init(uint64_t base, size_t size)
{
    if (size < MIN_BLOCK_SIZE) {
        serial_puts("[heap] FATAL — heap region smaller than one block\n");
        for (;;) __asm__ volatile("wfe");
    }

    heap_start = (uint8_t *)(uintptr_t)base;
    heap_end   = heap_start + size;

    struct block *b = (struct block *)heap_start;
    blk_set(b, size, 0);
    b->prev_size = 0;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size_t need = ALIGN_UP(size + HEADER_SIZE, 16);
    if (need < MIN_BLOCK_SIZE)
        need = MIN_BLOCK_SIZE;

    /* First-fit walk over the implicit list. */
    for (struct block *b = (struct block *)heap_start;
         b != NULL;
         b = blk_next(b)) {
        if (blk_in_use(b))
            continue;
        size_t bsize = blk_size(b);
        if (bsize < need)
            continue;

        /* Splittable? Leave at least MIN_BLOCK_SIZE for the tail. */
        if (bsize >= need + MIN_BLOCK_SIZE) {
            struct block *tail = (struct block *)((uint8_t *)b + need);
            blk_set(tail, bsize - need, 0);
            tail->prev_size = need;
            blk_set(b, need, 1);

            /* Tell the *next-next* block its prev_size changed. */
            struct block *after_tail = blk_next(tail);
            if (after_tail)
                after_tail->prev_size = blk_size(tail);
        } else {
            /* Take the whole block; no split. */
            blk_set(b, bsize, 1);
        }

        return blk_payload(b);
    }

    return NULL;    /* OOM */
}

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    /* Bounds-check pointer to confirm it's inside the heap. */
    if ((uint8_t *)ptr < heap_start || (uint8_t *)ptr >= heap_end) {
        serial_puts("[heap] PANIC: kfree of out-of-range ptr ");
        serial_puthex((uint64_t)(uintptr_t)ptr);
        serial_puts("\n");
        for (;;) __asm__ volatile("wfe");
    }

    struct block *b = blk_from_payload(ptr);
    blk_set(b, blk_size(b), 0);

    /* Forward coalesce. */
    struct block *next = blk_next(b);
    if (next && !blk_in_use(next)) {
        size_t merged = blk_size(b) + blk_size(next);
        blk_set(b, merged, 0);
        struct block *after = blk_next(b);
        if (after)
            after->prev_size = merged;
    }

    /* Backward coalesce. */
    struct block *prev = blk_prev(b);
    if (prev && !blk_in_use(prev)) {
        size_t merged = blk_size(prev) + blk_size(b);
        blk_set(prev, merged, 0);
        struct block *after = blk_next(prev);
        if (after)
            after->prev_size = merged;
    }
}

size_t kheap_used(void)
{
    size_t used = 0;
    for (struct block *b = (struct block *)heap_start;
         b != NULL;
         b = blk_next(b)) {
        if (blk_in_use(b))
            used += blk_size(b) - HEADER_SIZE;
    }
    return used;
}

size_t kheap_block_count(void)
{
    size_t n = 0;
    for (struct block *b = (struct block *)heap_start;
         b != NULL;
         b = blk_next(b)) {
        n++;
    }
    return n;
}
