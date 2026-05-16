/* psci.c — chapter 86 PSCI conduit + CPU_ON wrapper.
 *
 * Lives in arch/ because the call site is hand-written hvc/smc
 * inline asm.  All the higher-level "wake the secondary core"
 * orchestration is in cpu.c; this file is just the bare ABI.
 */

#include "psci.h"
#include "../core/fdt.h"
#include "../core/serial.h"
#include <stdint.h>

#define PSCI_FN_CPU_ON_64   0xC4000003u

static enum psci_conduit g_conduit = PSCI_CONDUIT_UNKNOWN;

void psci_init(const void *dtb)
{
    char method[8];
    if (fdt_read_psci_method(dtb, method, sizeof(method))) {
        if (method[0] == 's' && method[1] == 'm' && method[2] == 'c') {
            g_conduit = PSCI_CONDUIT_SMC;
        } else if (method[0] == 'h' && method[1] == 'v' && method[2] == 'c') {
            g_conduit = PSCI_CONDUIT_HVC;
        } else {
            serial_puts("[psci] unknown method, defaulting to HVC\n");
            g_conduit = PSCI_CONDUIT_HVC;
        }
    } else {
        /* No /psci node.  QEMU virt always provides one; if it's
         * gone we're booted on something exotic.  Default to HVC
         * because that's the common case and log loudly. */
        serial_puts("[psci] no /psci node in DTB, defaulting to HVC\n");
        g_conduit = PSCI_CONDUIT_HVC;
    }

    serial_puts("[psci] conduit = ");
    serial_puts(g_conduit == PSCI_CONDUIT_HVC ? "HVC\n" : "SMC\n");
}

enum psci_conduit psci_conduit_get(void)
{
    return g_conduit;
}

/* Hand-written hvc/smc.  We can't decide between the two
 * instructions at compile time and we don't want a branch in the
 * hot path, so we have two parallel inline-asm blocks selected
 * once via the conduit enum.
 *
 * Why we list x0..x17 in the clobber list: the AArch64 SMC/HVC
 * conduit (per SMC Calling Convention, sec. 2.6) reserves x0..x17
 * as caller-saved across the call.  GCC's calling convention
 * already treats x0..x18 as caller-saved for normal C calls, so
 * the only ones we need to mark explicitly here are the ones we
 * use as inputs/outputs.  We list x4..x17 defensively in case a
 * future PSCI revision starts returning data in them. */
int psci_cpu_on(uint64_t target_mpidr,
                uint64_t entry_point,
                uint64_t context_id)
{
    register uint64_t x0 __asm__("x0") = PSCI_FN_CPU_ON_64;
    register uint64_t x1 __asm__("x1") = target_mpidr;
    register uint64_t x2 __asm__("x2") = entry_point;
    register uint64_t x3 __asm__("x3") = context_id;

    if (g_conduit == PSCI_CONDUIT_SMC) {
        __asm__ volatile(
            "smc #0"
            : "+r"(x0)
            : "r"(x1), "r"(x2), "r"(x3)
            : "x4", "x5", "x6", "x7", "x8", "x9", "x10",
              "x11", "x12", "x13", "x14", "x15", "x16", "x17",
              "memory");
    } else {
        __asm__ volatile(
            "hvc #0"
            : "+r"(x0)
            : "r"(x1), "r"(x2), "r"(x3)
            : "x4", "x5", "x6", "x7", "x8", "x9", "x10",
              "x11", "x12", "x13", "x14", "x15", "x16", "x17",
              "memory");
    }
    return (int)(int64_t)x0;
}
