/* userspace/setjmptest/setjmptest.c — chapter 128a regression.
 *
 * Drives setjmp/longjmp through every case the spec promises:
 *
 *   1. setjmp returns 0 the first time.
 *   2. longjmp(env, N) for N != 0 makes setjmp return N.
 *   3. longjmp(env, 0)  makes setjmp return 1 (C99 7.13.2.1#3).
 *   4. Callee-saved state (variables the compiler chose to
 *      keep in x19..x28 across the longjmp) survives the jump.
 *
 * Prints "all checks passed" on success.  test_setjmp.py greps
 * for that line.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/setjmp.h"

static jmp_buf env;
static jmp_buf env2;
static int     g_inner_calls;

/* Force at least one local variable into a callee-saved slot:
 * `marker` is live across the call to inner() in main, which the
 * compiler will most likely satisfy by parking it in x19..x28
 * (the only registers the AAPCS lets it preserve across calls).
 * If setjmp/longjmp restores those correctly, marker == original
 * after the longjmp.  If it didn't, we'd see garbage. */
static __attribute__((noinline)) void inner(int target)
{
    g_inner_calls++;
    printf("  inner(target=%d) about to longjmp\n", target);
    longjmp(env, target);
    /* longjmp is __attribute__((noreturn)) so the compiler will
     * not emit any code after this point. */
}

static __attribute__((noinline)) void inner2(void)
{
    printf("  inner2() about to longjmp(env2, 0)\n");
    longjmp(env2, 0);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[setjmptest] starting\n");

    /* Marker lives across all the calls below.  See note above. */
    unsigned long marker = 0xC0FFEE00DEADBEEFUL;

    int r = setjmp(env);
    printf("  setjmp(env) returned %d (marker=0x%lx)\n", r, marker);
    if (marker != 0xC0FFEE00DEADBEEFUL) {
        printf("  FAIL: callee-saved marker clobbered by longjmp\n");
        return 1;
    }
    if (r == 0) {
        inner(7);   /* should reappear here with r == 7 */
    } else if (r == 7) {
        inner(42);  /* should reappear here with r == 42 */
    } else if (r == 42) {
        /* good -- final pass of the env chain */
    } else {
        printf("  FAIL: unexpected setjmp return value %d\n", r);
        return 1;
    }

    if (g_inner_calls != 2) {
        printf("  FAIL: inner called %d times (want 2)\n", g_inner_calls);
        return 1;
    }

    /* Test 3: longjmp(env, 0) must make setjmp return 1. */
    int r2 = setjmp(env2);
    printf("  setjmp(env2) returned %d\n", r2);
    if (r2 == 0) {
        inner2();   /* will longjmp with val=0 */
    } else if (r2 == 1) {
        /* good -- the 0 became 1 as the spec requires */
    } else {
        printf("  FAIL: setjmp(env2) returned %d, want 1\n", r2);
        return 1;
    }

    printf("[setjmptest] all checks passed\n");
    return 0;
}
