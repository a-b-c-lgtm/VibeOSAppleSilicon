/* userspace/libc/stdlib.h — chapter 169.
 *
 * The numeric / generic-algorithm slice of <stdlib.h> that real
 * upstream code expects.  We deliberately keep this header
 * focused on the things Doom, binutils, and (eventually) GCC
 * actually call -- *not* a kitchen-sink port of the entire C99
 * stdlib.  Surface today:
 *
 *   - qsort, bsearch
 *   - strtol, strtoul, strtoll, strtoull
 *   - atoi (redirected to strtol so call-sites that want the
 *     classic clamp-on-error semantics still get them), atol,
 *     atoll
 *   - getopt + the three externs the spec mandates (optarg,
 *     optind, optopt) and an osdev-specific opterr
 *   - abs, labs, llabs, div, ldiv, lldiv
 *
 * malloc / free / realloc / calloc / exit / atexit / _Exit /
 * getenv / setenv / unsetenv are already provided elsewhere
 * (malloc.h, atexit.h, env.h, syscall.h).  This header
 * forward-declares them so a single `#include <stdlib.h>` covers
 * the C99 expectations; the *definitions* still live in their
 * traditional homes.
 *
 * strtod / strtof / strtold are intentionally NOT defined here.
 * They return double / float / long double, which requires FP at
 * EL0 (chapter 171).  When chapter 171 ships we'll re-include
 * them in this header.  Doom uses none of the float strtoX
 * variants; binutils and GCC do, but only after we've turned FP
 * on anyway.
 *
 * Pattern: every function in this header is `static inline` so
 * including the header from multiple translation units does NOT
 * produce duplicate-symbol errors.  The qsort + getopt impls
 * are big enough (~80 lines each) to be worth pulling into a
 * dedicated .c file later, but for now leaving them header-only
 * keeps the chapter's "one header to add" property and matches
 * the policy described in
 * /memories/freestanding-c-memset-trap.md (avoid large statics
 * that GCC might lower to memcpy/memset calls).
 */

#ifndef OSDEV_LIBC_STDLIB_H
#define OSDEV_LIBC_STDLIB_H

#include <stddef.h>
#include <stdint.h>

/* C99 §7.20 says <stdlib.h> declares getenv, malloc, free,
 * exit, atexit, abort, atoi, etc.  All of those already live
 * as `static inline` (or are wired via crt0) in dedicated
 * header-only libs in this directory; declaring them again
 * here as `extern` would mismatch the existing `static`
 * linkage and break the build.  Instead we just pull those
 * headers in so a single `#include <stdlib.h>` gives callers
 * the full C99 surface.  Each header guards itself, so this
 * cascade is safe to include from any TU that already pulled
 * any of them in directly. */
#include "env.h"        /* getenv / setenv / unsetenv / putenv / clearenv */
#include "malloc.h"     /* malloc / free / realloc / calloc */
#include "atexit.h"     /* atexit + __cxa_finalize override */
#include "string.h"     /* atoi (and the rest of the mem/str surface) */
#include "signal.h"     /* abort (C99 7.20.4.1) — lives next to raise() */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 * EXIT_SUCCESS / EXIT_FAILURE
 *
 * C99 7.20: EXIT_SUCCESS is exit-status-zero, EXIT_FAILURE is
 * "an unsuccessful termination" of any non-zero value.  We pick
 * 1 because that's what every Unix tool does.
 * ------------------------------------------------------------- */
#ifndef EXIT_SUCCESS
# define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
# define EXIT_FAILURE 1
#endif

/* MB_CUR_MAX — C99 7.20.7, "maximum number of bytes in a
 * multibyte character for the current locale."  We're C-locale
 * only; that's always 1. */
#ifndef MB_CUR_MAX
# define MB_CUR_MAX ((size_t)1)
#endif

/* RAND_MAX — declared even though rand() isn't here (deferred
 * until something actually uses it).  Doom carries its own RNG;
 * binutils + GCC don't call rand(). */
#ifndef RAND_MAX
# define RAND_MAX 0x7fffffff
#endif

/* ---------------------------------------------------------------
 * abs / labs / llabs and div / ldiv / lldiv
 *
 * Trivial but C99 requires them by name; some upstream code
 * (binutils' libiberty in particular) prefers them over the
 * obvious open-coded forms because they're well-defined for
 * `INT_MIN` on twos-complement (we, like every real platform,
 * happen to define `abs(INT_MIN)` as `INT_MIN` -- undefined per
 * spec, but consistent).
 * ------------------------------------------------------------- */
static inline int      abs (int      x) { return x < 0 ? -x : x; }
static inline long     labs(long     x) { return x < 0 ? -x : x; }
static inline long long llabs(long long x) { return x < 0 ? -x : x; }

typedef struct { int      quot, rem; } div_t;
typedef struct { long     quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

static inline div_t  div (int  n, int  d)
    { div_t  r; r.quot = n / d; r.rem = n - r.quot * d; return r; }
static inline ldiv_t ldiv(long n, long d)
    { ldiv_t r; r.quot = n / d; r.rem = n - r.quot * d; return r; }
static inline lldiv_t lldiv(long long n, long long d)
    { lldiv_t r; r.quot = n / d; r.rem = n - r.quot * d; return r; }

/* ---------------------------------------------------------------
 * strtol / strtoul / strtoll / strtoull
 *
 * C99 7.20.1.{2,3,4}.  Single shared helper does the heavy
 * lifting (skip whitespace, sign, base detection, digit loop,
 * overflow clamp); each public entry point thin-wraps it.
 *
 * Overflow contract (per spec):
 *   - return LONG_MAX / LONG_MIN / ULONG_MAX (etc.) on overflow
 *   - set errno = ERANGE (we include errno.h to do this so
 *     callers that only #include <stdlib.h> still get correct
 *     errno semantics)
 *
 * Invalid input contract:
 *   - returns 0
 *   - *endptr (if non-NULL) is set to the original `nptr`
 * ------------------------------------------------------------- */

#include "errno.h"  /* errno macro; ERANGE constant */

#ifndef LONG_MAX
# define LONG_MAX  ((long)0x7fffffffffffffffL)
#endif
#ifndef LONG_MIN
# define LONG_MIN  (-LONG_MAX - 1L)
#endif
#ifndef ULONG_MAX
# define ULONG_MAX (~0UL)
#endif
#ifndef LLONG_MAX
# define LLONG_MAX  ((long long)0x7fffffffffffffffLL)
#endif
#ifndef LLONG_MIN
# define LLONG_MIN  (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
# define ULLONG_MAX (~0ULL)
#endif
#ifndef INT_MAX
# define INT_MAX   ((int)0x7fffffff)
#endif
#ifndef INT_MIN
# define INT_MIN   (-INT_MAX - 1)
#endif
#ifndef UINT_MAX
# define UINT_MAX  (~0U)
#endif

/* Map an ASCII byte to its digit value 0..35, or -1 if it's not
 * a valid digit in any supported base. */
static inline int __osdev_digit_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* Core unsigned converter.  Reads from `nptr`, stores end-of-scan
 * via `*endptr` (if non-NULL), returns the parsed magnitude.
 * Sets `*overflow` to non-zero if the value didn't fit in
 * `ULLONG_MAX`. */
static inline unsigned long long __osdev_strtoull_raw(
    const char *nptr, char **endptr, int base, int *overflow)
{
    const char *s = nptr;
    *overflow = 0;

    /* Skip whitespace.  C99 says "the same as for isspace()",
     * which in the C locale is " \t\n\v\f\r". */
    while (*s == ' '  || *s == '\t' || *s == '\n'
        || *s == '\v' || *s == '\f' || *s == '\r')
        s++;

    int neg = 0;
    if (*s == '+') { s++; }
    else if (*s == '-') { neg = 1; s++; }

    /* Base detection / validation. */
    if (base < 0 || base == 1 || base > 36) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')
                && __osdev_digit_value((unsigned char)s[2]) >= 0
                && __osdev_digit_value((unsigned char)s[2]) < 16) {
            s += 2; base = 16;
        } else if (s[0] == '0') {
            s += 1; base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16
            && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    /* Digit loop.  Track overflow per-digit. */
    const char *start = s;
    unsigned long long acc = 0;
    unsigned long long cutoff   = ULLONG_MAX / (unsigned)base;
    unsigned long long cutlim   = ULLONG_MAX % (unsigned)base;
    int any = 0;
    while (*s) {
        int d = __osdev_digit_value((unsigned char)*s);
        if (d < 0 || d >= base) break;
        if (acc > cutoff || (acc == cutoff && (unsigned long long)d > cutlim)) {
            *overflow = 1;
        } else {
            acc = acc * (unsigned)base + (unsigned)d;
        }
        s++;
        any = 1;
    }

    if (!any) {
        /* No digits scanned -- spec says *endptr = original nptr. */
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }
    if (endptr) *endptr = (char *)s;
    (void)start;

    /* Re-apply sign by *modular* unsigned negation so callers
     * see the same magnitude regardless of sign for the
     * unsigned API; the signed wrappers clamp explicitly. */
    if (neg) return (unsigned long long)0 - acc;
    return acc;
}

static inline unsigned long long strtoull(const char *nptr,
                                          char **endptr,
                                          int base)
{
    int ov = 0;
    unsigned long long v = __osdev_strtoull_raw(nptr, endptr, base, &ov);
    if (ov) { errno = ERANGE; return ULLONG_MAX; }
    return v;
}

static inline unsigned long strtoul(const char *nptr,
                                    char **endptr,
                                    int base)
{
    int ov = 0;
    unsigned long long v = __osdev_strtoull_raw(nptr, endptr, base, &ov);
    if (ov || v > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return (unsigned long)v;
}

static inline long long strtoll(const char *nptr,
                                char **endptr,
                                int base)
{
    /* We need the sign separately from the magnitude.  Re-scan
     * whitespace + sign here, then call the unsigned core on the
     * stripped-magnitude tail.  Slightly redundant; keeps the
     * sign logic explicit instead of buried inside the helper. */
    const char *s = nptr;
    while (*s == ' '  || *s == '\t' || *s == '\n'
        || *s == '\v' || *s == '\f' || *s == '\r')
        s++;
    int neg = 0;
    if (*s == '+')      { s++; }
    else if (*s == '-') { neg = 1; s++; }

    int ov = 0;
    /* Pass `s` (post-whitespace, post-sign) but if it parses
     * nothing we want endptr == original nptr per spec. */
    char *ep = (char *)nptr;
    unsigned long long mag = __osdev_strtoull_raw(s, &ep, base, &ov);
    /* If the raw helper saw no digits, ep == s; we want endptr
     * to point back at the *original* nptr in that case. */
    if (ep == s) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }
    if (endptr) *endptr = ep;

    if (neg) {
        unsigned long long limit = (unsigned long long)LLONG_MAX + 1ULL;
        if (ov || mag > limit) { errno = ERANGE; return LLONG_MIN; }
        if (mag == limit) return LLONG_MIN;
        return -(long long)mag;
    } else {
        if (ov || mag > (unsigned long long)LLONG_MAX) {
            errno = ERANGE; return LLONG_MAX;
        }
        return (long long)mag;
    }
}

static inline long strtol(const char *nptr, char **endptr, int base)
{
    long long v = strtoll(nptr, endptr, base);
    if (v > (long long)LONG_MAX) { errno = ERANGE; return LONG_MAX; }
    if (v < (long long)LONG_MIN) { errno = ERANGE; return LONG_MIN; }
    return (long)v;
}

/* atol / atoll — POSIX shortcuts equivalent to strto*(...,NULL,10). */
static inline long      atol(const char *s)  { return strtol(s, (char **)0, 10); }
static inline long long atoll(const char *s) { return strtoll(s, (char **)0, 10); }

/* atof — chapter 172.
 *
 * Minimal decimal-floating-point parser: optional sign, digits,
 * optional '.' fractional part, optional 'e[+-]?digits' exponent.
 * No hex, no INF/NAN, no locale.  Stops at the first byte that
 * isn't part of the recognised grammar.
 *
 * Used by Doom's m_config to parse float-typed config values
 * (mouse sensitivity, music volume scaling).  Doom doesn't
 * actually have float configs in the default config set, but
 * the parser walks every line so the call site must compile.
 *
 * Returns 0.0 on a string with no leading digits, matching the
 * POSIX shape "value (zero if conversion is not possible)".
 *
 * Requires EL0 FP (chapter 171).  If a TU that builds with
 * -mgeneral-regs-only includes this header and calls atof,
 * GCC will refuse the FP arithmetic — that's a build-time
 * signal, not a runtime issue. */
static inline double atof(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    double v = 0.0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (double)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (double)(*s - '0') * scale;
            scale *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        int eval = 0;
        while (*s >= '0' && *s <= '9') {
            eval = eval * 10 + (*s - '0');
            s++;
        }
        double mul = 1.0;
        for (int i = 0; i < eval; i++) mul *= 10.0;
        v = (esign < 0) ? (v / mul) : (v * mul);
    }
    return sign < 0 ? -v : v;
}

/* system — chapter 172.  POSIX: pass the command string to a
 * shell.  We don't have a /bin/sh equivalent the kernel can
 * exec yet (chapter 79's gui-term spawns sh.elf but that's
 * not reachable from a non-GUI process), so this stub always
 * reports failure.
 *
 * If `cmd == NULL`, POSIX says "report whether a shell is
 * available" — return 0 (no shell).  Otherwise return -1.
 * DoomGeneric's i_system.c uses this for two paths only:
 * the zenity-availability probe (returns "not available", we
 * just skip the GUI error popup), and the launch of an
 * external error dialog binary (we just suppress).  Both code
 * paths handle a negative return gracefully. */
static inline int system(const char *cmd)
{
    (void)cmd;
    return -1;
}

/* ---------------------------------------------------------------
 * qsort: simple in-place quicksort.
 *
 * O(n log n) average, O(n^2) worst case (sorted input + naive
 * pivot).  We pick the middle element as the pivot to make
 * common sorted-input cases hit the average case.  Insertion
 * sort for small partitions (<= 12 elements) per the standard
 * libstdc++ heuristic.
 *
 * Element swap is byte-at-a-time -- simple and avoids the
 * alignment / strict-aliasing landmines a 64-bit-aligned word
 * swap would create when the caller passes a `size` of 3 (yes,
 * binutils does this with struct relent[]).  Performance is
 * fine for everything we sort in-guest; binutils and GCC sort
 * thousands of items, not millions.
 * ------------------------------------------------------------- */
static inline void __osdev_swap_bytes(unsigned char *a,
                                      unsigned char *b,
                                      size_t n)
{
    while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; }
}

static inline void __osdev_qsort(unsigned char *base, size_t nmemb,
                                 size_t size,
                                 int (*cmp)(const void *,
                                            const void *))
{
    /* Insertion sort for small inputs. */
    if (nmemb < 12) {
        for (size_t i = 1; i < nmemb; i++) {
            size_t j = i;
            while (j > 0 && cmp(base + j * size,
                                base + (j - 1) * size) < 0) {
                __osdev_swap_bytes(base + j * size,
                                   base + (j - 1) * size, size);
                j--;
            }
        }
        return;
    }

    /* Pick pivot = middle element.  Swap it to position 0
     * so the partition loop has a simple invariant. */
    size_t mid = nmemb / 2;
    __osdev_swap_bytes(base, base + mid * size, size);

    /* Hoare-ish partition: scan from both ends, swap
     * out-of-order pairs, converge.  Pivot lives at base[0]. */
    size_t lo = 1, hi = nmemb - 1;
    while (1) {
        while (lo <= hi && cmp(base + lo * size, base) <= 0) lo++;
        while (hi >= lo && cmp(base + hi * size, base) >  0) {
            if (hi == 0) break;     /* avoid size_t underflow */
            hi--;
        }
        if (lo > hi) break;
        __osdev_swap_bytes(base + lo * size,
                           base + hi * size, size);
    }
    /* Place pivot in its final position. */
    __osdev_swap_bytes(base, base + hi * size, size);

    /* Recurse on the two partitions. */
    __osdev_qsort(base, hi, size, cmp);
    __osdev_qsort(base + (hi + 1) * size,
                  nmemb - hi - 1, size, cmp);
}

static inline void qsort(void *base, size_t nmemb, size_t size,
                         int (*cmp)(const void *, const void *))
{
    if (nmemb <= 1 || size == 0 || !base || !cmp) return;
    __osdev_qsort((unsigned char *)base, nmemb, size, cmp);
}

/* ---------------------------------------------------------------
 * bsearch: binary search of a sorted array.
 *
 * C99 7.20.5.1.  Caller guarantees the array is sorted by the
 * same comparator that will be passed here.  Returns a pointer
 * to one matching element (no specified tie-breaking) or NULL.
 * ------------------------------------------------------------- */
static inline void *bsearch(const void *key,
                            const void *base, size_t nmemb,
                            size_t size,
                            int (*cmp)(const void *,
                                       const void *))
{
    if (nmemb == 0 || size == 0 || !cmp) return (void *)0;
    const unsigned char *b = (const unsigned char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * size);
        if (c == 0) return (void *)(b + mid * size);
        if (c <  0) hi = mid;
        else        lo = mid + 1;
    }
    return (void *)0;
}

/* ---------------------------------------------------------------
 * getopt: POSIX argv parsing.
 *
 * POSIX 1003.1-2008 §getopt.  Per-process singleton state via
 * three externs (optarg / optind / optopt) plus our own opterr.
 * Single-character options only -- no GNU long-option extension
 * (--name=value is treated as an unknown option ``-'').  Doom's
 * argv parser, binutils' driver, and tar's flag parser all use
 * exactly this surface; GCC uses long options but already ships
 * its own getopt_long inside libiberty so we don't need to
 * mirror it here.
 *
 * Header-only static implementation -- per-binary singleton, but
 * every binary in-tree is one .c file so per-binary == per-
 * process, same as the rest of our libc.
 *
 * Optstring syntax (POSIX):
 *   "ab:c"  - "a" no arg, "b" requires arg, "c" no arg
 *   leading ":" - silence missing-arg diagnostics; return ':'
 *                 instead of '?' for that case
 *
 * Chapter 179: a single vendor TU (libiberty/getopt.c) defines
 * extern optarg / optind / optopt and provides _getopt_internal
 * for libiberty/getopt1.c (getopt_long).  Our static-storage
 * definitions would clash with its extern declarations at
 * compile time, so getopt.c #defines OSDEV_LIBC_NO_GETOPT before
 * including any header.  Every OTHER TU keeps the static getopt
 * (harmless — never referenced externally; per-TU optind state
 * is fine because no TU shares an argv parse across files). */
#ifndef OSDEV_LIBC_NO_GETOPT
static char *optarg = (char *)0;
static int   optind = 1;
static int   optopt = 0;
static int   opterr = 1;

static inline int getopt(int argc, char *const argv[], const char *opts)
{
    /* Per-call cursor across argv[optind][...].  Stays at 1
     * (skip the leading '-') when we're chewing through a
     * cluster like "-abc". */
    static int subindex = 1;

    if (optind >= argc) return -1;
    char *arg = argv[optind];
    if (arg == (char *)0)                return -1;
    if (arg[0] != '-' || arg[1] == '\0') return -1;
    /* "--" => end of options, consume it. */
    if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
        optind++;
        return -1;
    }

    int silent = (opts[0] == ':');

    int c = (unsigned char)arg[subindex];
    subindex++;
    if (arg[subindex] == '\0') {
        /* Last char of this cluster -- consume the whole token
         * and reset for next call. */
        optind++;
        subindex = 1;
    }

    /* Look up in opts. */
    const char *p = opts;
    if (silent) p++;
    while (*p) {
        if (*p == c) {
            if (p[1] == ':') {
                /* Argument-taking option.  Two flavours:
                 *   -bVALUE        (rest of current token)
                 *   -b VALUE       (next token) */
                if (subindex != 1) {
                    /* Mid-cluster -- the rest of the current
                     * token is the argument. */
                    optarg = arg + subindex;
                    optind++;
                    subindex = 1;
                    return c;
                }
                /* Token boundary -- the next argv element is
                 * the argument, if it exists. */
                if (optind >= argc) {
                    optopt = c;
                    if (!silent && opterr) {
                        /* In a fuller libc we'd print
                         * "<progname>: option requires an
                         * argument -- '<c>'\n" to stderr; our
                         * userspace doesn't routinely carry
                         * argv[0] through to a global, and
                         * Doom/binutils both silence opterr
                         * before calling getopt anyway.  Skip
                         * the diagnostic; honour the return
                         * convention. */
                    }
                    return silent ? ':' : '?';
                }
                optarg = argv[optind++];
                return c;
            }
            return c;
        }
        p++;
    }

    /* Unknown option. */
    optopt = c;
    return '?';
}
#endif /* OSDEV_LIBC_NO_GETOPT */

/* Chapter 178 — `mktemp(char *template)`.
 *
 * POSIX (deprecated, still ubiquitous): replace the trailing
 * "XXXXXX" of `template` with a string that makes the path
 * not currently exist.  Returns `template` on success, an
 * empty string on failure.
 *
 * Real glibc warns at link time because the open() race makes
 * mktemp unsafe; libiberty's choose-temp.c uses it anyway, with
 * a follow-up open(O_EXCL|O_CREAT) to detect the race.  That
 * follow-up open is what makes the contract safe for us too.
 *
 * Our entropy source is (getpid() << 16) ^ uptime_ms ^ a static
 * counter, hashed into base-62.  Six characters = 62^6 = 56B
 * combinations — collisions inside a single binutils build are
 * vanishingly rare. */
static inline char *mktemp(char *template_)
{
    if (!template_) return template_;
    size_t n = 0;
    while (template_[n]) n++;
    if (n < 6) { template_[0] = '\0'; return template_; }
    size_t tail = n - 6;
    for (size_t i = 0; i < 6; i++) {
        if (template_[tail + i] != 'X') {
            template_[0] = '\0';
            return template_;
        }
    }

    static uint32_t mktemp_seq = 0;
    uint64_t now = (uint64_t)uptime_ms();
    uint32_t pid = (uint32_t)getpid();
    uint32_t mix = (uint32_t)(now ^ ((uint64_t)pid << 16) ^ (uint64_t)(mktemp_seq++ * 2654435761u));
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < 6; i++) {
        template_[tail + i] = alphabet[mix % 62u];
        mix = mix / 62u + 1u;  /* +1 keeps the divisor moving */
    }
    return template_;
}

#ifdef __cplusplus
}
#endif

#endif /* OSDEV_LIBC_STDLIB_H */
