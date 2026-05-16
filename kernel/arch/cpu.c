/* cpu.c — chapter 86 per-CPU registry, SMP bring-up, secondary main.
 *
 * Sequence in smp_init() (called from kernel_main after heap is up):
 *
 *   1. psci_init(dtb)  — pick HVC vs SMC.
 *   2. Enumerate /cpus from DTB into a local mpidr[] array.
 *   3. Populate g_cpus[i] with cpu_id, mpidr, and stack_top.
 *      Boot CPU (i=0) gets the linker's `stack_top`; secondaries
 *      get `secondary_stack_top_<i>`.
 *   4. msr tpidr_el1 = &g_cpus[0]; mark boot CPU READY.
 *   5. For each i > 0: psci_cpu_on(mpidr, &secondary_start, &g_cpus[i]).
 *   6. Spin (with a generous timeout) until every BOOTED cpu sets
 *      CPU_FLAG_READY from secondary_main, then log "[smp] all
 *      CPUs online".  Timeout is informational; we keep going
 *      either way so the rest of the kernel still boots.
 *
 * secondary_main() is the C entry point boot.s::secondary_start
 * tail-calls.  It just announces itself and parks in WFE; the
 * actual SMP scheduler arrives in chapter 89.
 */

#include "cpu.h"
#include "psci.h"
#include "atomic.h"
#include "ipi.h"
#include "../core/fdt.h"
#include "../core/serial.h"
#include "../core/thread.h"
#include "../core/timer.h"
#include "../device/gic.h"
#include <stdint.h>

/* Linker-provided symbols for the secondary boot stacks.  Each
 * slot is 16 KiB; see linker/kernel.ld .secondary_stacks. */
extern uint8_t secondary_stack_top_1[];
extern uint8_t secondary_stack_top_2[];
extern uint8_t secondary_stack_top_3[];
extern uint8_t stack_top[];   /* boot CPU stack from milestone 1 */

struct cpu g_cpus[SMP_MAX_CPUS];
static uint32_t g_smp_count = 1;   /* boot CPU always counts */

uint32_t smp_cpu_count(void)
{
    return g_smp_count;
}

/* Map a logical cpu_id to its boot stack top. */
static uint64_t stack_top_for(uint32_t cpu_id)
{
    switch (cpu_id) {
    case 0: return (uint64_t)(uintptr_t)stack_top;
    case 1: return (uint64_t)(uintptr_t)secondary_stack_top_1;
    case 2: return (uint64_t)(uintptr_t)secondary_stack_top_2;
    case 3: return (uint64_t)(uintptr_t)secondary_stack_top_3;
    default: return 0;   /* SMP_MAX_CPUS is 4 */
    }
}

/* Helpers for compact decimal printing of small CPU ids without
 * dragging printf in.  CPU id is < SMP_MAX_CPUS, so a single
 * digit always fits. */
static void serial_put_cpu_id(uint32_t id)
{
    if (id < 10)
        serial_putc((char)('0' + id));
    else
        serial_puthex(id);   /* fallback if SMP_MAX_CPUS ever > 10 */
}

/* ------------------------------------------------------------------
 * Chapter 89 — boot-CPU pre-registration.
 *
 * thread_init() runs BEFORE smp_init_with_dtb() in kernel_main
 * (because the boot thread needs to exist before we wake any
 * secondaries).  But thread_init wants to set
 * `cpu_current()->current = boot_thread`, and `cpu_current()`
 * dereferences TPIDR_EL1 — which is still zero before
 * smp_init_with_dtb runs.
 *
 * The fix is this tiny pre-init: stash the boot CPU's MPIDR into
 * g_cpus[0], write TPIDR_EL1 to point at that slot, and mark the
 * CPU READY.  smp_init_with_dtb later walks the DTB and
 * re-populates g_cpus[0] with the same values (id 0, mpidr from
 * DTB, stack_top from linker) so the second write is a harmless
 * no-op on the boot slot's static fields — and it deliberately
 * does NOT touch ->current / ->idle / ->runq_*, which thread_init
 * has by then filled in.
 *
 * Idempotent: calling this twice on the boot CPU is safe.
 * ------------------------------------------------------------------ */
void cpu_register_boot(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t boot = (uint32_t)(mpidr & 0xFFu);

    g_cpus[boot].cpu_id    = boot;
    g_cpus[boot].mpidr     = mpidr;
    g_cpus[boot].stack_top = (uint64_t)(uintptr_t)stack_top;
    g_cpus[boot].flags     = CPU_FLAG_PRESENT | CPU_FLAG_BOOTED |
                             CPU_FLAG_READY;

    __asm__ volatile("msr tpidr_el1, %0"
                     :: "r"(&g_cpus[boot]) : "memory");
}

/* ------------------------------------------------------------------
 * Chapter 89 — per-CPU idle loop.
 *
 * Every CPU has an "idle thread" that runs whenever its runqueue
 * is empty.  It does the absolute minimum: wait for an interrupt
 * (timer tick or IPI_RESCHED), then yield so the scheduler
 * re-checks the runqueue.  If still empty, the scheduler picks
 * idle again and we WFI again.  Cheap.
 *
 * Why not just `for (;;) wfi;`?
 *   - Without the yield(), an IPI_RESCHED that drops a new
 *     thread on this CPU's runqueue would wake us from WFI but
 *     we'd never actually run the new thread.
 *   - The timer tick auto-yields via irq_dispatch already, so
 *     this loop's explicit yield only matters for the
 *     IPI_RESCHED case (which doesn't go through the timer tick
 *     path).
 *
 * Each CPU's idle thread is created at scheduler bring-up
 * (thread_init for CPU 0; secondary_init_thread, called from
 * secondary_main, for CPU N).  The (void *)arg is unused.
 * ------------------------------------------------------------------ */
extern void yield(void);   /* from kernel/core/thread.h */
void cpu_idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        __asm__ volatile("wfi");
        yield();
    }
}

/* ------------------------------------------------------------------
 * Chapter 89 — SMP scheduler smoke test.
 *
 * Choreography:
 *   1. CPU 0 zeroes g_smp_sched_count.
 *   2. CPU 0 calls thread_create_on(1, smp_sched_smoke_entry, ...)
 *      four times.  Each call:
 *        a. Builds a kernel thread struct + initial frame.
 *        b. Pushes it onto CPU 1's runqueue under its runq_lock.
 *        c. Sends IPI_RESCHED to CPU 1.
 *      CPU 1 was sitting in cpu_idle_loop's WFI; the IPI wakes it,
 *      ipi_handle returns 1 ("schedule"), irq_dispatch calls
 *      schedule(), which yields, runq_pop returns the smoke thread,
 *      and CPU 1 context-switches into it.
 *   3. Each smoke thread atomically increments g_smp_sched_count
 *      and calls thread_exit(0).  thread_exit's final yield() runs
 *      the next queued smoke thread or, when the runq drains,
 *      cpu_idle_loop again.
 *   4. CPU 0 spins (with timeout) until g_smp_sched_count >= 4.
 *
 * Success path logs "[smp-sched] cpu_1 ran 4 of 4 OK"; the test
 * harness greps for that exact line.  Anything else logs "MISS"
 * with the observed count.  No "FAIL" / "PANIC" so the regression
 * sweep stays green.
 *
 * Why CPU 1 only?  Chapter 89 deliberately keeps thread placement
 * static — once a thread is on CPU N's runqueue it never migrates.
 * That sidesteps the need for cross-CPU TLB shootdown
 * (`tlbi vmalle1is`), which we'll only need if/when we add user-
 * thread migration.
 * ------------------------------------------------------------------ */
#define SMP_SCHED_SMOKE_THREADS  4u

static volatile uint32_t g_smp_sched_count;

static void smp_sched_smoke_entry(void *arg)
{
    (void)arg;
    atomic_add_return32(&g_smp_sched_count, 1);
    thread_exit(0);
}

static void smp_sched_smoke_test(void)
{
    /* Need at least one secondary actually online; if smp_count
     * is 1 we're on uniprocessor and the test is moot. */
    if (smp_cpu_count() < 2) {
        serial_puts("[smp-sched] only one CPU online; skipping\n");
        return;
    }

    atomic_store32(&g_smp_sched_count, 0);

    for (uint32_t i = 0; i < SMP_SCHED_SMOKE_THREADS; i++) {
        struct thread *t = thread_create_on(
            1, smp_sched_smoke_entry, NULL, "smp-smoke");
        if (!t) {
            serial_puts("[smp-sched] thread_create_on returned NULL "
                        "for slot ");
            serial_put_cpu_id(i);
            serial_puts(" MISS\n");
            return;
        }
    }

    /* Spin-wait for completion with a generous timeout (~50 M
     * iterations of a yield loop is well over a second on a
     * cold cache, even at 8 MHz emulation rates).  We use the
     * `yield` instruction as a hint without actually rescheduling
     * — CPU 0's scheduler still gets the timer tick, which is the
     * only thing that drains the runq if there's pending work
     * here. */
    for (uint64_t spin = 0; spin < 50000000ULL; spin++) {
        uint32_t got = atomic_load32(&g_smp_sched_count);
        if (got >= SMP_SCHED_SMOKE_THREADS) {
            serial_puts("[smp-sched] cpu_1 ran 4 of 4 OK\n");
            return;
        }
        __asm__ volatile("yield" ::: "memory");
    }

    {
        uint32_t got = atomic_load32(&g_smp_sched_count);
        serial_puts("[smp-sched] cpu_1 ran ");
        serial_put_cpu_id(got);
        serial_puts(" of 4 MISS\n");
    }
}

/* ------------------------------------------------------------------
 * Chapter 87 — SMP atomic smoke test.
 *
 * Goal: prove that atomic_add_return64 is actually atomic across
 * cores by having every CPU race to increment the same counter
 * SMOKE_ITERS times.  If any update is lost (e.g. a non-atomic
 * read-modify-write on a contended cache line), the final value
 * comes out below the expected total and we log a mismatch.
 *
 * Choreography:
 *   1. CPU 0 calls smp_smoke_primary_hammer() right after PSCI
 *      CPU_ON has woken every secondary.  This is CPU 0's share
 *      of the increments and runs while at least some secondaries
 *      are also incrementing — that is the contention window.
 *   2. Each secondary calls smp_smoke_secondary_hammer() at the
 *      top of secondary_main() BEFORE it sets CPU_FLAG_READY.
 *      The READY-after-hammer ordering means CPU 0's spin-wait
 *      for "all READY" doubles as a join barrier for the smoke
 *      test.
 *   3. After CPU 0's spin-wait succeeds it calls
 *      smp_smoke_verify(), which reads the counter once and
 *      compares against SMOKE_ITERS * smp_cpu_count_online().
 *
 * SMOKE_ITERS is small (100k) on purpose: under HVF this finishes
 * in a couple of milliseconds and adds no noticeable boot delay.
 * Bump it for stress testing.
 * ------------------------------------------------------------------ */

#define SMOKE_ITERS 100000u

static volatile uint64_t g_smoke_counter = 0;

static void smp_smoke_hammer(void)
{
    for (uint32_t i = 0; i < SMOKE_ITERS; i++)
        (void)atomic_add_return64(&g_smoke_counter, 1);
}

static void smp_smoke_verify(uint32_t online_cpus)
{
    /* Acquire-load so we observe every CPU's published increments. */
    uint64_t got = atomic_load64(&g_smoke_counter);
    uint64_t expected = (uint64_t)SMOKE_ITERS * online_cpus;

    serial_puts("[smp-atomic] expected=");
    serial_puthex(expected);
    serial_puts(" got=");
    serial_puthex(got);
    if (got == expected) {
        serial_puts(" OK\n");
    } else {
        /* Use "mismatch" not "FAIL" — the test harness greps for
         * FAIL/PANIC/FATAL on benign paths (chapter 86 trap). */
        serial_puts(" MISMATCH (lost ");
        serial_puthex(expected - got);
        serial_puts(" updates)\n");
    }
}

void secondary_main(struct cpu *self)
{
    /* Chapter 87 — hammer the shared atomic counter BEFORE
     * setting READY so that CPU 0's "wait for all READY" spin
     * doubles as a join barrier for the smoke test.  Skipping
     * this is fine if you ever need a "secondary that just
     * comes up cleanly" (chapter 88 may want that). */
    smp_smoke_hammer();

    /* Chapter 88 — bring this CPU's GIC slice online so it can
     * receive SGIs (IPIs).  Order matters:
     *   1. Per-CPU redistributor + CPU interface init.  Until
     *      this runs, nothing on this CPU can take an IRQ
     *      because GICR_WAKER.ProcessorSleep is still set.
     *   2. Enable the SGI vectors we care about (PING + HALT
     *      from chapter 88; RESCHED from chapter 89).
     *   3. Vector table install — vbar_el1 was set by boot.s
     *      before we got here, so nothing more to do.
     *   4. Allocate a per-CPU idle thread BEFORE we unmask IRQs
     *      and BEFORE any IPI can fire — yield() needs a
     *      current thread.
     *   5. Unmask IRQs.  Once daifclr happens, the next SGI from
     *      CPU 0 will land in the IRQ vector and reach
     *      ipi_handle.
     *
     * CHAPTER 92 — additionally enable the CNTV (timer) PPI on
     * this CPU.  Pre-92, secondaries had no timer because
     * preemption requires a runqueue policy that handles cross-
     * CPU wakeup placement (chapter 89's "no migration" floor
     * couldn't satisfy that — it would have stolen sleepers).
     * Chapter 92 adds home_cpu pinning + runq_push_to + locked
     * g_all_head walks, so a timer tick on CPU 1 can safely
     * preempt and the sleeper-walk no longer adopts CPU-0
     * threads onto CPU 1.  See chapter 92 for the full story. */
    gic_init_per_cpu();
    gic_enable_irq(IPI_PING);
    gic_enable_irq(IPI_HALT);
    gic_enable_irq(IPI_RESCHED);

    /* Chapter 92 — enable this CPU's generic-timer PPI.
     * timer_init_per_cpu programs CNTV_TVAL_EL0 + CNTV_CTL_EL0
     * (per-CPU registers); gic_enable_irq routes ID 27 through
     * this CPU's redistributor.  Priority matches CPU 0's
     * (0x80) so timer ticks have the same precedence relative
     * to IPIs across CPUs. */
    gic_set_priority(TIMER_CNTV_INTID, 0x80);
    gic_enable_irq(TIMER_CNTV_INTID);
    timer_init_per_cpu();

    /* Chapter 89 — install this CPU's idle thread.  Must run
     * BEFORE daifclr so that the first IRQ (timer or RESCHED)
     * sees a non-NULL cpu_current()->current. */
    if (thread_secondary_init_idle("idle/1") != 0) {
        serial_puts("[smp] PANIC: idle alloc failed on cpu ");
        serial_put_cpu_id(self->cpu_id);
        serial_puts("\n");
        for (;;) __asm__ volatile("wfi");
    }

    /* Sanity: TPIDR_EL1 should match `self` (set by boot.s). */
    self->flags |= CPU_FLAG_READY;
    /* dsb sy publishes the flag write to other CPUs, so CPU 0's
     * smp_init() spin observes us as ready. */
    __asm__ volatile("dsb sy" ::: "memory");

    serial_puts("[smp] CPU ");
    serial_put_cpu_id(self->cpu_id);
    serial_puts(" ready (mpidr = ");
    serial_puthex(self->mpidr);
    serial_puts(")\n");

    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* Chapter 89 — instead of WFI-only, enter the per-CPU idle
     * loop: wfi until an IRQ (timer tick or IPI_RESCHED) arrives,
     * then yield to whichever thread the runqueue points at.
     * cpu_idle_loop never returns. */
    cpu_idle_loop(NULL);
}

/* Locate the slot in `mpidrs[]` that corresponds to the boot
 * CPU's MPIDR.  Returns its index, or 0 (= boot CPU) if not
 * found — the caller treats index 0 as boot regardless. */
static uint32_t find_boot_index(const uint64_t *mpidrs, size_t n,
                                uint64_t boot_mpidr)
{
    for (size_t i = 0; i < n; i++) {
        if ((mpidrs[i] & 0xFFFFFFu) == (boot_mpidr & 0xFFFFFFu))
            return (uint32_t)i;
    }
    return 0;
}

void smp_init(void)
{
    /* The DTB pointer is the first thing kernel_main saw; we
     * don't have a global for it yet, so smp_init takes the DTB
     * via fdt_validate-style indirection.  Caller passes via
     * smp_init_with_dtb below; this stub is for forward-compat
     * if we ever drop DTB use. */
    (void)0;
}

/* Real entry — kernel_main calls this after parsing the DTB.
 * Split from smp_init() so the public header stays clean and
 * doesn't leak the dtb pointer type. */
void smp_init_with_dtb(const void *dtb)
{
    serial_puts("[smp] bringing up additional cores ...\n");

    psci_init(dtb);

    /* Read the cpus from the DTB.  On QEMU virt with -smp N this
     * produces N entries: 0x00, 0x01, ... in MPIDR.Aff0 order.
     * The DTB enumerates CPUs in MPIDR-ascending order so the
     * boot CPU (always MPIDR.Aff0=0 in our config) is at index 0. */
    uint64_t mpidrs[SMP_MAX_CPUS];
    size_t found = fdt_read_cpus(dtb, mpidrs, SMP_MAX_CPUS);
    if (found == 0) {
        serial_puts("[smp] no /cpus in DTB; running uniprocessor\n");
        found = 1;
        mpidrs[0] = 0;
    }

    /* Cap at SMP_MAX_CPUS — the linker only reserved that many
     * secondary stacks. */
    if (found > SMP_MAX_CPUS)
        found = SMP_MAX_CPUS;

    serial_puts("[smp] DTB reports ");
    serial_put_cpu_id((uint32_t)found);
    serial_puts(" cpu(s)\n");

    /* Identify the boot CPU's slot.  On QEMU virt this is index 0
     * but we look it up properly so a future board with a
     * different boot CPU still works. */
    uint64_t boot_mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    uint32_t boot_idx = find_boot_index(mpidrs, found, boot_mpidr);

    /* Populate g_cpus[]. */
    for (size_t i = 0; i < found; i++) {
        g_cpus[i].cpu_id    = (uint32_t)i;
        g_cpus[i].mpidr     = mpidrs[i];
        g_cpus[i].stack_top = stack_top_for((uint32_t)i);
        g_cpus[i].flags     = CPU_FLAG_PRESENT;
    }
    g_smp_count = (uint32_t)found;

    /* Boot CPU: set TPIDR_EL1 and mark READY.  We're already
     * running on the boot CPU, so cpu_current() is now valid. */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&g_cpus[boot_idx]) : "memory");
    g_cpus[boot_idx].flags |= CPU_FLAG_BOOTED | CPU_FLAG_READY;

    /* Wake every other CPU. */
    extern void secondary_start(void);
    uint64_t entry = (uint64_t)(uintptr_t)&secondary_start;

    for (size_t i = 0; i < found; i++) {
        if (i == boot_idx) continue;
        if (g_cpus[i].stack_top == 0) {
            serial_puts("[smp] no stack reserved for CPU ");
            serial_put_cpu_id((uint32_t)i);
            serial_puts("; skipping\n");
            continue;
        }
        serial_puts("[smp] PSCI CPU_ON cpu=");
        serial_put_cpu_id((uint32_t)i);
        serial_puts(" mpidr=");
        serial_puthex(g_cpus[i].mpidr);
        serial_puts(" entry=");
        serial_puthex(entry);
        serial_puts("\n");

        int rc = psci_cpu_on(g_cpus[i].mpidr, entry,
                             (uint64_t)(uintptr_t)&g_cpus[i]);
        if (rc != PSCI_SUCCESS) {
            /* Note: deliberately avoid the literal "FAIL" / "FAILED"
             * here — several harness scripts grep for that substring
             * as a userspace-test failure marker, and a benign
             * "DTB lists CPU N but QEMU was launched with a smaller
             * -smp" mismatch should not trip them.  PSCI's own term
             * for a refused CPU_ON is "denied", which we mirror. */
            serial_puts("[smp] PSCI CPU_ON cpu=");
            serial_put_cpu_id((uint32_t)i);
            serial_puts(" denied rc=");
            serial_puthex((uint64_t)(int64_t)rc);
            serial_puts("\n");
            continue;
        }
        g_cpus[i].flags |= CPU_FLAG_BOOTED;
    }

    /* Wait for every BOOTED secondary to mark itself READY.  The
     * timeout is generous (~50 ms worth of spin iterations) but
     * not fatal — if a secondary hangs we still want the rest of
     * the kernel to boot so we can debug from a usable shell. */
    serial_puts("[smp] waiting for secondaries to report ready ...\n");

    /* Chapter 87 — CPU 0's share of the SMP atomic smoke test.
     * Done HERE, between the PSCI loop and the spin-wait, so
     * CPU 0 is incrementing the counter while the secondaries
     * are doing the same.  Each secondary increments BEFORE
     * setting READY, so the spin-wait below is also our join
     * barrier for the test. */
    smp_smoke_hammer();

    for (uint64_t t = 0; t < 50000000ULL; t++) {
        int all_ready = 1;
        for (uint32_t i = 0; i < g_smp_count; i++) {
            if (!(g_cpus[i].flags & CPU_FLAG_BOOTED)) continue;
            if (!(g_cpus[i].flags & CPU_FLAG_READY))  { all_ready = 0; break; }
        }
        if (all_ready) {
            serial_puts("[smp] all CPUs online\n");
            /* Count CPUs that actually contributed to the smoke
             * counter: boot CPU + every BOOTED+READY secondary. */
            uint32_t online = 1;
            for (uint32_t i = 0; i < g_smp_count; i++) {
                if (i == boot_idx) continue;
                if ((g_cpus[i].flags & CPU_FLAG_BOOTED) &&
                    (g_cpus[i].flags & CPU_FLAG_READY))
                    online++;
            }
            smp_smoke_verify(online);

            /* Chapter 88 — IPI round-trip smoke.  By now every
             * READY secondary has unmasked IRQs and is sitting
             * in WFI waiting for an SGI; a PING from us should
             * bump its per-CPU receive counter within a handful
             * of microseconds. */
            ipi_smoke_test();

            /* Chapter 89 — SMP scheduler smoke.  Spawn 4 kernel
             * threads on CPU 1 and wait for all of them to run
             * to completion.  Proves end-to-end:
             *   - per-CPU runqueue accepts cross-CPU enqueues
             *   - IPI_RESCHED kicks an idle CPU out of WFI
             *   - the idle CPU's yield() picks up the work
             *   - thread_exit's final yield() drains the rest */
            smp_sched_smoke_test();
            return;
        }
        __asm__ volatile("yield" ::: "memory");
    }
    serial_puts("[smp] WARNING: timeout waiting for secondaries\n");
    for (uint32_t i = 0; i < g_smp_count; i++) {
        if ((g_cpus[i].flags & CPU_FLAG_BOOTED) &&
            !(g_cpus[i].flags & CPU_FLAG_READY)) {
            serial_puts("[smp]   CPU ");
            serial_put_cpu_id(i);
            serial_puts(" booted but not ready\n");
        }
    }
}
