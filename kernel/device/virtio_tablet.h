/*
 * kernel/device/virtio_tablet.h — milestone-41 virtio-input tablet.
 *
 * On the QEMU `virt` machine the simplest pointing device is the
 * `-device virtio-tablet-device`, which speaks Linux evdev with
 * ABSOLUTE coordinates: every motion event arrives as EV_ABS X
 * and EV_ABS Y in the device's [min..max] range (typically
 * 0..0x7FFF).  The driver scales these into framebuffer pixels and
 * pushes them into the WM via wm_pointer_move().
 *
 * Mouse buttons (BTN_LEFT/RIGHT/MIDDLE) arrive as EV_KEY events with
 * value=1 (press) / 0 (release).  We forward them via
 * wm_pointer_button() so the WM can implement focus, drag and the
 * close button on the title bar.
 *
 * Why not a relative-mode mouse?
 *   `-device virtio-mouse-device` produces EV_REL deltas instead.
 *   Under HVF the host pointer is captured by Cocoa and the deltas
 *   come through fine, but the user has to click into the QEMU
 *   window first, and pointer-locked mode is hostile to ad-hoc
 *   testing.  The tablet is plug-and-play and matches what every
 *   other QEMU GUI demo uses.
 *
 * Polling vs IRQs — same story as virtio_input.c.  We poll lazily
 * from sys_yield / sys_gui_poll_event (see kernel/core/syscall.c
 * `pump_input_into_wm`).
 */

#ifndef KERNEL_DEVICE_VIRTIO_TABLET_H
#define KERNEL_DEVICE_VIRTIO_TABLET_H

#include <stdint.h>

/* Probe the virtio-mmio bus for an input device whose EV_BITS
 * advertise EV_ABS support.  Returns 0 on success, -1 if no
 * such device exists.  Safe to call when no tablet is attached. */
int  virtio_tablet_init(void);

/* 1 if a tablet was found and initialised. */
int  virtio_tablet_present(void);

/* Drain the eventq, push wm_pointer_* updates as needed.  Idempotent. */
void virtio_tablet_poll(void);

#endif /* KERNEL_DEVICE_VIRTIO_TABLET_H */
