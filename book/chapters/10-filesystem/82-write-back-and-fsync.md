# Chapter 82 — Write-back, fsync, and the durability gap

Chapter 81 made OSFS-2 writable.  Every `echo hi > /data/x`
issues fourteen 4 KiB block I/Os, most of them rewriting the
same handful of blocks (the inode-table block holding inode 1
is read+written three times in a single small write).  At
roughly 8 ms per virtio-blk transaction under HVF that adds
up: an interactive `echo`/`cat` round-trip felt visibly slow,
and writing a multi-megabyte file took longer than copying
the same bytes through `cat`'s pipe stage.

This chapter fixes that with a **write-back block cache** in
front of OSFS-2, and then plugs the durability hole the cache
opens up with two new pieces:

  - `SYS_FSYNC(fd)` — an explicit "make my writes durable
    *right now*" syscall.
  - A periodic background flusher kernel thread that flushes
    every dirty slot every 5 seconds, bounding the worst-case
    data-loss window for badly-behaved (or simply lazy)
    userspace.

By the end of the chapter we'll have a regression test
(`scripts/test_fsync.py`) that boots two QEMU instances
against the *same* `data.img`, hard-kills the first
mid-write, and verifies which writes survive.

## Why a separate cache

We already have `blk_cache` (chapter 23).  Why not extend it?

```
blk_cache               | osfs2_cache
------------------------|------------------------
512 B per slot          | 4 KiB per slot (one OSFS-2 block)
read-only               | read + write-back
hard-wired to device 0  | targets device 1 explicitly
boot-time hot path      | runtime hot path
```

`blk_cache` was designed with a deliberate invariant: *what's
on disk is the source of truth, the cache is just a hint*.
That invariant let chapter 23 keep the eviction code trivial
(throw away the slot, never write back).  Bolting write-back
on top would compromise it — every reader of `blk_cache.c`
would now have to ask "is this slot dirty?" everywhere a
read happens.

A separate cache also lets us reason about device-1 eviction
independently from boot-time ELF reads from device 0, which
matters because OSFS-2 traffic is **bursty** (a `notepad`
save is a few writes, then nothing for minutes) while OSFS-1
traffic is **dense at boot then quiet** (the ELF loader reads
init+sh+libc binaries back to back, then never reads them
again).  Mixing two such different access patterns in one
LRU would have neither end happy.

## The cache module

`kernel/core/osfs2_cache.{h,c}`.  32 slots × 4 KiB = 128 KiB
total.  Per-slot state:

```c
struct slot {
    uint32_t blk;                  /* disk block number    */
    uint8_t  valid;                /* 1 = holds a block    */
    uint8_t  dirty;                /* needs writeback      */
    uint8_t  pad[2];
    uint64_t last_used;            /* clock at last touch  */
    uint8_t  data[OSFS2_BLOCK_SIZE]; /* 4 KiB              */
};

static struct slot g_slots[SLOTS];
static uint64_t    g_clock;        /* monotonic touch ID   */
```

The eviction policy is clock-counter LRU.  `pick_victim`
prefers any invalid slot, then falls back to the slot with
the smallest `last_used`.  No locks — like the rest of OSFS-2
the cache is single-threaded for now.

### Read path

```c
int osfs2_cache_read(uint32_t blk, void *buf)
{
    int idx = find_slot(blk);
    if (idx >= 0) { /* hit */ ... copy out, return 0; }

    int v = pick_victim();
    if (g_slots[v].valid) {
        if (g_slots[v].dirty && flush_slot(v) != 0) return -1;
        g_evictions++;
    }
    if (raw_read(blk, g_slots[v].data) != 0) {
        g_slots[v].valid = 0;
        return -1;
    }
    g_slots[v].blk = blk; g_slots[v].valid = 1; g_slots[v].dirty = 0;
    g_slots[v].last_used = ++g_clock;
    /* copy out */
    return 0;
}
```

Two important points:

  1. **A dirty victim is flushed synchronously before its
     slot is reused.**  Skipping this would silently drop the
     dirty data on the floor and the cache would become a
     reliability liability, not an asset.
  2. **If `raw_read` fails after picking a victim, we mark
     the victim invalid.**  Otherwise the next access would
     find a slot tagged with the new block number but holding
     uninitialized memory, returning garbage to the caller.

### Write path

```c
int osfs2_cache_write(uint32_t blk, const void *buf)
{
    int idx = find_slot(blk);
    if (idx < 0) {
        /* No pre-fill from disk: caller is overwriting the
         * entire 4 KiB block. */
        idx = pick_victim();
        if (g_slots[idx].valid) {
            if (g_slots[idx].dirty && flush_slot(idx) != 0) return -1;
            g_evictions++;
        }
        g_slots[idx].blk = blk; g_slots[idx].valid = 1;
    }
    /* copy in */
    g_slots[idx].dirty = 1;
    g_slots[idx].last_used = ++g_clock;
    return 0;
}
```

The subtle line is "no pre-fill from disk on miss."  OSFS-2's
writers always rewrite a whole 4 KiB block — bitmap RMW
operations, inode-table block RMW operations, data block
writes, and dirent block writes all read-then-write the whole
block via `osfs2_cache_read` first.  By the time the kernel
calls `osfs2_cache_write` the caller already has the merged
contents in hand.  Pre-filling from disk would cost an extra
read of data we're about to overwrite anyway.

This invariant is fragile: if a future OSFS-2 path ever does
a partial-block write through the cache without reading
first, the gap will be filled with garbage from whatever
block previously occupied the victim slot.  The header
comment spells this out.

## Wiring osfs2.c through the cache

`kernel/core/osfs2.c` had two helpers — `read_block` and
`write_block` — that wrapped `virtio_blk_dev_read`/`_write`
with the OSFS-2 block-to-sector translation.  We re-route
them through the cache:

```diff
 static int read_block(uint32_t blk, void *buf)
 {
-    uint64_t lba = (uint64_t)blk * OSFS2_SECTORS_PER_BLOCK;
-    for (uint32_t i = 0; i < OSFS2_SECTORS_PER_BLOCK; i++)
-        if (virtio_blk_dev_read(OSFS2_DEVICE, lba + i, ...) != 0)
-            return -1;
-    return 0;
+    return osfs2_cache_read(blk, buf);
 }

 static int write_block(uint32_t blk, const void *buf)
 {
-    /* …direct virtio writes… */
+    return osfs2_cache_write(blk, buf);
 }
```

Three lines on each side.  No other osfs2.c path was touched
— the whole filesystem is now write-back without anyone
upstairs knowing.

## The durability gap

A write-back cache turns "writes are durable on `close()`"
into a lie.  Until the cache evicts the dirty slot or someone
calls fsync, the data lives only in kernel RAM.  Three things
can lose it:

  1. **Kernel panic.**  RAM goes away, slot was never written.
  2. **Power loss / hard kill.**  Same: RAM goes away.
  3. **Cooperative shutdown that doesn't call sync first.**
     The 'reboot' path in chapter 12 would lose the cache.

POSIX's answer to (1) and (2) is "call fsync if you care."
That puts the *correctness* burden on userspace, which is
the right place: the kernel can't know that 'echo hi >> log'
is a critical audit entry versus 'echo hi > /tmp/x' that's
fine to lose.  But it doesn't help with the long tail of
apps that *don't* call fsync — and we don't want a 30-second
power glitch to lose a notepad save that the user thought
finished a minute ago.

The classic Unix answer: a periodic background flusher.
BSD's `update` daemon called `sync(2)` every 30 seconds in
1980-something for exactly this reason.  Linux's modern
`writeback` kernel threads do the same with more knobs.  We
pick **5 seconds**: short enough that "I just saved" feels
durable on human timescales, long enough that a burst of
edits coalesces into one disk write.

## SYS_FSYNC

`SYS_FSYNC = 35`, dispatched in `kernel/core/syscall.c`:

```c
static long sys_fsync(long fdi)
{
    if (fdi < 0 || fdi >= FD_TABLE_SIZE) return -EBADF;
    struct thread *t = thread_current();
    struct fd_entry *e = &t->fds[fdi];
    if (!e->in_use) return -EBADF;
    if (e->kind != FD_OSFS2_FILE) return 0;
    if (osfs2_fsync(e->osfs2_ino) != 0) return -EIO;
    return 0;
}
```

Two design choices to flag:

  1. **fsync on a non-OSFS-2 fd returns 0**, not -EINVAL.
     Console, ramfs, OSFS-1, tmpfs, pipes, ptys, sockets —
     all of them either don't have durable storage (so
     there's nothing to flush) or have read-only storage
     (so there's nothing dirty).  Returning 0 lets userspace
     call `fsync(fd)` defensively without first asking the
     kernel what kind of fd it is.

  2. **The kernel-side implementation flushes the WHOLE
     cache**, not just the slots backing this file.  This is
     a coarse-grained primitive — fsync(notepad's_fd) waits
     for every dirty OSFS-2 block in the system to hit disk,
     not just the four or five blocks that belong to
     notepad's file.  That's the right trade-off here: tracking
     "which blocks belong to inode N" would require either an
     extra index on the slot table or scanning every dirty
     slot's stored block number against the inode's
     direct+indirect tier on every fsync.  The whole-cache
     flush is also strictly more durable than the per-file
     flush — there's no foot-gun where "I fsync'd my file but
     not its containing directory's dirent block."

The kernel-side helper lives in osfs2.c:

```c
int osfs2_fsync(uint32_t ino)
{
    /* `ino` is validated but otherwise advisory — the cache
     * flush is whole-system.  Callers pass the inode so that
     * if/when osfs2 gets finer-grained tracking the API can
     * grow into it without recompiling userspace. */
    if (ino == 0 || ino >= OSFS2_INODE_COUNT) return -1;
    return osfs2_cache_flush();
}
```

`osfs2_cache_flush` walks every slot and calls
`raw_write(blk, data)` on the dirty ones.  On failure it
leaves the slot dirty so the data isn't silently lost — the
caller can retry later.

The libc wrapper is a one-liner in `userspace/libc/syscall.h`:

```c
static inline int fsync(int fd) { return (int)syscall1(SYS_FSYNC, fd); }
```

## The background flusher

```c
#define OSFS2_FLUSH_INTERVAL_MS 5000ULL

static void osfs2_flush_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(OSFS2_FLUSH_INTERVAL_MS);
        if (osfs2_cache_dirty_count() == 0) continue;
        if (osfs2_cache_flush() != 0)
            serial_puts("[osfs2_cache] background flush hit -EIO\n");
    }
}
```

The check for `dirty_count() == 0` skips the no-op flush —
on an idle system with a clean cache the thread does almost
nothing every 5 seconds, leaving the CPU free for whatever
the user is doing.

### The reaper deadlock

Spawning the flusher was the chapter's only real bug.  We
naively put `thread_create(osfs2_flush_thread, …)` inside
the OSFS-2 mount block:

```c
if (osfs2_init() == 0) {
    osfs2_cache_init();
    thread_create(osfs2_flush_thread, NULL, "osfs2-flush");
}
```

Boot looked normal up to `[busy-B] done`, then hung forever.
The serial console froze on the last newline before the
`userspace_demo` banner that should have printed next.

The cause is in the kernel's own threading demo,
`preemption_demo()`, which has lived in `main.c` since
chapter 11:

```c
static void preemption_demo(void)
{
    thread_create(busy_worker, ..., "busy-A");
    thread_create(busy_worker, ..., "busy-B");

    /* Reap them as they exit.  thread_wait blocks until any
     * child exits, returns -1 once we have no children left. */
    while (thread_wait(NULL) >= 0) { }
}
```

`thread_wait(NULL)` reaps **any** child of the current
thread.  We'd just added a child — the flusher — that
**never exits**.  After busy-A and busy-B were reaped,
`thread_wait` blocked forever waiting for the flusher to die.

The fix is to spawn the flusher **after** the demo's reaper
loop runs:

```c
heap_demo();
preemption_demo();        /* reaps all current children */
osfs2_flusher_start();    /* spawns the long-lived flusher */
userspace_demo();
```

`osfs2_flusher_start` is a tiny gate that checks a flag set
by the mount path:

```c
static int g_osfs2_mounted = 0;

static void osfs2_flusher_start(void)
{
    if (!g_osfs2_mounted) return;
    thread_create(osfs2_flush_thread, NULL, "osfs2-flush");
    serial_puts("[osfs2_cache] background flusher spawned "
                "(every 5 s)\n");
}
```

Lesson: **kernel threads spawned during early init must
either be reapable or be spawned after every reaper has
finished.**  This applies to any future long-lived kernel
thread we add — netd, audio mixer, GC worker — not just the
flusher.

## Making notepad call fsync

The natural app to demonstrate the new durability story is
`notepad`.  Two changes in `userspace/notepad/notepad.c`:

  1. **Default save target.**  Pre-chapter-82 notepad
     defaulted to `/tmp/untitled.txt`, which lives in the
     RAM-backed tmpfs (lost on reboot).  Now we default to
     `/data/untitled.txt` — the writable OSFS-2 mount.

  2. **fsync before close.**  In `save_file`:

     ```c
     for (int r = 0; r < g_line_count; r++) { write(fd, ...); }
     /* Chapter 82 — force the write-back cache to disk before
      * we declare "saved".  Without this, the writes above
      * are only buffered in 4 KiB cache slots; close() does
      * not imply durability. */
     (void)fsync(fd);
     close(fd);
     ```

The user-visible behaviour: hit Ctrl-S in notepad, see
"Saved.", and that means the file really is on disk by the
time the message renders.

## The sync utility

For shell scripts and ad-hoc terminal use we add a tiny
`/bin/sync` binary in `userspace/sync/sync.c`:

```c
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fd = open("/data/.sync", OPEN_WRITE_CREATE);
    if (fd < 0) { printf("sync: open /data/.sync failed (%d)\n", fd); return 1; }
    int rc = fsync(fd);
    close(fd);
    if (rc != 0) { printf("sync: fsync failed (%d)\n", rc); return 1; }
    return 0;
}
```

Because `osfs2_fsync` flushes the whole cache, fsyncing any
OSFS-2 fd is enough to make every previous write durable.
We pick `/data/.sync` (created lazily on first run) so
repeated `sync` invocations don't litter `/data`.

## The regression test

`scripts/test_fsync.py` validates three durability properties
by booting QEMU twice against the same `data.img`:

  - **Test A — fsync persistence.**  Boot, `echo X >
    /data/persist_a; /bin/sync`, **SIGKILL** the VM (no
    graceful shutdown).  Boot again.  `cat /data/persist_a`
    must print X.

  - **Test B — background flusher catches lazy writers.**
    Boot, `echo Y > /data/persist_b` (no sync).  Wait 8
    seconds (>5 s flusher interval).  SIGKILL.  Boot again.
    `cat /data/persist_b` must print Y.

  - **Test C — without sync or flush window, writes are
    lost.**  Boot, `echo Z > /data/lost_c`, SIGKILL
    *immediately* (well under 5 s, no flusher run).  Boot
    again.  `cat /data/lost_c` must NOT print Z.

Test C is the negative control.  If it fails, either the
cache is accidentally write-through (defeating the chapter's
point) or fsync is being called somewhere we didn't intend.
Without it, Test A could pass for the wrong reason.

We use `SIGKILL` instead of `terminate()` so QEMU has zero
chance to drain anything on its way out — the closest we can
get to "yank the power cord" from a host-side test.

All three pass.

## What this chapter unlocks

- Editor save-to-disk that survives reboot (notepad).
- Shell history (`~/.history`) becomes feasible.
- The journal in chapter 83 will wrap the same fsync /
  cache-flush primitives — it's the *order* of those flushes
  that makes the difference between "consistent on crash"
  and "consistent only after a clean shutdown."

## What this chapter does NOT solve

Crash-consistency.  If we panic between bitmap-write and
data-block-write — or between data-block-write and
inode-update-write — the on-disk state is internally
inconsistent.  The bitmap claims a block is allocated that
the inode doesn't point to (best case, leaked block), or
the inode points to a block the bitmap thinks is free (worst
case, double-allocation on the next mount).

The background flusher actually makes this *worse* than a
write-through cache would: it groups writes by time-of-dirty,
not by causal ordering, so the order in which the writeback
issues `raw_write`s has no relation to the order the kernel
issued `osfs2_cache_write`s in.

Chapter 83 fixes this with a tiny write-ahead journal in a
reserved region of `data.img`.  Until then OSFS-2's
correctness story is: "if you fsync, your data is durable;
if you crash, the filesystem may need rebuilding."  Good
enough to write a book chapter against; not good enough to
deploy.
