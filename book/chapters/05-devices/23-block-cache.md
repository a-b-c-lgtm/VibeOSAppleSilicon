# Chapter 23 — A block cache in front of virtio-blk

## The problem

After chapter 22 every user binary lives on disk. `vfs_load("/bin/cat")`
calls `osfs_read`, which calls `virtio_blk_read` once per 512-byte
sector the file occupies. For our four binaries:

| binary | file size | sectors | virtio reads to spawn once |
|--------|----------:|--------:|---------------------------:|
| init   |     5352  |      11 |                         11 |
| sh     |     5720  |      12 |                         12 |
| cat    |     5032  |      10 |                         10 |
| hello  |     4656  |      10 |                         10 |

A polled virtio request is not free. The driver builds a
descriptor chain, kicks the device by writing to a memory-mapped
notify register, and spins in a loop reading the used-ring index
until the device increments it. On HVF in QEMU this is fast — tens
of microseconds — but doing 10–12 of them every time the shell
spawns a program is needlessly wasteful, especially when the
*next* time you type `/bin/cat` we know we're going to ask for
exactly the same sectors.

A block cache fixes that. The interface is dead simple: instead of
`virtio_blk_read(lba, buf)` directly, callers use
`blk_cache_read(lba, buf)`, which:

- on a **hit**, copies from an in-memory slot into `buf` and is done
- on a **miss**, evicts the least-recently-used slot, calls
  `virtio_blk_read` to fill it, copies into `buf`, and is done

That's the entire story.

## Sizing the cache

Tuning is a tradeoff between memory footprint and hit rate. Our
working set today, after a fresh boot through one shell session,
is:

```
osfs metadata          2 sectors
/bin/init             11 sectors
/bin/hello            10 sectors
/bin/cat              10 sectors
/bin/sh               12 sectors
/mnt/hello.txt         1 sector
                      --
total unique          46 sectors  =  23 KiB
```

So 32 KiB (64 slots) holds the entire warm set with room to spare.
That's the chosen size:

```c
#define SLOTS  64
#define SECTOR 512u

struct slot {
    uint64_t lba;          /* sector number when valid */
    uint64_t last_used;    /* clock counter at most-recent access */
    uint8_t  valid;
    uint8_t  pad[7];
    uint8_t  data[SECTOR];
};
static struct slot g_slots[SLOTS];
```

64 slots × `sizeof(struct slot)` = 32 KiB and change of `.bss`. On
a hobby kernel that's negligible. On a real OS this number is
tunable; Linux's page cache is "all RAM that's not otherwise
spoken for."

## LRU without a doubly linked list

You can implement LRU correctly with O(1) updates using a
hash-indexed doubly linked list. We don't need that. With 64
slots, a linear scan is 64 pointer comparisons per call — well
under the cost of any actual disk read it might trigger. So:

```c
static uint64_t g_clock;

static int find_slot(uint64_t lba) {
    for (int i = 0; i < SLOTS; i++)
        if (g_slots[i].valid && g_slots[i].lba == lba) return i;
    return -1;
}

static int pick_victim(void) {
    int victim = 0;
    uint64_t oldest = (uint64_t)-1;
    for (int i = 0; i < SLOTS; i++) {
        if (!g_slots[i].valid) return i;          /* prefer empty */
        if (g_slots[i].last_used < oldest) {
            oldest = g_slots[i].last_used;
            victim = i;
        }
    }
    return victim;
}

int blk_cache_read(uint64_t lba, void *buf) {
    int idx = find_slot(lba);
    if (idx >= 0) {
        g_slots[idx].last_used = ++g_clock;
        memcpy(buf, g_slots[idx].data, SECTOR);
        g_hits++;
        return 0;
    }
    int v = pick_victim();
    if (g_slots[v].valid) g_evictions++;
    if (virtio_blk_read(lba, g_slots[v].data) != 0) {
        g_slots[v].valid = 0;   /* don't install garbage */
        return -1;
    }
    g_slots[v].lba       = lba;
    g_slots[v].valid     = 1;
    g_slots[v].last_used = ++g_clock;
    memcpy(buf, g_slots[v].data, SECTOR);
    g_misses++;
    return 0;
}
```

The clock counter is a `uint64_t`. At one increment per disk read,
it overflows after 2^64 reads — never, in our lifetime.

## What the cache does NOT yet handle

- **Write-back.** No writes go through the cache because no caller
  writes through OSFS. The day we add a writable filesystem, we
  add a `dirty` flag (already in the struct, just unused) and a
  `blk_cache_flush()` that walks slots and writes back dirties via
  `virtio_blk_write`.
- **Write-through with a write path that bypasses the cache.** If
  some new code starts calling `virtio_blk_write` directly without
  going through the cache, the cache will silently serve stale
  data. The escape hatch is `blk_cache_invalidate(lba)`. We
  expose it but don't currently need it.
- **Concurrent access.** `blk_cache_read` is not reentrant safe.
  Today only one thread runs disk I/O at a time (boot thread or
  whichever user thread happens to be in `osfs_read` via
  syscall), so this is fine. When async I/O lands or per-CPU
  threads start hitting the cache, this needs a lock.
- **Read-ahead.** `osfs_read` of an N-sector file does N
  back-to-back single-sector reads. A real implementation would
  notice the sequential pattern and prefetch the next few
  sectors. We don't.

## Verification

Stats accessors:

```c
uint64_t blk_cache_hits(void);
uint64_t blk_cache_misses(void);
uint64_t blk_cache_evictions(void);
void     blk_cache_dump_stats(const char *prefix);
```

The boot-time trace dumps stats once after init exits.

Run a workload that re-spawns `/bin/cat` four times and `/bin/hello`
twice:

```sh
$ printf '/bin/cat /mnt/hello.txt\n/bin/cat /mnt/hello.txt\n
/bin/cat /mnt/hello.txt\n/bin/cat /mnt/hello.txt\n
/bin/hello\n/bin/hello\nexit\n' | make run
```

Result:

```
[blk_cache] ready, 64 slots × 512B = 32 KiB
... boot, init, sh, four cat repeats, two hello repeats ...
[blk_cache] hits=63 misses=46 evictions=0
```

That's the picture we want:

- **46 misses = exactly the count of unique sectors touched.** Every
  miss is a *cold* miss; nothing is ever read from disk twice.
  (Boot loads init, hello, cat, sh = 43 sectors; osfs metadata = 2;
  hello.txt = 1.  46 total.)
- **0 evictions.** The cache never had to throw anything out. Our
  working set fits.
- **63 hits.** All warm. These are the second through fourth
  invocations of `/bin/cat`, the second invocation of `/bin/hello`,
  and the repeated reads of the OSFS directory metadata.

Effective hit rate **63 / (63 + 46) = 58%** across the whole
session — but that's the wrong way to read it. The interesting
number is the steady-state rate, which is: every disk read after
the cold-load phase. For our workload that's **~100%**.

## What this changes for everything downstream

Now that disk reads are cheap on the warm path, we can stop
worrying about disk I/O cost in higher-level code. Specifically:

- ELF loaders can do as many small reads as they want; no need
  to slurp the whole file at once.
- A future `vfs_pread(fd, buf, len, off)` is straightforward: walk
  the affected sectors, `blk_cache_read` each, copy out the
  requested ranges. No more whole-file `kmalloc`.
- An interactive process re-reading the same file has zero
  per-syscall disk cost after the first read.
- A reader that processes a file sequentially (cat, less,
  grep…) will still pay one cold miss per sector, but never
  more.

The next chapter (24) will be **virtio-console**, which gives us a
proper console transport on the same virtio bus, freeing the
PL011 UART for debug-only use and clearing the way for multiple
consoles when the GUI lands.
