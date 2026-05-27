/* mmu.S — bring up the AArch64 MMU at EL1.
 *
 * Entry: x0 = physical address of the L1 translation table
 *             (4 KiB aligned; built statically by page_tables.c).
 *
 * Programs MAIR_EL1, TCR_EL1, and TTBR0_EL1 from the supplied table,
 * invalidates the TLBs, and finally sets SCTLR_EL1.{M,C,I} to enable
 * translation and the data + instruction caches.
 *
 * Pure assembly: no stack frame, no calls, no callee-saves. This
 * routine runs while the MMU is still off, and on Apple Silicon
 * HVF that means stp/ldp instructions against the stack would fault
 * with ESR.ISV=0, asserting the host. The C compiler cannot
 * guarantee absence of stp/ldp, so the code that brings up the MMU
 * must be hand-written. After mmu_enable returns, the MMU is on,
 * Normal-Cacheable mappings are in place, and full C is safe.
 *
 * MAIR_EL1 attribute layout:
 *   slot 0: 0xFF — Normal-Inner-Outer-WB-WA-RA cacheable
 *   slot 1: 0x00 — Device-nGnRnE
 *
 * TCR_EL1 layout (bit positions per ARM ARM D7.2.99):
 *   T0SZ   [5:0]    = 25         39-bit VA at TTBR0
 *   IRGN0  [9:8]    = 01         Inner WB-WA cacheable
 *   ORGN0  [11:10]  = 01         Outer WB-WA cacheable
 *   SH0    [13:12]  = 11         Inner Shareable
 *   TG0    [15:14]  = 00         4 KiB granule
 *   T1SZ   [21:16]  = 25         39-bit VA at TTBR1 (currently unused)
 *   EPD1   [23]     = 1          disable TTBR1 walks (no userspace yet)
 *   IRGN1  [25:24]  = 01
 *   ORGN1  [27:26]  = 01
 *   SH1    [29:28]  = 11
 *   TG1    [31:30]  = 10         4 KiB granule (note: different encoding!)
 *   IPS    [34:32]  = 101        48-bit physical address space
 *
 * Combined: 0x00000005B5993519 (worked out bit-by-bit in the comment
 * for chapter 6 of the book).
 */

.section .text
.global mmu_enable
mmu_enable:
    /* MAIR_EL1: idx0 = 0xFF (Normal WB), idx1 = 0x00 (Device). */
    mov     x1, #0x00FF
    msr     mair_el1, x1

    /* TCR_EL1 — pre-computed bitfield value. */
    ldr     x1, =0x00000005B5993519
    msr     tcr_el1, x1

    /* TTBR0_EL1 = caller-supplied L1 page-table address.
     * TTBR1_EL1 = 0 (not used yet; EPD1=1 disables walks). */
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, xzr

    /* Make MAIR/TCR/TTBR visible before invalidating TLBs. */
    isb

    /* Invalidate every TLB entry for EL1. dsb ish is the inner-
     * shareable-domain barrier: required so the TLB maintenance
     * completes before we try to use the new translation. */
    tlbi    vmalle1
    dsb     ish
    isb

    /* SCTLR_EL1: enable MMU (M), data cache (C), instruction cache (I).
     * Read-modify-write so we leave WXN, EE, and other bits untouched. */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)       /* M  — MMU enable                  */
    orr     x1, x1, #(1 << 2)       /* C  — D-cache enable              */
    orr     x1, x1, #(1 << 12)      /* I  — I-cache enable              */
    msr     sctlr_el1, x1
    isb

    /* Chapter 171 — enable FP/SIMD access at EL0 and EL1.
     *
     * CPACR_EL1.FPEN [21:20]:
     *   0b00 : every FP/SIMD instruction at EL0 or EL1 traps to EL1
     *          as EC=0x07 (FP/SIMD trapped).
     *   0b01 : EL1 access permitted, EL0 access traps.
     *   0b10 : reserved.
     *   0b11 : EL0 and EL1 access both permitted.
     *
     * We pick 0b11 so user binaries built without
     * -mgeneral-regs-only can issue FADD/LDR Q-reg/etc. without
     * the kernel having to lazy-allocate FP state on first use.
     * The kernel itself is still built with -mgeneral-regs-only
     * (see USER_CFLAGS vs CFLAGS in the top-level Makefile), so
     * kernel code never touches FP regs and cswitch_to is the
     * one and only place that saves/restores them.
     *
     * RMW so any other bits CPACR_EL1 brings up at reset (ZEN
     * for SVE, TTA for trace) are preserved. */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(0x3 << 20)    /* FPEN = 0b11 */
    msr     cpacr_el1, x1
    isb

    ret

/* mmu_disable — symmetric inverse, used only by debug/teardown.
 * Not called from the boot path. Lives here so the
 * pair stays in one place. */
.global mmu_disable
mmu_disable:
    mrs     x1, sctlr_el1
    bic     x1, x1, #(1 << 0)
    bic     x1, x1, #(1 << 2)
    bic     x1, x1, #(1 << 12)
    msr     sctlr_el1, x1
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    ret
