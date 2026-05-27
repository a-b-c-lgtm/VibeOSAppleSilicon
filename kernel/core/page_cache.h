/*
 * kernel/core/page_cache.h — chapter 91 unified page cache.
 *
 * The block cache (chapter 13) caches 512-byte sectors keyed by
 * LBA.  That's the right layer for OSFS-1 metadata and ELF loads
 * (where the whole point is to amortise virtio_blk round trips),
 * but it's the wrong layer for "I want to mmap a file."  The
 * mmap path needs to hand the user a 4 KiB page they can map
 * into their AS — not a sector that has to be copied somewhere
 * before it's useful.
 *
 * The page cache sits one layer up.  Its keys are
 *   (cache_id, offset_bytes)
 * where cache_id is opaque to the cache (the caller assigns
 * meaning) and offset_bytes is page-aligned.  Each entry owns
 * one 4 KiB pmem page and a refcount.
 *
 * Today (chapter 91) the only producer is anonymous & ramfs-
 * backed mmap.  Future chapters wire in OSFS-1 file content
 * (so re-reading the wallpaper doesn't re-walk the disk) and
 * the ELF text-segment dedupe.
 *
 * Lifetime contract:
 *   - page_cache_get_or_load(...) returns a PA with refcount
 *     bumped.  The caller must release it exactly once.
 *   - page_cache_release(pa) drops one refcount.  The cache
 *     keeps the entry around so the next lookup is a hit; only
 *     when we run out of slots do we evict refcount==0 entries
 *     in clock-LRU order.
 *
 * Sizing:
 *   - PAGE_CACHE_SLOTS = 32 (== 128 KiB total).  Tiny.  Enough
 *     for the chapter-90 smoke test and a couple of mmaps.
 *     Real workloads will need an order of magnitude more, but
 *     "make the cache bigger" doesn't teach anything new.
 */
#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <stdint.h>
#include <stddef.h>

/* Loader callback.  Called on a cache miss to populate a freshly-
 * allocated 4 KiB page.  `dst_pa` is the pmem PA the cache picked;
 * loader writes up to PAGE_SIZE bytes there (identity-mapped via
 * the boot L1).  Caller-supplied `ctx` is opaque.
 *
 * Returns 0 on success, -1 on failure (the cache will free the
 * page and return 0 from get_or_load).  Loader is permitted to
 * write fewer than PAGE_SIZE bytes; the rest stays as whatever
 * pmem_alloc_page left there (zeroes, since pmem zeros pages on
 * alloc).
 */
typedef int (*page_cache_loader_fn)(uint64_t dst_pa, uint64_t offset_bytes,
                                    void *ctx);

/* One-time setup.  No-op on second call. */
void     page_cache_init(void);

/* Look up (cache_id, offset).  Hit: bumps refcount and returns
 * the PA.  Miss: allocates a slot (evicting an idle one if
 * needed), allocates a fresh pmem page, calls `loader` to
 * populate it, then bumps refcount and returns the PA.
 *
 * Returns 0 on hard failure (cache full of pinned entries, OOM,
 * loader returned -1).  Caller treats 0 as "fall back to
 * uncached path or fail the syscall."
 *
 * `offset_bytes` must be a multiple of PAGE_SIZE. */
uint64_t page_cache_get_or_load(uint32_t cache_id, uint64_t offset_bytes,
                                page_cache_loader_fn loader, void *ctx);

/* Drop one refcount.  Idempotent for PAs the cache doesn't
 * track (silently no-op).  Does NOT free the underlying page —
 * the slot stays populated so repeat lookups still hit. */
void     page_cache_release(uint64_t pa);

/* Stats — useful for the chapter-90 smoke verification trace. */
uint64_t page_cache_hits(void);
uint64_t page_cache_misses(void);
uint64_t page_cache_evictions(void);
uint64_t page_cache_in_use(void);

#endif /* PAGE_CACHE_H */
