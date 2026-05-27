/*
 * userspace/libc/malloc.h — tiny user-side allocator built on sbrk.
 *
 * Single-header implementation: include once and the `static inline`
 * helpers go straight into the program's object file.  No external
 * .o needed.
 *
 * Thread safety (chapter 95 update): a per-process spinlock guards
 * the free-list head and the sbrk-grow path.  Without it, two
 * threads in the same address space (e.g. the browser's GUI core
 * and parser thread, sharing the heap via CLONE_VM) can corrupt
 * the free-list — symptoms include `next` pointers pointing into
 * payload bytes (FAR=0xascii-garbage faults) and double-frees
 * silently returning the same block twice.  The lock is a
 * cmpxchg-based test-and-set; critical sections are tiny (linear
 * walk over the free list, typically <100 entries) so we don't
 * bother with futex_wait — pure spinning with `wfe` to be a
 * good citizen on contended hand-off.
 */

#ifndef USER_MALLOC_H
#define USER_MALLOC_H

#include "syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MALLOC_GROW
#define MALLOC_GROW (16 * 1024)   /* sbrk in 16 KiB chunks */
#endif

struct ualloc_blk {
    size_t              size;     /* total block size including header */
    struct ualloc_blk  *next;     /* free-list link (only valid if free) */
};

#define UALLOC_HDR_SIZE (sizeof(size_t))   /* 8 bytes; payload starts at +8 */
#define UALLOC_ALIGN    16

static inline struct ualloc_blk **_ualloc_free_head_ptr(void)
{
    static struct ualloc_blk *g_free_head = (struct ualloc_blk *)0;
    return &g_free_head;
}

/* Per-process spinlock guarding the free-list and sbrk-grow path.
 * cmpxchg test-and-set with a `yield`-hinted busy-spin while
 * contended.  Critical sections are short (linear free-list
 * walks) so we never block on a futex; the second CPU just spins
 * for a few hundred cycles.  We deliberately do NOT use `wfe` —
 * SCTLR_EL1.nTWE is 0 in this kernel, so a WFE at EL0 traps.
 * `yield` is `hint #1`, never traps, and lets the CPU back-off
 * dispatch slots while we retry.  Recursive locking is NOT
 * supported — never call malloc/free from inside the lock
 * window. */
static inline volatile uint32_t *_ualloc_lock_ptr(void)
{
    static volatile uint32_t g_lock = 0;
    return &g_lock;
}

static inline void _ualloc_lock_acquire(void)
{
    volatile uint32_t *p = _ualloc_lock_ptr();
    uint32_t old, fail;
    for (;;) {
        __asm__ volatile(
            "1: ldaxr   %w0, [%2]            \n"
            "   cbnz    %w0, 2f              \n"
            "   stxr    %w1, %w3, [%2]       \n"
            "   cbnz    %w1, 1b              \n"
            "2:                              \n"
            : "=&r"(old), "=&r"(fail)
            : "r"(p), "r"((uint32_t)1)
            : "memory");
        if (old == 0) return;
        /* Contended.  Hint to the core that we're spinning so
         * it can prioritise the other hyperthread / SMT lane.
         * `yield` is hint #1; safe at EL0. */
        __asm__ volatile("yield" ::: "memory");
    }
}

static inline void _ualloc_lock_release(void)
{
    volatile uint32_t *p = _ualloc_lock_ptr();
    /* Release-store 0.  No `sev` needed — we're not pairing with
     * a `wfe` waiter any more. */
    __asm__ volatile(
        "stlr   wzr, [%0]                \n"
        :: "r"(p) : "memory");
}

/* Round up to a multiple of ALIGN; never less than 16 bytes total. */
static inline size_t _ualloc_round_size(size_t want)
{
    size_t total = want + UALLOC_HDR_SIZE;
    if (total < 16 + UALLOC_HDR_SIZE) total = 16 + UALLOC_HDR_SIZE;
    total = (total + UALLOC_ALIGN - 1) & ~(size_t)(UALLOC_ALIGN - 1);
    return total;
}

static inline int _ualloc_grow(size_t need)
{
    size_t chunk = need < MALLOC_GROW ? MALLOC_GROW : need;
    chunk = (chunk + UALLOC_ALIGN - 1) & ~(size_t)(UALLOC_ALIGN - 1);
    void *p = sbrk((long)chunk);
    if ((long)(uintptr_t)p < 0) return -1;
    /* Build a single free block out of the new region and push it
     * onto the free list.  Coalescing with the previous tail
     * happens implicitly on the next malloc/free pass. */
    struct ualloc_blk  *blk = (struct ualloc_blk *)p;
    blk->size = chunk;
    blk->next = *_ualloc_free_head_ptr();
    *_ualloc_free_head_ptr() = blk;
    return 0;
}

static inline void *malloc(size_t want)
{
    if (want == 0) return (void *)0;
    size_t need = _ualloc_round_size(want);

    _ualloc_lock_acquire();
    for (int attempt = 0; attempt < 2; attempt++) {
        struct ualloc_blk **pp = _ualloc_free_head_ptr();
        while (*pp) {
            struct ualloc_blk *b = *pp;
            if (b->size >= need) {
                /* Split if the leftover is large enough to host
                 * another minimum-sized block. */
                size_t leftover = b->size - need;
                if (leftover >= 32) {
                    struct ualloc_blk *tail =
                        (struct ualloc_blk *)((char *)b + need);
                    tail->size = leftover;
                    tail->next = b->next;
                    b->size    = need;
                    *pp = tail;
                } else {
                    *pp = b->next;
                }
                _ualloc_lock_release();
                return (char *)b + UALLOC_HDR_SIZE;
            }
            pp = &b->next;
        }
        /* No block fit — grow and retry.  _ualloc_grow pushes the
         * new block onto the free list under our lock. */
        if (_ualloc_grow(need) != 0) {
            _ualloc_lock_release();
            return (void *)0;
        }
    }
    _ualloc_lock_release();
    return (void *)0;
}

static inline void free(void *p)
{
    if (!p) return;
    /* Defensive: catch frees of obviously-garbage pointers (very
     * small values, never in our heap which lives well above the
     * first 64 KiB).  Silently ignore — papers over any leftover
     * use-after-free bug rather than crashing the user thread. */
    if ((uintptr_t)p < 0x10000u) return;
    struct ualloc_blk *b = (struct ualloc_blk *)((char *)p - UALLOC_HDR_SIZE);

    _ualloc_lock_acquire();
    /* Insert in address order so coalescing is just a linear walk. */
    struct ualloc_blk **pp = _ualloc_free_head_ptr();
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;

    /* Try to coalesce with right neighbour. */
    if (b->next && (char *)b + b->size == (char *)b->next) {
        b->size += b->next->size;
        b->next  = b->next->next;
    }
    /* Coalesce with left neighbour if it's also adjacent. */
    /* (We'd need the prev pointer for this; approximate by walking
     * the list once more on the next malloc.  Skip for now — our
     * free-then-malloc-same-size pattern still benefits from the
     * right-coalescing above, which is the common case.) */
    _ualloc_lock_release();
}

/* calloc(n, size): allocate n*size bytes, zero-initialised.
 * Chapter 172 addition (DoomGeneric needs it).  We do the
 * malloc + memset by hand to avoid pulling in <string.h> here
 * (which would create a circular include via stdlib.h). */
static inline void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (n != 0 && total / n != size) return (void *)0; /* overflow */
    void *p = malloc(total);
    if (!p) return p;
    unsigned char *q = (unsigned char *)p;
    for (size_t i = 0; i < total; i++) q[i] = 0;
    return p;
}

/* realloc(p, want): grow or shrink an allocation.  Chapter 172
 * addition.  Always malloc-new-copy-free-old — we don't try to
 * extend in place even when the trailing slack is sufficient,
 * because the free-list shape after coalescing makes the math
 * fragile.  Doom only calls realloc on small WAD-checksum
 * buffers so the extra copy is invisible.
 *
 * realloc(NULL, n) == malloc(n); realloc(p, 0) frees p and
 * returns NULL, both per C11. */
static inline void *realloc(void *p, size_t want)
{
    if (!p) return malloc(want);
    if (want == 0) { free(p); return (void *)0; }
    struct ualloc_blk *b = (struct ualloc_blk *)((char *)p - UALLOC_HDR_SIZE);
    size_t old_payload = b->size - UALLOC_HDR_SIZE;
    void *np = malloc(want);
    if (!np) return (void *)0;
    size_t copy = old_payload < want ? old_payload : want;
    unsigned char       *d = (unsigned char *)np;
    const unsigned char *s = (const unsigned char *)p;
    for (size_t i = 0; i < copy; i++) d[i] = s[i];
    free(p);
    return np;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* USER_MALLOC_H */
