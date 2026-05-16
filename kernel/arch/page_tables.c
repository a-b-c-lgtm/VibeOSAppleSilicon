/* page_tables.c — Static level-1 translation table for milestone-1 MMU.
 *
 * Layout (39-bit virtual address space, 4 KiB granule):
 *   L1[0]    covers VA [0, 1 GiB) — Device-nGnRnE block, identity
 *            mapped. Contains all of QEMU virt's MMIO: GIC
 *            distributor (0x08000000), GIC redistributor
 *            (0x080A0000), PL011 UART (0x09000000), virtio-mmio bus
 *            (0x0A000000-0x0A003E00), GPIO/RTC (0x09010000), etc.
 *
 *   L1[1]    covers VA [1 GiB, 2 GiB) — Normal-Inner-Outer-WB-WA-RA
 *            Inner-Shareable block, identity mapped.  Hardcoded
 *            because the kernel image, boot stack, page tables, and
 *            the DTB load address (0x44000000) all live here; we
 *            need this mapping live before C even starts, so we
 *            cannot rely on the dynamic path below to install it.
 *
 *   L1[2..N] installed AT RUNTIME by pmap_install_ram_block_1gib(),
 *            called from kernel_main once the DTB has been parsed
 *            and the actual physical-RAM map is known.  Each call
 *            programs one 1 GiB block descriptor with the same
 *            Normal-Cacheable Inner-Shareable attributes as L1[1],
 *            then flushes the TLB and re-syncs the I-stream.
 *
 *            This lets `qemu-system-aarch64 -m 8G` (or 16G, or
 *            anything below the 39-bit ceiling = 512 GiB) Just
 *            Work without rebuilding the kernel.
 *
 *   Slots not installed remain zero — descriptor type [1:0] = 00 =
 *   invalid — and any access to them raises a translation fault at
 *   level 1, caught by handle_sync in kernel/arch/vectors.S and
 *   reported by the panic handler.
 *
 * Block descriptor format at L1 with 4 KiB granule (ARM ARM D5.3.1):
 *   bits  [1:0]  = 0b01     (block descriptor at L1)
 *   bit       2  = AttrIdx[0]
 *   bit       3  = AttrIdx[1]
 *   bit       4  = AttrIdx[2]      AttrIdx selects the slot in MAIR_EL1
 *   bit       5  = NS              (Non-Secure; 0 = secure access)
 *   bits  [7:6]  = AP[2:1]         (00 = RW EL1, 01 = RW EL1+EL0)
 *   bits  [9:8]  = SH              (11 = Inner Shareable)
 *   bit      10  = AF              (Access Flag; 1 to suppress AF fault)
 *   bit      11  = nG              (non-Global; 0 = global TLB entry)
 *   bits [47:30] = output address bits 47:30 (1 GiB-aligned for L1 block)
 *   bit      53  = PXN             (Privileged eXecute Never; 0 = exec OK)
 *   bit      54  = UXN             (Unprivileged eXecute Never; 0 = OK)
 *
 * Why one big RWX block for kernel RAM?
 *   We will refine this in chapter 9 ("higher-half kernel and TTBR1")
 *   when we split .text (RX), .rodata (RO), and .data/.bss (RW-XN)
 *   into proper L2 tables. For milestone 1 a single RWX block is
 *   correct, simpler, and lets us focus on getting the MMU itself
 *   right. Real OSes do exactly this during early boot too. */

#include <stdint.h>

/* MAIR_EL1 attribute slots, programmed by mmu_enable in
 * kernel/arch/mmu.S:
 *   slot 0: 0xFF — Normal-Inner-Outer-WB-WA-RA cacheable
 *   slot 1: 0x00 — Device-nGnRnE                              */
#define MAIR_NORMAL          0u
#define MAIR_DEVICE          1u

#define DESC_BLOCK_L1        0x1ULL          /* low two bits = 01     */
#define DESC_AF              (1ULL << 10)
#define DESC_SH_INNER        (3ULL << 8)
#define DESC_AP_RW_EL1       (0ULL << 6)
#define DESC_AP_RW_EL1_EL0   (1ULL << 6)    /* AP[2:1]=01: RW EL1+EL0 */
#define DESC_ATTR_IDX(i)     (((uint64_t)(i)) << 2)

/* Kernel-only RAM block (used for L1[1] which contains the kernel
 * image, boot stack, and page tables themselves).  Keeping this
 * EL1-only is hygienic — user code has no business touching the
 * kernel image. */
#define BLOCK_NORMAL(addr) \
    ((uint64_t)(addr) | DESC_AF | DESC_SH_INNER | \
     DESC_AP_RW_EL1   | DESC_ATTR_IDX(MAIR_NORMAL) | DESC_BLOCK_L1)

/* User-accessible RAM block (used for L1[2..N] which is what pmem
 * carves up for user-mode allocations).  AP=RW for both EL1 and
 * EL0 so we can synthesise EL0 access without per-process L2
 * tables yet.  This is permissive: any user process can read or
 * write any other process's memory.  Chapter 14 fixes that with
 * per-process L1 tables that grant EL0 only on the specific 4 KiB
 * pages a process owns. */
#define BLOCK_NORMAL_USER(addr) \
    ((uint64_t)(addr) | DESC_AF | DESC_SH_INNER | \
     DESC_AP_RW_EL1_EL0 | DESC_ATTR_IDX(MAIR_NORMAL) | DESC_BLOCK_L1)

#define BLOCK_DEVICE(addr) \
    ((uint64_t)(addr) | DESC_AF | \
     DESC_AP_RW_EL1   | DESC_ATTR_IDX(MAIR_DEVICE) | DESC_BLOCK_L1)

/* The table itself: 512 × 8 B = 4 KiB, aligned to 4 KiB so TTBR0_EL1
 * can point at it directly. `used` keeps the linker from culling it
 * if static analysis ever decides nothing references the symbol. */
__attribute__((aligned(4096), used, section(".data.pgtables")))
uint64_t l1_pgtable[512] = {
    [0] = BLOCK_DEVICE(0x00000000UL),   /* MMIO range:    [0, 1 GiB)        */
    [1] = BLOCK_NORMAL(0x40000000UL),   /* RAM:           [1 GiB, 2 GiB)    */
    /* Slots 2..511 left zero; pmap_install_ram_block_1gib() fills
     * them in at runtime once the DTB-reported memory map is known. */
};

/* Install (or overwrite) the L1 block descriptor that identity-maps
 * the 1 GiB-aligned physical region starting at `pa`.  Idempotent.
 * After the write, the TLB is invalidated for the EL1 ASID so the
 * next access to the new VA range sees the fresh entry, and an isb
 * resyncs the instruction stream.
 *
 * Safe to call from C with the MMU on: the page table itself lives
 * in .data.pgtables, which sits inside the kernel image and is
 * therefore covered by the L1[1] mapping that was active at boot. */
void pmap_install_ram_block_1gib(uint64_t pa)
{
    if (pa & ((1ULL << 30) - 1))
        return;                  /* not 1 GiB-aligned — refuse silently */
    uint64_t idx = pa >> 30;
    if (idx >= 512)
        return;                  /* beyond 39-bit VA */
    /* L1[1] holds the kernel image (.text/.data/.bss/stack/page
     * tables themselves) and stays kernel-only.  Any attempt to
     * make it EL0-accessible would let user code clobber the
     * kernel; refuse silently so the dynamic-install loop in
     * kernel_main can naively iterate over every 1 GiB chunk
     * the DTB reports without special-casing the kernel block. */
    if (idx <= 1)
        return;
    /* As of milestone 16 these DRAM identity blocks are kernel-only
     * (BLOCK_NORMAL, AP=00).  Before milestone 15, user binaries
     * linked at a fixed VA inside one of these blocks and relied on
     * EL0-RW; with per-process address spaces (ch. 24) user pages
     * live in slot 64 with proper page-granularity AP, so the
     * identity DRAM range no longer needs to be exposed to EL0.
     * This closes the isolation hole where any EL0 code could
     * write anywhere in DRAM — including the kernel heap — just
     * because that DRAM happened to be 1 GiB-block identity-mapped. */
    l1_pgtable[idx] = BLOCK_NORMAL(pa);

    __asm__ volatile(
        "dsb ishst        \n"    /* publish the page-table write    */
        "tlbi vmalle1is   \n"    /* invalidate all EL1 TLB entries  */
        "dsb ish          \n"    /* wait for invalidation to drain  */
        "isb              \n"    /* resync I-stream                 */
        ::: "memory");
}
