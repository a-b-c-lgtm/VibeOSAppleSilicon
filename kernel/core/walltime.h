/*
 * kernel/core/walltime.h — wall-clock time API.
 *
 * Chapter 95 of the book.  Until now the only notion of time the
 * kernel had was the monotonic tick counter from `timer.c`
 * (chapter 10), which counts milliseconds since boot.  That is
 * great for measuring elapsed time inside the kernel, but it
 * cannot answer "what time is it?" — there is no datum.
 *
 * This header pairs the monotonic clock with a single boot-time
 * read of the PL031 RTC (compatible = "arm,pl031") that the QEMU
 * `virt` machine exposes at 0x09010000.  We snapshot the RTC's
 * 32-bit seconds-since-Unix-epoch ONCE during boot together with
 * the matching uptime, then derive walltime in O(1) without ever
 * touching the RTC again:
 *
 *     walltime_us = (rtc_at_boot_us) + (uptime_us - uptime_at_boot_us)
 *
 * Reading the RTC every gettimeofday call would mean an MMIO load
 * (and on some hosts a VM exit) per syscall — wasteful when the
 * generic timer already gives us microsecond resolution between
 * boot and now.  The downside is that we don't pick up host clock
 * adjustments after boot; that's fine for chapter 95 (NTP is a
 * future milestone).
 *
 * The boot snapshot also avoids the PL031 second-rollover race:
 * RTCDR ticks once a second, so two reads spanning a tick can
 * differ by a full second.  We just read it once.
 *
 * `walltime_init` is idempotent — main.c calls it after timer
 * setup; secondary CPUs do NOT need to call it again.
 *
 * `walltime_now_us` always returns success; if the RTC was never
 * found at boot, *secs_out is the Unix epoch (== 0) and the
 * usec part still progresses with uptime, so callers that just
 * want a monotonic timestamp keep working.  See
 * `walltime_have_rtc()` for the "did we find a real RTC?" probe.
 */

#ifndef WALLTIME_H
#define WALLTIME_H

#include <stdint.h>

/* Initialise the wall-clock subsystem.  Walks `dtb` for a
 * "arm,pl031"-compatible node, mmap-reads its first `reg` cell
 * as the MMIO base, reads RTCDR (offset 0) to get
 * seconds-since-1970-01-01 UTC, and pairs that read with the
 * current `timer_ticks()` value to form the boot snapshot.
 *
 * Safe to call before or after the timer subsystem is armed,
 * but uptime-derived microseconds will only advance after the
 * timer interrupt is enabled (chapters 10/89).
 *
 * Returns 1 on success (RTC found, snapshot taken) or 0 if no
 * "arm,pl031" node was discoverable; the latter is non-fatal —
 * walltime_now_us still works but counts from epoch.
 */
int walltime_init(const void *dtb);

/* True iff `walltime_init` actually found an RTC.  Useful for
 * tests / userspace tools that want to distinguish "1970"-shaped
 * fallback output from a real timestamp. */
int walltime_have_rtc(void);

/* Query the current wall-clock time.  *secs_out receives the
 * 64-bit seconds-since-1970-01-01-UTC count; *usecs_out receives
 * the sub-second part in microseconds (0 .. 999_999).  Either
 * pointer may be NULL.
 *
 * The split-int return shape (rather than a single 64-bit
 * microsecond count) matches POSIX `struct timeval`, which is
 * what userspace expects to receive via SYS_GETTIMEOFDAY.
 */
void walltime_now_us(int64_t *secs_out, uint32_t *usecs_out);

#endif /* WALLTIME_H */
