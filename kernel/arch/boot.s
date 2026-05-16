/* boot.s — aarch64 boot stub for QEMU `-machine virt`.
 *
 * Boot protocol (Linux/QEMU `-kernel` ELF on aarch64 virt):
 *   - PC      = ELF entry point (this label, _start)
 *   - x0      = physical address of the flat device tree blob (DTB)
 *   - x1..x3  = 0
 *   - PSTATE  = EL1h, DAIF masked, MMU off, caches off, little-endian
 *   - SP      = undefined — we set it ourselves before any C call
 *
 * Sequence (milestone 1):
 *   1. Save the DTB pointer (x0) into a callee-saved register.
 *   2. Set up the boot stack from the linker-script `stack_top` symbol.
 *   3. Zero the BSS so uninitialised globals read as 0 per the C spec.
 *   4. Install the EL1 exception vector table — vectors_init in
 *      kernel/arch/vectors.S writes VBAR_EL1. Done BEFORE we enable
 *      the MMU so any fault during MMU bring-up itself produces a
 *      proper crash dump instead of a silent host assertion.
 *   5. Enable the MMU — mmu_enable in kernel/arch/mmu.S programs
 *      MAIR/TCR/TTBR0 from the static l1_pgtable in
 *      kernel/arch/page_tables.c, invalidates the TLBs, then sets
 *      SCTLR_EL1.{M,C,I}. After this returns, normal C with stack
 *      frames is safe (the milestone-0 stp/ldp issue is gone).
 *   6. Hand the DTB pointer to kernel_main as the first argument.
 *
 * If kernel_main ever returns we drop into the wfe-loop below.
 */

.section .text.boot, "ax"
.global _start
_start:
    /* Step 1 — preserve DTB pointer across the boot setup. */
    mov     x19, x0

    /* Step 2 — set up the boot stack. AArch64 SP must be 16-byte
     * aligned at any public boundary; the linker reserves the
     * stack region with .balign 16 so this is automatic. */
    adrp    x0, stack_top
    add     x0, x0, :lo12:stack_top
    mov     sp, x0

    /* Step 3 — zero BSS. Single-register stores only (no stp/ldp)
     * because we are still MMU-off; HVF asserts on stp against
     * Device memory. */
    adrp    x0, bss_start
    add     x0, x0, :lo12:bss_start
    adrp    x1, bss_end
    add     x1, x1, :lo12:bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:

    /* Step 4 — install vector table at EL1.
     * vectors_init is a leaf function (no stack frame), safe with
     * MMU off. */
    bl      vectors_init

    /* Step 5 — enable the MMU.
     * x0 must hold the physical address of the L1 page table.
     * mmu_enable is hand-written assembly with no stack use. */
    adrp    x0, l1_pgtable
    add     x0, x0, :lo12:l1_pgtable
    bl      mmu_enable

    /* Step 6 — call into C.
     * kernel_main expects x0 = DTB physical address. The MMU is
     * now on, so full C (including non-leaf functions, loops,
     * register spills) is safe. */
    mov     x0, x19
    bl      kernel_main

    /* Fallthrough: park the CPU. */
.global _hang
_hang:
    wfe
    b       _hang

/* ----------------------------------------------------------------
 * secondary_start — chapter 86 PSCI secondary CPU entry.
 *
 * Entered when CPU 0 issues PSCI_CPU_ON for this core.  Per
 * PSCI spec sec. 5.1.4 the secondary lands here with:
 *   PC      = whatever entry_point we passed PSCI
 *   x0      = context_id (we passed &g_cpus[id], i.e. struct cpu *)
 *   x1..x29 = undefined
 *   PSTATE  = EL1h, DAIF masked, MMU off, caches off, LE
 *   SP      = undefined
 *
 * We must:
 *   1. Stash the cpu* in a callee-saved reg (x19) — mmu_enable
 *      clobbers x0/x1.
 *   2. Load SP from cpu->stack_top (which CPU_OFFSET_STACK_TOP
 *      asserts is at offset 0 — see arch/cpu.h).
 *   3. Install vectors_init (shared with CPU 0; leaf, MMU-off-safe).
 *   4. Defensively clear SCTLR_EL1.{M,C,I} in case PSCI / firmware
 *      left them set (it shouldn't — spec says off — but cheap).
 *   5. ic iallu — invalidate this CPU's I-cache.  TLB invalidation
 *      is done by mmu_enable below.
 *   6. mmu_enable with the SHARED kernel L1 (l1_pgtable).  After
 *      this returns, the secondary's view of memory matches CPU 0's
 *      kernel mapping exactly — same identity-mapped low RAM, same
 *      MMIO mapping, same heap.  User mappings are absent because
 *      the secondary is never going to enter user mode in chapter 86.
 *   7. Set TPIDR_EL1 = struct cpu * so cpu_current() works in C.
 *   8. Tail-call secondary_main(self).  Never returns.
 * ---------------------------------------------------------------- */
.section .text.boot, "ax"
.global secondary_start
secondary_start:
    /* Step 1 — stash the cpu* immediately. */
    mov     x19, x0

    /* Step 2 — SP from cpu->stack_top.  CPU_OFFSET_STACK_TOP == 0
     * is asserted in cpu.h via _Static_assert. */
    ldr     x1, [x19, #0]
    mov     sp, x1

    /* Step 3 — vector table.  vectors_init is a leaf function
     * (no stack frame) so it's safe to call here even though
     * the MMU is still off. */
    bl      vectors_init

    /* Step 4 — defensively turn the MMU off.  PSCI guarantees
     * SCTLR.{M,C,I} are clear on entry, but reading + clearing
     * costs three instructions and immunises us against firmware
     * bugs. */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)
    bic     x0, x0, #(1 << 2)
    bic     x0, x0, #(1 << 12)
    msr     sctlr_el1, x0
    isb

    /* Step 5 — invalidate this CPU's instruction cache.
     * `ic iallu` is per-PE (no broadcast).  dsb nsh is sufficient
     * since the I-cache is per-core and we don't yet care about
     * other CPUs seeing the invalidation. */
    ic      iallu
    dsb     nsh
    isb

    /* Step 6 — bring up the MMU on the shared kernel L1.
     * mmu_enable expects x0 = L1 PA. */
    adrp    x0, l1_pgtable
    add     x0, x0, :lo12:l1_pgtable
    bl      mmu_enable

    /* Step 7 — TPIDR_EL1 = struct cpu *.  cpu_current() reads
     * this from C. */
    msr     tpidr_el1, x19

    /* Step 8 — into C, never returns.  If secondary_main does
     * return, we fall through to a defensive WFE loop. */
    mov     x0, x19
    bl      secondary_main
1:  wfe
    b       1b
