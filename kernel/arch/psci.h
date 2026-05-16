/* psci.h — chapter 86 PSCI (Power State Coordination Interface).
 *
 * The kernel's only client of PSCI for chapter 86 is CPU_ON,
 * which wakes a parked secondary core and points it at the
 * supplied entry address.
 *
 * Conduit: HVC vs SMC.  The DTB's /psci node carries a `method`
 * property set to either "hvc" or "smc".  On QEMU virt under HVF
 * we run at EL1 with EL2 emulating the firmware, so the conduit
 * is HVC.  On bare-metal aarch64 with EL3 firmware (TF-A) it
 * would be SMC.  We detect at boot rather than hardcoding.
 *
 * Function IDs (PSCI 1.1, Table 5.1):
 *   CPU_OFF             0x84000002
 *   CPU_ON     (64-bit) 0xC4000003
 *   CPU_OFF / SUSPEND / etc. — not used yet.
 *
 * Calling convention (PSCI ABI, sec. 5.2):
 *   x0 = function ID
 *   x1 = target_cpu (MPIDR)        [for CPU_ON]
 *   x2 = entry_point_address       [for CPU_ON]
 *   x3 = context_id                [for CPU_ON; passed back in x0]
 *   ret in x0: 0 = SUCCESS, negative = error
 */
#ifndef KERNEL_ARCH_PSCI_H
#define KERNEL_ARCH_PSCI_H

#include <stdint.h>

/* PSCI return codes (subset).  Negative on error per spec. */
#define PSCI_SUCCESS              0
#define PSCI_NOT_SUPPORTED       -1
#define PSCI_INVALID_PARAMETERS  -2
#define PSCI_DENIED              -3
#define PSCI_ALREADY_ON          -4
#define PSCI_ON_PENDING          -5
#define PSCI_INTERNAL_FAILURE    -6
#define PSCI_NOT_PRESENT         -7
#define PSCI_DISABLED            -8
#define PSCI_INVALID_ADDRESS     -9

enum psci_conduit {
    PSCI_CONDUIT_UNKNOWN = 0,
    PSCI_CONDUIT_HVC,
    PSCI_CONDUIT_SMC,
};

/* Initialise from the DTB.  Looks for the /psci node; if absent,
 * defaults to HVC (the QEMU virt convention) and logs a warning.
 * Idempotent. */
void psci_init(const void *dtb);

/* Returns the detected conduit (HVC/SMC), or UNKNOWN if
 * psci_init() has not been called. */
enum psci_conduit psci_conduit_get(void);

/* Returns 0 on PSCI_SUCCESS, negative PSCI error otherwise.
 * `entry_point` is a physical address (the secondary boots with
 * MMU off; PSCI does not interpret it). */
int psci_cpu_on(uint64_t target_mpidr,
                uint64_t entry_point,
                uint64_t context_id);

#endif /* KERNEL_ARCH_PSCI_H */
