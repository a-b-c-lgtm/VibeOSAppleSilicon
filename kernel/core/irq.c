/*
 * kernel/core/irq.c — top-level IRQ dispatcher.
 *
 * Called from kernel/arch/vectors.S irq_entry once per interrupt
 * with the saved register frame as the only argument.  Reads the
 * highest-priority pending interrupt from the CPU interface, runs
 * its handler, and signals end-of-interrupt.
 *
 * Milestone 2 only routed the ARM generic timer (PPI 27).
 * Chapter 88 added SGIs (IDs 0..15) for IPIs; everything else
 * still falls through to the catch-all log+EOI.  When chapter 22
 * (virtio-mmio) gets enough devices to justify it, this switch
 * becomes a registered-handler array.
 */

#include "exception.h"
#include "serial.h"
#include "timer.h"
#include "thread.h"
#include "../arch/ipi.h"
#include "../device/gic.h"
#include <stdint.h>

/* Spurious interrupt ID returned by ICC_IAR1_EL1 when no real IRQ
 * is pending (e.g. because it was withdrawn between assertion and
 * acknowledge). */
#define INTID_SPURIOUS 1023u

void irq_dispatch(struct exception_frame *frame)
{
    (void)frame;

    uint32_t intid = gic_acknowledge_irq();
    if (intid == INTID_SPURIOUS)
        return;

    int do_schedule = 0;

    /* SGIs (IDs 0..15) are inter-processor interrupts.  They get
     * dispatched through the IPI module which knows the per-vector
     * handler (chapter 88).  Today the only IPIs that fire are
     * IPI_PING (smoke test) and IPI_HALT (parking secondaries on
     * shutdown); ipi_handle returns whether the interrupt should
     * trigger a reschedule on return.  Note: IPI_HALT never
     * returns, so the EOI below for that intid is dead code on
     * the halting CPU — that is expected. */
    if (intid < 16u) {
        do_schedule = ipi_handle(intid);
        gic_end_of_irq(intid);
        if (do_schedule)
            schedule();
        return;
    }

    switch (intid) {
    case TIMER_CNTV_INTID:
        timer_rearm();
        timer_tick();
        /* Note: we do NOT pump the tablet from IRQ context.
         * virtio_tablet_poll → wm_pointer_move → compose_all →
         * fb_present uses globals (g_avail_idx_seen,
         * g_used_idx_seen in virtio_gpu.c) that race with
         * whatever userspace fb_present happens to be in flight.
         * The cursor is instead pumped by the desktop process'
         * yield-loop (see userspace/desktop/desktop.c). */
        do_schedule = 1;
        break;
    default:
        serial_puts("[irq] unexpected intid ");
        serial_puthex(intid);
        serial_puts("\n");
        break;
    }

    /* Always finish the GIC handshake before re-entering the
     * scheduler.  Otherwise the running priority on the CPU
     * interface stays elevated and the next timer tick is masked
     * for the freshly-switched-in thread. */
    gic_end_of_irq(intid);

    if (do_schedule)
        schedule();
}
