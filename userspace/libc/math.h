/*
 * userspace/libc/math.h — minimal libm for chapter 130 (Doom).
 *
 * DoomGeneric's libm surface is tiny: r_main.c calls sin(),
 * tan(), atan() once each at renderer init to build finetangent
 * tables, and v_video.c calls fabs() once for a mouse-acceleration
 * dead-zone check.  See chapter 172 for the audit.
 *
 * Header-only / static-inline:
 *
 *   - No link dependency.  cc -I userspace/libc and you have libm.
 *   - GCC happily folds the constant-arg call sites in r_main.c
 *     at compile time, so the only runtime cost is the v_video.c
 *     fabs() (single branch).
 *
 * Implementation strategy:
 *
 *   sin    Range-reduce to [-pi/4, pi/4] with the four identities
 *          (sin period 2pi; sin(pi - x) == sin(x);
 *           sin(x) == cos(pi/2 - x) for x in (pi/4, pi/2]) then
 *          7-term Taylor.  Absolute error < 1e-13 on the reduced
 *          range; Doom converts the result to FRACUNIT=65536 fixed
 *          point so anything below 1e-5 is irrelevant.
 *   cos    sin(pi/2 - x).
 *   tan    sin(x) / cos(x).
 *   atan   Range-reduce |x| to [0, tan(pi/8)] via
 *          atan(x) = pi/2 - atan(1/x)        for |x| > 1
 *          atan(x) = pi/4 + atan((x-1)/(x+1))  for x in (tan(pi/8), 1]
 *          then 12-term Taylor on the reduced argument.
 *   fabs   Sign-flip.
 *
 * Anything else (sqrt, pow, exp, log, atan2, floor, ceil, ...)
 * isn't here on purpose -- Doom doesn't call them, and a future
 * chapter that ports a program that does will extend this header
 * (or replace it with a real libm) at that time.
 */

#ifndef LIBC_MATH_H
#define LIBC_MATH_H

#include <stdint.h>     /* uint64_t for ldexp / frexp bit-fiddling */

#ifdef __cplusplus
extern "C" {
#endif

#define M_PI    3.14159265358979323846
#define M_PI_2  1.57079632679489661923
#define M_PI_4  0.78539816339744830962

static inline double fabs(double x)
{
    return x < 0.0 ? -x : x;
}

/* sin/cos primitives on [-pi/4, pi/4] -- pure Taylor, no
 * range reduction.  Coefficients factored to a Horner-style
 * incremental product so each new term is one multiply +
 * one divide + one add. */
static inline double _libc_sin_red(double x)
{
    double x2 = x * x;
    double t = x;
    double s = t;
    t *= -x2 / (2.0 * 3.0);    s += t;
    t *= -x2 / (4.0 * 5.0);    s += t;
    t *= -x2 / (6.0 * 7.0);    s += t;
    t *= -x2 / (8.0 * 9.0);    s += t;
    t *= -x2 / (10.0 * 11.0);  s += t;
    t *= -x2 / (12.0 * 13.0);  s += t;
    return s;
}

static inline double _libc_cos_red(double x)
{
    double x2 = x * x;
    double t = 1.0;
    double s = t;
    t *= -x2 / (1.0 * 2.0);    s += t;
    t *= -x2 / (3.0 * 4.0);    s += t;
    t *= -x2 / (5.0 * 6.0);    s += t;
    t *= -x2 / (7.0 * 8.0);    s += t;
    t *= -x2 / (9.0 * 10.0);   s += t;
    t *= -x2 / (11.0 * 12.0);  s += t;
    return s;
}

/* Floor-style reduction: largest integer <= x.  Used only by
 * sin() to wrap by 2pi; we don't expose it because Doom
 * doesn't call floor() directly. */
static inline double _libc_floor(double x)
{
    long long i = (long long)x;
    if ((double)i > x) i--;
    return (double)i;
}

static inline double sin(double x)
{
    /* Wrap to [-pi, pi]. */
    double k = _libc_floor((x + M_PI) / (2.0 * M_PI));
    x -= k * (2.0 * M_PI);

    /* Fold to [-pi/2, pi/2] via sin(pi - x) == sin(x). */
    if (x >  M_PI_2) x =  M_PI - x;
    if (x < -M_PI_2) x = -M_PI - x;

    /* Fold to [-pi/4, pi/4] via sin(x) == cos(pi/2 - x). */
    if (x >  M_PI_4) return  _libc_cos_red(M_PI_2 - x);
    if (x < -M_PI_4) return -_libc_cos_red(M_PI_2 + x);

    return _libc_sin_red(x);
}

static inline double cos(double x)
{
    return sin(M_PI_2 - x);
}

static inline double tan(double x)
{
    return sin(x) / cos(x);
}

/* Taylor for atan on |x| <= tan(pi/8) ~ 0.4142.  12 terms gives
 * ~1e-13 abs error on that range. */
static inline double _libc_atan_red(double x)
{
    double x2 = x * x;
    double t = x;
    double s = t;
    int n;
    for (n = 1; n < 12; n++) {
        t *= -x2;
        s += t / (2.0 * (double)n + 1.0);
    }
    return s;
}

static inline double atan(double x)
{
    int neg = (x < 0.0);
    if (neg) x = -x;

    int invert = (x > 1.0);
    if (invert) x = 1.0 / x;
    /* Now 0 <= x <= 1. */

    double off = 0.0;
    if (x > 0.41421356237309515) {  /* tan(pi/8) */
        off = M_PI_4;
        x = (x - 1.0) / (x + 1.0);
    }
    double r = off + _libc_atan_red(x);
    if (invert) r = M_PI_2 - r;
    if (neg)    r = -r;
    return r;
}

/* Chapter 178 — `ldexp` / `frexp`.
 *
 * Direct IEEE-754 bit-fiddling on the 11-bit biased exponent.
 * libiberty's floatformat.c is the only caller in binutils-2.44
 * that this enables — and only when the host floatformat-helper
 * tooling actually runs, which is never on aarch64 targets.
 * Implementing them properly (rather than abort-stubs) costs ten
 * lines apiece and means future ports that DO touch FP get sane
 * behaviour automatically. */
static inline double ldexp(double x, int exp)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    uint64_t exp_field = (v.u >> 52) & 0x7FFu;
    /* Zero, NaN, infinity: leave bits unchanged. */
    if (exp_field == 0 || exp_field == 0x7FFu) return x;
    int e = (int)exp_field + exp;
    if (e <= 0)        return (v.u >> 63) ? -0.0 : 0.0;   /* underflow */
    if (e >= 0x7FF) {  /* overflow → infinity */
        v.u = (v.u & (1ULL << 63)) | (0x7FFULL << 52);
        return v.d;
    }
    v.u = (v.u & ~(0x7FFULL << 52)) | ((uint64_t)e << 52);
    return v.d;
}

static inline double frexp(double x, int *exp_out)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    uint64_t exp_field = (v.u >> 52) & 0x7FFu;
    /* Zero / subnormal / NaN / infinity: return as-is with exp=0. */
    if (exp_field == 0 || exp_field == 0x7FFu) {
        if (exp_out) *exp_out = 0;
        return x;
    }
    if (exp_out) *exp_out = (int)exp_field - 1022;
    v.u = (v.u & ~(0x7FFULL << 52)) | ((uint64_t)1022u << 52);
    return v.d;
}

#ifdef __cplusplus
}
#endif

#endif /* LIBC_MATH_H */
