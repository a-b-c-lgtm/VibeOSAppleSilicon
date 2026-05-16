/*
 * kernel/core/serial.c — minimal PL011 UART driver for QEMU 'virt'.
 *
 * Why we use inline asm for every MMIO access
 * -------------------------------------------
 * The PL011 registers live in Device-nGnRnE memory.  Per the ARMv8 ARM
 * (B2.7.2), load/store instructions that use writeback addressing modes
 * (post- or pre-indexed) against Device memory are CONSTRAINED-UNPREDICTABLE.
 * On Apple Silicon under HVF the CPU faults with ESR.ISV=0 and HVF
 * promptly aborts the host process with:
 *
 *   Assertion failed: (isv), function hvf_handle_exception, file hvf.c
 *
 * GCC, when handed two `volatile uint32_t *` MMIO accesses whose addresses
 * differ by a small constant (here 24 bytes between DR @+0x000 and FR @+0x018),
 * will happily emit `str w1, [x2], #24` to fold the pointer arithmetic into
 * a writeback store.  The `volatile` qualifier preserves access ordering and
 * prevents elision but does NOT forbid that addressing mode.
 *
 * The mmio_read32/mmio_write32 inline-asm helpers force a plain `[Xn]`
 * addressing mode for every access, so the compiler is structurally
 * incapable of generating writeback forms against MMIO.
 */

#include "serial.h"
#include "mmio.h"
#include "../arch/spinlock.h"
#include <stdint.h>

#define PL011_BASE  0x09000000UL
#define PL011_DR    (PL011_BASE + 0x000)
#define PL011_FR    (PL011_BASE + 0x018)
#define FR_TXFF     (1u << 5)
#define FR_RXFE     (1u << 4)

/* Chapter 86 — once a secondary CPU is awake it can also call
 * serial_puts/puthex/putc.  Without serialisation, two cores'
 * boot lines would interleave per-byte and the log would be
 * unreadable.  We use a recursive lock (reclock_t) so callers in
 * main.c that chain `puts` -> `puthex` -> `puts` to build a
 * single line keep that line atomic without each call site
 * having to track lock state.
 *
 * The lock is statically initialised; no serial_init() touch
 * required, which means even our very first boot print is safe
 * if the secondary races.  It does not — secondaries do nothing
 * until smp_init() PSCI's them — but the policy stays robust.
 *
 * IRQ policy: lock is taken with IRQs masked so an IRQ handler
 * that wants to print does not deadlock.  Today no IRQ handler
 * prints from inside a critical section, but the policy hardens
 * us against future changes (and matches Linux's printk_safe
 * approach).
 */
static reclock_t g_serial_lock = RECLOCK_INIT;

void serial_init(void)
{
    /* QEMU's virt machine pre-configures PL011 for 38400-8N1 with
     * FIFOs enabled.  Nothing to do at boot. */
}

/* Internal — assumes g_serial_lock is held by the caller. */
static void putc_locked(char c)
{
    while (mmio_read32(PL011_FR) & FR_TXFF) {
        /* spin until TX FIFO has room */
    }
    mmio_write32(PL011_DR, (uint32_t)(uint8_t)c);
}

void serial_putc(char c)
{
    uint64_t f = reclock_lock_irqsave(&g_serial_lock);
    putc_locked(c);
    reclock_unlock_irqrestore(&g_serial_lock, f);
}

void serial_puts(const char *s)
{
    uint64_t f = reclock_lock_irqsave(&g_serial_lock);
    while (*s) {
        if (*s == '\n')
            putc_locked('\r');
        putc_locked(*s++);
    }
    reclock_unlock_irqrestore(&g_serial_lock, f);
}

void serial_puthex(uint64_t val)
{
    uint64_t f = reclock_lock_irqsave(&g_serial_lock);
    putc_locked('0');
    putc_locked('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nib = (uint8_t)((val >> shift) & 0xFu);
        putc_locked(nib < 10 ? (char)('0' + nib)
                             : (char)('a' + (nib - 10)));
    }
    reclock_unlock_irqrestore(&g_serial_lock, f);
}

int serial_try_getc(char *out)
{
    /* RX path is not contended (only CPU 0 services keyboard
     * polling today) but we still take the lock for symmetry and
     * to keep ordering intuitive should that change. */
    uint64_t f = reclock_lock_irqsave(&g_serial_lock);
    int rc = 0;
    if (!(mmio_read32(PL011_FR) & FR_RXFE)) {
        if (out)
            *out = (char)(mmio_read32(PL011_DR) & 0xFFu);
        rc = 1;
    }
    reclock_unlock_irqrestore(&g_serial_lock, f);
    return rc;
}
