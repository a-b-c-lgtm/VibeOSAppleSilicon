/*
 * kernel/core/walltime.c — PL031 RTC driver + boot-snapshot
 * wall-clock for chapter 95.
 *
 * The PL031 is a deliberately tiny device — three relevant
 * registers, no setup needed because QEMU's `virt` machine
 * pre-arms it.  Every byte we touch:
 *
 *   +0x00  RTCDR  (RO)  current time, 32-bit seconds-since-epoch
 *   +0x04  RTCMR  (RW)  match register (alarm IRQ; unused here)
 *   +0x08  RTCLR  (RW)  load register (write to set time)
 *   +0x0C  RTCCR  (RW)  control register (bit 0 = enable; QEMU
 *                       hard-wires this on, so we never write it)
 *
 * We read RTCDR exactly once at boot and pair the value with a
 * `timer_ticks()` snapshot.  All future `walltime_now_us` calls
 * derive from those two numbers and the live tick counter — no
 * MMIO traffic in the hot path.  See walltime.h for the rationale.
 *
 * Y2038: the PL031 itself is a 32-bit register, so it wraps in
 * January 2038.  Inside the kernel we promote it to int64 at the
 * boot read so subsequent uptime-based extrapolation is well-
 * defined past the wrap, and the syscall ABI returns int64 too.
 * Re-snapshotting after a deliberate kernel-side RTC reload (we
 * don't ship that) would be the way to handle the wrap on a
 * long-lived install; on real M-class hardware we'd switch to a
 * 64-bit RTC entirely.
 *
 * Safety: the PL031 mapping is a Device-nGnRnE region inherited
 * from the early L1 block descriptor that already covers
 * 0x09000000–0x09FFFFFF (PL011 UART, GPIO, RTC).  No further
 * page-table work is needed.
 */

#include "walltime.h"
#include "fdt.h"
#include "timer.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

/* PL031 register offsets.  Only RTCDR is used today. */
#define PL031_RTCDR 0x000u

/* Hard-coded fallback base address.  Matches QEMU virt's
 * hardwired layout, which has been stable since the machine was
 * introduced.  We only fall back to this if the DTB lookup
 * fails — exotic boards / future virt revisions would override
 * by editing the DTB, not by editing the kernel. */
#define PL031_FALLBACK_BASE 0x09010000UL

/* Boot snapshot.  Set by walltime_init; read by walltime_now_us.
 * Both fields are immutable after init, so no locking is needed
 * even on SMP.  We deliberately keep the snapshot in BOOT_TICKS
 * rather than uptime-ms because timer_ticks() is the smallest
 * unit the timer code exports and converting to ms here would
 * lose precision below TICK_INTERVAL_MS. */
static volatile uint64_t g_boot_ticks    = 0;
static int64_t           g_boot_walltime = 0;
static int               g_have_rtc      = 0;

/* Re-derive ms-per-tick exactly the same way timer.c did, but
 * without exporting that intermediate value.  TICK_INTERVAL_MS
 * is the per-tick granularity in milliseconds (10/100/...) so
 * "ticks * TICK_INTERVAL_MS" gives uptime ms.  We multiply by
 * 1000 separately in walltime_now_us to keep this expression
 * exact in integer arithmetic. */
static inline uint64_t ticks_to_us(uint64_t ticks)
{
    return ticks * (uint64_t)TICK_INTERVAL_MS * 1000ULL;
}

/* Read PL031 RTCDR.  Volatile load through the device mapping;
 * the result is plain little-endian (PL031 has the standard
 * AMBA endianness). */
static uint32_t pl031_read_rtcdr(uint64_t base)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)
                           (base + PL031_RTCDR);
    return *p;
}

int walltime_init(const void *dtb)
{
    uint64_t base = 0;
    int found = fdt_read_pl031(dtb, &base);
    if (!found) {
        /* DTB lookup failed.  Try the well-known QEMU virt
         * address as a courtesy — chapters before 95 didn't
         * need this and we don't want a dropped DTB to break
         * boot. */
        serial_puts("[walltime] no /pl031 in DTB; trying "
                    "fallback 0x09010000\n");
        base = PL031_FALLBACK_BASE;
    }

    /* Read the RTC and pair it with the matching tick count.
     * Order matters slightly: we want the tick read to happen
     * AS CLOSE AS POSSIBLE to the RTCDR read so the residual
     * "elapsed since snapshot" is minimised.  An MMIO read
     * here is cheap (no VM exit on KVM/HVF for PL031); we do
     * RTC first, then ticks. */
    uint32_t rtc_now = pl031_read_rtcdr(base);
    uint64_t ticks_now = timer_ticks();

    /* If the RTC reads back as 0, treat that as "no real RTC"
     * — QEMU virt's PL031 is always live and returns the host
     * clock, so 0 only happens on broken hardware (or if the
     * mapping is bogus and we read garbage zeroed memory). */
    if (rtc_now == 0 && !found) {
        serial_puts("[walltime] PL031 RTCDR == 0 at fallback "
                    "base; treating as no-RTC\n");
        g_boot_walltime = 0;
        g_boot_ticks    = ticks_now;
        g_have_rtc      = 0;
        return 0;
    }

    g_boot_walltime = (int64_t)rtc_now;
    g_boot_ticks    = ticks_now;
    g_have_rtc      = 1;

    serial_puts("[walltime] PL031 base = ");
    serial_puthex(base);
    serial_puts(", boot wall-time = ");
    serial_puthex((uint64_t)rtc_now);
    serial_puts(" (s since epoch)\n");
    return 1;
}

int walltime_have_rtc(void)
{
    return g_have_rtc;
}

void walltime_now_us(int64_t *secs_out, uint32_t *usecs_out)
{
    /* Snapshot ticks once so we can split the same value into
     * seconds + microseconds without the underlying counter
     * incrementing between the two divisions. */
    uint64_t ticks_now   = timer_ticks();
    uint64_t delta_ticks = ticks_now - g_boot_ticks;
    uint64_t delta_us    = ticks_to_us(delta_ticks);

    int64_t  secs        = g_boot_walltime + (int64_t)(delta_us / 1000000ULL);
    uint32_t usecs       = (uint32_t)(delta_us % 1000000ULL);

    if (secs_out)  *secs_out  = secs;
    if (usecs_out) *usecs_out = usecs;
}
