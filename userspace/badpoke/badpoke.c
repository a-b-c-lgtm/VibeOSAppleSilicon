/* userspace/badpoke/badpoke.c — milestone 16 isolation smoke test.
 *
 * Writes to a kernel address from EL0.  Used to silently succeed
 * (and clobber the kernel heap) back when DRAM identity slots
 * were mapped EL0-RW.  As of milestone 16 the kernel has revoked
 * EL0 access to those slots, so this program SHOULD take a data
 * abort and the kernel should kill it cleanly.
 *
 * Run from the shell with `/bin/badpoke`.  Expected output is a
 * panic-style diagnostic from the EL1 fault handler followed by
 * the parent reaping the dead child.  The exit code reported back
 * to sh should be non-zero (we plan to start tagging killed-by-
 * fault processes with a special code in a later milestone).
 */

#include "../libc/syscall.h"

int main(void)
{
    puts("[badpoke] about to write to a kernel address; should fault");

    /* The kernel heap base is around 0x230000000.  Pick a VA in
     * slot 8 (0x200000000-0x240000000) — definitely DRAM
     * identity-mapped on the kernel side, definitely NOT mapped
     * in our per-process AS's slot 8 (which is inherited from
     * boot L1, where slot 8 is now BLOCK_NORMAL = AP=00 = no EL0).
     *
     * Pre-milestone-16 this would silently corrupt the heap.
     * Now it should take a Data Abort from EL0.
     */
    volatile unsigned long *kaddr = (volatile unsigned long *)0x230000000UL;
    *kaddr = 0xDEADBEEFCAFEBABEUL;

    /* If we somehow get here, the isolation is still broken. */
    puts("[badpoke] FAIL: write succeeded - isolation broken");
    return 1;
}
