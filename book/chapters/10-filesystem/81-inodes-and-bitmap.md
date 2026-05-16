# Chapter 81 — Inodes, dirents, and the free-space bitmap

Chapter 80 picked the design.  This chapter writes the code.
By the end of it the running system can:

```sh
$ echo hello-osfs2 > /data/foo
$ cat /data/foo
hello-osfs2
$ ls
       12 /data/foo
$ rm /data/foo
$ ls       # /data/foo is gone
```

…and the bytes survive `Ctrl-C` of QEMU and a fresh boot.
That last property is the point of the entire chapter.  But
to get there we first need three things on disk and three
things in the kernel.  Let's lay them out before pasting code.

## Where on disk

The Makefile now creates and attaches a **second** virtio-blk
device, `build/data.img`, alongside the kernel disk.  This
keeps OSFS-1 untouched — readers of the book can still work
through chapters 21..79b without us silently rewriting the
disk under their feet — and gives OSFS-2 a clean address
space to format however we like.

```make
DATA_DISK := $(BUILD)/data.img

$(DATA_DISK): scripts/mkosfs2.py
	python3 scripts/mkosfs2.py $@

QEMU_BLK := \
    -drive id=hd0,file=$(DISK),if=none,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -drive id=hd1,file=$(DATA_DISK),if=none,format=raw \
    -device virtio-blk-device,drive=hd1
```

Two disks means the virtio-blk driver now needs to talk to
two devices.  Until chapter 81 it was hard-coded to a single
queue, a single state struct, a single MMIO base address.

## The multi-device virtio-blk refactor

`kernel/device/virtio_blk.{h,c}` grew a per-device state
struct and an array of two:

```c
#define VIRTIO_BLK_MAX_DEVS 2

struct blk_dev {
    uintptr_t mmio_base;
    uint64_t  capacity;
    uint8_t  *page;            /* shared queue page */
    uint16_t  avail_idx_seen;
    uint16_t  used_idx_seen;
};

static struct blk_dev g_devs[VIRTIO_BLK_MAX_DEVS];
static int            g_dev_count;
```

A new `virtio_blk_dev_read(int dev, lba, buf)` /
`virtio_blk_dev_write(int dev, lba, buf)` pair takes an
explicit device index.  The legacy single-arg `virtio_blk_read`
and `_write` are preserved as `#define` shims that route to
device 0, so OSFS-1 (`osfs.c`) and the block cache
(`blk_cache.c`) work unchanged.

That's the easy half.  The hard half is the probe order trap.

## The slot-order trap

The virtio-mmio bus on QEMU's `virt` machine is 32 slots
starting at `0xa000000`, stride `0x200`.  Devices appear in
*some* slot — but which slot?  The driver originally walked
`for (s = 0; s < N; s++)`, took the first valid block device
it found, and called it dev 0.  With one disk this works:
QEMU puts the only `-drive` somewhere, the loop finds it,
and we bind it to dev 0.

With two disks we got this:

```
[virtio-blk] found block device at slot 0x1e -> dev0  cap=0x20000  (= 64 MiB)
[virtio-blk] found block device at slot 0x1f -> dev1  cap=0x8000   (= 16 MiB)
mounting OSFS-1 from disk ... [osfs] no OSFS-1 magic on sector 0
```

The 16 MiB image is `disk.img` (OSFS-1).  The 64 MiB image
is `data.img` (OSFS-2).  QEMU placed the **first** `-drive`
(`hd0`, the kernel disk) in the **highest-numbered** slot
(`0x1f`) and the second (`hd1`) in `0x1e`.  Our low-to-high
walk therefore handed dev 0 to OSFS-2 and dev 1 to OSFS-1 —
the opposite of what the rest of the kernel expected.  The
fix is one line:

```c
/* Walk slots HIGH to LOW.  QEMU's `virt` machine assigns the
 * first -drive (hd0) to the highest-numbered virtio-mmio
 * slot, the second (hd1) to the next one down, and so on. */
for (int s = (int)VIRTIO_MMIO_SLOTS - 1; s >= 0; s--) {
    ...
}
```

Now `hd0` lands at dev 0, `hd1` at dev 1, and OSFS-1's "I'm
always on dev 0" assumption holds.

## On-disk layout (recap)

From chapter 80, OSFS-2 uses 4 KiB blocks and lays them out
like this in `data.img`:

```
+-------------------------+ block 0       superblock
+-------------------------+ block 1       block bitmap (one bit per data block)
+-------------------------+ block 2       inode bitmap (one bit per inode)
+-------------------------+ blocks 3..66  inode table (64 blocks * 32 inodes/block = 2048 inodes)
+-------------------------+ blocks 67..   data blocks
```

Constants live in `kernel/core/osfs2.h`:

```c
#define OSFS2_BLOCK_SIZE      4096u
#define OSFS2_SECTORS_PER_BLOCK   8u   /* 8 * 512 = 4096 */
#define OSFS2_INODE_SIZE       128u
#define OSFS2_INODES_PER_BLOCK  32u    /* 4096 / 128 */
#define OSFS2_DIRENT_SIZE       64u
#define OSFS2_DIRENTS_PER_BLOCK 64u    /* 4096 / 64 */
#define OSFS2_DIRECT_PTRS       16u
#define OSFS2_INDIRECT_PTRS   1024u    /* 4096 / 4 */
#define OSFS2_NAME_MAX          60u    /* 64 - 4-byte ino field */
#define OSFS2_INODE_NULL         0u    /* sentinel; never a real file */
#define OSFS2_INODE_ROOT         1u
#define OSFS2_DEVICE             1     /* virtio-blk index */
```

The on-disk types are direct C structs, no padding, no
endian-swapping (the OS only ever runs on the same CPU it
was formatted by):

```c
struct osfs2_superblock {
    char     magic[8];        /* "OSFS-002" */
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t block_bitmap_block;
    uint32_t inode_bitmap_block;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t data_start_block;
    uint32_t root_inode;
    uint32_t reserved[2];
};

struct osfs2_inode {
    uint32_t type;            /* 0=free, 1=file, 2=dir */
    uint32_t size;
    uint32_t link_count;
    uint32_t mtime;
    uint32_t direct[OSFS2_DIRECT_PTRS];
    uint32_t indirect;        /* block# of a 1024-entry array of u32s */
    uint32_t reserved[9];
};

struct osfs2_dirent {
    uint32_t ino;
    char     name[OSFS2_NAME_MAX];
};
```

The maximum file size follows directly from the pointer
counts:

$$\text{MAX\_FILE\_BYTES} = (16 + 1024) \times 4096 = 4{,}259{,}840 \text{ bytes} \approx 4.06\ \text{MiB}$$

Chapter 82 will add a doubly-indirect tier; chapter 83 won't
need one at all.

## Format-time tool

`scripts/mkosfs2.py` formats a fresh `data.img`:

1. Allocate 16384 blocks (= 64 MiB) of zeros.
2. Write the superblock at block 0.
3. Mark blocks 0..66 used in the block bitmap (super, two
   bitmaps, 64 inode-table blocks), plus block 67 (the root
   directory's first data block).
4. Mark inodes 0 (sentinel) and 1 (root) used.
5. Write the root inode: `type=2 (dir)`, `size=0`,
   `direct[0]=67`.

The optional `name=path` arguments seed the image with files,
which we use during host-side smoke tests to prove the read
path works without first having to make the kernel write
anything.

## Driver entry points

`kernel/core/osfs2.c` is around 500 lines and exports just
nine functions:

```c
int  osfs2_init(void);
int  osfs2_present(void);

int  osfs2_lookup(const char *name, uint32_t *ino_out,
                  uint32_t *size_out, uint32_t *type_out);
int  osfs2_create(const char *name, uint32_t *ino_out);
int  osfs2_unlink(const char *name);
long osfs2_read(uint32_t ino, uint64_t off, void *buf, size_t n);
long osfs2_write(uint32_t ino, uint64_t off, const void *buf, size_t n);
int  osfs2_truncate(uint32_t ino, uint64_t new_size);
uint32_t osfs2_size(uint32_t ino);
int  osfs2_listdir(int idx, char *name_out, size_t cap,
                   uint32_t *size_out);
```

`osfs2_init` reads block 0, checks the magic, and caches the
superblock plus both bitmaps in memory — these are tiny
(4 KiB each) and accessing them on every allocation through
the disk would be wasteful even with a cache.

`osfs2_lookup` walks the root directory's data blocks one at
a time, scanning each as a `struct osfs2_dirent[]` array, and
returns the inode number whose name matches.  Allocation and
the indirect tier only show up when files grow past 16 direct
blocks — the rest is reading.

## The bitmap allocator

```c
static int alloc_block(uint32_t *out)
{
    for (uint32_t b = g_sb.data_start_block; b < g_sb.total_blocks; b++) {
        if (!bitmap_get(g_block_bitmap, b)) {
            bitmap_set(g_block_bitmap, b);
            flush_block_bitmap();
            uint8_t zero[OSFS2_BLOCK_SIZE] = { 0 };
            write_block(b, zero);   /* zero on alloc, just like pmem */
            *out = b;
            return 0;
        }
    }
    return -1;
}
```

Identical pattern for inodes, except we skip indices 0 and
1 (sentinel + root).  The flushes after every set/clear make
this driver brutally synchronous — every allocation costs at
least one round-trip to the disk for the bitmap update *and*
one for the new block's zero fill.  That's a chapter 82
problem.

## Indirect-block resolution

```c
/* Walk inode's block-pointer tier(s) and return the disk block
 * number for logical block `lbn`, allocating along the way if
 * `allocate` is set.  Returns 0 (= OSFS2_INODE_NULL, never a
 * valid data block) on read past EOF when not allocating. */
static uint32_t resolve_block(struct osfs2_inode *ino,
                              uint32_t lbn, int allocate)
{
    if (lbn < OSFS2_DIRECT_PTRS) {
        if (ino->direct[lbn] == 0 && allocate)
            alloc_block(&ino->direct[lbn]);
        return ino->direct[lbn];
    }
    uint32_t idx = lbn - OSFS2_DIRECT_PTRS;
    if (idx >= OSFS2_INDIRECT_PTRS) return 0;   /* past indirect tier */
    if (ino->indirect == 0 && allocate)
        alloc_block(&ino->indirect);
    if (ino->indirect == 0) return 0;
    uint32_t tbl[OSFS2_INDIRECT_PTRS];
    read_block(ino->indirect, tbl);
    if (tbl[idx] == 0 && allocate) {
        alloc_block(&tbl[idx]);
        write_block(ino->indirect, tbl);
    }
    return tbl[idx];
}
```

A single direct-tier check, a single indirect-tier check,
and we're done.  No third tier yet.  This is the "shortest
filesystem that can prove it works" — which is exactly what
chapter 81 is for.

## Wiring it into the VFS

The VFS gained a new fd kind:

```c
enum fd_kind {
    FD_FILE, FD_PIPE_R, FD_PIPE_W, FD_CONSOLE, FD_SOCKET,
    FD_PTY_MASTER, FD_PTY_SLAVE,
    FD_OSFS2_FILE,        /* /data/...  */
};

struct fd_entry {
    ... existing fields ...
    uint32_t osfs2_ino;   /* valid when kind == FD_OSFS2_FILE */
};
```

`vfs_open` (and its friend `vfs_open_into` for the
fork-then-exec path) gained a `/data/...` branch:

```c
if (path_starts_with(path, "/data/")) {
    const char *name = path + 6;          /* skip "/data/" */
    uint32_t ino, size, type;
    int rc = osfs2_lookup(name, &ino, &size, &type);
    if (rc < 0) {
        if (!(flags & O_CREAT)) return -ENOENT_VFS;
        rc = osfs2_create(name, &ino);
        if (rc < 0) return rc;
    } else if (flags & O_TRUNC && !(flags & O_APPEND)) {
        osfs2_truncate(ino, 0);
    }
    e->kind      = FD_OSFS2_FILE;
    e->osfs2_ino = ino;
    e->offset    = 0;
    return fd;
}
```

`sys_write` gained a matching branch that calls
`osfs2_write` and advances the fd offset.  `sys_unlink`
dispatches `/tmp/...` to tmpfs and `/data/...` to OSFS-2 in
the same switch.  `vfs_listdir` now enumerates OSFS-2 entries
after OSFS-1 entries (and before tmpfs), so `ls` shows the
union of all three.  Every file in the tree mentioning
`fd_entry` was rebuilt — see the next section for why that
matters.

## The "stale-object" trap that ate a debugging session

We added `osfs2_ino` at the **end** of `struct fd_entry`,
which sits inside `struct thread` as
`fd_entry fds[FD_TABLE_SIZE]`.  Adding 4 bytes to fd_entry
(plus 4 bytes of padding) shifted every field in
`struct thread` after `fds[]` by 8 bytes — including
`as`, the per-process address-space pointer.

Make's old rules tracked only `.c` -> `.o` dependencies, not
`.h` -> `.o`.  After a tree-wide source change but no
`make clean`, the resulting `kernel.elf` had **two**
different layouts of struct thread linked together: some
`.o` files wrote `t->as = real_pointer` at the new offset,
others read `t->as` from the old offset and got 0.

Symptom: every newly-spawned process saw `t->as == NULL`
when calling `sys_sbrk`, so `malloc()` failed on the first
call, but the same process could `write(1, ...)` just fine
(serial console doesn't need an AS).

```
desktop: out of memory for chunk buffer (122880 bytes)
```

Two sessions of debugging later, the fix was twofold:

1. `make clean && make all` — rebuild all the things.
2. Add `-MMD -MP` to CFLAGS and slurp `*.d` files at the
   bottom of the Makefile so future struct-layout changes
   trigger every translation unit that depends on the
   header.  See [Makefile](../../../Makefile).

The `_dbg_capture_boot.sh` script under `scripts/` is the
helper that finally surfaced this: it boots QEMU with
`-serial file:/tmp/raw.log`, kills it after a fixed time,
and `grep`s for the symptoms.  Kept in the tree per our
debug-script policy.

## Smoke test

[scripts/test_osfs2.py](../../../scripts/test_osfs2.py)
boots headless, drives the shell over the serial socket,
and asserts:

1. Right after boot, `ls` shows no `/data/` entries.
2. `echo hello-osfs2 > /data/foo` then `cat /data/foo`
   prints `hello-osfs2`.
3. `ls` now shows `/data/foo`.
4. `rm /data/foo` then `ls` shows it gone.
5. `cat /mnt/wallpaper.bgra > /data/big` writes a file
   that exercises both direct and indirect tiers; `ls`
   confirms the result is at least 1 MiB.

Test 5 conveniently maxes out at exactly
$(16 + 1024) \times 4096 = 4\,259\,840$ bytes — the per-
file cap.  That single number proves both that the
indirect tier works and that the cap is enforced
correctly.

## What chapters 82–84 add

- **82** — A write-back block cache for OSFS-2 (mirroring
  what `blk_cache.c` does for OSFS-1) so the synchronous
  bitmap flushes stop hurting us.  Plus `fsync()`.
- **83** — A small WAL-style journal in front of the FS
  so `mkfs.osfs2` and `unlink` are crash-safe.
- **84** — Migrate the in-memory state of notepad,
  shell history, and (eventually) the browser cookie jar
  into `/data/...`.

## Prerequisites

- [Chapter 80](80-writable-fs-design.md) — design
- [Chapter 11](../05-devices/11-virtio-blk.md) — virtio-blk
- [Chapter 12](../05-devices/12-osfs-mount.md) — OSFS-1
  (the read-only sibling)
