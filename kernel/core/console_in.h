/*
 * kernel/core/console_in.h — unified console input source.
 *
 * Two input sources can deliver bytes to fd 0 (the console):
 *   1. The PL011 UART (`serial_try_getc`) — always available.
 *   2. The virtio-input keyboard, if one is attached.
 *
 * `console_try_getc` checks the keyboard ring first, then the
 * serial RX FIFO.  Callers (the cooked / raw read paths in
 * vfs.c) treat its return identically to `serial_try_getc`.
 *
 * Keeping this in its own TU lets the rest of the kernel stay
 * agnostic to whether keyboard input arrives over evdev or over
 * a PL011 — including future virtio-console replacements.
 */

#ifndef KERNEL_CORE_CONSOLE_IN_H
#define KERNEL_CORE_CONSOLE_IN_H

/* Non-blocking single-byte read.  Returns 1 if a byte was written
 * to *out, 0 otherwise.  Drains a virtio-input keyboard event
 * (if present) on every call so events do not pile up.  */
int console_try_getc(char *out);

#endif /* KERNEL_CORE_CONSOLE_IN_H */
