/*
 * kernel/core/timer.h — ARM generic timer (CNTV) public API.
 *
 * The generic timer fires PPI ID 27 ("CNTV" — the virtual timer)
 * on every expiry.  Under HVF (and any other AArch64 hypervisor)
 * the physical timer CNTP is owned by EL2; EL1 guests must use
 * the virtual timer instead.  Writing CNTP_TVAL_EL0 from a guest
 * traps to EL2, which on HVF presents as ESR.EC=0 ("Unknown
 * reason") — the giveaway that you are touching a trapped
 * system register.  See chapter 7 of the book for the full
 * derivation.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Interrupt ID assigned to CNTV on the QEMU virt machine. */
#define TIMER_CNTV_INTID 27u

/* Per-tick interval that timer_init was called with.  We rearm
 * the timer with this value after every IRQ, and SYS_UPTIME_MS
 * uses it to convert tick count into wall milliseconds. */
#define TICK_INTERVAL_MS 100u

/* Configure the generic timer to fire every `interval_ms`
 * milliseconds.  Reads CNTFRQ_EL0 to translate ms into timer ticks
 * and arms CNTV_TVAL_EL0 + CNTV_CTL_EL0.  The IRQ must be enabled
 * separately via gic_enable_irq(TIMER_CNTV_INTID). */
void timer_init(uint32_t interval_ms);

/* Chapter 90 — arm THIS CPU's CNTV down-counter using the
 * interval previously set by timer_init.  Must be called once
 * per secondary CPU during bring-up; CNTV_TVAL_EL0 / CNTV_CTL_EL0
 * are per-CPU registers, so each CPU programs its own.  Safe to
 * call before gic_enable_irq(TIMER_CNTV_INTID): the IRQ stays
 * masked at the GIC until that. */
void timer_init_per_cpu(void);

/* Re-arm the timer for another `interval_ms` worth of ticks.  Call
 * from the IRQ handler after acknowledging the interrupt so the
 * next tick fires on schedule. */
void timer_rearm(void);

/* Total ticks observed since timer_init.  Updated by the IRQ
 * handler. */
uint64_t timer_ticks(void);

/* Increment the tick counter.  Called by irq_dispatch after
 * gic_end_of_irq for TIMER_CNTV_INTID. */
void timer_tick(void);

#endif /* TIMER_H */
