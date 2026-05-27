/* atomic.h — chapter 88 LL/SC atomic vocabulary for AArch64.
 *
 * Up to chapter 87 every shared scalar in the kernel was modified
 * by exactly one CPU (CPU 0), so a plain `x++` was correct
 * because `++` on uniprocessor AArch64 is "load, increment,
 * store" with no possibility of another CPU observing the load
 * without seeing the matching store.
 *
 * Once chapter 90's scheduler runs threads on CPU 1 the same
 * `x++` becomes a race: both CPUs can read the same value, both
 * increment locally, and both store back, losing one update.
 * The fix is to use the architectural Load-Exclusive /
 * Store-Exclusive (LDXR/STXR) primitive, which the rest of the
 * world calls "LL/SC" (Load-Linked / Store-Conditional).
 *
 * The contract:
 *
 *   1. LDXR Wt, [Xn] — read 32 bits from [Xn] AND set the
 *      *exclusive monitor* on that cache line for this CPU.
 *
 *   2. STXR Ws, Wt, [Xn] — write 32 bits to [Xn] but ONLY if
 *      this CPU's exclusive monitor still owns the line.
 *      Returns 0 on success, 1 on failure (someone else touched
 *      the line in between).
 *
 *   3. On STXR failure we re-execute the LDXR/modify/STXR
 *      sequence.  Eventually we win — under bounded contention
 *      the retry count is small.
 *
 * The acquire/release variants (LDAXR / STLXR / STLR) add
 * memory-ordering semantics: an LDAXR's effect is visible to
 * subsequent loads/stores on this CPU (acquire), and an STLXR /
 * STLR's effect is published to all CPUs before subsequent
 * loads/stores on this CPU complete (release).  This is the
 * standard "acquire/release pair" idiom from C11 / Linux kernel.
 *
 * We deliberately do NOT use ARMv8.1 LSE atomics (LDADD, CAS,
 * SWP) here.  LSE is faster under heavy contention and shorter
 * code, but:
 *   - cortex-a72 (the QEMU virt baseline) does NOT implement
 *     LSE; we'd need a runtime feature check to pick a path.
 *   - Our scale is at most 4 CPUs; LL/SC retry-loop pressure is
 *     not measurable.
 *   - Pedagogically the LL/SC retry loop makes the underlying
 *     atomicity contract explicit.  LSE hides it.
 *
 * Naming: `atomic_<op>_return<width>` mirrors the Linux kernel's
 * pattern.  The `_return` makes the read-modify-write semantics
 * visible at the call site: there's no "fire and forget" atomic
 * because the new value is information you usually want.
 */
#ifndef KERNEL_ARCH_ATOMIC_H
#define KERNEL_ARCH_ATOMIC_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * Memory barriers.
 *
 * Inner Shareable (ISH) is the ARM term for "all the CPUs in our
 * cache-coherent cluster" — exactly the set of cores PSCI lit up
 * in chapter 87.  We almost always want ISH; the heavier `OSH`
 * (Outer Shareable) and `SY` (system) include device-side
 * observers and are only needed for MMIO or DMA descriptor
 * publishing.
 *
 *   dmb_ish()   — Data Memory Barrier, full.  Loads/stores
 *                 issued before the dmb are observable to other
 *                 CPUs before any load/store issued after.
 *   dmb_ishst() — store-only variant; cheaper.  Use after a
 *                 chain of stores you want published as a group.
 *   dsb_ish()   — Data Synchronisation Barrier.  Stronger than
 *                 dmb: this CPU stalls until all in-flight
 *                 loads/stores have actually completed.  Needed
 *                 before TLBI / ICache maintenance.
 *   isb_()      — Instruction Synchronisation Barrier.  Flushes
 *                 the prefetch pipeline; required after writing
 *                 SCTLR/TTBR/etc so subsequent fetches see the
 *                 new state.
 * ------------------------------------------------------------------ */

static inline void dmb_ish(void)   { __asm__ volatile("dmb ish"   ::: "memory"); }
static inline void dmb_ishst(void) { __asm__ volatile("dmb ishst" ::: "memory"); }
static inline void dsb_ish(void)   { __asm__ volatile("dsb ish"   ::: "memory"); }
static inline void isb_(void)      { __asm__ volatile("isb"       ::: "memory"); }

/* ------------------------------------------------------------------
 * 32-bit atomics.
 *
 * Pointer arguments are `volatile uint32_t *` so the compiler
 * doesn't fold a stale plain-load over our atomic and so the
 * call-site doesn't need a cast for variables declared volatile
 * (e.g. flags shared with IRQ handlers).
 * ------------------------------------------------------------------ */

static inline uint32_t atomic_load32(const volatile uint32_t *p)
{
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]"
                     : "=r"(v)
                     : "r"(p)
                     : "memory");
    return v;
}

static inline void atomic_store32(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("stlr %w1, [%0]"
                     :
                     : "r"(p), "r"(v)
                     : "memory");
}

/* Returns the NEW value (post-add).  Mirrors Linux's
 * atomic_add_return naming.  If you want the old value, subtract
 * delta from the return. */
static inline uint32_t atomic_add_return32(volatile uint32_t *p, uint32_t delta)
{
    uint32_t old, new_, fail;
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]            \n"
        "   add     %w1, %w0, %w4        \n"
        "   stlxr   %w2, %w1, [%3]       \n"
        "   cbnz    %w2, 1b              \n"
        : "=&r"(old), "=&r"(new_), "=&r"(fail)
        : "r"(p), "r"(delta)
        : "memory");
    return new_;
}

static inline uint32_t atomic_sub_return32(volatile uint32_t *p, uint32_t delta)
{
    uint32_t old, new_, fail;
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]            \n"
        "   sub     %w1, %w0, %w4        \n"
        "   stlxr   %w2, %w1, [%3]       \n"
        "   cbnz    %w2, 1b              \n"
        : "=&r"(old), "=&r"(new_), "=&r"(fail)
        : "r"(p), "r"(delta)
        : "memory");
    return new_;
}

/* Compare-and-swap: if *p == expected, store new and return 1;
 * otherwise leave *p alone and return 0.  The classic building
 * block for lock-free data structures. */
static inline int atomic_cmpxchg32(volatile uint32_t *p,
                                   uint32_t expected,
                                   uint32_t new_)
{
    uint32_t cur, fail;
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]            \n"
        "   cmp     %w0, %w4             \n"
        "   b.ne    2f                   \n"
        "   stlxr   %w1, %w5, [%3]       \n"
        "   cbnz    %w1, 1b              \n"
        "   mov     %w1, #0              \n"   /* success → fail=0 */
        "   b       3f                   \n"
        "2: clrex                        \n"
        "   mov     %w1, #1              \n"   /* mismatch → fail=1 */
        "3:                              \n"
        : "=&r"(cur), "=&r"(fail)
        : "r"(p), "r"(p), "r"(expected), "r"(new_)
        : "cc", "memory");
    return fail == 0;
}

/* OR a bit-mask atomically.  Used by the SMP smoke test to set
 * "this CPU is done" bits in a shared word from multiple CPUs. */
static inline uint32_t atomic_or_return32(volatile uint32_t *p, uint32_t mask)
{
    uint32_t old, new_, fail;
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]            \n"
        "   orr     %w1, %w0, %w4        \n"
        "   stlxr   %w2, %w1, [%3]       \n"
        "   cbnz    %w2, 1b              \n"
        : "=&r"(old), "=&r"(new_), "=&r"(fail)
        : "r"(p), "r"(mask)
        : "memory");
    return new_;
}

/* ------------------------------------------------------------------
 * 64-bit atomics.
 *
 * Same shape as the 32-bit versions, just with X-register
 * operands (no `%w` modifier).  64-bit atomics are essential for
 * counters that might run for years (network byte counts, total
 * page-faults handled, etc).
 * ------------------------------------------------------------------ */

static inline uint64_t atomic_load64(const volatile uint64_t *p)
{
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]"
                     : "=r"(v)
                     : "r"(p)
                     : "memory");
    return v;
}

static inline void atomic_store64(volatile uint64_t *p, uint64_t v)
{
    __asm__ volatile("stlr %1, [%0]"
                     :
                     : "r"(p), "r"(v)
                     : "memory");
}

static inline uint64_t atomic_add_return64(volatile uint64_t *p, uint64_t delta)
{
    uint64_t old, new_;
    uint32_t fail;
    __asm__ volatile(
        "1: ldaxr   %0, [%3]             \n"
        "   add     %1, %0, %4           \n"
        "   stlxr   %w2, %1, [%3]        \n"
        "   cbnz    %w2, 1b              \n"
        : "=&r"(old), "=&r"(new_), "=&r"(fail)
        : "r"(p), "r"(delta)
        : "memory");
    return new_;
}

static inline uint64_t atomic_sub_return64(volatile uint64_t *p, uint64_t delta)
{
    uint64_t old, new_;
    uint32_t fail;
    __asm__ volatile(
        "1: ldaxr   %0, [%3]             \n"
        "   sub     %1, %0, %4           \n"
        "   stlxr   %w2, %1, [%3]        \n"
        "   cbnz    %w2, 1b              \n"
        : "=&r"(old), "=&r"(new_), "=&r"(fail)
        : "r"(p), "r"(delta)
        : "memory");
    return new_;
}

#endif /* KERNEL_ARCH_ATOMIC_H */
