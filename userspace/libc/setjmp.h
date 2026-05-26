/* userspace/libc/setjmp.h — non-local jumps, chapter 128a.
 *
 * Tiny C99 setjmp/longjmp for our user libc.  Two functions,
 * one fixed-size opaque-ish buffer.  No signal-mask plumbing
 * (no sigsetjmp/siglongjmp — those land with chapter 128b's
 * signal wrappers).
 *
 * Buffer layout (22 × uint64_t = 176 bytes, 16-byte aligned):
 *
 *     index  contents
 *      0..1  x19, x20      ── AArch64 callee-saved integer regs
 *      2..3  x21, x22         (per AAPCS64 §6.1.1).  The compiler
 *      4..5  x23, x24         is required to preserve these
 *      6..7  x25, x26         across a function call, so saving
 *      8..9  x27, x28         + restoring them is sufficient to
 *      10    x29 (FP)         "unwind" the callee state without
 *      11    x30 (LR)         touching the heap.
 *      12    sp
 *      13..20 d8..d15      ── FP callee-saved (LOWER 64 bits of
 *                             v8..v15).  Reserved here so the
 *                             buffer layout doesn't change when
 *                             chapter 129 turns FP on at EL0;
 *                             until then the asm leaves these
 *                             slots untouched and longjmp does
 *                             not restore them.
 *      21    padding       ── keeps total size a multiple of 16.
 *
 * jmp_buf is an *array* type (per C89/C99) so callers naturally
 * pass it by reference without writing &.  Matches every other
 * libc on the planet.
 *
 * Semantics:
 *   - setjmp(env) returns 0 on the initial call.
 *   - longjmp(env, val) returns control to the matching setjmp,
 *     which now appears to return `val`.
 *   - If `val == 0` the apparent return is 1 (C99 7.13.2.1#3).
 *   - It is undefined behaviour to longjmp into a function that
 *     has already returned, or to longjmp from a different
 *     thread than the one that called setjmp.  We don't try to
 *     trap either; document and move on.
 *
 * Why aarch64 callee-saved only?  Because that's the only state
 * the compiler is *forbidden* from clobbering across calls.  We
 * intentionally do NOT save x0..x18 (caller-saved): they're free
 * to change across any function call, including setjmp itself,
 * so the C abstract-machine view doesn't depend on them.
 *
 * Where this gets used: chapter 132's GCC port relies on longjmp
 * for the parser's diagnostic-recovery path.  Several upstream C
 * libraries (BearSSL's t_*.c selftests, the Lua interpreter's
 * error handler, parts of GNU make) also depend on it.  We add
 * it now so those ports don't have to wait.
 */

#ifndef USERSPACE_LIBC_SETJMP_H
#define USERSPACE_LIBC_SETJMP_H

typedef unsigned long __jmp_slot_t;

#define _SETJMP_NSLOTS 22

typedef __jmp_slot_t jmp_buf[_SETJMP_NSLOTS];

#ifdef __cplusplus
extern "C" {
#endif

extern int  setjmp(jmp_buf env);
extern void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIBC_SETJMP_H */
