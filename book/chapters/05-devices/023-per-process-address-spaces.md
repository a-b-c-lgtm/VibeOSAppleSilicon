# Chapter 23 — Per-process address spaces

## What we have, and what we don't

Up to the previous chapter every "user" program shared the same page
tables — the boot L1 the kernel built once at startup. That L1
identity-maps the kernel image, the heap, devices, and (because
we used 1 GiB block descriptors with the user-RW attribute) every
DRAM page is reachable from EL0 too. It worked for one program at
a time. It worked for two programs spawned in sequence because
each one could load itself anywhere it wanted in DRAM and run.

It does **not** isolate programs. If `cat` and `sh` are both
running, `cat` can stomp on `sh`'s memory just by writing to it.
Neither does it let two binaries link at the same VA, the way real
ELFs do (ours all link at the same fixed address; we got away with
it because they don't run concurrently).

This chapter is the first half of a fix: every process gets its
own L1 page table. The kernel address space is shared (we'll see
how in a moment), but the user range is private. Two threads
scheduled at the same time see different user pages at the same
user VA.

The second half — true memory protection that catches a wild
write before it corrupts another process — needs page-granularity
permissions on user mappings, which is exactly what 4 KiB page
descriptors give us. So this chapter is also when the user range
graduates from 1 GiB blocks to 4 KiB pages.

The second half — true memory protection that catches a wild
write before it corrupts another process — needs page-granularity
permissions on user mappings, which is exactly what 4 KiB page
descriptors give us. So this chapter is also when the user range
graduates from 1 GiB blocks to 4 KiB pages.

## Design choice: TTBR0-only with per-process L1

ARMv8 gives you two top-level page tables wired into the MMU at
once: TTBR0 covers low VAs, TTBR1 covers high VAs. The split is
configured by `TCR_EL1.T0SZ` / `T1SZ`. The standard arrangement on
a real OS is "kernel in TTBR1 (high VAs), user in TTBR0 (low
VAs)". Context switches only touch TTBR0 and the kernel half of
the address space is invariant.

We are not going to do that yet. Splitting kernel and user
between TTBR0 and TTBR1 means relinking the kernel at a high VA
(`0xFFFF...`), updating every linker script, every assembly stub
that uses absolute addresses, and the boot path that expects to
run identity-mapped at low PA == low VA. It's a deep change, and
we don't need it for isolation.

What we'll do instead:

- Keep the kernel where it is (TTBR0, low VAs).
- Each process gets its own L1 page table (TTBR0 swaps on context
  switch).
- The kernel slots in every per-process L1 are **inherited from
  the boot L1** — they all point at the same 1 GiB block
  descriptors. Identical kernel mappings everywhere.
- Exactly one slot of the L1 is *replaced* with a per-process
  table descriptor pointing at a 4 KiB-page user mapping. That
  slot is where user binaries live.

The pros: no relinking, no high-VA shuffle, no TTBR1 setup. Boot
path is unchanged. Kernel can dereference any kernel pointer no
matter which process is current, because the kernel slots are the
same in every L1.

The cons (deferred to later milestones):

- No ASIDs. Every TTBR0 swap flushes the entire EL1/EL0 TLB.
  Costly but correct.
- The kernel can only see one process's user memory at a time
  (whichever AS is currently active). When we add `fork(2)` or
  `copy_from_user` between processes, we'll either swap ASes or
  add a temporary mapping window.

## Picking the user VA range

The boot L1 has nine slots filled today (slot 0 = devices, slots
1..8 = the kernel image plus DRAM identity mapping covering all
8 GiB QEMU gives us). Every "kernel slot" is a 1 GiB block. A
per-process AS inherits all of them. So whichever slot we reserve
for the user range *replaces* whatever the boot L1 has there.

The catch: when the kernel allocates a fresh page from `pmem` for
a user mapping, that page has some PA somewhere in DRAM. To copy
ELF bytes into it the kernel needs to be able to write to it via
its PA (`*(uint8_t *)pa = ...`). That works only if the PA falls
in a slot that's still identity-mapped in the *currently active*
AS. If we picked the user slot to be one of the DRAM slots, the
kernel could allocate a page that lives "underneath" the user
range; once a user AS was active, that PA would no longer be
reachable by the kernel and the copy would fault.

The fix is to put the user range *above* all DRAM. We're using
slot 64 — VA `0x1000000000` (64 GiB). That's well above any
DRAM PA (our 8 GiB QEMU tops out at `0x240000000` ≈ 9 GiB), so
pmem PAs and user VAs can't collide:

```
USER_VA_BASE     = 0x1000000000   (slot 64, 64 GiB)
USER_TEXT_BASE   = 0x1000100000   (link address for all binaries)
USER_STACK_TOP   = 0x1040000000   (one past the top of the slot)
USER_STACK_PAGES = 4              (16 KiB user stack)
```

The user binary linker script (`userspace/linker_user.ld`) is
updated to match:

```
USER_LOAD_ADDR = 0x1000100000;
```

Every user binary now links at the same VA. When two of them are
running, both see their own code at `0x1000100000` — the page
tables resolve those VAs to different PAs.

## The page-table walk in detail

We have a 39-bit VA, 4 KiB granule. The walk has three levels:

```
bit 38 ... 30 ... 21 ... 12 ... 0
        L1      L2      L3   page offset
        9 bits  9 bits  9 bits  12 bits
```

- L1 entry covers 2^30 = 1 GiB.
- L2 entry covers 2^21 = 2 MiB.
- L3 entry covers 2^12 = 4 KiB.

Slot 64 of the L1 covers `0x1000000000 .. 0x1040000000` (1 GiB).
For each per-process AS we allocate:

- one L1 page (4 KiB, 512 entries × 8 bytes)
- one L2 page (4 KiB, also 512 entries) installed into slot 64 of
  the L1
- one L3 page per **2 MiB** of user VA actually mapped. Lazily
  allocated.

For our small binaries (≤16 pages of code+data plus a 4-page
stack at the high end), only **one** L3 page is needed for text/data
and **one** more for the stack — about 16 KiB of metadata per
process plus the actual user pages. Cheap.

## The `address_space` API

```c
struct address_space {
    uint64_t  l1_pa;       /* physical addr of L1 (for TTBR0) */
    uint64_t *l1_va;       /* same page, accessed by PA-as-VA */
    uint64_t  l2_pa;       /* this AS's L2 (slot USER_L1_SLOT) */
    uint64_t *l2_va;
    size_t    user_pages_alloced;  /* for accounting */
};

struct address_space *address_space_create(void);
void                  address_space_destroy(struct address_space *as);
int                   address_space_map(struct address_space *as,
                                        uint64_t va, uint64_t pa,
                                        int writable, int executable);
void                  address_space_activate(struct address_space *as);
uint64_t              address_space_boot_l1_pa(void);
```

`create` allocates the L1 + L2, then walks the boot L1 copying
every entry **except** slot 64 into the new L1, then installs the
new L2 in slot 64 as a TABLE descriptor. The result: a fresh AS
that looks identical to the boot L1 from the kernel's perspective
and has an empty user range.

`map` walks down to L3, allocating L3 pages on demand, and
installs a 4 KiB PAGE descriptor with the requested permissions
(`AP=01` for kernel-RW + EL0-RW, `PXN=1` to forbid kernel
execution of user pages, `UXN=!executable`).

`activate` writes `l1_pa` to `TTBR0_EL1`, `isb`s, then issues
`tlbi vmalle1; dsb ish; isb` to flush the entire EL1/EL0 TLB.
With no ASIDs every swap costs us a global TLB flush — accepted
for now. `activate(NULL)` restores the boot L1, used when
switching to a kernel thread.

`destroy` walks the L2 freeing every L3 (and every user page each
L3 maps), frees the L2 and L1, and frees the struct itself.

## Wiring it into the scheduler

The thread struct gains a pointer:

```c
struct thread {
    ...
    struct address_space *as;   /* NULL for kernel threads */
};
```

`user_thread_create` takes an `as` parameter and stores it.
Kernel-thread paths (`thread_create`, the boot thread) leave `as`
NULL.

In `yield()`, just before `cswitch_to`:

```c
if (next->as != prev->as)
    address_space_activate(next->as);
cswitch_to(&prev->sp, next->sp);
```

The `next->as != prev->as` check is the small optimization that
keeps two kernel threads (both `as == NULL`) from paying the
TLB-flush cost on every reschedule. Two user threads of the same
process would share an `as` pointer too (we don't have shared
processes yet, but the check is free).

When a thread is reaped in `thread_wait`:

```c
if (exited->as) address_space_destroy(exited->as);
```

The AS owns its user pages; freeing it returns them to pmem.

## ELF loader changes

The old `elf_load_user(data, size, &img)` did a single `pmem`
allocation big enough to hold the whole binary contiguously,
copied bytes by PA arithmetic, and returned the PA as the entry
point — relying on the fact that low PA == low VA in the boot L1.

The new `elf_load_user(data, size, as, &img)` does this per
PT_LOAD segment:

1. Allocate one 4 KiB physical page per page of the segment.
2. Copy the file bytes into the page via its PA. (This works
   because we're called *before* `activate(as)` — the boot L1 is
   active, and the page's PA is in a DRAM slot identity-mapped
   there. After `activate(as)` the kernel could no longer see
   most of these PAs the same way, which is fine because we don't
   need to.)
3. Call `address_space_map(as, va, pa, writable, executable)`.

Then map `USER_STACK_PAGES` fresh pages descending from
`USER_STACK_TOP`. Set `img.entry_va = eh->e_entry` (the
link-time VA, no longer a PA), `img.stack_top_va = USER_STACK_TOP`.

The contiguity requirement of the old loader is gone: each user
page can come from anywhere in DRAM. That's what enables real
allocation patterns later (lazy mapping, copy-on-write, sparse
heaps).

## Spawning gets one more line

Boot's `userspace_demo` and the syscall `sys_spawn` follow the
same pattern:

```c
struct address_space *as = address_space_create();
if (!as) { kfree(data); return -ENOMEM; }

struct user_image img;
if (elf_load_user(data, size, as, &img) != 0) {
    address_space_destroy(as);
    return -EINVAL;
}

struct thread *t = user_thread_create(img.entry_va, img.stack_top_va,
                                      path, as);
if (!t) {
    address_space_destroy(as);
    return -ENOMEM;
}
```

After `user_thread_create` returns, the thread owns the AS.
Failure paths between create-AS and create-thread destroy the AS
to avoid a leak.

## Verification

The smoke test is the same shell session we've run since
chapter 21: load `init` from disk, let it spawn `sh`, run a few
binaries, exit cleanly. What's different is the page tables
underneath:

```
[user] loading /bin/init (0x14a0 bytes)
[user] entry = 0x1000100000, sp = 0x1040000000
[init] starting (pid 1)
[init] spawn /bin/hello -> tid=4
hello from EL0!
$ /bin/cat /mnt/hello.txt
hello from disk!
$ /bin/hello
hello from EL0!
$ exit
[init] /bin/sh exited code=0
[blk_cache] hits=19 misses=45 evictions=0
```

Every binary you see launch — `init`, `sh`, `cat`, `hello` — is
running with its own L1 in TTBR0. They all execute from VA
`0x1000100000`. Their page tables route that VA to four different
PAs. When one exits, its AS is destroyed and its user pages go
back to pmem.

That's per-process address spaces in 600 lines of new code.

## A subtle bug to watch for

The thread struct used to store `name` as `const char *`. When
`sys_spawn` was called from EL0, we passed the user-mode pointer
straight through. With the boot L1 active everywhere, that
pointer was always valid (the user binary's `.rodata` was
identity-mapped DRAM). With per-process ASes, that pointer
becomes stale the moment the user AS is destroyed: the next time
the kernel printed `[sys_exit] thread '...'` for an exited
process, it dereferenced a pointer into a torn-down AS and read
garbage (or worse, faulted).

The fix is in two lines: change `const char *name` to
`char name[THREAD_NAME_MAX]`, copy the path string into the
thread struct at creation. Owned by the struct, lives as long as
the struct does, no allocation, no ownership question.

This is the recurring theme of moving from "shared address space"
to "per-process address space" — every kernel-side reference to a
user pointer now has a lifetime question attached. We'll see it
again when we add `argv` on the user stack, when we add
`copy_from_user`, and when we add `exec`. Track ownership early.

## What's deferred

- **ASIDs.** Every context switch costs a full TLB flush. Real
  systems tag TLB entries with an 8- or 16-bit ASID per AS so the
  flush only happens on AS recycle. Drop-in change in `activate`
  later.
- **`copy_from_user` / `copy_to_user`.** Today the kernel
  dereferences user pointers raw. That's only safe because we
  fully control the user binaries. A real syscall has to validate
  the address (in user range, mapped, right permission) and copy
  defensively. Otherwise a malicious user can read kernel memory
  via crafted pointers.
- **`fork`.** Needs an AS-clone primitive (deep-copy page tables,
  mark all user pages copy-on-write). Page-fault handler then
  allocates a private copy on the first write.
- **`exec`.** Tear down the current AS (or rebuild it) and load
  a new binary into it, preserving fds.
- **Real `argv`/`envp` on the user stack.** Today we cheat with a
  single string in `thread->args[]` and a `getargs(2)` syscall.
  Once we have ELF auxiliary vectors and a real C runtime,
  `_start` will read argv off the stack like it does on Linux.
- **Heap.** No `brk`/`mmap` yet. User binaries today have only
  what their ELF declares.

Five items, each maybe 100 lines of code. Add them one at a time
and the kernel grows up.
