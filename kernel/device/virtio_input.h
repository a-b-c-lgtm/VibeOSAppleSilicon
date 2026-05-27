/*
 * kernel/device/virtio_input.h — virtio-input keyboard.
 *
 * The QEMU `virt` machine has no PS/2 controller.  To accept keyboard
 * input from a graphical window (Cocoa, GTK, ...) we attach a
 * virtio-input device of subtype "keyboard":
 *
 *     -device virtio-keyboard-device
 *
 * On the wire, virtio-input speaks Linux evdev: each event is the
 * 8-byte `struct virtio_input_event { type, code, value }`.  Type
 * codes we care about are EV_KEY (1) for press/release and EV_SYN
 * (0) for end-of-batch markers (which we ignore).
 *
 * Event delivery model
 * --------------------
 * The device has two virtqueues: eventq (id 0, device-to-driver) and
 * statusq (id 1, driver-to-device, used for LED control etc).  We
 * only set up the eventq.  At init time we publish QUEUE_SIZE
 * device-writable descriptors, each pointing at a fresh 8-byte slot
 * in our shared page; QEMU fills them as keys are pressed.
 *
 * Polling vs IRQ
 * --------------
 * We poll the used ring lazily from inside `console_in_try_getc`
 * (called from the shell's read(0) loop).  This keeps the driver
 * tiny and side-steps GIC SPI registration.  Once the WM lands
 * we register a real GIC handler so the
 * compositor wakes on input even when no thread is polling.
 *
 * ASCII translation
 * -----------------
 * evdev keycodes are stable (KEY_A == 30 forever).  We carry a
 * small unshifted+shifted table for printable ASCII, plus a few
 * control codes (Backspace, Tab, Enter, Esc) and shift-state
 * tracking for the modifier keys.  Cursor keys arrive as the
 * standard ANSI escape sequences (ESC `[A`/`B`/`C`/`D`) so the
 * existing readline editor in /bin/sh sees
 * them unchanged.
 */

#ifndef KERNEL_DEVICE_VIRTIO_INPUT_H
#define KERNEL_DEVICE_VIRTIO_INPUT_H

#include <stdint.h>

/* Probe the virtio-mmio bus for an input device of subtype keyboard.
 * Returns 0 on success, negative if no keyboard was found or the
 * handshake failed.  Safe to call when no virtio-keyboard-device is
 * attached — the kernel will simply have no keyboard input source
 * besides the PL011 serial port. */
int virtio_input_init(void);

/* 1 if a virtio-input keyboard was found and brought up. */
int virtio_input_present(void);

/* Drain the eventq into the internal ASCII ring buffer.  Idempotent
 * — safe to call repeatedly with no events pending.  Called on
 * every console_in_try_getc tick. */
void virtio_input_poll(void);

/* Pop one ASCII byte from the internal ring.  Returns 1 on success
 * and writes the byte into *out, 0 if the ring is empty. */
int  virtio_input_try_getc(char *out);

/* Pop one key-release event from the internal release ring.  The
 * out value is the same GUI key code (ASCII byte 0..255, or one of
 * the GUI_KEY_* extended codes 0x101..0x108) that the matching
 * press would have delivered through the byte-stream path, so the
 * focused WM ring sees press / release as a symmetric pair.  Modifier
 * keys (shift, ctrl, alt, capslock) are NOT enqueued — their press
 * never produces a byte either; the existing g_shift_down /
 * g_ctrl_down trackers handle modifier release internally.
 *
 * Returns 1 on success (writes one uint32_t to *out), 0 if empty. */
int  virtio_input_try_get_release(uint32_t *out);

#endif /* KERNEL_DEVICE_VIRTIO_INPUT_H */
