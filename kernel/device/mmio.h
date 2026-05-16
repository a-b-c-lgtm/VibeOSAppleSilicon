/*
 * kernel/device/mmio.h — type-safe AArch64 MMIO accessors.
 *
 * Every MMIO load and store in the kernel goes through these helpers
 * because the obvious form
 *
 *     #define REG (*(volatile uint32_t *)0x09000018)
 *     uint32_t v = REG;
 *
 * gives the compiler latitude to emit writeback addressing modes
 * (`str w1, [x2], #N`) when two related MMIO addresses end up in
 * one base register. Writeback addressing against Device memory is
 * CONSTRAINED-UNPREDICTABLE per ARMv8 ARM B2.7.2; on Apple Silicon
 * under HVF the resulting fault has ESR.ISV=0 and aborts the host
 * process. See chapter 3 of the book for the full derivation.
 *
 * The `[%1]` constraint with `"r"(addr)` forces a plain register
 * addressing mode and re-materialises the address each call, so the
 * optimizer is structurally unable to fold MMIO accesses into
 * writeback forms.
 */

#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static inline void mmio_write32(uintptr_t addr, uint32_t v)
{
    __asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(addr) : "memory");
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
    uint8_t v;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static inline void mmio_write8(uintptr_t addr, uint8_t v)
{
    __asm__ volatile("strb %w0, [%1]" :: "r"(v), "r"(addr) : "memory");
}

static inline uint64_t mmio_read64(uintptr_t addr)
{
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static inline void mmio_write64(uintptr_t addr, uint64_t v)
{
    __asm__ volatile("str %0, [%1]" :: "r"(v), "r"(addr) : "memory");
}

#endif /* MMIO_H */
