# Chapter 80 — Why we need a writable filesystem

OSFS-1 is read-only by design.  The kernel mounts the disk at
`/mnt/`, the directory and file table are baked in by
`scripts/mkosfs.py` at build time, and there is no kernel code
path that ever calls `virtio_blk_write`.  Any file the running
system creates lives in tmpfs (`/tmp/`), which evaporates at
the next reboot.

That has been fine for chapters 21–79b.  The system could be
treated as an immutable image: the kernel boots, mounts read-
only state, and runs.  Persistence meant `make` rebuilt the
disk image — the system itself could not modify what was on
disk.  But every chapter in Part X is about *the running
system being able to change its own future*:

- Notepad's "save" button writes to tmpfs and disappears at
  reboot — the user cannot keep notes.
- The shell's `up-arrow` history is in-memory only — every
  reboot starts from an empty buffer.
- A future browser cookie jar would need to survive reboot
  for any login flow to work.
- A future package manager could not exist at all, because
  there is nowhere to install anything.

This chapter does not implement any of that.  It is the
**design document** for the second on-disk filesystem,
nicknamed **OSFS-2**.  Chapters 81–84 implement it
incrementally.

## What "writable" forces us to think about

Adding writes is not just adding a `write()` path next to a
`read()` path.  Three new concerns appear that OSFS-1
sidestepped entirely:

1. **Allocation.**  When a user `write()`s past the end of a
   file, the FS must find a free block on disk and remember
   that the block is now in use.  OSFS-1 has no free-space
   map because every byte is placed at build time; OSFS-2
   needs one.
2. **Crash consistency.**  A power loss in the middle of an
   `unlink` could leave the directory entry gone but the
   blocks still marked allocated, leaking space forever.  Or
   the entry still present but pointing at a half-truncated
   inode, which would corrupt the next `cat`.  We need a
   plan — even a deliberately weak one — for what an `fsck`
   has to do after a crash.
3. **Deletion semantics.**  POSIX requires that an unlinked-
   but-still-open file remains accessible to the holder until
   the last `close()`.  OSFS-1 has no `unlink` path so the
   question never arose.

Each of these maps directly to one of chapters 81–83:

| Chapter | Concern | What it adds |
|---|---|---|
| 81 | Allocation | Inodes, dirents, free-block bitmap, the actual on-disk format |
| 82 | Performance + reliability | Block cache + write-back + `fsync` |
| 83 | Crash consistency | A small journal in front of the FS |
| 84 | Apps actually use it | Notepad, shell history, browser cookies move to OSFS-2 |

## Trade-off survey

There are three families of writable FS designs to pick from.

### A. ext2-shaped (inodes + bitmaps + block table)

The textbook layout: a superblock, a bitmap of free blocks, a
table of inodes, and a sea of data blocks.  Each file is an
inode plus a list of block pointers.  Directories are special
files whose contents are an array of `(inode_num, name)`
records.

Pros:
- Matches every reader's mental model of "what a filesystem
  looks like."
- Independent allocation of metadata vs data — directory
  growth doesn't touch unrelated files.
- Trivially extends with a journal in front (chapter 83).

Cons:
- Two on-disk authorities to keep in sync (the bitmap and
  the inode table).  Crash consistency requires either
  ordering writes carefully or shipping a journal.
- Random in-place writes mean every flush is a scatter of
  small disk operations.

### B. Log-structured (LFS)

Write everything to the end of the disk — never overwrite.
Read the latest version of an inode by walking a single
forward-only log; periodically compact.

Pros:
- Trivially crash-consistent: a torn write at the head of
  the log just truncates back to the last good record on
  recovery.
- Writes are sequential — fast on spinning rust, kind to
  flash erase blocks.

Cons:
- Reads are slow without a separate index structure (which
  reintroduces the consistency problem we were trying to
  avoid).
- Compaction is a whole second filesystem of its own and
  has subtle real-time guarantees we don't want to teach.
- Mental-model overhead: readers have to learn about
  segments, generation counters, and inode-number ⇄
  inode-position indirection before anything works.

### C. Copy-on-write trees (btrfs / zfs in miniature)

Every change writes a new copy of the modified blocks AND
every parent block up to a single root pointer.  The atomic
update is the swap of that root pointer.

Pros:
- Atomic snapshots are essentially free.
- Crash-consistent without a journal.

Cons:
- Tree traversal on every read.
- Garbage collection of the old copies needs reference
  counts or a mark-sweep — neither is "tiny."
- Asks the reader to understand persistent data structures
  before the FS does anything useful.

### Decision: OSFS-2 is ext2-shaped

For a teaching kernel:
- **Familiarity wins.**  If you have ever read any FS source
  before, OSFS-2's layout will look like a stripped-down
  version of it.
- **The journal is additive.**  Chapter 83 bolts a small
  journal in front of OSFS-2 without redesigning the on-
  disk format.  Try doing that retroactively to LFS or CoW.
- **Allocation is the lesson.**  Spending chapter 81 on
  bitmaps and inode tables teaches the things every
  filesystem reader will eventually need to know, instead
  of teaching mechanics specific to one design.

## On-disk layout

OSFS-2 uses **4 KiB blocks** (eight 512-byte virtio sectors
each).  This matches our page size and lets us reason about
"a block" and "a page" interchangeably for cache and DMA
purposes once chapter 89's mmap shows up.

For a 64 MiB image (16 384 blocks):

| Block range | Contents | Size |
|---|---|---|
| 0 | Superblock | 1 block |
| 1 | Block bitmap | 1 block (32 768 bits = enough for 128 MiB) |
| 2 | Inode bitmap | 1 block (32 768 bits, but we only allocate 2048 inodes) |
| 3 .. 66 | Inode table | 64 blocks × 32 inodes/block × 128-byte inode = 2048 inodes |
| 67 .. 16383 | Data blocks | ~16 317 blocks ≈ 64 MiB |

```
+---------+----------+----------+----------+----------+----------+
| Block 0 | Block 1  | Block 2  | Block 3  | …  Block 66 | Blocks 67…|
+---------+----------+----------+----------+----------+----------+
| Super-  | Block    | Inode    | Inode    | Inode    | Data     |
| block   | bitmap   | bitmap   | table    | table    | blocks   |
+---------+----------+----------+----------+----------+----------+
```

### Superblock (block 0)

```
struct osfs2_sb {
    char     magic[8];              // "OSFS-002"
    uint32_t block_size;            // 4096
    uint32_t total_blocks;          // 16384 for the 64 MiB image
    uint32_t inode_count;           // 2048
    uint32_t block_bitmap_block;    // 1
    uint32_t inode_bitmap_block;    // 2
    uint32_t inode_table_block;     // 3
    uint32_t inode_table_blocks;    // 64
    uint32_t data_start_block;      // 67
    uint32_t root_inode;            // 1
    /* rest of the block is zero */
};
```

The superblock is the contract between `mkosfs2.py` and the
kernel driver.  Once we have a journal (chapter 83) it grows
two more fields for the journal head/tail.

### Inode (128 bytes)

```
struct osfs2_inode {
    uint32_t type;          // 0=free, 1=file, 2=dir
    uint32_t size;          // bytes
    uint32_t nlink;         // hard link count (always 1 in chapter 81)
    uint32_t mode;          // permission bits, cosmetic for now
    uint32_t ctime_ms;      // creation time (uptime ms; chapter 91 will use real time)
    uint32_t mtime_ms;      // modification time
    uint32_t direct[16];    // 16 × 4 KiB = 64 KiB direct mapping
    uint32_t indirect;      // single indirect block → 1024 × 4 KiB = 4 MiB more
    uint8_t  reserved[36];  // pad to 128 bytes
};
```

Math: $16 \times 4096 + 1024 \times 4096 = 4{,}259{,}840$
bytes ≈ 4 MiB max file size.  A double-indirect block would
push that to 4 GiB but the disk is only 64 MiB; not worth the
extra code.

Inode 0 is reserved (the "null inode" sentinel).  Inode 1 is
the root directory.  Inodes 2.. are user files.

### Directory entry (64 bytes)

```
struct osfs2_dirent {
    uint32_t ino;           // 0 = empty slot
    char     name[60];      // NUL-padded, no path separators
};
```

A directory's data blocks contain a packed array of these
records.  Lookup is linear scan.  In chapter 81 the root
directory is one block (64 entries max); subdirectories are
not implemented.  Chapter 82 grows the root to multiple
blocks transparently.

## Mount strategy

OSFS-2 lives on a **second virtio-blk device** (`build/data.img`),
exposed to QEMU as `hd1`.  That keeps OSFS-1 working unchanged
on `hd0` and lets readers compare the two formats side by
side in the source tree.

OSFS-1 stays as the kernel's binary store: `/bin/<name>` and
`/mnt/<name>` continue to read from the OSFS-1 disk at boot.
The `osfs.{c,h}` files stay in the tree for the rest of the
book — they are the *simpler* example of an on-disk
filesystem and the chapter 21 narrative continues to refer to
them.

OSFS-2 mounts at **`/data/`** alongside `/mnt/` and `/tmp/`.
Notepad, shell history persistence, and browser cookies will
all be migrated to `/data/` in chapter 84.  Long-term plan:
once we trust OSFS-2 (post-journal), all userland data
eventually moves there and `/mnt/` becomes the equivalent of
a read-only `/usr` partition.

## What `fsck` will eventually have to do

This is a useful lens for evaluating any FS design.  Even
though chapter 81 won't ship an fsck, knowing what it would
have to do tells us which invariants the filesystem must
maintain on the happy path.

After a crash, an OSFS-2 fsck has to:

1. **Reconcile the block bitmap with the inode pointers.**
   For every inode marked used, walk its direct + indirect
   pointers and assert each pointed-to block is marked
   allocated in the bitmap.  Free any leaked blocks.
2. **Reconcile the inode bitmap with the directory tree.**
   Walk every directory; for every dirent, assert the
   referenced inode is marked used.  Free orphaned inodes.
3. **Validate sizes.**  If `inode->size = 5000` then
   `direct[0]` and `direct[1]` should be allocated, and
   `direct[2]..[15]` should be 0.  Any contradiction
   indicates a torn write.
4. **Validate the indirect block.**  If `inode->indirect != 0`
   then it must itself be a valid 4 KiB block of `uint32_t`
   block pointers, each of which is either 0 or a valid
   in-range data block.

The "happy-path invariants" we have to preserve to make
that fsck possible are:

- Bitmap entry for a block is set ⇔ that block is reachable
  from some inode.
- Inode bitmap entry is set ⇔ that inode is reachable from
  the root directory tree.
- An inode's `size` field is consistent with which `direct`
  / `indirect` entries are non-zero.

Chapter 82 shows that "consistent" is hard to maintain
across a crash without a journal.  Chapter 83 ships the
journal.

## What this chapter unlocks

Nothing yet — this chapter is design only.  Chapter 81
ships the format and the read+write driver; running code
appears there.

## What you'll learn

- Why every textbook filesystem looks structurally similar:
  the trade-off survey above keeps converging on the same
  shape because the constraints (allocation, crash safety,
  deletion) are universal.
- The "what does fsck have to do after a crash" framing:
  the right way to evaluate an FS design before writing
  a single line of it.
- Why the journal can be left for chapter 83 instead of
  designed into the format from day one.

## Prerequisites

- Chapter 21 — OSFS-1 read-only mount.
- Chapter 23 — block cache (in front of virtio-blk).
- Chapter 20 — virtio-blk driver.

## What chapters 81–84 will deliver

- **Chapter 81.**  `mkosfs2.py` host tool.  Second virtio-
  blk device wired into the kernel and every test harness.
  `kernel/core/osfs2.{c,h}` driver — synchronous,
  uncached, full read+create+write+truncate+unlink.
  `/data/<name>` paths in the VFS.  `echo hi > /data/foo`
  works; reboots forget the file because writes are
  in-memory until flushed.
- **Chapter 82.**  A 4 KiB-block writeback cache in front of
  the virtio-blk device.  `fsync()` syscall.  Dirty inode
  tracking.  Reboots remember files; crashes still corrupt.
- **Chapter 83.**  A tiny circular journal in front of every
  metadata write.  Crash recovery on mount.  Now reboots
  AND mid-write power-cycles preserve invariants.
- **Chapter 84.**  Notepad saves to `/data/note.txt`.
  Shell history persists in `/data/.sh_history`.  Browser
  cookies (chapter 82-of-the-future) target `/data/cookies`.
  The "running system can change its own future" promise
  is finally kept.
