#ifndef EXCEPTION_H
#define EXCEPTION_H

#include <stdint.h>

/* Saved register frame produced by save_context in
 * kernel/arch/vectors.S. Layout MUST stay in sync with that macro. */
struct exception_frame {
    uint64_t x[31];        /* x0..x30 */
    uint64_t pad;          /* alignment to 16 B */
    uint64_t elr;          /* ELR_EL1   — return address at fault */
    uint64_t spsr;         /* SPSR_EL1  — saved PSTATE             */
};

/* Called from kernel/arch/vectors.S panic_entry. Does not return. */
void kernel_panic_from_vector(uint64_t vector_id,
                              struct exception_frame *frame);

#endif /* EXCEPTION_H */
