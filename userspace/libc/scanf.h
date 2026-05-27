/* userspace/libc/scanf.h — chapter 170.
 *
 * Header-only sscanf / vsscanf.  The fscanf/scanf wrappers
 * live in stdio.h (since they need FILE *); this header
 * provides the core formatter parameterised over a "source"
 * callback so the same loop can chew through either a string
 * or a FILE *.
 *
 * Supported conversion specifiers (subset of C99 §7.19.6.2,
 * matched against what Doom + binutils + GCC actually use):
 *
 *     %d      signed decimal int
 *     %i      signed integer with C-style base prefix
 *     %u      unsigned decimal int
 *     %o      unsigned octal int
 *     %x / %X unsigned hex int
 *     %s      whitespace-delimited string (NUL terminated)
 *     %c      one character (no skip-whitespace; honours width)
 *     %n      store running input-char count into (int *)
 *     %%      literal '%'
 *
 *     %[set]  scanset — accept only the listed chars; supports
 *             %[^set] to invert, "]" must appear first in the
 *             set to be a literal.  This is what GCC's option
 *             parser uses for token grabbing.
 *
 * Length modifier `l` is accepted on integer conversions and
 * causes the destination to be `long *` instead of `int *`
 * (on LP64 both are 64-bit but a strict reading of va_arg
 * still matters).  `ll`, `z`, `j`, `t` likewise -> `long long *`
 * etc.  Float specifiers (%f / %e / %g) deferred until ch 129
 * lifts -mgeneral-regs-only.
 *
 * `*` flag (assignment suppression): the conversion is performed
 * but the result is discarded -- the corresponding va_arg
 * pointer is NOT consumed.  This is exactly what scanf(3)
 * does and what binutils' linker-script parser uses to skip
 * uninteresting fields.
 *
 * Width: max chars consumed by the conversion (NOT including
 * the leading whitespace skip).  For %s and %[...] the width
 * caps how many chars are stored (one fewer than the buffer
 * size).
 *
 * Return value matches C99: number of successful conversions,
 * or EOF if input ran out before any successful conversion.
 */
#ifndef USER_SCANF_H
#define USER_SCANF_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifndef EOF
# define EOF (-1)
#endif

/* The "source" is two callbacks the caller plumbs in.
 *   _scn_get  returns the next byte as 0..255, or EOF.
 *   _scn_unget pushes one byte back into the source (LIFO,
 *              depth 1 is sufficient for our formatter --
 *              scanf only ever needs one-byte lookahead). */
struct _scn_src {
    int (*get)(void *cookie);
    int (*unget)(void *cookie, int c);
    void *cookie;
    size_t consumed;                  /* chars read off the source */
};

static inline int _scn_get(struct _scn_src *src)
{
    int c = src->get(src->cookie);
    if (c != EOF) src->consumed++;
    return c;
}

static inline int _scn_unget(struct _scn_src *src, int c)
{
    if (c == EOF) return EOF;
    int r = src->unget(src->cookie, c);
    if (r != EOF && src->consumed) src->consumed--;
    return r;
}

static inline int _scn_isspace(int c)
{
    return c == ' '  || c == '\t' || c == '\n'
        || c == '\v' || c == '\f' || c == '\r';
}

static inline int _scn_digit_value(int c, int base)
{
    int v;
    if (c >= '0' && c <= '9')      v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

/* Consume whitespace from `src`.  Returns 0 on success, EOF if
 * the source is exhausted before any non-whitespace. */
static inline int _scn_skip_ws(struct _scn_src *src)
{
    int c;
    for (;;) {
        c = _scn_get(src);
        if (c == EOF) return EOF;
        if (!_scn_isspace(c)) {
            _scn_unget(src, c);
            return 0;
        }
    }
}

/* Core formatter. */
static inline int _scn_vformat(struct _scn_src *src, const char *fmt,
                               va_list ap)
{
    int matched = 0;
    int got_any = 0;       /* did we ever read a char off the src? */

    while (*fmt) {
        if (_scn_isspace((unsigned char)*fmt)) {
            /* Whitespace in fmt matches zero or more whitespace
             * chars in the input. */
            int c;
            do { c = _scn_get(src); }
            while (c != EOF && _scn_isspace(c));
            if (c != EOF) _scn_unget(src, c);
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            /* Literal char: must match exactly. */
            int c = _scn_get(src);
            if (c == EOF) goto done;
            got_any = 1;
            if (c != (unsigned char)*fmt) { _scn_unget(src, c); goto done; }
            fmt++;
            continue;
        }

        /* % seen.  Parse [*] [width] [length] specifier. */
        fmt++;
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;          /* l / ll / z / j / t */
        if (*fmt == 'l') {
            is_long = 1; fmt++;
            if (*fmt == 'l') fmt++;
        } else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            is_long = 1; fmt++;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
        }

        char spec = *fmt;
        if (spec == '\0') break;
        fmt++;

        switch (spec) {

        case '%': {
            int c = _scn_get(src);
            if (c == EOF) goto done;
            got_any = 1;
            if (c != '%') { _scn_unget(src, c); goto done; }
            break;
        }

        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            /* Integer conversion.  Skip whitespace first. */
            if (_scn_skip_ws(src) == EOF) goto done;
            int c = _scn_get(src);
            if (c == EOF) goto done;
            got_any = 1;
            int neg = 0;
            int chars_used = 0;
            if (c == '+' || c == '-') {
                if (c == '-') neg = 1;
                chars_used++;
                c = _scn_get(src);
                if (c == EOF) goto done;
            }

            /* Pick base. */
            int base;
            if (spec == 'd' || spec == 'u')      base = 10;
            else if (spec == 'o')                 base = 8;
            else if (spec == 'x' || spec == 'X') base = 16;
            else /* %i */                         base = 0;

            /* For %i and %x with explicit 0x, swallow the prefix. */
            if (base == 0 || base == 16) {
                if (c == '0') {
                    chars_used++;
                    int p = _scn_get(src);
                    if (p == 'x' || p == 'X') {
                        chars_used++;
                        if (base == 0) base = 16;
                        c = _scn_get(src);
                        if (c == EOF) goto done;
                    } else {
                        if (base == 0) base = 8;
                        /* The '0' itself is a valid digit;
                         * fold it into the accumulator on the
                         * first iteration by un-getting `p`
                         * and pretending c == '0'. */
                        if (p != EOF) _scn_unget(src, p);
                        c = '0';
                    }
                } else if (base == 0) {
                    base = 10;
                }
            }

            int dv = _scn_digit_value(c, base);
            if (dv < 0) {
                /* Not a digit -- conversion fails. */
                if (c != EOF) _scn_unget(src, c);
                goto done;
            }
            uint64_t acc = (uint64_t)dv;
            chars_used++;
            while (width == 0 || chars_used < width) {
                int n = _scn_get(src);
                if (n == EOF) break;
                int v = _scn_digit_value(n, base);
                if (v < 0) { _scn_unget(src, n); break; }
                acc = acc * (unsigned)base + (unsigned)v;
                chars_used++;
            }

            if (!suppress) {
                if (spec == 'd' || spec == 'i') {
                    long long val = neg ? -(long long)acc : (long long)acc;
                    if (is_long) *va_arg(ap, long *) = (long)val;
                    else         *va_arg(ap, int  *) = (int)val;
                } else {
                    if (is_long) *va_arg(ap, unsigned long *) = (unsigned long)acc;
                    else         *va_arg(ap, unsigned int  *) = (unsigned int)acc;
                }
                matched++;
            }
            break;
        }

        case 's': {
            /* Skip leading whitespace, then read non-whitespace
             * chars up to width or next whitespace. */
            if (_scn_skip_ws(src) == EOF) goto done;
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int wrote = 0;
            int budget = (width > 0) ? width : 0x7fffffff;
            int got_chars = 0;
            for (int i = 0; i < budget; i++) {
                int c = _scn_get(src);
                if (c == EOF) break;
                got_any = 1;
                if (_scn_isspace(c)) { _scn_unget(src, c); break; }
                if (dst) dst[wrote++] = (char)c;
                got_chars++;
            }
            if (got_chars == 0) goto done;
            if (dst) dst[wrote] = '\0';
            if (!suppress) matched++;
            break;
        }

        case 'c': {
            /* Reads `width` chars (default 1) -- no skip ws. */
            int budget = (width > 0) ? width : 1;
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int wrote = 0;
            for (int i = 0; i < budget; i++) {
                int c = _scn_get(src);
                if (c == EOF) {
                    if (wrote == 0) goto done;
                    break;
                }
                got_any = 1;
                if (dst) dst[wrote++] = (char)c;
            }
            if (!suppress) matched++;
            break;
        }

        case '[': {
            /* Scanset: build a 256-bit acceptance table. */
            int invert = 0;
            if (*fmt == '^') { invert = 1; fmt++; }
            unsigned char accept[32];
            for (int i = 0; i < 32; i++) accept[i] = 0;
#define _SCN_SET(c) (accept[((unsigned)(c)) >> 3] |= (1u << ((c) & 7)))
            /* A ']' as the first char of the set is literal. */
            if (*fmt == ']') { _SCN_SET(']'); fmt++; }
            while (*fmt && *fmt != ']') {
                _SCN_SET((unsigned char)*fmt);
                fmt++;
            }
            if (*fmt == ']') fmt++;
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int budget = (width > 0) ? width : 0x7fffffff;
            int wrote = 0;
            int got_chars = 0;
            for (int i = 0; i < budget; i++) {
                int c = _scn_get(src);
                if (c == EOF) break;
                got_any = 1;
                int in_set = (accept[((unsigned)c) >> 3] >> (c & 7)) & 1;
                if (invert) in_set = !in_set;
                if (!in_set) { _scn_unget(src, c); break; }
                if (dst) dst[wrote++] = (char)c;
                got_chars++;
            }
            if (got_chars == 0) goto done;
            if (dst) dst[wrote] = '\0';
            if (!suppress) matched++;
#undef _SCN_SET
            break;
        }

        case 'n': {
            if (!suppress) {
                int *dst = va_arg(ap, int *);
                if (dst) *dst = (int)src->consumed;
            }
            /* %n does NOT count as a conversion. */
            break;
        }

        default:
            /* Unknown specifier — stop. */
            goto done;
        }
    }

done:
    if (matched == 0 && !got_any) return EOF;
    return matched;
}

/* --- sscanf source --------------------------------------------------- */

struct _scn_str_src {
    const char *base;
    size_t      pos;
    size_t      undo;   /* pushed-back char if undo > 0 (depth 1) */
    unsigned char undo_ch;
};

static inline int _scn_str_get(void *c)
{
    struct _scn_str_src *ss = (struct _scn_str_src *)c;
    if (ss->undo) { ss->undo = 0; return ss->undo_ch; }
    unsigned char ch = (unsigned char)ss->base[ss->pos];
    if (ch == 0) return EOF;
    ss->pos++;
    return ch;
}

static inline int _scn_str_unget(void *c, int ch)
{
    struct _scn_str_src *ss = (struct _scn_str_src *)c;
    if (ss->undo) return EOF;            /* one-level pushback */
    ss->undo = 1;
    ss->undo_ch = (unsigned char)ch;
    return ch;
}

static inline int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct _scn_str_src ss = { s, 0, 0, 0 };
    struct _scn_src src = { _scn_str_get, _scn_str_unget, &ss, 0 };
    return _scn_vformat(&src, fmt, ap);
}

static inline int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

#endif /* USER_SCANF_H */
