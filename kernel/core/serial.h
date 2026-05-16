#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* PL011 UART driver (ARM PrimeCell, MMIO-mapped).
 *
 * On `qemu-system-aarch64 -machine virt` the first PL011 instance
 * lives at physical 0x09000000.  QEMU pre-configures the device for
 * 115200 8N1 with both FIFOs enabled, so milestone-0 code can call
 * serial_putc() with no prior init beyond a no-op serial_init().
 *
 * The API mirrors the x86 build's serial.h verbatim so that
 * arch-neutral kernel/core code can call serial_puts() / puthex()
 * without any aarch64 awareness.  The UART base address moves
 * (and the access pattern moves from `outb` to MMIO loads/stores),
 * but the contract for callers is unchanged. */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puthex(uint64_t val);
int  serial_try_getc(char *out);

#endif /* SERIAL_H */
