/*
 * kernel/device/gic.h — GIC v3 driver public API.
 *
 * The QEMU 'virt' machine with `-cpu host -accel hvf` exposes a
 * single GICv3 with the standard MMIO layout:
 *
 *   GICD (Distributor)            : 0x08000000
 *   GICR (Redistributor) for CPU N : 0x080A0000 + N * 0x20000
 *     GICR_RD_BASE                : base + 0x00000  (control)
 *     GICR_SGI_BASE               : base + 0x10000  (SGI/PPI control)
 *
 * Each redistributor frame is 128 KiB (two 64 KiB halves) and they
 * are laid out contiguously in MPIDR.Aff0 order.  CPU N's
 * redistributor base is therefore `0x080A0000 + (N << 17)`.
 *
 * The CPU interface is *not* MMIO; it is reached through system
 * registers prefixed `ICC_*_EL1`, accessed with mrs/msr.
 *
 * Interrupt classes (terminology used throughout this driver):
 *
 *   SGI  — Software Generated Interrupt   ID 0..15
 *   PPI  — Private Peripheral Interrupt   ID 16..31  (per-CPU)
 *   SPI  — Shared Peripheral Interrupt    ID 32..1019
 *
 * The ARM generic timer's CNTV signal arrives as PPI ID 27
 * on the QEMU virt machine.
 */

#ifndef GIC_H
#define GIC_H

#include <stdint.h>

/* Initialise the GIC.  Must be called once on the boot CPU before
 * any other CPU is brought up.  Configures the distributor (one
 * shared instance) and brings up the boot CPU's redistributor +
 * CPU interface.  Equivalent to `gic_init_distributor() +
 * gic_init_per_cpu()`. */
void gic_init(void);

/* Per-CPU init: wake this CPU's redistributor, configure its
 * SGI/PPI defaults, and enable the CPU interface (system regs).
 * Each secondary calls this from secondary_main once its MMU is on
 * and it has a valid stack — no MMIO is required besides the
 * device-mapped GICR window covered by our identity map.
 *
 * Safe to call multiple times; it is idempotent on a per-CPU basis
 * (waking an already-awake redistributor is a no-op).
 *
 * The boot CPU does NOT need to call this separately — gic_init()
 * does it for the boot CPU as part of its sequence. */
void gic_init_per_cpu(void);

/* Enable an interrupt by global ID.  For SGIs/PPIs (ID 0..31) the
 * enable register lives in the *current* CPU's redistributor; for
 * SPIs (ID 32+) it lives in the shared distributor.  Both cases
 * are handled here.
 *
 * IMPORTANT: SGI/PPI enables are per-CPU.  Each CPU that wants a
 * given SGI/PPI must call this from its own context. */
void gic_enable_irq(uint32_t intid);

/* Set an IRQ's priority (0 = highest, 0xFF = lowest).
 * Default priority used by gic_init for everything is 0xA0. */
void gic_set_priority(uint32_t intid, uint8_t priority);

/* Acknowledge the highest-priority pending interrupt and return
 * its ID.  ID 1023 means "spurious — no real interrupt pending".
 * Pairs with gic_eoi.  Wraps `mrs ICC_IAR1_EL1`. */
uint32_t gic_acknowledge_irq(void);

/* Signal end-of-interrupt for the ID returned by gic_acknowledge_irq.
 * Wraps `msr ICC_EOIR1_EL1`. */
void gic_end_of_irq(uint32_t intid);

#endif /* GIC_H */
