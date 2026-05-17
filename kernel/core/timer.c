/*
 * kernel/core/timer.c — ARM generic timer driver.
 *
 * The "generic timer" is the architecturally-required per-CPU
 * timer block on every AArch64 system.  It exposes a free-running
 * counter (CNTPCT_EL0) ticking at a frequency reported by
 * CNTFRQ_EL0, plus a per-EL "physical" downcounter (CNTP_TVAL_EL0)
 * that fires an interrupt when it reaches zero.
 *
 * For our tick we use:
 *
 *   CNTFRQ_EL0   — read once at init; counter rate (Hz).
 *   CNTV_TVAL_EL0— write a positive value to set time-until-fire,
 *                   in ticks.  The hardware decrements it.
 *   CNTV_CTL_EL0 — bit 0 ENABLE, bit 1 IMASK, bit 2 ISTATUS.
 *
 * We use CNTV (the *virtual* timer) rather than CNTP (physical)
 * because under HVF the physical timer is owned by EL2 and writes
 * to CNTP_*_EL0 from EL1 trap.  The virtual timer is exposed to
 * guests directly.
 *
 * On Apple Silicon under HVF, CNTFRQ_EL0 typically reads as
 * 24_000_000 (24 MHz), the same value bare-metal M-series CPUs
 * use.  TCG with -cpu cortex-a72 reports 62_500_000 (62.5 MHz).
 * We do the math at runtime so either works.
 */

#include "timer.h"
#include "../arch/atomic.h"
#include <stdint.h>

static uint32_t g_interval_ticks = 0;
/* Chapter 92 — atomic because both CPUs now take the timer PPI
 * and call timer_tick from IRQ context.  A plain `g_ticks++`
 * would lose updates under the resulting cross-CPU race.  The
 * counter is also read from yield()'s sleeper-walk and from
 * SYS_UPTIME_MS, so an atomic load is required to avoid tearing
 * a partial 64-bit read on the slow ARMv8 path.
 *
 * Chapter 106b — DEPRECATED for wall-time use.  Under SMP every
 * CPU takes its own timer PPI every TICK_INTERVAL_MS, so a
 * naive `g_ticks++` per IRQ on N CPUs counts at N× the real
 * rate.  We now read CNTVCT_EL0 directly in timer_ticks() and
 * convert to TICK_INTERVAL_MS units.  The `g_ticks` IRQ counter
 * is kept (for any caller that still cares about "how many
 * scheduler quanta have elapsed on this CPU" — none today) but
 * is NOT what timer_ticks() returns. */
static volatile uint64_t g_ticks = 0;

/* Counter frequency in Hz, captured at init.  Used to convert
 * CNTVCT_EL0 (free-running, per-CPU but synchronised by HVF /
 * TCG so the value is monotone-coherent across CPUs) into our
 * TICK_INTERVAL_MS-quantised "ticks" unit. */
static uint64_t g_cntfrq = 0;
/* Pre-computed: ticks-per-our-quantum.  freq * interval_ms / 1000.
 * Same value as g_interval_ticks above; kept separately so we
 * never accidentally read 0 if timer_init hasn't run yet. */
static uint64_t g_ticks_per_quantum = 1;

static inline uint64_t cntvct_el0_read(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t cntfrq_el0_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void cntv_tval_el0_write(uint64_t v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(v) : "memory");
}

static inline void cntv_ctl_el0_write(uint64_t v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0\n\tisb" :: "r"(v) : "memory");
}

void timer_init(uint32_t interval_ms)
{
    uint64_t freq = cntfrq_el0_read();      /* ticks per second */
    /* ticks_per_ms = freq / 1000.  interval_ticks = interval_ms *
     * ticks_per_ms.  Rearrange to avoid losing precision on small
     * intervals: interval_ticks = freq * interval_ms / 1000. */
    g_interval_ticks = (uint32_t)((freq * interval_ms) / 1000ULL);
    g_cntfrq = freq;
    g_ticks_per_quantum = (freq * (uint64_t)interval_ms) / 1000ULL;
    if (g_ticks_per_quantum == 0) g_ticks_per_quantum = 1;

    /* Arm the timer: write the down-counter, then enable. */
    cntv_tval_el0_write(g_interval_ticks);
    cntv_ctl_el0_write(1ULL);   /* ENABLE=1, IMASK=0 */
}

/* Per-CPU re-arm.  Used by chapter 89's secondary_main: the
 * down-counter (CNTV_TVAL_EL0) and control register
 * (CNTV_CTL_EL0) are per-CPU, so each CPU has to program its
 * own.  We reuse the already-computed g_interval_ticks; on
 * cortex-a72 cntfrq_el0 is identical across cores, so re-reading
 * it would give the same value — saving the MRS keeps the
 * secondary path tiny. */
void timer_init_per_cpu(void)
{
    cntv_tval_el0_write(g_interval_ticks);
    cntv_ctl_el0_write(1ULL);
}

void timer_rearm(void)
{
    cntv_tval_el0_write(g_interval_ticks);
}

uint64_t timer_ticks(void)
{
    /* Chapter 106b: derive wall-time "ticks" (units of
     * TICK_INTERVAL_MS) from the free-running CNTVCT_EL0 counter
     * rather than the per-IRQ counter `g_ticks`.  Under SMP both
     * CPUs took the timer PPI and each incremented g_ticks, so
     * g_ticks ran at N× wall time and uptime_ms() reported
     * inflated values (a 30-second wall fetch claimed
     * 60_000 ms ms).  CNTVCT is monotone, free-running, and
     * coherent across CPUs on QEMU virt + HVF + TCG, so it gives
     * a single source of truth.  g_ticks is kept for callers
     * that still want "how many scheduler quanta did this CPU
     * see" but is no longer the wall clock.
     *
     * Quantisation matches the old behaviour: we round down to
     * units of TICK_INTERVAL_MS so callers that compared
     * `timer_ticks()` values see the same coarse granularity. */
    if (g_ticks_per_quantum <= 1) return 0;
    return cntvct_el0_read() / g_ticks_per_quantum;
}

void timer_tick(void)
{
    (void)atomic_add_return64(&g_ticks, 1);
}
