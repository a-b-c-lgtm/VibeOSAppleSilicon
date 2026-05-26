/* userspace/libc/setjmp.S — AArch64 setjmp/longjmp, chapter 128a.
 *
 * Implements the two-function contract documented in setjmp.h.
 * AAPCS64 (Procedure Call Standard for the Arm 64-bit
 * Architecture, §6.1.1) defines the callee-saved set as:
 *
 *   integer:   x19..x28, x29 (FP), x30 (LR), SP
 *   FP/SIMD:   d8..d15 (lower 64 bits of v8..v15)
 *
 * Anything else is caller-saved and need not be preserved.
 * Saving the callee-saved set + SP is therefore enough to
 * restore the abstract-machine state of the function that
 * called setjmp.
 *
 * Layout matches setjmp.h's diagram (offsets in bytes):
 *
 *     x0+  0  x19/x20
 *     x0+ 16  x21/x22
 *     x0+ 32  x23/x24
 *     x0+ 48  x25/x26
 *     x0+ 64  x27/x28
 *     x0+ 80  x29/x30
 *     x0+ 96  sp
 *     x0+104  d8/d9     ── chapter 129: FP/SIMD callee-saved set
 *     x0+120  d10/d11
 *     x0+136  d12/d13
 *     x0+152  d14/d15
 *     x0+168  pad
 *
 * Why stp pairs?  AArch64's STP/LDP load and store two 64-bit
 * regs in a single instruction with a 7-bit signed offset
 * (#-512..504, scaled by 8).  Faster, smaller, and the natural
 * way to encode the AAPCS register-pair convention.  Total cost
 * here: 6 stp + 1 str + 4 stp(d) + 2 movs ≈ 13 instructions for setjmp.
 *
 * x0 is the caller's jmp_buf pointer (first arg per AAPCS).
 * setjmp returns 0 in x0; longjmp returns `val` (or 1 if `val`
 * was 0) in x0.  Neither function touches any of the saved
 * registers in its own prologue/epilogue — we ARE the
 * prologue/epilogue, and the whole point is to not perturb the
 * register state we're saving.
 *
 * The trailing `ret` in setjmp uses x30 directly; the trailing
 * `ret` in longjmp uses the freshly-restored x30, which is the
 * caller of the original setjmp.  That's the long jump.
 */

    .section .text.setjmp, "ax"
    .global setjmp
    .global _setjmp
    .type   setjmp,  @function
    .type   _setjmp, @function
setjmp:
_setjmp:
    stp     x19, x20, [x0,  #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    stp     d8,  d9,  [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     x0, #0
    ret

    .section .text.longjmp, "ax"
    .global longjmp
    .global _longjmp
    .type   longjmp,  @function
    .type   _longjmp, @function
longjmp:
_longjmp:
    ldp     x19, x20, [x0,  #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2,  [x0, #96]
    mov     sp,  x2
    ldp     d8,  d9,  [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]

    /* C99 7.13.2.1#3: longjmp(env, 0) must make setjmp return 1.
     * Cleanly encoded with CSINC: if val != 0 -> x0 = val + 0;
     * if val == 0 -> x0 = xzr + 1 = 1.  Single instruction. */
    cmp     x1, #0
    csinc   x0, x1, xzr, ne
    ret

    /* No executable stack -- mark the object accordingly so the
     * linker doesn't add a PT_GNU_STACK with X=1. */
    .section .note.GNU-stack, "", @progbits
