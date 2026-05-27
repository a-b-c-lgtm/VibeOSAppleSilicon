/*
 * kernel/device/gic.c — GIC v3 driver.
 *
 * Brings up the shared distributor and per-CPU redistributors so
 * the kernel can take PPIs (the ARM generic timer at ID 27) and
 * SGIs (chapter 89's IPIs).
 *
 * The MMU is on by the time gic_init / gic_init_per_cpu run: the
 * [0, 1 GiB) region is mapped Device-nGnRnE in our identity-mapped
 * L1 page table, which covers the entire GICD + GICR window
 * (0x0800_0000..0x0810_0000-ish on QEMU virt with up to 4 CPUs).
 *
 * Reference: Arm Generic Interrupt Controller Architecture
 *   Specification GIC architecture version 3 (IHI 0069H), chapters
 *   8 (Distributor), 9 (Redistributor), 12 (CPU interface registers).
 */

#include "gic.h"
#include "mmio.h"
#include <stdint.h>

/* MMIO bases — fixed by the QEMU virt machine. */
#define GICD_BASE        0x08000000UL
#define GICR_BASE_CPU0   0x080A0000UL

/* Each redistributor frame is 128 KiB (one RD_base half + one
 * SGI_base half, both 64 KiB) and the per-CPU frames are laid
 * out contiguously in MPIDR.Aff0 order on QEMU virt.  See
 * IHI 0069H section 9.2. */
#define GICR_FRAME_STRIDE  0x20000UL

/* GICD distributor registers we touch. */
#define GICD_CTLR        (GICD_BASE + 0x0000)
#define GICD_TYPER       (GICD_BASE + 0x0004)
#define GICD_ISENABLER(n)  (GICD_BASE + 0x0100 + 4U * (n))
#define GICD_ICENABLER(n)  (GICD_BASE + 0x0180 + 4U * (n))
#define GICD_IPRIORITYR(n) (GICD_BASE + 0x0400 + 4U * (n))
#define GICD_IGROUPR(n)    (GICD_BASE + 0x0080 + 4U * (n))

/* GICD_CTLR bits in non-secure access mode (which is what we get
 * under HVF / qemu virt).  Single-security-state systems treat
 * EnableGrp1NS as "EnableGrp1" and EnableGrp1A as a separate alias. */
#define GICD_CTLR_ENABLE_G1NS   (1U << 1)
#define GICD_CTLR_ARE_NS        (1U << 4)

/* Per-CPU GICR offsets (relative to that CPU's GICR base). */
#define GICR_CTLR_OFF        0x0000
#define GICR_WAKER_OFF       0x0014

/* GICR_WAKER bits. */
#define GICR_WAKER_PROCESSOR_SLEEP   (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP   (1U << 2)

/* SGI_base half lives 64 KiB above RD_base. */
#define GICR_SGI_OFF         0x10000

/* Per-CPU GICR_SGI_base offsets (relative to RD_base). */
#define GICR_IGROUPR0_OFF        (GICR_SGI_OFF + 0x0080)
#define GICR_ISENABLER0_OFF      (GICR_SGI_OFF + 0x0100)
#define GICR_ICENABLER0_OFF      (GICR_SGI_OFF + 0x0180)
#define GICR_IPRIORITYR_OFF(n)   (GICR_SGI_OFF + 0x0400 + 4U * (n))

/* Compute this CPU's GICR base from MPIDR.Aff0.  On QEMU virt
 * that is a dense 0..N-1 index; future cluster topologies would
 * need to fold Aff1/Aff2 in, but SMP_MAX_CPUS is 4 so we'll
 * cross that bridge when (if) it ever comes. */
static inline uintptr_t gicr_base_self(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint64_t aff0 = mpidr & 0xFFu;
    return (uintptr_t)GICR_BASE_CPU0 + (uintptr_t)(aff0 * GICR_FRAME_STRIDE);
}

/* CPU interface system registers — accessed via ICC_*_EL1.
 * Use the friendly mnemonic; binutils 2.40+ knows them. */
static inline void icc_sre_el1_write(uint64_t v)
{
    __asm__ volatile("msr ICC_SRE_EL1, %0\n\tisb" :: "r"(v) : "memory");
}
static inline uint64_t icc_sre_el1_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(v));
    return v;
}
static inline void icc_pmr_el1_write(uint64_t v)
{
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(v) : "memory");
}
static inline void icc_bpr1_el1_write(uint64_t v)
{
    __asm__ volatile("msr ICC_BPR1_EL1, %0" :: "r"(v) : "memory");
}
static inline void icc_igrpen1_el1_write(uint64_t v)
{
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0\n\tisb"
                     :: "r"(v) : "memory");
}
static inline uint64_t icc_iar1_el1_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(v) :: "memory");
    return v;
}
static inline void icc_eoir1_el1_write(uint64_t v)
{
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(v) : "memory");
}

/* Wake the redistributor for the current CPU.
 * After reset, GICR_WAKER.ProcessorSleep = 1 and ChildrenAsleep = 1.
 * Clearing ProcessorSleep wakes the CPU interface; we then poll
 * ChildrenAsleep until the redistributor confirms it is awake. */
static void redist_wake(uintptr_t gicr)
{
    uint32_t waker = mmio_read32(gicr + GICR_WAKER_OFF);
    waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
    mmio_write32(gicr + GICR_WAKER_OFF, waker);

    /* Poll for ChildrenAsleep == 0.  In hardware this can take a
     * few cycles; under QEMU it is effectively instant. */
    while (mmio_read32(gicr + GICR_WAKER_OFF) & GICR_WAKER_CHILDREN_ASLEEP) {
        /* spin */
    }
}

/* Enable EL1 system-register access to the CPU interface and
 * configure the priority mask and group enable. */
static void cpu_interface_init(void)
{
    /* ICC_SRE_EL1.SRE = 1 — use system registers, not MMIO, for
     * the CPU interface.  Some hypervisors/firmware leave this at
     * 0 by default; if so, mrs/msr to ICC_*_EL1 traps. */
    uint64_t sre = icc_sre_el1_read();
    icc_sre_el1_write(sre | 1ULL);

    /* Allow all priorities through (mask = 0xFF means "no
     * priority blocked"). */
    icc_pmr_el1_write(0xFFULL);

    /* No preemption grouping — every priority preempts every
     * lower priority.  Fine for our single-IRQ design. */
    icc_bpr1_el1_write(0ULL);

    /* Enable group-1 interrupts on this CPU.  Without this the
     * CPU interface acknowledges nothing. */
    icc_igrpen1_el1_write(1ULL);
}

/* Configure the SHARED distributor.  Called exactly once on the
 * boot CPU.  Sets every SPI as group-1 non-secure with default
 * priority and disabled, then enables ARE_NS + Group1NS so SPIs
 * can be routed by affinity once we ever decide to enable any. */
static void distributor_init(void)
{
    /* Disable the distributor while we configure it. */
    mmio_write32(GICD_CTLR, 0);

    /* Disable every SPI, default priority.  IDs 32..1019 → regs
     * 1..31 of ICENABLER (each reg covers 32 IDs). */
    for (uint32_t r = 1; r < 32; r++) {
        mmio_write32(GICD_ICENABLER(r), 0xFFFFFFFFu);
    }
    for (uint32_t r = 0; r < 256; r++) {
        mmio_write32(GICD_IPRIORITYR(r), 0xA0A0A0A0u);
    }

    /* Re-enable the distributor: ARE_NS lets the redistributor
     * own affinity routing for SPIs (required for GICv3),
     * EnableGrp1NS turns on group-1 non-secure delivery. */
    mmio_write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);
}

void gic_init_per_cpu(void)
{
    uintptr_t gicr = gicr_base_self();

    redist_wake(gicr);

    /* Mark every PPI/SGI on this CPU as group 1 (non-secure).
     * ICC_IGRPEN1 later enables that group on the CPU interface. */
    mmio_write32(gicr + GICR_IGROUPR0_OFF, 0xFFFFFFFFu);

    /* Disable every PPI/SGI initially; gic_enable_irq enables
     * specific ones on demand. */
    mmio_write32(gicr + GICR_ICENABLER0_OFF, 0xFFFFFFFFu);

    /* Default priority for every PPI/SGI on this CPU: 0xA0
     * (mid-range).  The priority byte is the low byte; one
     * register holds 4 IDs. */
    for (uint32_t r = 0; r < 8; r++) {
        mmio_write32(gicr + GICR_IPRIORITYR_OFF(r), 0xA0A0A0A0u);
    }

    cpu_interface_init();
}

void gic_init(void)
{
    /* Distributor first; it must be quiesced before any
     * redistributor wakes up. */
    distributor_init();

    /* Then bring up the boot CPU's redistributor + CPU interface. */
    gic_init_per_cpu();
}

void gic_enable_irq(uint32_t intid)
{
    if (intid < 32) {
        /* SGI or PPI — lives in the *current* CPU's redistributor. */
        uintptr_t gicr = gicr_base_self();
        mmio_write32(gicr + GICR_ISENABLER0_OFF, 1u << intid);
    } else {
        /* SPI — lives in the distributor.  Use the appropriate
         * 32-bit ISENABLER register. */
        uint32_t reg = intid / 32;
        uint32_t bit = intid % 32;
        mmio_write32(GICD_ISENABLER(reg), 1u << bit);
    }
}

void gic_set_priority(uint32_t intid, uint8_t priority)
{
    if (intid < 32) {
        uintptr_t gicr = gicr_base_self();
        uint32_t reg = intid / 4;
        uint32_t lane = intid % 4;
        uint32_t v = mmio_read32(gicr + GICR_IPRIORITYR_OFF(reg));
        v &= ~(0xFFu << (lane * 8));
        v |= ((uint32_t)priority) << (lane * 8);
        mmio_write32(gicr + GICR_IPRIORITYR_OFF(reg), v);
    } else {
        uint32_t reg = intid / 4;
        uint32_t lane = intid % 4;
        uint32_t v = mmio_read32(GICD_IPRIORITYR(reg));
        v &= ~(0xFFu << (lane * 8));
        v |= ((uint32_t)priority) << (lane * 8);
        mmio_write32(GICD_IPRIORITYR(reg), v);
    }
}

uint32_t gic_acknowledge_irq(void)
{
    return (uint32_t)icc_iar1_el1_read();
}

void gic_end_of_irq(uint32_t intid)
{
    icc_eoir1_el1_write((uint64_t)intid);
}
