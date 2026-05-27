# Chapter 7 — Physical memory and the device tree

> **Milestone in this chapter:** 6 — DTB-driven physical-memory map
> + 4 KiB page allocator.
> **Code referenced:** [kernel/core/fdt.c](../../../kernel/core/fdt.c),
> [kernel/core/fdt.h](../../../kernel/core/fdt.h),
> [kernel/core/pmem.c](../../../kernel/core/pmem.c),
> [kernel/core/pmem.h](../../../kernel/core/pmem.h),
> [kernel/arch/page_tables.c](../../../kernel/arch/page_tables.c)
> (`pmap_install_ram_block_1gib`),
> [kernel/core/main.c](../../../kernel/core/main.c)
> (DTB scan + L1 install + heap bring-up),
> [scripts/build_dtb.sh](../../../scripts/build_dtb.sh).
>
> **At the end of this chapter** the kernel discovers all of its
> physical RAM at runtime by parsing the flat device tree (FDT)
> handed to it by the firmware, dynamically installs L1 block
> descriptors so the CPU can reach every page, and feeds the
> resulting region into a 4 KiB page allocator.  Booting with
> `make run QEMU_MEM=8G` (or 16, or 64) Just Works without
> rebuilding the kernel.

## Why this chapter exists

Up to chapter 4 the heap lived in a 16 MiB span the linker
script reserved with `.heap (NOLOAD)`, sitting cleanly inside
the 1 GiB block descriptor that the static `l1_pgtable[]`
already mapped.  That arrangement has two ceilings:

1. The **mapping** ceiling.  Anything beyond 2 GiB physical was
   simply unmapped — accessing it raised an L1 translation
   fault.
2. The **policy** ceiling.  The heap was a fixed 16 MiB chosen
   at link time.  Wanting more required rebuilding the kernel.

This chapter demolishes both ceilings.  We learn the real
memory layout from the firmware-provided device tree, install
mappings to cover every page of it, and hand the result to a
proper page allocator that the heap (and, in later chapters, user
processes) can carve into chunks of any size.

The work splits naturally into three pieces, in this order:

1. Parse the DTB to extract `/memory/reg`.
2. Install 1 GiB block descriptors at runtime so the CPU can
   actually touch the discovered RAM.
3. Hand the mapped range to a frame allocator and re-back the
   heap with whatever it returns.

We will walk each in turn.

## The device tree, briefly

The flat device tree is the boot-firmware-to-kernel handoff
mechanism that the embedded ARM/POWER world standardised on
twenty years ago.  In the QEMU virt machine it lives in RAM at
a fixed address that we can either read out of register `x0`
on entry (the Linux/aarch64 boot protocol) or — when QEMU's
`-kernel ELF` loader bypasses that protocol — at a known
offset we picked ourselves.

Physically, the DTB is a self-contained little blob with three
parts:

* a 40-byte header (magic 0xD00DFEED in big-endian, plus
  offsets),
* a *strings block* containing all the property names back-to-back,
* a *structure block* containing tokens that describe the tree:
  `BEGIN_NODE`, `END_NODE`, `PROP`, `NOP`, `END`.

Every multi-byte field is big-endian on disk regardless of the
host CPU.  We need a byteswap helper and a stack big enough to
track the current depth and the inherited
`#address-cells` / `#size-cells` values; everything else is
straightforward token-driven recursion.

### What we actually need

For this chapter the only property we care about is `reg` on
nodes named `memory` or `memory@<addr>`.  Its layout is
`<addr-cells × address> <size-cells × size>` — on the QEMU
virt machine both values are 2, so each entry is four 32-bit
big-endian words encoding a 64-bit base and a 64-bit size.

A complete `/memory` node from `dtc -I dtb -O dts assets/virt.dtb`
looks like

```
memory@40000000 {
        reg = <0x00 0x40000000 0x02 0x00>;
        device_type = "memory";
};
```

— meaning "RAM begins at `0x0000000040000000` and runs for
`0x0000000200000000` bytes" (8 GiB).

### A 200-line FDT walker

[kernel/core/fdt.c](../../../kernel/core/fdt.c) implements
exactly the slice of libfdt we need — no allocations, no
copies, no dependency on the rest of the kernel beyond a tiny
serial-print fallback for diagnostics:

```c
int     fdt_validate(const void *blob, uint32_t *total_size);
size_t  fdt_read_memory(const void *blob, struct fdt_memory_map *out);
```

The walker's main loop is a simple token machine:

```c
while (cursor < end) {
    uint32_t token = read_be32(cursor);  cursor += 4;

    switch (token) {
    case FDT_BEGIN_NODE: /* push depth, read name, descend */
    case FDT_END_NODE:   /* pop depth */
    case FDT_PROP:       /* parse_prop, optionally remember */
    case FDT_NOP:        break;
    case FDT_END:        return out->count;
    default:             /* malformed — bail */
    }
}
```

`#address-cells` and `#size-cells` are tracked as inherited
attributes via parallel stacks indexed by depth.  When a
`PROP` token announces these names on a node, we update the
*current* depth's slot — children inherit on `BEGIN_NODE`.

There is one easy mistake worth flagging: the depth counting.
The root node's `BEGIN_NODE` increments the depth from 0 to 1,
so the root sits at depth 1 and its children at depth 2.  An
earlier version of this walker checked `depth == 1` for
`memory@...` and quietly never fired.  Real kernels (Linux,
*BSD) use the same convention; if you're cross-checking
behaviour, remember to count the root.

### Why a custom walker rather than libfdt

libfdt is a fine library, but it pulls in standard-libc
expectations (memcpy, strcmp, errno) that we have no
freestanding equivalents for yet.  The whole point of this
chapter is one focused task — read `/memory` — and writing it
ourselves keeps the kernel free of any dependency we did not
deliberately add.  Once we want full pretty-printing, alias
resolution, mutable-DTB editing, etc., libfdt will earn its
keep.  That is not now.

## Sidebar — getting a DTB out of QEMU

QEMU's `-kernel ELF` boot path on aarch64 has one quirk that
trips up every first-time DTB build: it does *not*
follow the Linux boot protocol.  It loads the ELF and jumps to
the entry point with `x0 = 0`, not `x0 = DTB_phys`.

The fix is to load a DTB ourselves at a known address.
[scripts/build_dtb.sh](../../../scripts/build_dtb.sh) extracts one
from QEMU itself by running `qemu-system-aarch64 -M virt,...,
dumpdtb=assets/virt.dtb -kernel /dev/null` — the `/dev/null`
"kernel" fails to load, but only after `dumpdtb` has captured
the same DTB QEMU would normally hand to a real kernel.

We then load it at `0x44000000` with

```
-device loader,file=assets/virt.dtb,addr=0x44000000
```

(safely above the kernel image at `0x40080000` and below where
the page allocator will start handing out frames) and have
kernel_main fall back to that constant when `x0 == 0`.

### A small DTB-dump pitfall

`dumpdtb` only emits a `/memory` node when QEMU's *Linux loader*
runs, which it does for `-kernel /dev/null` (a BFB attempt that
fails harmlessly) but does not for `-kernel build/kernel.elf`
(succeeds via the ELF path before populating memory regions).
If you ever wonder why your DTB looks fine in `dtc` but the
`/memory` node is absent, check what kernel argument was used
for the dump.

## Mapping the discovered RAM

Knowing where RAM lives is half the problem.  The other half
is making the CPU able to touch it.  Before this chapter the
kernel had a static L1 page table with two block descriptors:

```c
[0] = BLOCK_DEVICE(0x00000000UL),   /* MMIO            */
[1] = BLOCK_NORMAL(0x40000000UL),   /* RAM 1\u20132 GiB    */
```

Anything above `0x80000000` raised an L1 translation fault.
We keep L1[0] and L1[1] hardcoded — the kernel image,
boot stack, page tables, and the DTB-load address all live in
that 1 GiB window, so the mapping must be live before C even
starts — and adds a runtime helper for everything else:

```c
void pmap_install_ram_block_1gib(uint64_t pa)
{
    if (pa & ((1ULL << 30) - 1)) return;     /* not 1 GiB-aligned */
    uint64_t idx = pa >> 30;
    if (idx >= 512) return;                  /* beyond 39-bit VA */
    l1_pgtable[idx] = BLOCK_NORMAL(pa);

    __asm__ volatile(
        "dsb ishst        \n"
        "tlbi vmalle1is   \n"
        "dsb ish          \n"
        "isb              \n"
        ::: "memory");
}
```

Three things deserve notice:

1. **The barrier sequence**.  `dsb ishst` publishes the
   page-table write to the inner-shareable domain so the next
   table walk sees it.  `tlbi vmalle1is` invalidates every EL1
   TLB entry across all CPUs in the IS domain.  `dsb ish`
   waits for the invalidation to drain.  `isb` resyncs the
   instruction stream so subsequent loads observe the new
   translation.  Skip any one and you can spend an afternoon
   debugging "this works on the second access but not the
   first."

2. **Idempotency**.  Calling the helper for L1[1] is a no-op
   in steady state but architecturally legal — we overwrite an
   already-correct entry, then flush.  That makes the
   kernel_main loop simpler: it can iterate every 1 GiB chunk
   the DTB reports without special-casing the boot range.

3. **No splitting**.  Sixteen 1 GiB block descriptors is
   already enough for 16 GiB of RAM.  The 39-bit VA gives us
   512 such slots, which means we are good up to 512 GiB
   without ever growing a level-2 table.  Real OSes
   eventually split blocks into 2 MiB or 4 KiB pages for finer
   permission control; we will do that in chapter 8 once we
   actually need page-granularity protection.

The driver code in `kernel_main` is then just:

```c
const uint64_t GIB = 1ULL << 30;
for (size_t i = 0; i < mem.count; i++) {
    uint64_t base = mem.regions[i].base & ~(GIB - 1);
    uint64_t end  = mem.regions[i].base + mem.regions[i].size;
    for (uint64_t pa = base; pa < end; pa += GIB)
        pmap_install_ram_block_1gib(pa);
}
```

Verified output for `make run QEMU_MEM=16G`:

```
[fdt] memory[0] base = 0x40000000, size = 0x400000000
[pmap] installed 0x10 x 1 GiB RAM block descriptor(s)
```

— sixteen blocks, exactly matching `0x400000000 / GIB`.

## The page allocator

With the mapping problem solved, the policy problem is small.
[kernel/core/pmem.c](../../../kernel/core/pmem.c) is an
in-band-freelist allocator: each free 4 KiB page stores a
pointer to the next free page in its first 8 bytes, the global
`g_free_head` is the head, alloc peels off the front, free
pushes onto the front.  Total cost per operation: two memory
references and a counter update.  No bitmap, no per-region
metadata, no fragmentation worries because we only ever hand
out and accept whole 4 KiB frames.

```c
static void push_free(uint64_t pa)
{
    *(uint64_t *)(uintptr_t)pa = g_free_head;
    g_free_head = pa;
    g_free_pages++;
}

uint64_t pmem_alloc_page(void)
{
    if (!g_free_head) return 0;
    uint64_t pa  = g_free_head;
    g_free_head  = *(uint64_t *)(uintptr_t)pa;
    g_free_pages--;
    /* Zero on hand-out so callers don't see freelist links. */
    uint64_t *p = (uint64_t *)(uintptr_t)pa;
    for (size_t i = 0; i < PAGE_SIZE / 8; i++) p[i] = 0;
    return pa;
}
```

The interesting part is initialisation.  We walk every
DTB-reported region, round the bounds to page boundaries,
and push each page onto the freelist *unless* it overlaps a
**carve-out**:

```c
struct pmem_carveout {
    uint64_t base;
    uint64_t size;
};
```

[kernel/core/main.c](../../../kernel/core/main.c) builds the
carve-out list from three sources:

1. **The kernel image**, from `KERNEL_LOAD_ADDR` (= 0x40080000)
   up to the linker symbol `kernel_end`, padded to the next
   4 KiB boundary.
2. **The DTB itself**, padded to a page.  We are still reading
   from it during init, so the allocator must not hand its
   pages out.
3. **The first 1 GiB**, defensively excluded because that
   range is MMIO on virt; the DTB does not report it as
   memory but a future board might, and a carve-out costs
   nothing.

Nothing else has to be excluded — the early boot
stack, the static page table itself, the heap region (none of
those exist yet at this point in boot, *or* they are inside
the kernel-image carve-out).

A plain run on 8 GiB reports

```
[pmem] usable pages = 0x1ffeea (= 0x7ffba8 KiB)
```

— ≈ 8 191 MiB, the difference from a clean 8 192 MiB being
exactly the kernel-image and DTB carve-outs.

## Re-backing the heap

The heap allocator from chapter 12 does not care where its
region lives, only that the region is contiguous physical RAM
that the CPU can touch.  We change `kheap_init`'s signature to
take `(base, size)` and feed it a slab of pages that pmem
hands out:

```c
const size_t HEAP_BYTES = 256ULL * 1024 * 1024;
const size_t HEAP_PAGES = HEAP_BYTES / PAGE_SIZE;
uint64_t heap_first = pmem_alloc_page();
uint64_t expected   = heap_first - PAGE_SIZE;
for (size_t i = 1; i < HEAP_PAGES; i++) {
    uint64_t got = pmem_alloc_page();
    if (got != expected) panic("non-contiguous page");
    expected -= PAGE_SIZE;
}
uint64_t heap_base = heap_first - (HEAP_PAGES - 1) * PAGE_SIZE;
kheap_init(heap_base, HEAP_BYTES);
```

The contiguity check works because pmem hands out pages in
reverse-push order: on a freshly-initialised pool that means
"the highest address first, decrementing by 4 KiB each call".
We allocate `HEAP_PAGES` consecutive frames, verify the
arithmetic, and treat them as one block for the heap's
implicit-list allocator.

Sized at 256 MiB, the heap can host the kernel-side bookkeeping
for dozens of user processes without ever revisiting kheap.
Those user processes' actual memory — chapter 13's
ELF-loaded code, chapter 14's user heap — will come straight
from `pmem_alloc_page()`, bypassing kheap entirely.  The
chapter-13 heap is for kernel data structures only.

## Tunable RAM size

The DTB and the L1 mapping path are both DTB-driven, so
changing how much RAM the kernel sees is purely a host-side
exercise.  The Makefile exposes:

```
QEMU_MEM ?= 8G
```

— overridable on the command line:

```
$ QEMU_MEM=16G scripts/build_dtb.sh    # rebuild DTB to match
$ make run QEMU_MEM=16G
```

The kernel does not need to be rebuilt.  Verified output:

```
[fdt] memory[0] base = 0x40000000, size = 0x400000000
[pmap] installed 0x10 x 1 GiB RAM block descriptor(s)
[pmem] usable pages = 0x3ffeea (= 0xfffba8 KiB)
initialising kernel heap (0x10000000 bytes @ 0x430000000) ... ok
```

— 16 mappings, ≈ 16 GiB usable, heap living at 17 GiB physical.

## What chapter 8 will (eventually) do

The current kernel runs entirely out of TTBR0 with identity
mapping.  That works fine for now, but there are two reasons
real OSes split the address space:

1. **Per-process page tables.**  Each user process needs its
   own TTBR0 with code/data mapped at low addresses.  If the
   kernel also lives in TTBR0 we have to copy the kernel
   mappings into every process's page tables.
2. **Address-space layout discipline.**  Putting the kernel in
   TTBR1 (high half, addresses with all the upper bits set)
   gives a clean separation: low addresses = "this process",
   high addresses = "the kernel".  A null pointer dereference
   in user code becomes obviously unmapped.

Chapter 8 covers the higher-half port — moving the kernel's
virtual addresses to a TTBR1-only region, splitting the linker
script, and updating the boot path to set up both translation
regimes at once.  It is a self-contained refactor that we will
land when we have a concrete reason to want it (most likely
during the MMIO-mapping cleanup, when we start
caring about per-device address-space hygiene).

## Checkpoint

A build with the default 8 GiB target should
produce, in order:

* the early banner output,
* an `[fdt] memory[0]` line confirming the DTB was found,
* a `[pmap] installed 0x8 x 1 GiB` line,
* a `[pmem] usable pages = …` summary,
* the heap smoke test,
* the preemption demo, and
* the heartbeat tick.

That cascade — DTB → mapping → frames → heap → threads →
preemption → ticks — is the full kernel-bootstrap pipeline
that everything from chapter 13 onward will build on top of.
