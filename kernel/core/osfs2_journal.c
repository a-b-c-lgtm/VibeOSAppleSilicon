/*
 * kernel/core/osfs2_journal.c — chapter 84 implementation.
 *
 * See osfs2_journal.h for the contract and design rationale.
 *
 * Internals
 * =========
 *
 * The journal occupies blocks J..J+N on disk, where J and N are
 * pinned at init time from the superblock.  Layout:
 *
 *   J+0          header (struct osfs2_journal_header)
 *   J+1..J+N     data slot i = the contents that should land at
 *                              header.dest[i]
 *
 * Commit protocol (write-ahead log):
 *
 *   step 1  raw_write each data block to its journal slot J+1..J+n
 *   step 2  raw_write the header (magic + crc + dest[]) to J+0
 *   step 3  raw_write each data block to its REAL destination
 *   step 4  raw_write a zeroed header to J+0     (= checkpoint)
 *
 * Crash analysis (where step S means "between writing block S-1
 * and block S"):
 *
 *   crash <  step 2:  header still has previous (zero / committed)
 *                     value.  If zero → no replay.  If a previous
 *                     committed-but-not-checkpointed batch was in
 *                     flight when our flush started, the previous
 *                     batch will be replayed on next mount — which
 *                     is correct, because nothing here changed
 *                     destinations yet.
 *   crash == step 2:  header is being written; either it lands
 *                     (good crc → replay happens, idempotent with
 *                     destinations that were never updated → fine)
 *                     or it tears (bad crc → no replay → state is
 *                     "old destinations, new journal data" which
 *                     also is "as if the flush never happened" —
 *                     still consistent).
 *   crash <  step 4:  header is committed, destinations may be
 *                     partial.  Replay re-applies all destinations.
 *   crash == step 4:  zeroing header is being torn; either it lands
 *                     (no replay needed) or it doesn't (replay re-
 *                     applies, idempotent → fine).
 *
 * Idempotence is what makes the whole thing work without an
 * "applied bit per slot": writing the same (dest, data) pair
 * twice is indistinguishable from writing it once.
 *
 * The journal NEVER goes through osfs2_cache.  Caching the
 * journal would mean a "durable" commit lives in RAM until the
 * cache flushes it later — which is the exact problem we're
 * trying to solve.  Every read and write below uses raw_*.
 *
 * CRC32
 * -----
 *
 * IEEE 802.3 polynomial 0xEDB88320, software-only.  Plenty fast
 * at journal granularity (≤ 132 KiB per commit) and it's entirely
 * self-contained — no table generation at boot, just a byte-at-a-
 * time loop.  Good enough to detect torn writes.  See chapter
 * for why we don't bother with CRC32C / hardware acceleration.
 */
#include "osfs2_journal.h"
#include "osfs2.h"
#include "serial.h"
#include "../device/virtio_blk.h"

#include <stdint.h>
#include <stddef.h>

/* ---------- module state ---------- */

static uint32_t g_header_block;     /* J */
static uint32_t g_data_block0;      /* J + 1 */
static uint32_t g_max_blocks;       /* N (≤ OSFS2_JOURNAL_DATA_BLOCKS) */
static uint32_t g_next_txn_id;
static uint64_t g_commit_count;
static uint64_t g_replay_count;
static uint64_t g_journalled_blocks;

/* Sanity check: the header must fit in exactly one block.  If we
 * ever bump OSFS2_JOURNAL_DATA_BLOCKS too far, this static assert
 * fires at compile time. */
_Static_assert(sizeof(struct osfs2_journal_header) == OSFS2_BLOCK_SIZE,
               "osfs2_journal_header must be exactly one OSFS-2 block");

/* ---------- raw I/O (bypasses the cache) ---------- */

/* Same shape as osfs2_cache.c's raw_read/raw_write — reproduced
 * here so the journal has zero dependency on the cache module
 * (and so a future change to the cache can't accidentally route
 * journal writes through it). */
static int raw_read(uint32_t blk, uint8_t *dst)
{
    uint64_t lba = (uint64_t)blk * OSFS2_SECTORS_PER_BLOCK;
    for (uint32_t i = 0; i < OSFS2_SECTORS_PER_BLOCK; i++) {
        if (virtio_blk_dev_read(OSFS2_DEVICE, lba + i,
                                dst + i * 512u) != 0)
            return -1;
    }
    return 0;
}
static int raw_write(uint32_t blk, const uint8_t *src)
{
    uint64_t lba = (uint64_t)blk * OSFS2_SECTORS_PER_BLOCK;
    for (uint32_t i = 0; i < OSFS2_SECTORS_PER_BLOCK; i++) {
        if (virtio_blk_dev_write(OSFS2_DEVICE, lba + i,
                                 src + i * 512u) != 0)
            return -1;
    }
    return 0;
}

/* ---------- CRC32 (IEEE 802.3) ---------- */

/* Byte-at-a-time CRC32, polynomial 0xEDB88320 (reflected).  No
 * table — at our throughputs the inner loop dominates the loop
 * overhead anyway, and a 1 KiB lookup table would be wasted bss. */
static uint32_t crc32_step(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
    return crc;
}

static uint32_t crc32_buf(uint32_t crc, const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) crc = crc32_step(crc, buf[i]);
    return crc;
}

/* Compute the CRC the journal stores in `header.crc32`.  Defined
 * over: dest[0..count-1] little-endian u32 bytes, then the
 * `count` payload blocks in order.  Excludes the header's magic /
 * txn_id / block_count / crc32 / reserved / dest tail. */
static uint32_t compute_commit_crc(const uint32_t *dest, uint32_t count,
                                   const uint8_t *const *data)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t d = dest[i];
        uint8_t le[4] = {
            (uint8_t)(d         & 0xFF),
            (uint8_t)((d >>  8) & 0xFF),
            (uint8_t)((d >> 16) & 0xFF),
            (uint8_t)((d >> 24) & 0xFF),
        };
        crc = crc32_buf(crc, le, 4);
    }
    for (uint32_t i = 0; i < count; i++) {
        crc = crc32_buf(crc, data[i], OSFS2_BLOCK_SIZE);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Re-derive a committed-batch's expected CRC during replay, where
 * the payloads come from disk slots rather than caller buffers. */
static uint32_t compute_replay_crc(const uint32_t *dest, uint32_t count,
                                   const uint8_t (*payload)[OSFS2_BLOCK_SIZE])
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t d = dest[i];
        uint8_t le[4] = {
            (uint8_t)(d         & 0xFF),
            (uint8_t)((d >>  8) & 0xFF),
            (uint8_t)((d >> 16) & 0xFF),
            (uint8_t)((d >> 24) & 0xFF),
        };
        crc = crc32_buf(crc, le, 4);
    }
    for (uint32_t i = 0; i < count; i++) {
        crc = crc32_buf(crc, payload[i], OSFS2_BLOCK_SIZE);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------- header helpers ---------- */

/* Static scratch — the header is one block (4 KiB).  Single-
 * threaded, so a single static is fine. */
static struct osfs2_journal_header g_hdr_scratch;

static void zero_header(struct osfs2_journal_header *h)
{
    uint8_t *p = (uint8_t *)h;
    for (size_t i = 0; i < sizeof(*h); i++) p[i] = 0;
}

static int header_is_committed(const struct osfs2_journal_header *h)
{
    static const char m[8] = { 'O','S','F','S','J','R','N','L' };
    for (int i = 0; i < 8; i++) {
        if (h->magic[i] != (uint8_t)m[i]) return 0;
    }
    if (h->block_count == 0) return 0;
    if (h->block_count > g_max_blocks) return 0;
    return 1;
}

static int write_committed_header(uint32_t txn_id, uint32_t count,
                                  const uint32_t *dest, uint32_t crc)
{
    zero_header(&g_hdr_scratch);
    static const char m[8] = { 'O','S','F','S','J','R','N','L' };
    for (int i = 0; i < 8; i++) g_hdr_scratch.magic[i] = (uint8_t)m[i];
    g_hdr_scratch.txn_id      = txn_id;
    g_hdr_scratch.block_count = count;
    g_hdr_scratch.crc32       = crc;
    g_hdr_scratch.reserved    = 0;
    for (uint32_t i = 0; i < count; i++) {
        g_hdr_scratch.dest[i] = dest[i];
    }
    return raw_write(g_header_block, (const uint8_t *)&g_hdr_scratch);
}

static int write_zero_header(void)
{
    zero_header(&g_hdr_scratch);
    return raw_write(g_header_block, (const uint8_t *)&g_hdr_scratch);
}

/* ---------- public API ---------- */

void osfs2_journal_init(uint32_t header_block, uint32_t data_blocks)
{
    g_header_block      = header_block;
    g_data_block0       = header_block + 1;
    g_max_blocks        = data_blocks;
    if (g_max_blocks > OSFS2_JOURNAL_DATA_BLOCKS) {
        g_max_blocks = OSFS2_JOURNAL_DATA_BLOCKS;
    }
    g_next_txn_id       = 1;
    g_commit_count      = 0;
    g_replay_count      = 0;
    g_journalled_blocks = 0;

    serial_puts("[osfs2_journal] ready, header=");
    serial_puthex((uint64_t)g_header_block);
    serial_puts(" slots=");
    serial_puthex((uint64_t)g_max_blocks);
    serial_puts("\n");
}

int osfs2_journal_replay(void)
{
    if (g_max_blocks == 0) {
        /* Journal not initialised; should never happen post- */
        /* osfs2_journal_init().  Treat as "nothing to do" so a   */
        /* mis-init is loud but recoverable. */
        serial_puts("[osfs2_journal] WARN: replay before init\n");
        return 0;
    }

    if (raw_read(g_header_block, (uint8_t *)&g_hdr_scratch) != 0) {
        serial_puts("[osfs2_journal] header read failed\n");
        return -1;
    }
    if (!header_is_committed(&g_hdr_scratch)) {
        /* No commit pending.  Common case (clean shutdown). */
        return 0;
    }

    uint32_t count = g_hdr_scratch.block_count;
    uint32_t saved_dest[OSFS2_JOURNAL_DATA_BLOCKS];
    for (uint32_t i = 0; i < count; i++) {
        saved_dest[i] = g_hdr_scratch.dest[i];
    }
    uint32_t saved_crc    = g_hdr_scratch.crc32;
    uint32_t saved_txn_id = g_hdr_scratch.txn_id;

    /* Pull each payload block off the journal so we can both
     * verify CRC and re-apply if it matches. */
    static uint8_t payload[OSFS2_JOURNAL_DATA_BLOCKS][OSFS2_BLOCK_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        if (raw_read(g_data_block0 + i, payload[i]) != 0) {
            serial_puts("[osfs2_journal] payload read failed\n");
            return -1;
        }
    }
    uint32_t computed = compute_replay_crc(saved_dest, count, payload);
    if (computed != saved_crc) {
        /* Torn write between data and header (or torn header).
         * Treat as "no commit" — the destinations were not
         * touched yet, so the FS state is the pre-flush one. */
        serial_puts("[osfs2_journal] CRC mismatch — discarding\n");
        if (write_zero_header() != 0) return -1;
        return 0;
    }

    /* Re-apply.  Idempotent — if we crashed after writing some
     * destinations but before zeroing the header, we'll write
     * the same bytes again. */
    serial_puts("[osfs2_journal] replaying txn ");
    serial_puthex((uint64_t)saved_txn_id);
    serial_puts(" (");
    serial_puthex((uint64_t)count);
    serial_puts(" blocks)\n");
    for (uint32_t i = 0; i < count; i++) {
        if (raw_write(saved_dest[i], payload[i]) != 0) {
            serial_puts("[osfs2_journal] replay write failed\n");
            return -1;
        }
    }

    /* Checkpoint.  After this, no further replay. */
    if (write_zero_header() != 0) return -1;
    g_replay_count++;
    /* Keep the on-disk txn_id monotonically advancing across
     * mounts so a subsequent commit's id is greater than any
     * we've replayed. */
    if (saved_txn_id >= g_next_txn_id) g_next_txn_id = saved_txn_id + 1;
    return 0;
}

int osfs2_journal_commit(const uint32_t *dest,
                         const uint8_t *const *data,
                         uint32_t count)
{
    if (count == 0) return 0;
    if (count > g_max_blocks) return -1;

    /* Step 1 — write payloads to journal slots.  Order doesn't
     * matter; we won't rely on any of these landing until the
     * header is committed. */
    for (uint32_t i = 0; i < count; i++) {
        if (raw_write(g_data_block0 + i, data[i]) != 0) {
            return -1;
        }
    }

    /* Step 2 — write the commit header LAST.  A torn write here
     * is fine: replay will see a magic-or-CRC mismatch and
     * discard the batch. */
    uint32_t crc = compute_commit_crc(dest, count, data);
    uint32_t txn = g_next_txn_id++;
    if (write_committed_header(txn, count, dest, crc) != 0) {
        /* Header write itself failed; without a commit marker the
         * batch will be discarded on next mount.  Caller sees -1
         * and can retry. */
        return -1;
    }

    /* From here on, the batch IS committed: a crash leaves it
     * replayable.  Apply destinations. */
    for (uint32_t i = 0; i < count; i++) {
        if (raw_write(dest[i], data[i]) != 0) {
            /* A destination write failed but the header is on
             * disk — replay on next mount will re-apply.  The
             * caller's data isn't lost; surface the error so
             * the cache can keep the slot dirty in case a
             * retry helps. */
            return -1;
        }
    }

    /* Step 4 — checkpoint.  After this, no replay needed. */
    if (write_zero_header() != 0) {
        /* Header zeroing failed.  Replay will re-apply on next
         * mount — harmless because all destinations are already
         * up to date.  Don't fail the whole commit for this. */
        serial_puts("[osfs2_journal] WARN: checkpoint write failed\n");
    }

    g_commit_count++;
    g_journalled_blocks += count;
    return 0;
}

uint64_t osfs2_journal_commits(void)            { return g_commit_count; }
uint64_t osfs2_journal_replays(void)            { return g_replay_count; }
uint64_t osfs2_journal_blocks_journalled(void)  { return g_journalled_blocks; }
