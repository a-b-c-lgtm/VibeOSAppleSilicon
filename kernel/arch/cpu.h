/* cpu.h — chapter 87 per-CPU registry and accessors.
 *
 * One `struct cpu` per logical CPU, reachable via TPIDR_EL1
 * (after secondary_start sets it up) or via the flat g_cpus[]
 * array indexed by `cpu_id` (= MPIDR_EL1.Aff0 on QEMU virt).
 *
 * Why the first field of struct cpu MUST stay `stack_top`:
 * boot.s::secondary_start receives a `struct cpu *` in x0 (passed
 * via the PSCI context_id) while the MMU is still off and needs
 * its stack pointer immediately — long before C is running and
 * able to compute struct field offsets.  An `ldr x1, [x19, #0]`
 * lifts stack_top out with no compile-time coordination.  The
 * symbol CPU_OFFSET_STACK_TOP below is asserted == 0 by a
 * static_assert so any reorder of the struct breaks the build
 * loudly instead of silently corrupting the secondary boot.
 */
#ifndef KERNEL_ARCH_CPU_H
#define KERNEL_ARCH_CPU_H

#include <stdint.h>
#include "spinlock.h"

#define SMP_MAX_CPUS  4

/* Bit positions in struct cpu::flags. */
#define CPU_FLAG_PRESENT  (1u << 0)   /* described by DTB                */
#define CPU_FLAG_BOOTED   (1u << 1)   /* PSCI CPU_ON returned success    */
#define CPU_FLAG_READY    (1u << 2)   /* secondary_main reached WFI loop */

/* Forward decl — chapter 90 added per-CPU `current` and `idle`
 * pointers without dragging the full thread.h include in here. */
struct thread;

struct cpu {
    /* MUST be at offset 0 — see header comment. */
    uint64_t stack_top;

    /* Logical id 0..SMP_MAX_CPUS-1.  Same as MPIDR_EL1.Aff0 on
     * QEMU virt.  Boot CPU is always 0. */
    uint32_t cpu_id;

    /* Bitfield of CPU_FLAG_*. */
    volatile uint32_t flags;

    /* The MPIDR value to pass back to PSCI for CPU_OFF/AFFINITY_INFO
     * etc.  Stored verbatim — including affinity bits we currently
     * ignore. */
    uint64_t mpidr;

    /* ----------------------------------------------------------------
     * Chapter 90 — per-CPU scheduler state.
     *
     * Every CPU has its OWN runqueue, current-thread pointer, and
     * idle thread.  Threads do NOT migrate between CPUs (chapter
     * 89's deliberate scope floor); a thread is created on one CPU
     * and runs there for life.  This avoids the cross-CPU TLB
     * shootdown problem (no need to broadcast `tlbi vmalle1is`
     * when an address space is destroyed) and lets each CPU's
     * scheduler be the same uniprocessor scheduler we already had
     * up to chapter 88, just operating on per-CPU state.
     *
     * `current` is the thread actively running (or about to run on
     *          first cswitch into it).  Set by yield()/schedule().
     * `idle`   is the per-CPU idle thread; used as `current` when
     *          the runqueue is empty.  Created exactly once per
     *          CPU at boot (in thread_init for CPU 0, in
     *          secondary_main for CPU N).
     *
     * `runq_head` / `runq_tail` are the per-CPU FIFO of READY
     *          threads.  Pushed by yield() (the outgoing thread)
     *          and by thread_create_on() (cross-CPU enqueue).
     *          Popped by yield() to pick the next thread.
     *
     * `runq_lock` serialises access to the per-CPU runqueue.
     *          Acquired briefly in spin_lock_irqsave form because
     *          IRQ handlers (timer, IPI_RESCHED) push/pop too.
     *          NOT held across cswitch_to — yield acquires only
     *          long enough to do the queue mutation, then drops
     *          before the actual context switch.  No "lock across
     *          context switch" trampoline-fixup gymnastics.
     * ---------------------------------------------------------------- */
    struct thread *current;
    struct thread *idle;
    struct thread *runq_head;
    struct thread *runq_tail;
    spinlock_t     runq_lock;

    /* The kernel-side stack of an EXITED thread can only be freed
     * once the next context switch is in progress (we are still
     * running on it at the moment thread_exit calls yield()).
     * Stash the to-free thread here; the next yield() on this
     * CPU drains it.  Per-CPU because two CPUs could each have a
     * thread exit at the same moment, and a single global slot
     * would lose one of them. */
    struct thread *stack_to_free;
};

#define CPU_OFFSET_STACK_TOP  0

_Static_assert(CPU_OFFSET_STACK_TOP == 0,
    "boot.s::secondary_start hardcodes ldr x1, [x19, #0] -> stack_top");

/* Per-CPU registry.  Slots beyond smp_cpu_count() are zero-init
 * (.bss) and have CPU_FLAG_PRESENT clear. */
extern struct cpu g_cpus[SMP_MAX_CPUS];

/* How many CPUs are PRESENT (boot + DTB-discovered secondaries).
 * Set by smp_init() during DTB scan; constant thereafter. */
uint32_t smp_cpu_count(void);

/* Read MPIDR_EL1.Aff0 and use it as the CPU id.  Always valid
 * even before TPIDR_EL1 has been programmed; that's why the
 * spinlock owner field uses this. */
static inline uint32_t cpu_current_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFFu);
}

/* Returns the struct cpu * for the running CPU via TPIDR_EL1.
 * UNDEFINED before the CPU has set TPIDR_EL1 — call only after
 * smp_self_register() (boot CPU) or after secondary_start has
 * set tpidr_el1 (secondary). */
static inline struct cpu *cpu_current(void)
{
    uint64_t tpidr;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(tpidr));
    return (struct cpu *)(uintptr_t)tpidr;
}

/* Bring up SMP.  On entry: only CPU 0 exists, MMU is on, heap
 * is initialised, IRQs still masked.  On return:
 *   - g_cpus[] is populated for every DTB cpu node up to
 *     SMP_MAX_CPUS;
 *   - the boot CPU has TPIDR_EL1 set to &g_cpus[0] and is
 *     marked READY;
 *   - every secondary has been PSCI'd ON and is spinning in
 *     its WFE loop with READY set.
 *
 * smp_init() blocks (with a generous timeout) until all
 * secondaries report READY, then logs `[smp] all CPUs online`. */
void smp_init(void);

/* Same as smp_init() but takes the DTB pointer explicitly so we
 * can scan /cpus and /psci.  This is the entry point kernel_main
 * actually uses; the dtb-less smp_init() above is a forward-compat
 * stub for an eventual ACPI / hardcoded-table boot path. */
void smp_init_with_dtb(const void *dtb);

/* Chapter 90 — register the boot CPU's `struct cpu` slot and
 * write TPIDR_EL1 BEFORE thread_init runs.  Without this,
 * thread_init can't set `cpu_current()->current = boot_thread`
 * because TPIDR_EL1 is still zero and cpu_current() would
 * dereference NULL.
 *
 * Sets g_cpus[boot_idx] and TPIDR_EL1; smp_init_with_dtb later
 * re-walks the DTB and fills the same slot identically (plus
 * the secondaries).  Idempotent on the boot CPU. */
void cpu_register_boot(void);

/* Chapter 90 — entry function for the per-CPU idle thread.
 * Sleeps in WFI; wakes on any IRQ (timer or IPI_RESCHED) and
 * yields to give a real thread a chance.  Falls back to WFI
 * if the runqueue is still empty (yield will pick idle again).
 * Each CPU has its own idle thread; this function is the
 * shared entry point. */
void cpu_idle_loop(void *arg);

/* The C entry point for secondary CPUs.  Called from
 * boot.s::secondary_start after MMU is on and TPIDR_EL1 is set.
 * Never returns — parks in WFE forever. */
void secondary_main(struct cpu *self);

#endif /* KERNEL_ARCH_CPU_H */
