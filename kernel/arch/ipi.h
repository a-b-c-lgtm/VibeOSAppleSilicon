/* ipi.h — chapter 89 inter-processor interrupt vocabulary.
 *
 * GICv3 distinguishes three classes of interrupts: SPIs (shared
 * peripherals), PPIs (per-CPU peripherals like the timer), and
 * SGIs (Software-Generated Interrupts).  SGIs are 16 IDs (0..15)
 * any CPU can fire at any other CPU using the system register
 * ICC_SGI1R_EL1.  We use them as our IPI mechanism: each SGI ID
 * is a *vector* — a small enum of "things one CPU might want
 * another CPU to do RIGHT NOW."
 *
 * Why a real IPI mechanism matters at all in chapter 89:
 *
 *   - Up to chapter 88 the secondary CPUs sit in WFE forever.
 *     They have no way to be told "wake up, you have work."
 *     Once chapter 90's scheduler arrives, CPU 0 will need to
 *     IPI CPU 1 every time it puts a thread on CPU 1's runqueue
 *     — that is IPI_RESCHED (added in chapter 90).
 *
 *   - The kernel needs a "halt all CPUs cleanly" path for
 *     panics: today CPU 1 is in WFE so a CPU-0 panic is
 *     trivially survivable, but the moment CPU 1 runs threads
 *     a CPU-0 panic without a HALT IPI leaves CPU 1 still
 *     running stale code.  IPI_HALT is the wired-but-not-yet-
 *     called primitive for that.
 *
 *   - Future TLB shootdowns (when an mmap unmap on CPU 0 has to
 *     be observable to CPU 1's TLB) are also IPIs.
 *
 * Chapter 89 ships TWO vectors and proves the round-trip works
 * with a smoke test.  Adding more is a one-line edit.
 *
 * Design choice — one SGI ID per vector, NOT a coalescing
 * mailbox.  Linux kernel uses a per-CPU `volatile uint32_t
 * pending_ipis` bitmap so that two rapid RESCHED IPIs collapse
 * into one delivery.  At our scale (4 CPUs, <1 IPI/ms) the
 * coalescing optimisation is invisible; the simpler "one SGI
 * per vector" makes irq_dispatch just look at the intid and
 * dispatch.
 */
#ifndef KERNEL_ARCH_IPI_H
#define KERNEL_ARCH_IPI_H

#include <stdint.h>

/* The SGI IDs we use as IPI vectors.  IDs 0..15 are the SGI
 * range; we reserve everything ≥4 for future expansion. */
enum {
    IPI_PING    = 0,   /* "are you there?" — used by the SMP smoke
                        * test.  Receiver bumps a per-CPU atomic
                        * counter; sender spin-waits for the bump. */
    IPI_HALT    = 1,   /* "stop forever" — receiver enters WFI loop
                        * with IRQs masked.  Wired but not yet
                        * triggered from the panic path; chapter 90
                        * will hook it once secondaries do real work. */
    IPI_RESCHED = 2,   /* "you have a new thread on your runqueue
                        * (or your current may need to step aside)" —
                        * receiver's irq_dispatch sees ipi_handle
                        * return 1 and calls schedule().  Chapter 90
                        * uses this to wake an idle CPU when CPU 0
                        * cross-enqueues a kernel thread to it. */
    /* IPI_TLB     = 3,  // future TLB shootdown */
    IPI_VECTOR_MAX = 16
};

/* Send an IPI to one specific CPU by logical id.  Internally
 * builds the ICC_SGI1R_EL1 value (Aff3:Aff2:Aff1:0:TargetList)
 * for the target's MPIDR — on QEMU virt that's just
 * `(1 << target_cpu) << TargetList` since every CPU lives in
 * cluster 0/0/0.  No ack, no waiting; the SGI is "fire and
 * forget" from the sender's perspective. */
void ipi_send(uint32_t target_cpu, uint32_t ipi_id);

/* Broadcast an IPI to every CPU EXCEPT the caller.  Uses IRM=1
 * in ICC_SGI1R_EL1, which is the GICv3-architected way to do
 * "everyone but me" without enumerating CPUs.  This is what the
 * future panic_halt_others() will use. */
void ipi_broadcast_others(uint32_t ipi_id);

/* Top-level IPI handler.  Called from irq_dispatch when the
 * intid returned by gic_acknowledge_irq is < 16 (SGI range).
 * Dispatches to the per-vector handler.  Caller has already
 * acknowledged but not yet EOI'd; ipi_handle does its work,
 * caller does the EOI.
 *
 * Returns 1 if this IPI should cause a reschedule on return,
 * 0 otherwise.  (Today only IPI_RESCHED would do this; both
 * shipping vectors return 0.) */
int ipi_handle(uint32_t intid);

/* SMP smoke test for the IPI path.  Called from smp_init_with_dtb
 * after every secondary is READY and the atomic-smoke has passed.
 *
 * For each BOOTED+READY secondary, sends an IPI_PING and
 * spin-waits (with timeout) for that CPU's per-CPU ping counter
 * to bump.  Logs `[smp-ipi] cpu=N OK round-trip` per CPU and
 * `[smp-ipi] all OK` at the end.
 *
 * If smp_cpu_count() == 1, prints `[smp-ipi] all OK (no
 * secondaries)` and returns immediately. */
void ipi_smoke_test(void);

#endif /* KERNEL_ARCH_IPI_H */
