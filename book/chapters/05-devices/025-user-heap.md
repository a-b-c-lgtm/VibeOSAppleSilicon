# Chapter 25 — A user heap via `sbrk`

Until now every user program had only what its ELF declared.
`hello` and `cat` got away with a few hundred bytes of `.bss` on
the kernel-allocated 16 KiB user stack. `sh` got 128 bytes for a
line buffer. Anything bigger required either a bigger static
buffer (wasteful, capped at link time) or a fundamentally
different program structure.

That ends here. This chapter adds `sbrk` — the simplest possible
heap-growing primitive — to the syscall ABI, then layers a tiny
first-fit `malloc`/`free` on top of it in user code. The
end-to-end test allocates 64, 128, 256, and 32 KiB blocks,
verifies them, and frees them.

## What `sbrk` does

The classical Unix interface:

> `void *sbrk(intptr_t inc);`
>
> Adjust the program break by `inc` bytes (positive grows,
> negative shrinks) and return the *previous* break.

The "program break" is just a watermark: the lowest address above
which the heap is *not* mapped. `malloc` calls `sbrk(N)` to bump
the watermark up by N, gets back a pointer to the (now mapped)
freshly-allocated region, and uses it. `free` is something the
kernel doesn't see — it's purely a user-side bookkeeping
operation against the same pre-mapped region.

The kernel side is small. We add one field to `struct
address_space`:

```c
struct address_space {
    ...
    uint64_t  heap_brk;     /* page-aligned program break */
};
```

initialized to `USER_HEAP_BASE` (`0x1010000000`) when the AS is
created. And one new function:

```c
int address_space_set_brk(struct address_space *as, uint64_t new_brk);
```

which rounds the request up to a page, then either:

- maps fresh anonymous pages (one `pmem_alloc_page` + one
  `address_space_map` per page) between `heap_brk` and `new_brk`
  if growing, or
- walks the L3 entries, frees the backing page to pmem, and
  invalidates the descriptor between `new_brk` and `heap_brk` if
  shrinking, then issues a TLB invalidate.

The shrink path matters because the alternative — leaking pages
across `free` cycles — would let any long-running program slowly
exhaust DRAM.

## VA layout for the heap

The user range is one 1 GiB L1 slot (`0x1000000000` -
`0x1040000000`). We slice it like this:

```
0x1000000000  USER_VA_BASE       ┐
0x1000100000  USER_TEXT_BASE     ├ binary text/data
              up to ~64 KiB      ┘
0x1010000000  USER_HEAP_BASE     ┐ heap grows up
              ...                ├ (capped at USER_HEAP_MAX)
0x1030000000  USER_HEAP_MAX      ┘
0x103FFFC000  USER_STACK_BOTTOM  ┐ stack, 16 KiB,
0x1040000000  USER_STACK_TOP     ┘ grows down
```

`USER_HEAP_MAX` (`0x1030000000`) caps the heap at 512 MiB per
process, leaving plenty of unmapped space below the stack. Beyond
keeping a single rogue process from exhausting all of pmem, the
cap is mostly a sanity rail — when we add `mmap` later there'll
be a real allocator instead of this watermark.

## The syscall

```c
static long sys_sbrk(long inc)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -1;

    uint64_t old_brk = t->as->heap_brk;

    int64_t  signed_inc = (int64_t)inc;
    uint64_t new_brk;
    if (signed_inc >= 0) {
        new_brk = old_brk + (uint64_t)signed_inc;
        if (new_brk < old_brk) return -1;          /* wrap */
    } else {
        uint64_t mag = (uint64_t)(-signed_inc);
        new_brk = mag > old_brk - USER_HEAP_BASE
            ? USER_HEAP_BASE                       /* clamp */
            : old_brk - mag;
    }

    if (address_space_set_brk(t->as, new_brk) != 0)
        return -1;
    return (long)old_brk;
}
```

The unsigned-arithmetic dance handles negative `inc` without
relying on signed-overflow being well-defined. Returning the *old*
break is what makes `malloc`-on-top-of-`sbrk` natural: the caller
already knows what range it just acquired.

Concurrency note in the source: this mutates the per-process page
tables. It's safe today because we're single-CPU and never preempt
inside a syscall — the only thing that can run between map
operations is an IRQ, and our IRQ handlers don't touch user page
tables. Once we have multiple cores or in-syscall preemption,
this'll need a per-AS lock.

## A tiny user-side allocator

`userspace/libc/malloc.h` is a header-only first-fit free-list
allocator. The whole thing is ~120 lines. The block layout:

```
+---------------+----------------------------------+
| size (8 B)    | payload (size - 8 bytes)         |
+---------------+----------------------------------+
```

`malloc(N)` rounds N up to a multiple of 16 (so payload pointers
stay AArch64-aligned for any type), walks the free list first-fit,
splits a block if the leftover would still hold a minimum-sized
block, and returns `block + 8`. If no free block fits, it calls
`sbrk(MALLOC_GROW)` for a 16 KiB chunk, pushes that as a single
free block, and retries.

`free(p)` walks the address-ordered free list to find the right
insertion point, links the block back in, and tries to coalesce
with its right neighbour if they're adjacent. Coalescing with the
left neighbour is deferred to the next `malloc` walk for
simplicity (we'd need a prev pointer otherwise).

This is the dumbest allocator that survives realistic workloads.
Fragmentation under hostile patterns is bad, performance is O(N)
per call, and it never returns memory to the kernel. None of that
matters for what we're building toward.

## Verification

```
$ /bin/heaptest
[heaptest] starting
  a  = 0x1010000008
  b  = 0x1010000058
  c  = 0x10100000e8
  b2 = 0x1010000058           ← reused freed slot
  big = 0x1010004008          ← triggered sbrk()
[heaptest] all checks passed
```

Reading those addresses left-to-right tells the story:

- `a` payload starts 8 bytes into the heap (right after the
  block header). The block itself starts at `USER_HEAP_BASE`.
- `b` starts at `+0x50` from `a` — that's an 80-byte block (8
  header + 72 payload, rounded up from 64).
- `c` starts at `+0x90` from `b` — 144 bytes (8 header + 136
  payload, rounded up from 128).
- After `free(b)`, `b2 = malloc(128)` returned the *same address*
  as `b`. The free-list reused the slot.
- `big = malloc(32 KiB)` got a fresh address `0x1010004008`,
  meaning the allocator triggered an `sbrk` to grow the heap.

The full battery of test programs still passes:

| program     | what it tests                                  |
|-------------|------------------------------------------------|
| `heaptest`  | this chapter — malloc/free, sbrk grow          |
| `badpoke`   | EL0 cannot write to kernel memory directly     |
| `badptr`    | syscalls reject kernel-pointer args (-EFAULT)  |
| `cat /mnt/` | OSFS file read through block cache             |
| `hello`     | round-trip syscall path + cooperative yield    |
| `sh`        | shell prompts, line edit, spawn, wait          |
| `init`      | reaps children, then launches the shell        |

Block cache stats: 19 hits / 76 misses / 12 evictions across the
full trace. The evictions show up because `heaptest` plus a
second pass through `cat` and `hello` finally pushes the working
set above 64 sectors — first time we've seen the cache turn over
in a session.

## What this unlocks

With dynamic allocation in user code, programs can finally build
data structures sized at runtime: hash tables, parse trees, line
buffers that grow, caches. Concretely, the next few things on the
roadmap that would have been awkward without a heap:

- A real `printf` (the format buffer is unbounded).
- A line editor that handles arbitrarily long lines.
- An interpreter or REPL of any kind.
- The HTML tokenizer and DOM that the eventual browser needs to
  build per-page.

## What's still missing

- **`mmap` / file-backed mappings.** `sbrk` is a single linear
  arena; we can't mmap a file or grab a region at a specific VA.
  Comes when the FS grows write support and we want to share
  memory between processes.
- **Lazy allocation.** Today `sbrk(N)` eagerly maps every page.
  Real OSes mark the pages "demand allocate" in the page table
  and only allocate them when first touched. Saves pmem when
  programs reserve more than they actually use.
- **Per-AS lock around brk.** Required as soon as we have SMP or
  preemptible syscalls.
- **Left-neighbour coalescing in `free`.** Cheap to add (a
  doubly-linked free list); deferred until fragmentation
  actually hurts.

## What changed

```
kernel/arch/address_space.{h,c}    heap_brk field, set_brk function,
                                   USER_HEAP_BASE/MAX constants
kernel/core/syscall.{h,c}          SYS_SBRK = 11; sys_sbrk handler
userspace/libc/syscall.h           SYS_SBRK enum + sbrk wrapper
userspace/libc/malloc.h            NEW — header-only malloc/free
userspace/heaptest/heaptest.c      NEW — exercises the allocator
Makefile                           wires heaptest into disk image
```

One new field, one new syscall, one header-only allocator, one
test. Programs can now allocate.
