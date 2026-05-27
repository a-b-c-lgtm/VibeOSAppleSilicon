/*
 * kernel/core/osfs2_journal.h — single-active-transaction
 * physical-block journal for OSFS-2.  Chapter 84.
 *
 * Goal
 * ====
 *
 * Make every cache flush ATOMIC with respect to power loss.  Either
 * all dirty blocks land at their destinations, or none of them do.
 *
 * Strategy
 * --------
 *
 * Write-ahead log.  Before applying any of the dirty blocks to
 * their real destinations on disk, we copy them to a fixed
 * journal region with a header that records each block's intended
 * destination.  The header is written LAST, with a magic word and
 * a CRC32 over the descriptor + payload.  Only after the header is
 * durable do we apply the writes to their real destinations.
 *
 * If the kernel dies anywhere between "started writing data slots"
 * and "header is durable", the header either has stale magic/zero
 * or fails CRC, so on the next mount we discard the partial
 * transaction.
 *
 * If the kernel dies after the header is durable but before all
 * destinations are written (or while writing them), the next mount
 * sees a valid header, replays each (dest, data) pair, and
 * idempotently brings the FS into the post-flush state.
 *
 * Single active transaction is enough because the cache is single-
 * threaded and a flush either finishes or it doesn't.  We never
 * have two flushes concurrently in flight.
 *
 * Mode = data=journal: every dirty block (data and metadata
 * alike) is double-written.  Cost: ~2x flush traffic.  Worth it
 * because the implementation is small enough to fit in a chapter.
 * data=ordered would skip data blocks but needs per-slot type
 * bookkeeping the cache doesn't have.  Punted to a follow-up.
 *
 * On-disk layout
 * --------------
 *
 *   Block J+0          header (struct osfs2_journal_header)
 *   Block J+1..J+N     payload data slot i = block contents
 *
 * J = g_sb.journal_header_block (= 67 in current mkosfs2.py).
 * N = g_sb.journal_data_blocks  (= 32, matches the 32-slot cache).
 *
 * The journal NEVER goes through the cache.  Caching the journal
 * defeats its purpose: a "durable" commit would just sit in RAM.
 * All journal I/O bypasses osfs2_cache and talks straight to
 * virtio-blk.
 */
#ifndef OSFS2_JOURNAL_H
#define OSFS2_JOURNAL_H

#include <stdint.h>
#include <stddef.h>
#include "osfs2.h"          /* OSFS2_BLOCK_SIZE, OSFS2_JOURNAL_DATA_BLOCKS */

#define OSFS2_JOURNAL_MAGIC0 'O'
#define OSFS2_JOURNAL_MAGIC1 'S'
#define OSFS2_JOURNAL_MAGIC2 'F'
#define OSFS2_JOURNAL_MAGIC3 'S'
#define OSFS2_JOURNAL_MAGIC4 'J'
#define OSFS2_JOURNAL_MAGIC5 'R'
#define OSFS2_JOURNAL_MAGIC6 'N'
#define OSFS2_JOURNAL_MAGIC7 'L'

/* Persistent header — fits in one 4 KiB block.  Layout is fixed
 * by the on-disk format: do NOT reorder. */
struct osfs2_journal_header {
    uint8_t  magic[8];          /* "OSFSJRNL" when valid; zero otherwise */
    uint32_t txn_id;            /* monotonic, debug only */
    uint32_t block_count;       /* 0 = no commit; 1..N otherwise */
    uint32_t crc32;             /* CRC32 over dest[] + payload bytes */
    uint32_t reserved;
    uint32_t dest[OSFS2_JOURNAL_DATA_BLOCKS];   /* destination block #s */
    uint8_t  pad[OSFS2_BLOCK_SIZE
                 - 8 - 4 - 4 - 4 - 4
                 - (OSFS2_JOURNAL_DATA_BLOCKS * 4)];
} __attribute__((packed));

/* Wire-up.  Reads the journal region's location from the (already
 * cached) superblock.  Safe to call once the superblock has been
 * read; called by osfs2_init() right before the bitmaps are
 * loaded. */
void osfs2_journal_init(uint32_t header_block, uint32_t data_blocks);

/* Replay a committed-but-not-checkpointed transaction, if any.
 *
 * Returns:
 *    0  no replay was needed (header zero / magic absent / CRC
 *       fail), or replay completed successfully.
 *   -1  I/O error during replay; FS should be considered
 *       inconsistent and the kernel should refuse to mount.
 *
 * Idempotent: calling replay twice with the same on-disk state
 * produces the same result. */
int osfs2_journal_replay(void);

/* Commit a batch of (block_num, data) pairs atomically.
 *
 * `dest[]` is the array of destination block numbers; `data[]` is
 * the array of 4 KiB block contents.  `count` must be in [1, N].
 *
 * On success returns 0 and the destinations are durably updated.
 * On failure returns -1; caller's data is left in some state
 * between "fully old" and "fully new" — the journal will roll
 * forward (or discard, if the failure was before the commit
 * marker landed) on the next mount. */
int osfs2_journal_commit(const uint32_t *dest,
                         const uint8_t *const *data,
                         uint32_t count);

/* Stats — for the chapter and the test harness. */
uint64_t osfs2_journal_commits(void);
uint64_t osfs2_journal_replays(void);
uint64_t osfs2_journal_blocks_journalled(void);

#endif /* OSFS2_JOURNAL_H */
