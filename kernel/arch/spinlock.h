/* spinlock.h — chapter 86 minimal SMP synchronisation primitives.
 *
 * Up to chapter 85 the kernel ran strictly uniprocessor: every
 * "lock" was the implicit lock that comes free from "only one CPU
 * can be in kernel code at a time."  Chapter 86 wakes a second
 * core, so any global mutable state that the secondary touches
 * (right now: the PL011 transmit register and the boot-status
 * reclock counters) needs an explicit lock.
 *
 * Two primitives:
 *
 *   spinlock_t  — flat ticketless test-and-set spin lock built on
 *                 LDAXR/STXR/STLR.  Acquire = load-acquire,
 *                 release = store-release.  Cheap, but NOT
 *                 recursive — taking the same spinlock twice on
 *                 one CPU deadlocks.
 *
 *   reclock_t   — recursive spinlock that records the owning CPU
 *                 (via MPIDR_EL1.Aff0) and a depth counter.  An
 *                 owner re-acquiring just bumps the depth.  This
 *                 is what serial_putc / serial_puts / serial_puthex
 *                 use, because main.c freely chains those calls
 *                 (e.g. puthex inside a puts/puts pair to print
 *                 "[smp] CPU 1 ready 0x..." in three steps) and
 *                 we want the whole boot line to be atomic across
 *                 CPUs without the caller having to track lock
 *                 state by hand.
 *
 * IRQ policy: both lock kinds have an `_irqsave` variant that
 * masks IRQs (DAIF.I) for the duration of the critical section.
 * That is the only way to be safe against an IRQ handler that
 * also takes the same lock.  Today no IRQ handler does, but the
 * pattern will start to matter once the GIC delivers IRQs to
 * CPU 1 (chapter 88+).
 *
 * Both primitives are header-only because they are entirely
 * inline asm — there is no .c file to compile and no linker
 * symbol to resolve.  Drop-in usable from any TU.
 */
#ifndef KERNEL_ARCH_SPINLOCK_H
#define KERNEL_ARCH_SPINLOCK_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * CPU identity helper.
 *
 * MPIDR_EL1.Aff0 is the within-cluster CPU index.  On QEMU virt
 * with -smp <= 8 every CPU is in cluster 0, so Aff0 (bits 7:0) is
 * a dense 0..N-1 index suitable for use as a CPU id without
 * requiring the per-CPU `struct cpu` to be wired up yet.  This is
 * important because the boot path takes locks BEFORE TPIDR_EL1 is
 * programmed.
 *
 * For SMP_MAX_CPUS > 8 (some other cluster topology) we'd need to
 * fold Aff1/Aff2 in.  Out of scope for chapter 86.
 * ------------------------------------------------------------------ */
static inline uint32_t spinlock_self_cpu_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFFu);
}

/* ------------------------------------------------------------------
 * Flat (non-recursive) spinlock.
 * ------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t locked;       /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT  { 0 }

static inline void spin_lock(spinlock_t *l)
{
    uint32_t tmp, scratch;
    __asm__ volatile(
        "1:                                  \n"
        "   ldaxr   %w0, [%2]                \n"
        "   cbnz    %w0, 1b                  \n"
        "   mov     %w1, #1                  \n"
        "   stxr    %w0, %w1, [%2]           \n"
        "   cbnz    %w0, 1b                  \n"
        : "=&r"(tmp), "=&r"(scratch)
        : "r"(&l->locked)
        : "memory");
}

static inline void spin_unlock(spinlock_t *l)
{
    __asm__ volatile("stlr wzr, [%0]"
                     :
                     : "r"(&l->locked)
                     : "memory");
}

static inline uint64_t irq_save_disable(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif"        : "=r"(daif));
    __asm__ volatile("msr daifset, #2"     ::: "memory");
    return daif;
}

static inline void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t f = irq_save_disable();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
    spin_unlock(l);
    irq_restore(f);
}

/* ------------------------------------------------------------------
 * Recursive spinlock with CPU-id ownership.
 *
 * Field layout chosen so the whole struct fits in two cache lines'
 * worth of state and the fast-path is just a load-and-compare on
 * `owner_cpu` plus a depth bump.  The owner field is sentinel
 * UINT32_MAX when the lock is free; that value is never a real
 * CPU id (we'd need MPIDR.Aff0 = 0xFF == 255, which would mean
 * SMP_MAX_CPUS > 255 — not happening any time soon).
 * ------------------------------------------------------------------ */

#define RECLOCK_NO_OWNER   0xFFFFFFFFu

typedef struct {
    spinlock_t        gate;        /* protects owner_cpu / depth   */
    volatile uint32_t owner_cpu;   /* RECLOCK_NO_OWNER when free   */
    volatile uint32_t depth;       /* recursion count              */
} reclock_t;

#define RECLOCK_INIT  { SPINLOCK_INIT, RECLOCK_NO_OWNER, 0 }

/* Acquire — recursive.  Returns the saved DAIF so the caller can
 * pair this with reclock_unlock_irqrestore().
 *
 * Implementation note: the *only* atomic operation in here is the
 * gate spinlock acquire/release.  Everything else (owner_cpu read,
 * depth increment) runs while we own the gate, so plain memory
 * accesses are correct.  The gate is the truth. */
static inline uint64_t reclock_lock_irqsave(reclock_t *r)
{
    uint32_t self = spinlock_self_cpu_id();
    uint64_t f    = irq_save_disable();

    /* Fast path: already own it?  Bump depth without taking the
     * gate at all.  Safe because IRQs are masked, so nobody else
     * on THIS cpu can race us, and other cpus that look at this
     * lock will see owner_cpu != self and go through the slow
     * path. */
    if (r->owner_cpu == self) {
        r->depth++;
        return f;
    }

    /* Slow path: acquire the gate, become the owner. */
    spin_lock(&r->gate);
    /* gate held => owner_cpu == NO_OWNER (the previous owner
     * dropped it before releasing the gate in unlock).            */
    r->owner_cpu = self;
    r->depth     = 1;
    return f;
}

static inline void reclock_unlock_irqrestore(reclock_t *r, uint64_t f)
{
    /* Underflow guard would be a nice-to-have but the kernel calls
     * lock/unlock in matched pairs by construction; if not, the
     * subsequent serial output will look broken and we'll catch
     * it during bring-up.  Cheap. */
    if (--r->depth == 0) {
        r->owner_cpu = RECLOCK_NO_OWNER;
        spin_unlock(&r->gate);
    }
    irq_restore(f);
}

#endif /* KERNEL_ARCH_SPINLOCK_H */
