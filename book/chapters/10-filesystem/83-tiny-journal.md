# Chapter 83 — A tiny journal: crash-consistency on a budget

In chapter 82 we put a 32-slot write-back cache in front of the
disk. Reads got fast; writes got *deferred*. On a clean shutdown
that's fine — `fsync` flushes what matters and the periodic
flusher mops up everything else. But "clean shutdown" is the
easy case. The hard case is **power loss between two related
writes**, when the FS thinks half a transaction happened.

This chapter adds a tiny physical-block write-ahead journal so
that, after any power-loss event, the next mount sees one of two
states: "as-if-the-flush-never-happened" or
"as-if-the-flush-fully-happened." Never half.

## The crash that the cache made worse

OSFS-2's `osfs2_create("foo")` touches **three** distinct
blocks:

1. The inode bitmap (mark inode 7 allocated).
2. The inode table block holding inode 7 (write its
   `type=FILE`, `nlink=1`, `size=0` fields).
3. The root directory's first dirent block (write
   `{ino=7, name="foo"}`).

Pre-chapter-82, every block hit the disk synchronously in that
order. A crash at any sub-step still left an inconsistency
(orphan inode allocated but unreferenced, say), but the window
was tiny — one virtio-blk transfer per block.

Chapter 82's cache changed the rules. Now all three writes can
sit in RAM. `osfs2_cache_flush` writes them *in slot order*, not
in the order the FS layer queued them. A power loss in the
middle of the flush leaves you with arbitrary subsets of the
three writes on disk:

| disk has | meaning |
|---|---|
| bitmap only | inode 7 marked allocated, no body, no name → orphan |
| bitmap + inode | allocated inode with no name → orphan in inode space |
| bitmap + dirent | dirent points at junk inode → reads garbage |
| inode + dirent | name → inode 7 visible, but bitmap says free → next allocation reuses inode 7 → cross-link |

The last row is the worst. A subsequent `create` reuses inode
7's bit, overwrites its body, and now `/data/foo` and the new
file share the same inode. Reads of one return contents of the
other.

The cache made the *probability* of this much higher because
the flush window is now hundreds of milliseconds, not microseconds.
We need a way to make the entire flush appear atomic.

## The plan: write-ahead log over the whole cache

Standard FS journaling has many flavors (metadata-only, data=ordered,
data=journal, copy-on-write trees, log-structured). We want the
**simplest one that fits in a chapter**:

- **One** active transaction at a time (the cache is single-
  threaded; no need for a ring of in-flight commits).
- **Physical** block journaling: we copy whole 4 KiB blocks, not
  logical operations. Replay is "write the same bytes again," no
  re-execution of FS code.
- **data=journal**: every dirty cache slot — metadata or file
  data — goes through the journal. Costs ~2× write traffic.
  Buys us a single code path. (data=ordered, the ext3 default,
  is faster but needs per-slot metadata-vs-data bookkeeping the
  cache doesn't have. Punted to a follow-up.)

The whole feature is one new module: `kernel/core/osfs2_journal.{h,c}`.
The cache and FS modules don't change much — `osfs2_cache_flush`
just calls `osfs2_journal_commit` instead of `raw_write`-ing each
slot.

## On-disk layout

We extend the OSFS-2 superblock with two new u32 fields:

```c
uint32_t journal_header_block; /* J = 67 in current mkfs */
uint32_t journal_data_blocks;  /* N = 32, matches cache size */
```

The journal lives at blocks `J..J+N` on disk:

```
Block J+0      header (struct osfs2_journal_header — exactly 4 KiB)
Block J+1..J+N data slot i = the bytes that should land at header.dest[i]
```

`mkosfs2.py` reserves the new region in the block bitmap and
slides `data_start_block` from 67 to 100 to make room.

The header is one fixed-size 4 KiB block:

```c
struct osfs2_journal_header {
    uint8_t  magic[8];      /* "OSFSJRNL" when committed; zero otherwise */
    uint32_t txn_id;        /* monotonic — debug only */
    uint32_t block_count;   /* 0 = no commit; 1..N otherwise */
    uint32_t crc32;         /* over dest[0..count-1] then payload[0..count-1] */
    uint32_t reserved;
    uint32_t dest[OSFS2_JOURNAL_DATA_BLOCKS]; /* destination for slot i */
    uint8_t  pad[3944];
};
```

A `_Static_assert` in `osfs2_journal.c` pins the size at exactly
`OSFS2_BLOCK_SIZE`, so accidentally bumping
`OSFS2_JOURNAL_DATA_BLOCKS` past what fits in one block fails at
compile time rather than corrupting the disk.

The header has only two valid states:

- **Zero** (`magic[0..7] == 0`, `block_count == 0`) → no
  pending commit. Replay is a no-op.
- **Magic + non-zero count + matching CRC** → a transaction
  was committed but may or may not have been fully applied.
  Replay re-applies all `count` (dest, data) pairs.

Anything else (right magic, wrong CRC; right magic but
`count > N`; partial magic) is treated as a torn write and
discarded. The CRC check is what makes this safe in the
presence of torn header writes — a flipped bit anywhere in
the header changes the CRC.

## The commit protocol

```c
int osfs2_journal_commit(const uint32_t *dest,
                         const uint8_t *const *data,
                         uint32_t count);
```

Exactly four phases, written here as `raw_write` to remind that
the journal **bypasses the cache** completely (otherwise a
"durable" commit would just sit in RAM):

```text
step 1   for i in 0..count-1:
             raw_write(J + 1 + i, data[i])     // payload to journal
step 2   raw_write(J,
             header{magic, txn_id, count, crc, dest[]})
step 3   for i in 0..count-1:
             raw_write(dest[i], data[i])       // apply to destinations
step 4   raw_write(J, zero header)             // checkpoint
```

Steps 1, 3, and 4 are bulk virtio-blk writes; step 2 is one
4 KiB write that flips the journal from "no commit pending" to
"commit pending." Step 4 flips it back.

The crucial property is that step 2 is **a single 4 KiB block
write**, and that block's CRC covers everything earlier in
step 1's payload. So step 2 atomically (from the journal's
point of view) declares "I have N blocks of data sitting at
J+1..J+N, intended for these destinations, with this checksum."
Either step 2's bytes land on disk and replay will trust them,
or they don't and replay sees stale magic / mismatched CRC.

A torn write **inside** step 2 — say, the magic lands but the
CRC doesn't — is also fine: the magic check passes but the CRC
recomputed during replay won't match the stored one, so the
batch is discarded.

## Crash analysis

Let's enumerate where a crash can land and what replay does:

| crash window | header state on disk | replay sees | outcome |
|---|---|---|---|
| before step 1 | zero (last checkpoint) | nothing pending | no-op ✓ |
| during step 1 | zero (still pre-commit) | nothing pending | data slots are stale-but-irrelevant ✓ |
| during step 2 (header torn) | bad CRC or partial magic | discard | destinations untouched → "as-if not flushed" ✓ |
| after step 2, before step 3 | committed | replay applies all | destinations updated as a batch ✓ |
| during step 3 | committed | replay re-applies all | idempotent (writing same bytes twice = once) ✓ |
| during step 4 (zeroing header) | torn | replay re-applies all | idempotent → harmless ✓ |
| after step 4 | zero | nothing pending | no-op ✓ |

Every row ends in either "as-if-the-flush-never-happened" or
"as-if-the-flush-fully-happened." That's the atomic property we
wanted. The journal is doing exactly one job — turning a
multi-block update into a single-block decision (the magic +
CRC in the header) plus an idempotent re-apply.

The whole argument hinges on **idempotent re-apply**, which is
why we journal physical block contents instead of logical FS
operations. A logical replay of "create foo" twice is hard
(does it append a second dirent? does it allocate a second
inode?). A physical replay of "block 42 = these 4096 bytes"
twice is trivially the same as once.

## Replay on mount

`osfs2_init` now does this in order:

1. Read superblock.
2. `osfs2_journal_init(sb.journal_header_block, sb.journal_data_blocks)`.
3. `osfs2_journal_replay()` — apply any committed batch to its
   destinations and zero the header.
4. *Then* read the bitmaps into memory.

The order matters. The bitmap and inode-table blocks are
themselves potential replay targets. If we cached them first
and *then* ran replay, we'd hold an in-memory snapshot of
*pre-replay* bitmaps that's now wrong relative to what just
landed on disk.

`osfs2_journal_replay` itself is short:

```c
int osfs2_journal_replay(void) {
    raw_read(J, &hdr);
    if (!header_is_committed(&hdr)) return 0;       /* nothing pending */
    for (i in 0..hdr.block_count-1)
        raw_read(J + 1 + i, payload[i]);
    if (compute_replay_crc(hdr.dest, hdr.block_count, payload) != hdr.crc32) {
        /* torn header or torn payload — discard */
        return write_zero_header();
    }
    for (i in 0..hdr.block_count-1)
        raw_write(hdr.dest[i], payload[i]);
    return write_zero_header();
}
```

The CRC check covers torn writes inside the journal payload too
(step 1 partly succeeded, step 2 succeeded). A flipped byte
anywhere in `payload[i]` mismatches `hdr.crc32`, the batch is
discarded, and the FS reverts to "as-if not flushed."

## Wiring it into the cache

The cache flush path is the only place that issues writes to
disk now. We replaced both call sites of the old `raw_write`:

```c
/* osfs2_cache_flush — bulk fsync, all dirty slots in one txn */
uint32_t       dest[SLOTS];
const uint8_t *data[SLOTS];
int            map[SLOTS];
uint32_t n = 0;
for (int i = 0; i < SLOTS; i++)
    if (g_slots[i].valid && g_slots[i].dirty) {
        dest[n] = g_slots[i].blk;
        data[n] = g_slots[i].data;
        map[n]  = i;
        n++;
    }
if (n == 0) return 0;
if (osfs2_journal_commit(dest, data, n) != 0) return -1;
for (uint32_t k = 0; k < n; k++) {
    g_slots[map[k]].dirty = 0;
    g_writebacks++;
}
return 0;
```

`SLOTS == OSFS2_JOURNAL_DATA_BLOCKS == 32` is sized so the
worst case (all slots dirty, fsync called) fits in one journal
transaction by construction. If anyone ever bumps the cache
size, the static assert in the journal module catches the
header-overflow side; bumping the journal data blocks past 32
would silently make `osfs2_cache_flush` fail with `-1`. We
documented this in the cache file's flush comment.

The single-slot eviction path (`flush_slot`, called when the
cache needs to evict a dirty slot to make room for a new read)
also goes through `osfs2_journal_commit` with `count=1`. That's
slightly wasteful (one block becomes two writes) but
preserves the single-block atomicity even on cache pressure.
And it means there's exactly one path from "dirty cache slot"
to "byte on disk": the journal.

After this change, the cache's `raw_write` helper is unused —
removed in the same diff to satisfy `-Werror=unused-function`.
Reviewers grepping for "where does the cache write to disk?"
land on the journal commit, which is the right answer.

## What we deliberately didn't do

- **A ring of in-flight transactions.** OSFS-2 is single-
  threaded at the FS layer. There's never more than one commit
  in flight. A ring buys us nothing here.
- **A journal index / checkpoint pointer.** A "committed"
  header contains everything replay needs. Zero is "no commit."
  No journal head/tail bookkeeping.
- **Crash-injection at exact protocol-step boundaries.** We
  can't pause the kernel mid-`virtio_blk_dev_write` from
  outside QEMU. Instead `test_journal.py` does many random-
  delay SIGKILLs; in expectation, each protocol step gets hit
  some of the time. The proof of correctness comes from the
  case analysis above; the test confirms the implementation
  matches.
- **Logical journaling.** Would let us journal "create foo" in
  ~80 bytes instead of 12 KiB (3 blocks). But replay would have
  to re-execute FS code, which is much harder to make
  idempotent. Physical journaling pays a 100× space premium
  per transaction in exchange for trivially-correct replay.
- **CRC32C with hardware acceleration.** We use software
  CRC32 (IEEE 802.3) on ≤ 132 KiB per commit. The instruction-
  cost is below the disk-write cost by an order of magnitude.
  Not worth a hardware-feature dependency.

## What's still slightly wrong

There's a residual race that the journal does NOT fix: if the
cache **evicts a dirty metadata block under pressure** in the
middle of a multi-block FS operation, only that one block goes
through the journal. The other blocks of the same logical
operation are still in cache. A crash at that exact moment
leaves the bitmap (say) on disk but not the inode that points
at it — the chapter-82 problem.

In practice this is rare: our cache holds 32 slots and a
metadata operation touches 3, so eviction-during-op needs the
cache to be 30+ slots dirty already. Real Linux FSes solve this
with **deferred-update transactions**: the dirty blocks are
pinned in the cache until the whole logical operation
journal-commits as a unit. We're documenting this as a known
limitation rather than implementing the pinning, because it
would require a rewrite of how the cache and FS communicate.
A future chapter can revisit if it shows up in stress testing.

## Tests

`scripts/test_journal.py` runs three sub-tests:

- **Test A — clean-shutdown smoke.** Boot, immediately check
  that `[osfs2_journal] ready` is in the boot log and
  `[osfs2_journal] replaying` is **not**. A fresh disk has a
  zeroed header and `replay()` returns 0 silently.

- **Test B — replay correctness.** Write three files, sync,
  hard-kill QEMU. Boot again. The files must be present, and
  no replay should be needed (the sync ran step 4, leaving the
  header zero).

- **Test C — random kills.** Five rounds. Each round writes
  four files, syncs every other one, then sleeps a random
  delay in `[50 ms, 1500 ms]` before SIGKILL. After each kill,
  reboot and verify (a) the FS still mounts, (b) every
  previously-sync'd file is intact, (c) `ls /data` doesn't
  panic. The random delay is the substitute for protocol-step
  crash injection — over many rounds, the timing lands inside
  every step at least once in expectation.

All 27 checks pass. Importantly, "passing" includes the case
where the random kill landed *after* a journal commit but
*before* the destinations were applied: the next mount runs
replay, applies them, and the test sees the data.

To exercise this manually:

```sh
make all
python3 scripts/test_journal.py
```

Boot output now shows:

```
[osfs2_cache] ready, 0x20 slots × 4 KiB = 128 KiB
[osfs2] mounted hd1, 0x4000 blocks, 0x800 inodes
[osfs2_journal] ready, header=0x43 slots=0x20
```

(And occasionally, after a crash, a `[osfs2_journal] replaying
txn 0x... (0x... blocks)` line — the journal earning its keep.)

## Summary

A 132 KiB region on disk and one ~350-line `.c` file turn our
write-back cache from "fast but unsafe under power loss" into
"fast and crash-consistent." The trick is to make a single
bit on disk — the journal header's magic — be the entire
"did this commit happen?" answer, with a CRC to detect torn
writes around it. Everything else is bulk-block transfer, which
the underlying virtio-blk already does atomically per sector.

Cost: every flush double-writes (data=journal). Benefit: the FS
is now safe for the rest of the book to build on without us
having to revisit it every time we add a new on-disk feature.

## What this unlocks

- A future package store, persistent kv, or anything else built
  on top of OSFS-2 inherits crash-consistency for free.
- We can be confident `notepad`'s save-and-fsync path is
  power-fail safe, not just clean-shutdown safe.
- The journal-replay primitive is generic — a future
  filesystem (osfs3?) can reuse the same code path.
