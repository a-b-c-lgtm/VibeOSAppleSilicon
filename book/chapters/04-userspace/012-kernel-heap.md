# Chapter 12 — The kernel heap

> **Milestone in this chapter:** 3 — kernel heap.
> **Code referenced:** [kernel/core/heap.c](../../../kernel/core/heap.c),
> [kernel/core/heap.h](../../../kernel/core/heap.h),
> [linker/kernel.ld](../../../linker/kernel.ld) (`.heap` section),
> [kernel/core/main.c](../../../kernel/core/main.c) (the `heap_demo`).
>
> **At the end of this chapter** you will have a `kmalloc` /
> `kfree` pair that allocates 16-byte-aligned blocks from a
> 16 MiB linker-reserved region, splits free blocks on demand,
> and coalesces both forwards and backwards on free. Every
> subsequent chapter that needs heap memory — threads, file-
> system buffers, virtio descriptor tables, the GUI compositor —
> sits on top of these two functions.

## Why this is its own chapter

Up through the earliest chapters every piece of state in the kernel was
either a static global (`g_ticks` in the timer, `l1_pgtable` in
the MMU code) or stack-local. That is enough to bring up
hardware, but it is not enough to *manage* anything: a process
table needs a variable number of `struct process` records, the
file system will need buffer-cache pages, and the GUI compositor
will need windows and back-buffers. All of those want the
classic "ask for N bytes; release them later" interface.

The heap is the smallest infrastructure that unlocks all of
them. Once it works, the rest of the kernel can stop carving up
fixed arrays.

## Where the heap lives

We reserve a fixed 16 MiB span in the linker script:

```ld
.heap (NOLOAD) : ALIGN(4K) {
    heap_start = .;
    . = . + 0x01000000;     /* 16 MiB */
    heap_end = .;
} :data
```

A few things to notice:

- **`(NOLOAD)`** tells the linker not to put any bytes for this
  region into the ELF image. The kernel image stays small; the
  region is reserved by *address* and the loader (QEMU) leaves
  the underlying RAM uninitialised.
- **`ALIGN(4K)`** makes the heap base a clean page boundary.
  We do not actually need page alignment for the allocator
  itself, but it makes future page-granularity bookkeeping
  trivial: every linear scan of the heap can also be a scan
  over pages without re-aligning.
- **The two symbols `heap_start` and `heap_end`** are exported
  to C as `extern uint8_t heap_start[]; extern uint8_t
  heap_end[];`. Address-of-symbol semantics give us the byte
  pointers we want without any `&`.
- **Placed in the `data` PHDR** so it lives in the writable
  segment, alongside `.data`, `.bss`, and `.stack`.

After `make all`, `objdump -h build/kernel.elf` shows the layout:

```
  3 .bss          00000010  0000000040084000  0000000040084000  ALLOC
  4 .stack        00010000  0000000040084010  0000000040084010  ALLOC
  5 .heap         01000000  0000000040095000  0000000040095000  2**12  ALLOC
```

The heap sits at `0x40095000` (16 MiB above the kernel base) and
runs to `0x41095000`. Both addresses fall inside our identity-
mapped Normal-Cacheable range from chapter 6, so the MMU does
the right thing immediately — no extra mapping work is needed.

## The block format

Every block — used or free — starts with a 16-byte header:

```c
struct block {
    size_t total_size;     /* header + payload, low bit = in_use */
    size_t prev_size;      /* total_size of preceding block, 0 at start */
};
```

Two design choices worth defending up front:

- **Why a 16-byte header.** AArch64's AAPCS requires 16-byte
  alignment at every function-entry stack pointer; in practice
  every `void *` we pass around the kernel is most useful when
  it is 16-byte aligned (DMA descriptors, page-table entries,
  `struct virtio_pci_cap`, …). A 16-byte header guarantees
  every payload is aligned-by-construction with no per-block
  arithmetic.
- **Why store `prev_size` instead of a pointer.** A back pointer
  would be 8 bytes on AArch64 — same cost — but storing the
  *size* of the previous block lets us locate it by simple
  pointer subtraction (`(uint8_t *)b - b->prev_size`) without
  walking from the start of the heap. That makes O(1) backward
  coalescing possible.

The low bit of `total_size` doubles as the "in use" flag.
Because `total_size` is always a multiple of 16, the low four
bits are otherwise unused, so packing the flag into the LSB is
free:

```c
static inline int   blk_in_use(const struct block *b)        { return (int)(b->total_size & 1); }
static inline size_t blk_size  (const struct block *b)        { return b->total_size & ~1ULL; }
static inline void   blk_set   (struct block *b, size_t s, int u) { b->total_size = s | (u ? 1 : 0); }
```

## Walking the implicit list

There is no separate "free list" structure. Every block in the
heap is reachable by starting at `heap_start` and adding
`total_size` to step to the next block:

```c
static inline struct block *blk_next(struct block *b)
{
    uint8_t *p = (uint8_t *)b + blk_size(b);
    return (p >= heap_end) ? NULL : (struct block *)p;
}
```

Backward navigation is the dual: subtract `prev_size`. A
`prev_size == 0` marks the very first block:

```c
static inline struct block *blk_prev(struct block *b)
{
    return b->prev_size ? (struct block *)((uint8_t *)b - b->prev_size) : NULL;
}
```

This *is* O(N) for first-fit allocation and is the obvious
upgrade target when N grows large. Chapter 12's allocator is
deliberately the simplest correct implementation; chapter 21
(virtio-mmio) adds an explicit segregated free list when we
start allocating thousands of small descriptor blocks per
second.

## `kmalloc` step by step

```c
void *kmalloc(size_t size)
{
    if (size == 0) return NULL;

    size_t need = ALIGN_UP(size + HEADER_SIZE, 16);
    if (need < MIN_BLOCK_SIZE) need = MIN_BLOCK_SIZE;

    for (struct block *b = (struct block *)heap_start; b; b = blk_next(b)) {
        if (blk_in_use(b))           continue;
        if (blk_size(b) < need)      continue;

        if (blk_size(b) >= need + MIN_BLOCK_SIZE) {
            /* split */
            struct block *tail = (struct block *)((uint8_t *)b + need);
            blk_set(tail, blk_size(b) - need, 0);
            tail->prev_size = need;
            blk_set(b, need, 1);

            struct block *after_tail = blk_next(tail);
            if (after_tail) after_tail->prev_size = blk_size(tail);
        } else {
            /* take the whole block */
            blk_set(b, blk_size(b), 1);
        }
        return blk_payload(b);
    }
    return NULL;    /* OOM */
}
```

The two non-obvious details:

1. **Split threshold.** We only split if the leftover would be
   at least `MIN_BLOCK_SIZE` (32 bytes — header + 16-byte
   payload). Otherwise we take the whole block, which adds at
   most `MIN_BLOCK_SIZE - 1` bytes of internal fragmentation
   per allocation but guarantees that every block we ever leave
   on the heap is large enough to hold a future allocation.
2. **Updating `prev_size` of the block *after* the split.** When
   we split block B into B' (used) and T (free), the block
   that *used to follow* B (call it A) is now the block that
   follows T. A's `prev_size` was `B.total_size` and must
   become `T.total_size`. Forgetting this is the most common
   coalesce-related bug.

## `kfree` and coalescing

```c
void kfree(void *ptr)
{
    if (!ptr) return;

    struct block *b = blk_from_payload(ptr);
    blk_set(b, blk_size(b), 0);

    /* Forward coalesce. */
    struct block *next = blk_next(b);
    if (next && !blk_in_use(next)) {
        size_t merged = blk_size(b) + blk_size(next);
        blk_set(b, merged, 0);
        struct block *after = blk_next(b);
        if (after) after->prev_size = merged;
    }

    /* Backward coalesce. */
    struct block *prev = blk_prev(b);
    if (prev && !blk_in_use(prev)) {
        size_t merged = blk_size(prev) + blk_size(b);
        blk_set(prev, merged, 0);
        struct block *after = blk_next(prev);
        if (after) after->prev_size = merged;
    }
}
```

Coalescing is the only thing that makes a first-fit allocator
viable long-term. Without it, repeated `kmalloc(64) ; kfree;
kmalloc(64) ; kfree; …` patterns would leave a sea of
64-byte-shaped holes; eventually a `kmalloc(128)` would fail
even though 90 % of the heap was free. With it, free blocks
merge with their free neighbours so the heap converges to a
small number of large free blocks.

The order matters: forward first, then backward. Doing forward
first leaves `b` as the surviving block and updates `after`'s
`prev_size`. The subsequent backward coalesce then correctly
merges `prev + b` (where `b` already contains the forward-merged
size).

## Verification — the demo

`kernel_main` runs `heap_demo()` immediately after enabling
IRQs:

```c
void *a = kmalloc(64);
void *b = kmalloc(256);
void *c = kmalloc(1024);

kfree(b);   /* leaves 'a [hole] c' — no coalescing possible */
kfree(a);   /* now 'a' touches the hole; forward coalesce */
kfree(c);   /* now 'c' touches the merged block; both directions */
```

With `kheap_used()` and `kheap_block_count()` reporting after
each step, the expected trace is:

```text
[heap] initial used = 0x0,    blocks = 0x1   (one big free block)
[heap] after 3 allocs:    used = 0x540, blocks = 0x4
[heap] after kfree(middle):           blocks = 0x4
[heap] after kfree(all):              blocks = 0x1, used = 0x0
```

`0x540` = `1344` = `64 + 256 + 1024` (the payload bytes, not
counting headers). `blocks = 4` after the three allocations is
the three used blocks plus the one trailing free block. The
final `blocks = 1` is the proof that bidirectional coalescing
brought the heap all the way back to its initial state — every
free was reabsorbed.

If you see `blocks > 1` at the final step, *something* is
broken: either the in-use bit logic, or one of the
`after->prev_size` updates, or the split's `tail->prev_size`
write. Add a `kheap_dump()` (loop over `blk_next` printing
`(addr, size, in_use)`) and walk forward from `heap_start` to
spot the inconsistency.

## What chapter 13 adds

Now that we have a heap, the next set of chapters can store
state per-thing rather than per-statically-sized array. Chapter
14 (the SVC syscall ABI) introduces the SVC instruction and the
syscall vector — slot 8 of the vector table from chapter 5,
where userspace lands when it executes `svc #0`. That is the
last piece needed before chapter 14 ELF-loads our first
userspace program and gives `kmalloc` its first really
substantial caller: the per-process page tables, address-space
descriptors, and ELF segment metadata.
