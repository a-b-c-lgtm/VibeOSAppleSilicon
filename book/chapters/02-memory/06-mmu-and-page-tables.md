# Chapter 6 — The MMU, translation tables, and MAIR

> **Milestone in this chapter:** 1 — vectors + MMU.
> **Code referenced:** [kernel/arch/mmu.S](../../../kernel/arch/mmu.S),
> [kernel/arch/page_tables.c](../../../kernel/arch/page_tables.c),
> [kernel/arch/boot.s](../../../kernel/arch/boot.s),
> [kernel/core/serial.c](../../../kernel/core/serial.c).
>
> **At the end of this chapter** you will have an MMU configured to
> identity-map the lower 2 GiB of physical address space using two
> L1 block descriptors, with `[0, 1 GiB)` mapped Device-nGnRnE for
> MMIO and `[1 GiB, 2 GiB)` mapped Normal Inner+Outer Write-Back
> Write-Allocate Inner-Shareable for kernel RAM. After
> `mmu_enable` returns, every C function — including `stp/ldp`
> register spills — runs against properly-attributed memory.

## What "MMU on" buys us

Before the MMU is on, the architecture treats every byte of RAM
as Device-nGnRnE memory. Three things are forbidden in that
state:

1. **`stp/ldp` against Device memory** is constrained-unpredictable
   (chapter 3). Any non-leaf C prologue uses `stp x29, x30`, so we
   are limited to leaf-only call chains.
2. **Cacheable accesses** are not just disabled, they are illegal.
   The data and instruction caches do not even snoop coherently
   with each other, so a self-modifying-code-style sequence
   (uncommon, but not unheard of) silently fails.
3. **Speculative reads** are heavily restricted. The CPU cannot
   prefetch anything useful, so even straight-line code runs at a
   fraction of native speed.

Switching the MMU on with appropriate `MAIR_EL1` and `TCR_EL1`
values turns a cold, slow, fragile kernel environment into a fast
one where the C compiler can emit whatever it wants.

## The translation regime we choose

AArch64 supports several combinations of:

- **Granule size:** 4 KiB, 16 KiB, or 64 KiB.
- **Virtual address size:** 25–48 bits (ARMv8.0; up to 52 with
  FEAT_LPA2).
- **Number of translation table levels:** 0 to 3.
- **TTBR splitting:** TTBR0 alone, TTBR1 alone, or both (the
  classic split: TTBR0 = low half = userspace, TTBR1 = high half
  = kernel).

Milestone 1 picks the simplest combination that works:

| Choice                     | Value         | Why                                                      |
|----------------------------|---------------|----------------------------------------------------------|
| Granule                    | 4 KiB         | Standard everywhere; matches Linux/macOS guests          |
| Virtual address size       | 39 bits       | Two levels of lookup with 4 KiB granule                  |
| Number of levels           | 2 (L1 + L0?)  | 39-bit VA at 4 KiB ⇒ start at L1, 1 GiB blocks at L1     |
| TTBR0 only                 | yes           | TTBR1 disabled (`TCR_EL1.EPD1 = 1`)                      |
| Block descriptors          | L1            | Each L1 entry covers 1 GiB, no L2/L3 table needed        |

A 39-bit VA at 4 KiB granule has the following decomposition:

- Bits 38:30 (9 bits) → L1 index, 512 entries, each covering 1 GiB
- Bits 29:21 (9 bits) → L2 index, 512 entries, each covering 2 MiB
- Bits 20:12 (9 bits) → L3 index, 512 entries, each covering 4 KiB
- Bits 11:0  → page offset (12 bits)

Because we can stop at L1 with block descriptors (each describing
a 1 GiB region), we need *one* L1 table — 512 entries × 8 bytes =
4 KiB — and that is the entire page-table memory cost of milestone
1. We map two 1 GiB regions (entries `[0]` and `[1]`) and leave the
other 510 entries zero, which causes `FAR ≥ 0x80000000` to take a
translation fault — exactly what `DEMO_FAULT` exercised in chapter
5.

## MAIR: how memory types live in the descriptor

A page-table descriptor does not encode a memory type directly.
Instead it carries a 3-bit *attribute index* (`AttrIndx`) into a
table called `MAIR_EL1` (Memory Attribute Indirection Register).
`MAIR_EL1` is 64 bits wide — eight 8-bit *attribute slots* —
and a descriptor's `AttrIndx` selects which slot applies to that
page or block.

We use two slots:

| Slot | Encoding | Memory type                                          |
|------|----------|------------------------------------------------------|
| 0    | 0xFF     | Normal, Inner WB-WA, Outer WB-WA, non-transient      |
| 1    | 0x00     | Device-nGnRnE                                        |

A Normal Inner+Outer Write-Back Write-Allocate slot reads as
`0xFF` because each nibble of the 8-bit slot encodes one of the
two cacheabilities, and the encoding for "WB read-allocate write-
allocate non-transient" is `0xF` per side (high nibble outer, low
nibble inner). Device-nGnRnE — "non-Gathering, non-Reordering,
no-Early-write-acknowledge" — is `0x00`. (The `MAIR_EL1` encoding
table in the ARM ARM is one of the more confusing in the
architecture; the two values above are the only ones we will use
until chapter 22.)

The full register value is therefore:

```
MAIR_EL1 = (0xFF <<  0)   // slot 0: Normal Cacheable
         | (0x00 <<  8)   // slot 1: Device-nGnRnE
         | (0    << 16)   // slots 2..7: zero (unused)
         | ...
         = 0x00000000_000000FF
```

`mmu.S` writes that constant directly with `mov x1, #0xFF; msr
MAIR_EL1, x1`.

## Block descriptors

A 1 GiB L1 block descriptor packs:

| Bits   | Field          | Our value              | Meaning                         |
|--------|----------------|------------------------|---------------------------------|
| 0      | Valid          | 1                      | Entry is in use                  |
| 1      | TableOrBlock   | 0                      | Block (not a table pointer)      |
| 4:2    | AttrIndx       | 0 or 1                 | Index into MAIR (Normal or Dev)  |
| 5      | NS             | 0                      | Non-Secure bit (we are S/NS-blind under HVF) |
| 7:6    | AP             | 0b00                   | Read/write at EL1, no EL0 access |
| 9:8    | SH             | 0b11 (Normal) / 0 (Dev)| Inner Shareable for Normal       |
| 10     | AF             | 1                      | Access flag pre-set              |
| 11     | nG             | 0                      | Global (not ASID-tagged)         |
| 47:30  | Output addr    | physical >> 30         | Block base, 1 GiB-aligned        |
| 53     | PXN            | 0                      | Privileged execute-never (off)   |
| 54     | UXN            | 0                      | Unprivileged execute-never (off) |

The two macros in `kernel/arch/page_tables.c` build that pattern:

```c
#define DESC_BLOCK_L1   (1ULL << 0)   // valid + block type
#define ATTR_AF         (1ULL << 10)
#define ATTR_SH_INNER   (3ULL << 8)
#define ATTR_AP_RW_EL1  (0ULL << 6)
#define ATTR_IDX(n)     ((uint64_t)(n) << 2)

#define BLOCK_NORMAL(pa) ((pa) | ATTR_AF | ATTR_SH_INNER | \
                          ATTR_AP_RW_EL1 | ATTR_IDX(0) | DESC_BLOCK_L1)
#define BLOCK_DEVICE(pa) ((pa) | ATTR_AF | ATTR_AP_RW_EL1 | \
                          ATTR_IDX(1) | DESC_BLOCK_L1)
```

Note that Device blocks deliberately do *not* set `SH_INNER` —
shareability is unused for Device memory; the architecture says
all Device accesses are inherently outer-shareable. Setting
`SH_INNER` on a Device block is reserved-zero in some
architectures, so we leave it off.

The L1 table itself is one 4 KiB-aligned array of 512 64-bit
descriptors:

```c
__attribute__((aligned(4096), section(".data.pgtables")))
uint64_t l1_pgtable[512] = {
    [0] = BLOCK_DEVICE(0x00000000),  // [0, 1 GiB) MMIO
    [1] = BLOCK_NORMAL(0x40000000),  // [1 GiB, 2 GiB) RAM
};
```

Putting it in its own section, `.data.pgtables`, is a habit we
will lean on in chapter 8 when we add a TTBR1 table and
chapter 13 when we add per-process page tables — keeping page-
table storage easy to find in the linker map matters as soon as
there is more than one of them.

## TCR_EL1: the translation control word

`TCR_EL1` is the configuration register that tells the MMU
*everything* about the translation regime: how many bits of VA,
which granule, whether to walk TTBR1 at all, what cacheability to
use for page-table walks, and so on. It is a long, bit-packed
register and the only one in this milestone whose value we have to
hand-compute.

For our regime (39-bit VA, 4 KiB granule, TTBR0 only, walks are
Normal Inner+Outer WB Inner-Shareable, ASID size 8 bits), the
fields that matter are:

| Bits   | Field   | Value    | Meaning                                            |
|--------|---------|----------|----------------------------------------------------|
| 5:0    | T0SZ    | 25       | TTBR0 covers 2^(64-25) = 2^39 bytes = 512 GiB      |
| 7      | EPD0    | 0        | Walk TTBR0 (do not disable)                         |
| 9:8    | IRGN0   | 0b01     | Normal Inner WBWA, write-allocate                  |
| 11:10  | ORGN0   | 0b01     | Normal Outer WBWA, write-allocate                  |
| 13:12  | SH0     | 0b11     | Inner Shareable                                    |
| 15:14  | TG0     | 0b00     | 4 KiB granule for TTBR0                            |
| 22:16  | T1SZ    | 25       | (set, but `EPD1=1` makes it inert)                  |
| 23     | EPD1    | 1        | Disable TTBR1 walks entirely                       |
| 25:24  | IRGN1   | 0b01     | (set, but inert under EPD1)                         |
| 27:26  | ORGN1   | 0b01     | (set, but inert under EPD1)                         |
| 29:28  | SH1     | 0b11     | (set, but inert under EPD1)                         |
| 31:30  | TG1     | 0b10     | 4 KiB granule for TTBR1 — *different encoding from TG0* |
| 34:32  | IPS     | 0b101    | 48-bit physical address space                      |
| 36     | AS      | 0        | 8-bit ASID                                         |
| 37     | TBI0    | 0        | Top byte not ignored on TTBR0                       |
| 38     | TBI1    | 0        |                                                    |

A subtlety the table makes explicit: `TG0` and `TG1` use *different
encodings* for the same granule sizes. `TG0 = 0b00` and `TG1 =
0b10` both mean "4 KiB". This is one of the ARM ARM's all-time
gotchas; mistyping TG1 as `0b00` selects "reserved" and most CPUs
treat it as 16 KiB. Even though `EPD1 = 1` would mask the bug for
us today, we set TG1 correctly so that lighting up TTBR1 in
chapter 8 is a one-line change rather than a hunting expedition.

Plug each value into its field and you get the constant we use:

```
TCR_EL1 = 0x00000005_B5993519
        = (25  <<  0)   // T0SZ
        | (1   <<  8)   // IRGN0[0]
        | (1   << 10)   // ORGN0[0]
        | (3   << 12)   // SH0
        | (0   << 14)   // TG0  = 4 KiB
        | (25  << 16)   // T1SZ
        | (1   << 23)   // EPD1
        | (1   << 24)   // IRGN1[0]
        | (1   << 26)   // ORGN1[0]
        | (3   << 28)   // SH1
        | (2   << 30)   // TG1  = 4 KiB (note: 0b10, not 0b00!)
        | (5ULL << 32); // IPS  = 48-bit PA
```

The constant is too wide for a single `mov` immediate, so `mmu.S`
loads it from a literal pool with `ldr x1, =0x00000005B5993519`.
Apple Silicon supports a 48-bit physical address space, which is
why we set IPS to its widest value — even though the kernel only
addresses the first 2 GiB today, picking the largest IPS lets us
expand without revisiting TCR.

## The bring-up sequence

`mmu_enable` in `kernel/arch/mmu.S` is pure assembly — there is
no opportunity for it to use the stack, which means there is no
opportunity for it to suffer the milestone-0 `stp/ldp`-on-Device
problem during the very moments the MMU is being lit up. The
sequence is:

```asm
    .global mmu_enable
mmu_enable:
    // x0 = physical address of the L1 table

    mov     x1, #0xFF
    msr     MAIR_EL1, x1            // slot 0 = Normal, slot 1 = Device

    ldr     x1, =0x00000005B5993519 // TCR_EL1 (literal pool)
    msr     TCR_EL1, x1

    msr     TTBR0_EL1, x0           // page-table base
    msr     TTBR1_EL1, xzr          // TTBR1 unused
    isb

    tlbi    vmalle1                  // invalidate all stage-1 TLBs at EL1
    dsb     ish                       // wait for the invalidate
    isb

    mrs     x1, SCTLR_EL1
    orr     x1, x1, #(1 <<  0)       // M  = MMU enable
    orr     x1, x1, #(1 <<  2)       // C  = D-cache enable
    orr     x1, x1, #(1 << 12)       // I  = I-cache enable
    msr     SCTLR_EL1, x1
    isb                              // serialise: next fetch sees MMU on
    ret
```

A few notes on what each barrier does:

- The `isb` after `msr TTBR0_EL1, x0` is required by the
  architecture before any TLB maintenance, because TLB
  maintenance is allowed to assume the new TTBR is in effect.
- `tlbi vmalle1` invalidates every TLB entry for stage 1 at EL1.
  We have not set anything in the TLB yet — QEMU starts with the
  TLB empty — but the architecture says you must invalidate
  before the first MMU enable, so we do.
- `dsb ish` ("data synchronisation barrier, inner shareable")
  waits for the TLB invalidate to complete and propagates it to
  any other PEs in the inner shareable domain. We have only one
  CPU, but the barrier is still required.
- The `isb` at the very end is what makes the *next instruction*
  fetch through the new translation regime. Without it, the `ret`
  could legally fetch through the old (no-MMU) view and behave
  unpredictably. With it, `ret` is fetched through the freshly-
  enabled MMU; the kernel's identity mapping covers the same PA
  the CPU was already at, so execution continues uninterrupted.

The boot stub (`kernel/arch/boot.s`) calls `mmu_enable` with the
L1 table's *physical* address in `x0`:

```asm
    adrp    x0, l1_pgtable
    add     x0, x0, :lo12:l1_pgtable
    bl      mmu_enable
```

Because we are running with the MMU off when this runs, "physical
address" and "linker-computed address" are the same thing. After
`mmu_enable` returns, we are running with the MMU on but in an
identity-mapped world, so the values are *still* the same. This
is why the identity map is so useful for bring-up: it lets you
turn the MMU on without changing a single pointer.

## What we just unlocked

Three concrete things now work that did not before:

1. **`serial_puthex(dtb_phys)`** prints a clean hex address. Before
   the MMU was on, GCC's prologue would `stp x29, x30, [sp, #-N]!`
   against Device memory and HVF would abort.
2. **The vector table can be reached.** A fault in `kernel_main`
   takes a translation fault (or whatever) and lands in the slot
   we built in chapter 5; the C panic handler runs against
   Normal-Cacheable memory and produces a clean dump.
3. **The compiler is unconstrained.** From this point on we can
   write ordinary C — recursion, arrays of structs on the stack,
   `memcpy`, anything — without thinking about which addressing
   modes GCC might emit. The Device-memory writeback rule from
   chapter 3 still binds *MMIO accesses*, but the helpers in
   `serial.c` (`mmio_read32`, `mmio_write32`) handle that uniformly
   and every later device driver in this book will use the same
   pattern.

## What chapter 7 adds

Identity-mapping the lower 2 GiB is a useful fixture for
milestone 1, but it is not a real memory map. Real memory
management needs to: discover physical RAM (which means parsing
the device tree QEMU handed us in `x0`), track which page frames
are free, build a higher-half mapping so the kernel lives at the
top of the address space, and switch over to it. Chapters 7 and 8
do exactly that, in order, and finally retire the identity map.
