/* userspace/crt/crt0.S — CRT for user programs (argv-aware).
 *
 * On entry from the kernel:
 *   PC = ELF entry point (which the linker has set to _user_start).
 *   SP = top of the per-process argv blob.  The kernel's ELF
 *        loader has laid out the user stack as:
 *
 *           sp -> argc                  (8 bytes)
 *           sp+8  -> argv[0]            (8 bytes)
 *           ...
 *           sp+8(argc) -> NULL          (argv terminator)
 *           sp+8(argc+1) -> NULL        (envp terminator)
 *
 *        SP is 16-byte aligned at entry per AAPCS.
 *
 * Our job: load argc into x0 and the address of argv[0] into x1
 * (the standard `int main(int argc, char **argv)` ABI), call
 * main, then if main ever returns invoke SYS_EXIT with its
 * return value.  Programs that declare `int main(void)` simply
 * ignore x0/x1 — AAPCS allows that.
 *
 * We do not zero BSS — the kernel zeroes pages on hand-out via
 * pmem_alloc_page, which is the source of every byte in the
 * loaded image.
 */

.section .text._user_start, "ax"
.global _user_start
_user_start:
    ldr     x0, [sp]            /* x0 = argc                          */
    add     x1, sp, #8          /* x1 = &argv[0]                      */
    bl      main

    /* main returned — invoke SYS_EXIT(rc).  rc is in x0. */
    mov     x8, #2          /* SYS_EXIT */
    svc     #0
1:  wfe
    b       1b

/* ── Chapter 77 — signal-return trampoline ──────────────────
 *
 * deliver_signal sets the user-mode LR to this address before
 * eret'ing into the user's signal handler.  When the handler
 * issues its natural `ret` (which jumps to LR), it lands here.
 *
 * AAPCS contract: at function entry SP is 16-byte aligned and
 * equal to whatever the caller had set.  Our "caller" is the
 * kernel's deliver_signal, which set SP_EL0 to the address of
 * the sigframe it just wrote on the user stack.  The handler
 * therefore enters with `sp == sigframe`.  Standard prologue/
 * epilogue restores SP before `ret`, so when execution reaches
 * us here, `sp` once again points at the sigframe.
 *
 * Pass that pointer to SYS_SIGRETURN.  The kernel copies the
 * saved register state back into the trap frame and erets to
 * the original interrupted PC; we never return from the SVC. */
.section .text.__sigreturn_trampoline, "ax"
.global __sigreturn_trampoline
.balign 4
__sigreturn_trampoline:
    mov     x0, sp
    mov     x8, #32         /* SYS_SIGRETURN */
    svc     #0
    /* Unreachable — but spin defensively in case a future kernel
     * change ever returns from SYS_SIGRETURN. */
1:  wfe
    b       1b
