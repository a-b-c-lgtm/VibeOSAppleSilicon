/*
 * kernel/core/osfs2_cache.h — write-back block cache in front of OSFS-2.
 *
 * The chapter-81 OSFS-2 driver issues a fresh virtio-blk read or
 * write for every 4 KiB block touch.  A typical `echo hi > /data/x`:
 *
 *   read  block bitmap                    (alloc data block)
 *   write block bitmap                    (commit alloc)
 *   write data block                      (zero-init)
 *   write data block                      (file contents)
 *   read  inode bitmap                    (alloc inode)
 *   write inode bitmap                    (commit alloc)
 *   read  inode-table block               (RMW the new inode)
 *   write inode-table block
 *   read  inode-table block               (RMW the root dir's inode)
 *   write inode-table block
 *   read  root dir block                  (find/append dirent)
 *   write root dir block
 *   read  inode-table block               (write root size++)
 *   write inode-table block
 *
 * That's 14 block I/Os = 112 virtio-blk transactions for a single
 * tiny write.  Most of them touch the same handful of blocks
 * repeatedly (the inode-table block holding inode 1 is read+written
 * three times).
 *
 * This cache fixes that:
 *
 *   - 4 KiB-granularity (one slot = one OSFS-2 block); 32 slots →
 *     128 KiB total, more than enough to hold every metadata block
 *     OSFS-2 currently uses (superblock + 2 bitmaps + 64 inode-table
 *     blocks won't fit, but the *hot* set — root dir block, inode 1's
 *     table block, plus the two bitmaps — fits trivially).
 *   - Write-back: `osfs2_cache_write` only marks the slot dirty;
 *     the actual disk write happens lazily on eviction or on an
 *     explicit `osfs2_cache_flush()` (driven by `fsync` or the
 *     periodic background flusher).
 *   - Eviction is clock-counter LRU.  A dirty victim is flushed
 *     synchronously before its slot is reused; we never silently
 *     drop dirty data.
 *
 * Why a separate cache instead of extending blk_cache?
 *
 *   blk_cache (chapter 23) is sector-granularity (512 B), read-only,
 *   and hard-wired to virtio-blk device 0 (where OSFS-1 lives).
 *   Bolting write-back on it would compromise the OSFS-1 invariant
 *   that "what's on disk is the source of truth, the cache is just
 *   a hint".  Keeping the two caches disjoint also lets us reason
 *   about device-1 eviction independently from boot-time ELF reads.
 *
 * Concurrency note: like the rest of the OSFS-2 stack the cache is
 * single-threaded for now (called from syscall context with
 * preemption inhibited by the surrounding spinlock-free design).
 * If/when osfs2 gains parallel callers, every public function here
 * needs to take a per-cache lock.
 */
#ifndef OSFS2_CACHE_H
#define OSFS2_CACHE_H

#include <stdint.h>

/* One-time setup.  Call after `osfs2_init()` succeeds.  Safe to
 * call when OSFS-2 is absent — the cache just stays inert. */
void osfs2_cache_init(void);

/* Read one OSFS-2 block (4 KiB) into `buf`.  Returns 0 on success
 * or -1 if the underlying device read failed.  A miss falls
 * through to `virtio_blk_dev_read` and installs the result. */
int osfs2_cache_read(uint32_t blk, void *buf);

/* Write one OSFS-2 block (4 KiB) from `buf`.  Marks the slot dirty
 * but does NOT issue a disk write — durability is the caller's
 * responsibility (fsync) or the background flusher's.  Returns 0
 * on success or -1 if a dirty victim eviction failed (the
 * underlying disk write back-propagated an error). */
int osfs2_cache_write(uint32_t blk, const void *buf);

/* Flush every dirty slot to disk synchronously.  Returns 0 on
 * success.  On error the offending slot is left dirty (so the
 * data isn't silently lost) and -1 is returned; the caller can
 * retry later or surface the error to userspace via fsync. */
int osfs2_cache_flush(void);

/* Drop a slot without flushing.  Idempotent; only used by the
 * (future) format-on-the-fly path.  Refuses to drop a dirty
 * slot — call `osfs2_cache_flush()` first. */
void osfs2_cache_invalidate(uint32_t blk);

/* Stats accessors — used by tests and the chapter-82
 * verification trace.  All counters are zeroed by
 * `osfs2_cache_init`. */
uint64_t osfs2_cache_hits(void);
uint64_t osfs2_cache_misses(void);
uint64_t osfs2_cache_writebacks(void);
uint64_t osfs2_cache_evictions(void);
uint32_t osfs2_cache_dirty_count(void);

/* Pretty-print "[osfs2_cache] hits=H misses=M evictions=E writebacks=W"
 * to the serial console.  `prefix` defaults to "[osfs2_cache]" if NULL. */
void osfs2_cache_dump_stats(const char *prefix);

#endif /* OSFS2_CACHE_H */
