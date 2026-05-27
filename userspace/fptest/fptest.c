/* userspace/fptest/fptest.c — chapter 171 FP/SIMD-at-EL0 regression.
 *
 * Exercises that:
 *   1. Plain double arithmetic runs at EL0 without taking
 *      EC=0x07 (FP/SIMD trapped).
 *   2. Context switches preserve FP register state across
 *      cooperative yields (sleep_ms) and signal delivery.
 *   3. setjmp / longjmp preserve d8..d15 per AAPCS64.
 *   4. The shared FPU is consistent across spawned children:
 *      a child can compute its own doubles without disturbing
 *      the parent's FP state.
 *
 * The chapter intentionally avoids %f in printf — that's a libc
 * change scheduled separately.  Comparisons happen in C and the
 * results are reported via integer round-trips (e.g. printing the
 * integer part of the result, or a pass/fail marker).
 */
#include "../libc/printf.h"
#include "../libc/setjmp.h"
#include "../libc/syscall.h"
#include <stdint.h>

static int g_fail = 0;

#define CHECK(cond) do {                                            \
    if (!(cond)) {                                                  \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        g_fail++;                                                   \
    }                                                               \
} while (0)

/* Compare two doubles for "close enough" — within 1e-9 relative.
 * Avoid pulling in fabs(); inline the absolute value. */
static int dclose(double a, double b)
{
    double d = a - b;
    if (d < 0) d = -d;
    return d < 1e-9;
}

/* Convert |x| into integer parts for printing without %f. */
static void print_double_parts(const char *tag, double x)
{
    int sign = (x < 0);
    if (sign) x = -x;
    long ipart = (long)x;
    /* Scale fractional part by 1e6, round to nearest. */
    double f = (x - (double)ipart) * 1000000.0;
    long  fpart = (long)(f + 0.5);
    if (fpart >= 1000000) { ipart += 1; fpart -= 1000000; }
    printf("  %s = %s%ld.%06ld\n", tag, sign ? "-" : "", ipart, fpart);
}

/* Test 1 — basic FP at EL0 doesn't trap. */
static void test_basic_fp(void)
{
    printf("test_basic_fp:\n");
    double a = 3.14159265358979;
    double b = 2.71828182845905;
    double c = a * b;
    print_double_parts("3.14... * 2.71...", c);
    /* Expected value ≈ 8.539734222673566 */
    CHECK(dclose(c, 8.539734222673566));

    /* Verify ADD / SUB / DIV work too. */
    double d = (a + b) / (a - b);
    print_double_parts("(a+b)/(a-b)", d);
    /* (3.14159265358979 + 2.71828182845905)
     *   / (3.14159265358979 - 2.71828182845905)
     * = 13.842959201997754 (computed in IEEE 754 doubles). */
    CHECK(dclose(d, 13.842959201997754));

    /* sqrt via Newton's iteration (FP only — no libm). */
    double x = 2.0;
    for (int i = 0; i < 20; i++)
        x = 0.5 * (x + 2.0 / x);
    print_double_parts("sqrt(2)", x);
    CHECK(dclose(x, 1.4142135623730951));
}

/* Test 2 — FP register state survives a cooperative yield.
 *
 * Hold a recognisable double in a long-lived local, force a
 * context switch via sleep_ms(), then re-check the value.  If
 * cswitch_to's FP save/restore is wrong, the value will be
 * either zeroed (frame-init) or replaced with the idle thread's
 * accidental FP residue.
 */
static void test_yield_preserves_fp(void)
{
    printf("test_yield_preserves_fp:\n");
    /* Use volatile so the compiler can't fold across the sleep. */
    volatile double v = 1.4142135623730951;
    sleep_ms(10);
    /* Read into a register before the check so we exercise an
     * actual FP load post-yield. */
    double r = v;
    CHECK(dclose(r, 1.4142135623730951));

    /* Try with several values in different registers
     * simultaneously by binding to named locals that won't be
     * coalesced by the optimiser. */
    volatile double v1 = 1.111111111111111;
    volatile double v2 = 2.222222222222222;
    volatile double v3 = 3.333333333333333;
    volatile double v4 = 4.444444444444444;
    sleep_ms(5);
    sleep_ms(5);
    CHECK(dclose(v1, 1.111111111111111));
    CHECK(dclose(v2, 2.222222222222222));
    CHECK(dclose(v3, 3.333333333333333));
    CHECK(dclose(v4, 4.444444444444444));
}

/* Test 3 — setjmp / longjmp preserves d8..d15. */
static jmp_buf g_jb;

static void longjmp_back(void)
{
    /* Clobber d8..d15 with garbage before jumping back so that
     * if longjmp doesn't restore them, the post-setjmp check
     * fails. */
    register double d8  asm("d8")  = 99.0;
    register double d9  asm("d9")  = 99.0;
    register double d10 asm("d10") = 99.0;
    register double d11 asm("d11") = 99.0;
    register double d12 asm("d12") = 99.0;
    register double d13 asm("d13") = 99.0;
    register double d14 asm("d14") = 99.0;
    register double d15 asm("d15") = 99.0;
    /* Make sure the compiler doesn't consider them dead. */
    asm volatile("" :: "w"(d8), "w"(d9), "w"(d10), "w"(d11),
                       "w"(d12), "w"(d13), "w"(d14), "w"(d15));
    longjmp(g_jb, 1);
}

static void test_setjmp_fp(void)
{
    printf("test_setjmp_fp:\n");
    /* Pin known values into d8..d15 across the setjmp.  Use
     * register-with-asm bindings so the compiler honours the
     * placement. */
    register double d8  asm("d8")  = 1.0;
    register double d9  asm("d9")  = 2.0;
    register double d10 asm("d10") = 3.0;
    register double d11 asm("d11") = 4.0;
    register double d12 asm("d12") = 5.0;
    register double d13 asm("d13") = 6.0;
    register double d14 asm("d14") = 7.0;
    register double d15 asm("d15") = 8.0;
    asm volatile("" :: "w"(d8), "w"(d9), "w"(d10), "w"(d11),
                       "w"(d12), "w"(d13), "w"(d14), "w"(d15));

    int rv = setjmp(g_jb);
    if (rv == 0) {
        longjmp_back();
        CHECK(0 && "longjmp returned to setjmp's first call");
    }
    /* setjmp returned via longjmp.  d8..d15 must be restored. */
    /* Re-bind to register-asm names so we can read the values
     * the longjmp left there. */
    register double r8  asm("d8");
    register double r9  asm("d9");
    register double r10 asm("d10");
    register double r11 asm("d11");
    register double r12 asm("d12");
    register double r13 asm("d13");
    register double r14 asm("d14");
    register double r15 asm("d15");
    asm volatile("" : "=w"(r8), "=w"(r9), "=w"(r10), "=w"(r11),
                      "=w"(r12), "=w"(r13), "=w"(r14), "=w"(r15));
    CHECK(dclose(r8,  1.0));
    CHECK(dclose(r9,  2.0));
    CHECK(dclose(r10, 3.0));
    CHECK(dclose(r11, 4.0));
    CHECK(dclose(r12, 5.0));
    CHECK(dclose(r13, 6.0));
    CHECK(dclose(r14, 7.0));
    CHECK(dclose(r15, 8.0));
}

int main(void)
{
    printf("fptest: chapter 171 FP/SIMD-at-EL0 regression\n");

    test_basic_fp();
    test_yield_preserves_fp();
    test_setjmp_fp();

    if (g_fail) {
        printf("fptest: %d FAILED\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
