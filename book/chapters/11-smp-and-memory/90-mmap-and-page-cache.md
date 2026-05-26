# Chapter 90 — mmap and a unified page cache

Three things this kernel cannot do, that almost every userspace
program out in the world expects:

- **Map a file into memory and read it as a string.** Today
  every userspace tool reads files in 4 KiB chunks via the
  `read()` syscall and copies the bytes into a buffer it owns.
  Reading the wallpaper bitmap on every desktop boot does that.
  So does the browser when it opens an HTML file from `/mnt`.
  Each load is a fresh round-trip through the read path; nothing
  survives across processes.
- **Allocate large amounts of zero-filled memory cheaply.** The
  user-heap (chapter 17) hands out bytes from a fixed
  `sbrk()`-grown arena. Asking for 16 MiB right now actually
  *gets you* 16 MiB of resident pages whether or not you touch
  them, because the kernel eagerly maps the whole arena. A real
  `malloc` wants to ask the kernel for a region that is virtual
  immediately and physical only on touch.
- **Share a read-only file across multiple processes without
  reading it twice.** Every `/bin/sh` instance pulls its text
  segment off disk separately. Every `/bin/browser` re-reads
  the same OSFS asset on first use. There is no
  one-physical-page-many-virtual-mappings pattern available.

Each of these wants a page cache: a kernel data structure that
keys 4 KiB pages of physical RAM by *what is in them* rather
than by *who allocated them*. Once a page cache exists, the
`mmap` syscall is the user's window into it, and lazy fault-in
makes the whole thing pay-as-you-go.

This chapter installs both pieces — page cache and mmap — at
the smallest possible floor that lets us build on top of them in
later chapters.

## Prerequisites

- **Chapter 14** — the block cache. Same pattern, one layer
  lower (cached 512-byte sectors keyed by LBA). The page cache
  sits *above* the block cache in the eventual stack: future
  chapters will have file-content cache pages populated by
  pulling sectors out of the block cache, not by re-reading
  the disk.
- **Chapter 17** — the user heap. mmap is the second source of
  user pages (after `sbrk`); both have to coexist in the same
  user VA layout.
- **Chapter 75** — copy-on-write. Two mechanisms (the
  `DESC_SW_COW` software bit, the AS-clone deferred-copy walk)
  carry over: chapter 90 introduces a *second* software-defined
  bit, `DESC_SW_PAGECACHE`, with similar plumbing in
  `address_space_destroy` and `address_space_clone*`.

## What "page cache" means here

Linux's page cache is a beast. Per-inode radix trees, per-file
LRU lists, dirty/writeback bits, `address_space_operations`
callbacks, an entire two-list LRU promotion algorithm. Ours is
none of those. The chapter-90 page cache is:

- **32 fixed slots.** That's 128 KiB total. Big enough for the
  smoke test plus a couple of file mmaps; not big enough for any
  real workload. (Resizing is one constant change away when we
  need it; see [What we did NOT do](#what-we-did-not-do).)
- **Keyed on `(cache_id, offset_bytes)`** where `cache_id` is
  opaque to the cache. Today the only producer is ramfs file
  mmap, which packs the ramfs index into the lower 24 bits and
  sets the top bit `(1 << 24)` to identify "ramfs space". Future
  producers (OSFS-1, OSFS-2, ELF text segments) get disjoint id
  ranges.
- **Linear scan** for lookup. O(N) with N=32 is fine; the
  alternative (a hash table) is a chapter on its own.
- **Clock-LRU eviction.** When all slots are busy and a new
  miss arrives, scan from `g_clock`; the first slot with
  refcount 0 wins. Clock advances past pinned slots so they
  cycle out of the way.
- **Refcounted.** A page handed out via `page_cache_get_or_load`
  carries one refcount. `page_cache_release` drops it. The cache
  *never* frees a page on release — the slot stays populated, so
  the next lookup is a hit. Only eviction (or explicit teardown)
  frees.
- **Loaded by callback.** The cache doesn't know how to read a
  ramfs blob or an OSFS file. The producer passes a
  `page_cache_loader_fn` and an opaque context; the cache
  allocates a fresh pmem page (zero-filled), drops its lock,
  invokes the loader, then re-validates the slot. The lock-drop
  is important: a loader that does virtio-blk I/O can't be holding
  a system-wide lock.

That's the whole API:

```c
uint64_t page_cache_get_or_load(uint32_t cache_id, uint64_t offset_bytes,
                                page_cache_loader_fn loader, void *ctx);
void     page_cache_release(uint64_t pa);
```

Hits, misses, evictions, in-use counts are exported for the
smoke test to consult.

## The vma list — finally a real "address space"

Pre-chapter-90, an `address_space` was three things: an L1 page
table, a heap brk pointer, and a "user pages alloced" counter.
Two of those were enough to handle ELF load + sbrk + COW.

mmap forces a fourth: a list of virtual ranges that the kernel
*knows about but hasn't backed yet*. The shape is the
classical Linux VMA:

```c
enum vma_kind { VMA_ANON = 0, VMA_FILE_RAMFS = 1 };

struct vma {
    struct vma *next;        /* sorted by va */
    uint64_t    va;          /* page-aligned start */
    uint64_t    len;         /* page-aligned bytes */
    uint32_t    prot;        /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint32_t    kind;        /* enum vma_kind */
    uint32_t    ramfs_index; /* VMA_FILE_RAMFS only */
    uint32_t    _pad;
    uint64_t    file_offset; /* VMA_FILE_RAMFS only */
};
```

Plus the bump pointer:

```c
struct address_space {
    /* …existing fields… */
    struct vma *vmas;       /* sorted singly-linked list */
    uint64_t    mmap_brk;   /* next free VA in [USER_MMAP_BASE, USER_MMAP_MAX) */
};
```

User VA layout grew a slot:

```
USER_VA_BASE        0x10_0000_0000   (slot 64, 64 GiB)
USER_TEXT_BASE      0x10_0010_0000
USER_HEAP_BASE      0x10_1000_0000
USER_HEAP_MAX       0x10_3000_0000
USER_MMAP_BASE      0x10_3000_0000   ← chapter 90
USER_MMAP_MAX       0x10_3F00_0000   ← chapter 90 (240 MiB)
USER_STACK_TOP      0x10_4000_0000
```

The mmap region sits between the heap and the stack. 240 MiB is
generous for the chapter-90 floor; the cap is structural rather
than a guess.

`mmap_brk` is a one-way bump pointer. munmap detaches the vma
but does not roll back the bump. That means a long-running
process that mmaps and munmaps repeatedly will eventually run
out of VA — tolerable for the floor, fixed in a future chapter
by recycling holes (the standard mmap-region-walker pattern).

## The lazy fault-in path

The whole point of mmap is that the bookkeeping is cheap. When
the user calls `mmap(NULL, 16 MiB, PROT_READ|PROT_WRITE,
MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`, the kernel:

1. Validates the flags.
2. Reserves 16 MiB of VA from `mmap_brk`.
3. Creates a `struct vma` describing the region.
4. Returns the start VA.

That's it. Zero physical pages allocated. Zero L3 entries
written. The user gets back a pointer; the moment they touch
any byte of the region, the MMU walks the page tables, finds no
entry, and traps into the kernel's data-abort handler:

```
ESR_EL1.EC = 0x24      data abort from a lower EL
ISS bit 6 (WnR)        write (1) or read (0)
ISS DFSC[5:0] 0x04..7  translation fault (level 0..3)
ISS DFSC[5:0] 0x0C..F  permission fault (level 0..3)
```

Pre-chapter-90, the only fault we handled here was the
**permission fault** with WnR=1: the COW path from chapter 75.
A translation fault was always fatal — "you read garbage,
goodbye."

Chapter 90 splits the dispatch:

```c
if (xlat && t && t->as) {
    if (address_space_handle_mmap_fault(t->as, far, wnr) == 0)
        return;          /* lazy mmap fault-in */
}
if (wnr && perm && t && t->as) {
    if (address_space_handle_cow_fault(t->as, far) == 0)
        return;          /* COW (chapter 75) */
}
```

Translation fault first, then COW permission fault. The two are
mutually exclusive — a translation fault means "no descriptor
at all", which COW could never resolve.

Inside `address_space_handle_mmap_fault`:

```c
struct vma *v = vma_find(as, va);
if (!v) return -1;                            /* not in any vma → kill */
if (is_write && !(v->prot & PROT_WRITE))      /* RO mmap, write attempt */
    return -1;
uint64_t *ent = l3_entry_lookup(as, va);
if (ent && (*ent & DESC_VALID)) return -1;    /* already mapped — bug */

if (v->kind == VMA_ANON) {
    uint64_t pa = pmem_alloc_page();          /* zero-filled */
    /* install descriptor with is_pagecache=0 */
} else {
    /* VMA_FILE_RAMFS — consult the page cache */
    uint32_t cache_id = (1u << 24) | v->ramfs_index;
    uint64_t pa = page_cache_get_or_load(cache_id, file_off,
                                         ramfs_loader, &ctx);
    /* install descriptor with is_pagecache=1 */
}
```

Each branch ends with `l3_tbl[L3_INDEX(va)] = build_user_desc(...)`.
An early version of this code treated `l3_for(as, va)`'s
return as a direct entry pointer rather than the L3 table base —
the kernel built fine and silently corrupted random table
descriptors on every mmap fault. The fix is to actually index
into the returned table.

## The DESC_SW_PAGECACHE bit

Anonymous and file-backed mappings produce descriptors that
look identical at the MMU level — same access permissions,
same cacheability, same physical address. The kernel needs to
remember which is which so that **teardown frees the right
pool**:

- An anon mmap page is owned by the AS that faulted it in.
  When the AS dies, `pmem_dec_and_free(pa)` returns it.
- A page-cache page is owned by the cache. When the AS dies,
  `page_cache_release(pa)` drops the AS's refcount; the cache
  keeps the entry around for the next mmap to hit.

The hook is a software-defined bit in the AArch64 page-table
descriptor, identical pattern to chapter 75's `DESC_SW_COW`:

```c
#define DESC_SW_COW        (1ULL << 55)   /* chapter 75 */
#define DESC_SW_PAGECACHE  (1ULL << 56)   /* chapter 90 */
```

Architecture says bits 55–58 are software-defined; we get to
use them. (The MMU ignores them, so they survive every page-
table walk untouched.)

`teardown_user_range` becomes a tiny three-way switch:

```c
if (l3_ent & DESC_SW_PAGECACHE) {
    page_cache_release(pa);
} else {
    pmem_dec_and_free(pa);
}
```

## fork() does not propagate mmaps (yet)

Chapter 90's deliberate floor: **mmaps do not survive fork**.
Both `address_space_clone` and `address_space_clone_cow` skip
descriptors with `DESC_SW_PAGECACHE` set, and the child's vma
list is empty:

```c
if (src_ent & DESC_SW_PAGECACHE) continue;
```

This punts a real design question: how should two processes
that fork while holding the same MAP_PRIVATE file mapping
behave? The answer involves cross-AS refcounts (the cache
already has them) plus a `page_cache_share()` API to bump
refcounts at fork time, plus probably also a per-AS clone of
the vma list. None of that is hard, but each pebble adds 50–100
lines, and the chapter-90 floor doesn't need them: nothing in
userspace today calls `fork()` after `mmap()`.

When chapter 91+ adds threading (or someone writes a forking
mmap test), this rule goes. The repo memory captures the fix
sketch.

## What the syscall surface looks like

Two new syscalls — numbers 70 and 71 — and a small constants
header shared between kernel and userspace
([kernel/core/mmap_uapi.h](../../../kernel/core/mmap_uapi.h)):

```c
#define PROT_NONE       0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void *)-1L)
```

Numbers match Linux/glibc so the muscle memory transfers.

`sys_mmap` is strict about flags. It rejects:

- `MAP_FIXED` (no chosen-VA support).
- Absent `MAP_PRIVATE` (no `MAP_SHARED` yet).
- Any flag bits we don't recognise.
- `PROT_NONE` (no guard pages yet — see future chapter).
- File mmap without `PROT_READ`-only (writes need COW on a
  cached page, deferred).
- File mmap on a non-ramfs FD (osfs/osfs2 deferred — they
  want a sector-loading callback and we want to land the
  floor first).
- Non-page-aligned `len` or `offset`.

On success it returns the start VA. On failure, errno mirrored
into `-EINVAL_VFS` / `-ENOMEM_VFS` / `-EBADF`, plus the libc
wrapper returns `MAP_FAILED` for any value in errno range:

```c
static inline void *mmap(void *addr, size_t len, int prot, int flags,
                         int fd, long offset) {
    long r = _svc6(SYS_MMAP, (long)addr, (long)len, prot, flags,
                   fd, offset);
    if (r < 0 && r > -4096) return MAP_FAILED;
    return (void *)r;
}
```

`sys_munmap(addr, len)` is also strict: `addr` must equal a
vma's start VA, and `len` is ignored (whole-vma unmap only).
Partial unmap would split the vma into two, which would
require a vma-splitter; deferred.

## Testing it: the smoke

[`userspace/mmaptest/mmaptest.c`](../../../userspace/mmaptest/mmaptest.c)
does three things:

1. **Anon mmap of 4 pages.** Reads each page first to verify
   zero-fill on lazy fault. Then writes a per-page byte pattern
   (page N gets `0xA0+N`) across every byte. Reads back; any
   cross-page mismatch would mean lazy fault-in is producing
   wrong-page mappings. Then `munmap`s.

2. **File mmap of `/motd`.** Opens the file, `read()`s 64 bytes
   for reference, then `mmap`s a single page `PROT_READ
   MAP_PRIVATE`. Verifies the mapped bytes match the reference.
   Then mmaps the *same* file at a *new* VA (chapter-90 cache hit
   semantics: same backing, same content, different VA). Verifies
   again. **Then attempts `PROT_WRITE` and checks the kernel
   returns `MAP_FAILED`** — chapter-90 floor rejects writable
   file mmap.

3. **munmap both, close, exit.**

Markers printed for the regression scaffolding:

```
[mmap] anon OK
[mmap] file OK
[mmap] OK
```

[`scripts/test_mmap.py`](../../../scripts/test_mmap.py) boots
the kernel, runs the binary, and asserts all three markers
appear and no `[mmap] FAIL` substring is present.

## A regression-suite footnote: -smp 2 was always required

Chapter 89 declared the 24-test regression sweep "passes
under `-smp 2`" but never updated the test scripts to actually
pass `-smp 2`. The default uniprocessor boot gets stuck after
`preemption_demo()` because the chapter-89 per-CPU plumbing
relies on TPIDR\_EL1 being valid plus an idle thread on every
running CPU. Chapter 90 had to surface this to honestly run a
regression sweep:

```python
# scripts/test_*.py — add -smp 2 to every QEMU command line.
# Mechanical: regex-replace `"-m", "8G",` → `"-m", "8G", "-smp", "2",`
```

48 scripts patched (test scripts plus the `_dbg_*` and
`_snoop_*` helpers). The chapter-89 "sticky to creating CPU"
contract is still honoured — userspace processes all run on
CPU 0 — but every test now boots a real two-CPU configuration
and exercises the per-CPU paths under realistic load.

## What this unlocks

- **A real userspace `malloc`** that requests a large region
  via `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` and only consumes
  physical pages on touch. Today's `sbrk`-based heap can stop
  growing the heap eagerly.
- **Shared text for ELF binaries.** When the loader uses
  `mmap` to map an ELF text segment, every invocation of the
  same binary maps the same cached pages. RAM savings
  proportional to instance count. (Chapter 91+.)
- **MAP_SHARED IPC.** Two processes that mmap the same file
  with `MAP_SHARED` see each other's writes. That's a one-API
  step from where we are: keep the cache page refcounted
  across ASes, plumb writes back to disk via the loader's
  inverse. (Future.)
- **A real cache for browser assets.** Once OSFS-1 file mmap
  works, the wallpaper bitmap is read once per boot and every
  subsequent `paint` reuses the same physical pages.

## What we did NOT do

The chapter-90 floor punts on a long list of things, mostly
because each one is its own chapter:

- **No `MAP_SHARED`.** Chapter 90 has the cache infrastructure
  but doesn't expose it as shared writable memory between
  processes. Adding it requires `page_cache_share()` for
  fork inheritance and writeback for dirty pages.
- **No file mmap with `PROT_WRITE`.** Would need COW on the
  cached page (i.e. `DESC_SW_COW` and `DESC_SW_PAGECACHE`
  coexistence in the descriptor and in the fault handler).
  Mechanically straightforward, but a chapter on its own
  because it touches every teardown path.
- **No msync.** With no writable file mmap, there's nothing
  to flush. Comes with `MAP_SHARED`.
- **No MAP_FIXED.** No way for userspace to choose its own VA.
- **No PROT_NONE.** Guard pages need a way to install a
  descriptor that's "valid but not accessible"; we don't have
  one yet.
- **No /mnt or /data file mmap.** OSFS-1 and OSFS-2 are
  blocked on writing a sector-pulling loader callback. Easy
  but mechanical; deferred to keep this chapter small.
- **No ELF text-segment dedupe.** The ELF loader still copies
  the text segment into per-AS pages instead of mmap-ing it.
  Dedupe is the headline use case for the page cache, but
  retrofitting the ELF loader is a separate change.
- **No mmap survives fork.** Both AS-clone walks skip
  `DESC_SW_PAGECACHE` descriptors; child gets an empty vma
  list. See [the fork section](#fork-does-not-propagate-mmaps-yet).
- **Cache size is tiny.** 32 slots is enough to prove the
  pattern works; nothing useful at scale.
- **No mmap-region recycler.** munmap detaches a vma but
  doesn't roll the bump pointer back. Long-running processes
  with churn-y mmap patterns will run out of VA.

## Files added or changed

- **[`kernel/core/page_cache.h`](../../../kernel/core/page_cache.h)** — new. Public API.
- **[`kernel/core/page_cache.c`](../../../kernel/core/page_cache.c)** — new. 32-slot fixed array, clock-LRU
  eviction, single spinlock, IRQ-saved.
- **[`kernel/core/mmap_uapi.h`](../../../kernel/core/mmap_uapi.h)** — new. `PROT_*` and `MAP_*` constants
  shared between kernel and userspace.
- **[`kernel/arch/address_space.h`](../../../kernel/arch/address_space.h)** — added `USER_MMAP_BASE` /
  `USER_MMAP_MAX`, `enum vma_kind`, `struct vma`, `vmas` and
  `mmap_brk` fields, prototypes for `address_space_mmap_anon`,
  `address_space_mmap_ramfs`, `address_space_munmap`,
  `address_space_handle_mmap_fault`.
- **[`kernel/arch/address_space.c`](../../../kernel/arch/address_space.c)** — added `DESC_SW_PAGECACHE`
  bit; teardown branches by bit; both AS-clone walks skip
  page-cache pages; vma helpers; the four new entry points;
  `ramfs_loader` callback.
- **[`kernel/core/vfs.h`](../../../kernel/core/vfs.h)** / **[`vfs.c`](../../../kernel/core/vfs.c)** — three new public ramfs
  accessors (`vfs_ramfs_blob`, `vfs_ramfs_count`,
  `vfs_ramfs_lookup`) so the mmap path can read blob bytes
  without reaching into VFS internals.
- **[`kernel/core/syscall.h`](../../../kernel/core/syscall.h)** — added `SYS_MMAP=70`, `SYS_MUNMAP=71`.
- **[`kernel/core/syscall.c`](../../../kernel/core/syscall.c)** — added `sys_mmap` / `sys_munmap`,
  patched the data-abort handler to dispatch translation
  faults to the mmap path before COW.
- **[`kernel/core/main.c`](../../../kernel/core/main.c)** — calls `page_cache_init()` after
  `vfs_init()`.
- **[`Makefile`](../../../Makefile)** — added `kernel/core/page_cache.c` and the
  `mmaptest` userspace binary.
- **[`userspace/libc/syscall.h`](../../../userspace/libc/syscall.h)** — added `mmap()` and `munmap()`
  wrappers plus the `MAP_*` / `PROT_*` constants.
- **[`userspace/mmaptest/mmaptest.c`](../../../userspace/mmaptest/mmaptest.c)** — new. Smoke test.
- **[`scripts/test_mmap.py`](../../../scripts/test_mmap.py)** — new. Drives the smoke.
- **48 existing test/dbg scripts** — added `-smp 2` to QEMU
  command line to honour the chapter-89 invariant.

## Build & test

```
$ make all
$ python3 scripts/test_mmap.py
--- mmaptest output: ---
mmaptest
[mmap] start
[mmap] anon OK
[mmap] file OK
[mmap] OK
PASS: chapter 90 mmap smoke test
```

The full 25-test regression suite (24 inherited tests + the
new `test_mmap.py`) passes:

```
=== test_directories ===     PASS
=== test_journal ===         PASS
=== test_osfs2 ===           PASS
=== test_notepad ===         PASS
=== test_notepad_save_as ===     PASS
=== test_notepad_save_as_nav === PASS
=== test_wm ===              PASS
=== test_taskbar ===         PASS
=== test_clock ===           PASS
=== test_dns ===             PASS
=== test_httpget ===         PASS
=== test_dhcp ===            PASS
=== test_layout ===          PASS
=== test_html_dom ===        PASS
=== test_fork_exec ===       PASS
=== test_cow ===             PASS
=== test_sigaction ===       PASS
=== test_sigchld ===         PASS
=== test_minimize ===        PASS
=== test_launcher ===        PASS
=== test_html_tokenizer ===  PASS
=== test_css ===             PASS
=== test_arrow_keys ===      PASS
=== test_gui_term ===        PASS
=== test_mmap ===            PASS
PASS=25 FAIL=0
```
