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
    /* ── Chapter 131f — local-exec TLS bootstrap ────────────
     * Real binutils (gas, ld) uses __thread variables (e.g.
     * bfd's _bfd_error_data) and the AArch64 local-exec TLS
     * model emits `mrs xN, tpidr_el0` + `add xN, xN, #imm`
     * accesses with link-time-fixed offsets, the first
     * variable sitting at TP + 16 (variant-I TCB size).
     * The kernel does not set TPIDR_EL0 on a fresh exec, so
     * a zero TP would make those accesses fault near NULL.
     * Point TPIDR_EL0 at our reserved zero-initialised bss
     * area so the first 16 bytes act as the (unused-for-
     * static binaries) TCB header and the rest serves the
     * .tbss image.  64 KiB is comfortably larger than any
     * binutils binary's TLS image (gas/as-new = 24 B today,
     * ld-new = 24 B).  Process-private — each exec'd image
     * sets its own __tls_area on entry.  Per-thread save/
     * restore across context switches is not yet implemented
     * in the kernel; only one TLS-using process can run at a
     * time without TPIDR_EL0 bleed-through (acceptable for
     * the chapter-131f /bin/as + /bin/ld smoke tests). */
    adrp    x9, __tls_area_start
    add     x9, x9, :lo12:__tls_area_start
    msr     tpidr_el0, x9

    ldr     x0, [sp]            /* x0 = argc                          */
    add     x1, sp, #8          /* x1 = &argv[0]                      */

    /* ── Chapter 120 — global constructors ──────────────────
     * Walk pointers in [__init_array_start, __init_array_end)
     * and call each one before main.  The linker script
     * defines both bounds via PROVIDE_HIDDEN; if no
     * constructors are linked the section is empty and
     * start == end, so the loop is a no-op.
     *
     * argc/argv get parked in callee-saved regs across the
     * walk; constructors take no args but may clobber x0..x18. */
    mov     x19, x0             /* save argc */
    mov     x20, x1             /* save argv */
    adrp    x21, __init_array_start
    add     x21, x21, :lo12:__init_array_start
    adrp    x22, __init_array_end
    add     x22, x22, :lo12:__init_array_end
0:  cmp     x21, x22
    b.eq    1f
    ldr     x0, [x21], #8
    blr     x0
    b       0b
1:
    /* Restore argc/argv into the AAPCS arg regs. */
    mov     x0, x19
    mov     x1, x20

    bl      main
    mov     x19, x0             /* save main's return code */

    /* ── Chapter 120 — atexit chain + global destructors ────
     * __cxa_finalize(NULL) walks the atexit list (LIFO) and
     * the .fini_array section.  Defined as a `.weak` no-op
     * below so binaries that never registered any handler
     * link cleanly; userspace/libc/atexit.h provides the
     * strong override. */
    mov     x0, #0
    bl      __cxa_finalize

    /* main's rc → SYS_EXIT. */
    mov     x0, x19
    mov     x8, #2          /* SYS_EXIT */
    svc     #0
1:  wfe
    b       1b

/* Weak default: no atexit chain registered means nothing to
 * finalize.  userspace/libc/atexit.h provides a strong
 * version that walks its LIFO slot table. */
.section .text.__cxa_finalize_default, "ax"
.weak __cxa_finalize
.type __cxa_finalize, %function
__cxa_finalize:
    ret

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

/* ── Chapter 116a — process-global errno slot ──────────────
 *
 * Defined here so every userspace binary picks it up by virtue
 * of linking crt0.o, without having to thread an extra ERRNO_OBJ
 * through ~60 *_OBJS lists in the Makefile.  The slot lives in
 * .bss; the kernel hands out zeroed pages so the implicit
 * initial value of 0 is correct.
 *
 * Read/written from userspace/libc/errno.h (the `errno` macro)
 * and userspace/libc/syscall.h (the `__svc_check` helper).
 */
.section .bss.__errno_value, "aw", %nobits
.global __errno_value
.balign 4
.type __errno_value, %object
.size __errno_value, 4
__errno_value:
    .skip 4

/* ── Chapter 131f — TLS storage for local-exec __thread vars ─
 *
 * 64 KiB zero-initialised area, 16-byte aligned.  TPIDR_EL0 is
 * set to __tls_area_start at _user_start entry (above).  The
 * first 16 bytes are the AArch64 variant-I TCB header (unused
 * for static link); from offset 16 onwards the linker's
 * R_AARCH64_TLSLE_*_TPREL relocations write the .tdata + .tbss
 * image of whatever binary this crt0 was linked into.  We don't
 * walk a PT_TLS segment because the host linker emits none for
 * static AArch64 binaries — it inlines tprel offsets directly.
 * Sized at 64 KiB so future chapter-132 gcc / libstdc++ TLS
 * needs fit without revisiting crt0; non-TLS binaries pay zero
 * disk cost (NOBITS) and 64 KiB of zeroed page-cache RAM. */
.section .bss.__tls_area, "aw", %nobits
.balign 16
.global __tls_area_start
.type __tls_area_start, %object
.size __tls_area_start, 65536
__tls_area_start:
    .skip 65536
