/* exception.c — C-side handler for every AArch64 exception.
 *
 * Reached from kernel/arch/vectors.S. Reads the architectural
 * exception registers (ESR_EL1, FAR_EL1) and the saved frame, prints
 * a structured crash dump over the UART, and halts the CPU.
 *
 * This is a deliberately verbose dump because milestone 1 is the
 * first time the kernel can produce one — before vectors were
 * installed, every fault either hard-locked the CPU or asserted
 * inside HVF. Future milestones will refine the synchronous handler
 * to forward page faults to the VM layer, IRQs to the GIC dispatch,
 * and SVCs to the syscall trampoline; the panic path will only fire
 * for genuinely unrecoverable cases. */

#include <stdint.h>
#include "exception.h"
#include "serial.h"

/* Vector-ID names mirror kernel/arch/vectors.S slot order.  The
 * index here matches the integer pushed into x0 by each vector
 * slot's `mov x0, #N` instruction. */
static const char *const k_vector_names[16] = {
    "Sync   from current EL, SP_EL0",
    "IRQ    from current EL, SP_EL0",
    "FIQ    from current EL, SP_EL0",
    "SError from current EL, SP_EL0",
    "Sync   from current EL, SP_ELx",
    "IRQ    from current EL, SP_ELx",
    "FIQ    from current EL, SP_ELx",
    "SError from current EL, SP_ELx",
    "Sync   from lower EL, AArch64",
    "IRQ    from lower EL, AArch64",
    "FIQ    from lower EL, AArch64",
    "SError from lower EL, AArch64",
    "Sync   from lower EL, AArch32",
    "IRQ    from lower EL, AArch32",
    "FIQ    from lower EL, AArch32",
    "SError from lower EL, AArch32",
};

/* Decode the EC (Exception Class) field of ESR_EL1. Lifted from
 * ARM ARM D17.2.37; covers the values we are likely to see during
 * the early milestones. Anything not listed prints as "unknown EC". */
static const char *esr_ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x07: return "Trapped FP/SIMD use (CPACR_EL1.FPEN=0)";
    case 0x0E: return "Illegal execution state";
    case 0x15: return "SVC instruction execution from AArch64";
    case 0x18: return "Trapped MSR/MRS or System instruction";
    case 0x20: return "Instruction Abort from a lower EL";
    case 0x21: return "Instruction Abort from same EL (translation fault?)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort from a lower EL";
    case 0x25: return "Data Abort from same EL (page fault?)";
    case 0x26: return "SP alignment fault";
    case 0x2F: return "SError interrupt";
    case 0x30: return "Breakpoint from a lower EL";
    case 0x31: return "Breakpoint from same EL";
    case 0x32: return "Software step from a lower EL";
    case 0x33: return "Software step from same EL";
    case 0x34: return "Watchpoint from a lower EL";
    case 0x35: return "Watchpoint from same EL";
    case 0x3C: return "BRK instruction (BRK #imm)";
    default:   return "(unknown EC)";
    }
}

/* Read ESR_EL1 / FAR_EL1 inline. These two registers carry the
 * full description of why a synchronous exception happened. */
static inline uint64_t read_esr_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(v));
    return v;
}

static inline uint64_t read_far_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

void kernel_panic_from_vector(uint64_t vector_id,
                              struct exception_frame *frame)
{
    const uint64_t esr = read_esr_el1();
    const uint64_t far = read_far_el1();
    const uint32_t ec  = (uint32_t)((esr >> 26) & 0x3Fu);
    const uint32_t iss = (uint32_t)(esr & 0x01FFFFFFu);

    serial_puts("\n");
    serial_puts("############### KERNEL PANIC ###############\n");
    serial_puts("vector: ");
    if (vector_id < 16) serial_puts(k_vector_names[vector_id]);
    else                serial_puts("(out of range)");
    serial_puts("\n");

    serial_puts("ESR_EL1  = ");  serial_puthex(esr);  serial_puts("\n");
    serial_puts("  EC     = ");  serial_puthex((uint64_t)ec);
    serial_puts("  ");           serial_puts(esr_ec_name(ec));   serial_puts("\n");
    serial_puts("  ISS    = ");  serial_puthex((uint64_t)iss);   serial_puts("\n");
    serial_puts("FAR_EL1  = ");  serial_puthex(far);             serial_puts("\n");
    serial_puts("ELR_EL1  = ");  serial_puthex(frame->elr);      serial_puts("\n");
    serial_puts("SPSR_EL1 = ");  serial_puthex(frame->spsr);     serial_puts("\n");

    /* General-purpose register dump. Keeping it compact: two
     * registers per line, no symbolic names — addresses and values
     * are what a debugger needs. */
    for (int i = 0; i < 31; i += 2) {
        serial_puts("x");
        if (i < 10) serial_putc((char)('0' + i));
        else { serial_putc((char)('0' + i / 10)); serial_putc((char)('0' + i % 10)); }
        serial_puts(" = "); serial_puthex(frame->x[i]);
        if (i + 1 < 31) {
            serial_puts("   x");
            int j = i + 1;
            if (j < 10) serial_putc((char)('0' + j));
            else { serial_putc((char)('0' + j / 10)); serial_putc((char)('0' + j % 10)); }
            serial_puts(" = "); serial_puthex(frame->x[j]);
        }
        serial_puts("\n");
    }

    serial_puts("############################################\n");
    serial_puts("kernel halted — Ctrl-A X to quit QEMU\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
