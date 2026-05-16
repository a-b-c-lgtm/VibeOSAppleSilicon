/*
 * userspace/libc/printf.h — header-only printf for our user libc.
 *
 * Single-translation-unit allocator-style header (matches the
 * convention from malloc.h): include it from one .c per binary.
 * Each binary that includes this header gets its own private copy
 * of the formatter — fine because every binary in-tree today is a
 * single .c file.
 *
 * Supported format specifiers:
 *
 *     %d  / %i      signed int (decimal)
 *     %u            unsigned int (decimal)
 *     %x  / %X      unsigned int (hex; lowercase / uppercase)
 *     %p            pointer (printed as 0x%lx)
 *     %s            NUL-terminated string (NULL prints as "(null)")
 *     %c            single character
 *     %%            literal '%'
 *
 * Length modifiers:
 *
 *     l             long          (matches AArch64 LP64: 64-bit)
 *     ll            long long     (also 64-bit on LP64; treated same)
 *     z             size_t        (also 64-bit)
 *
 * Flags:
 *
 *     0             zero-pad to width
 *     -             left-justify (overrides zero-pad)
 *     <number>      minimum field width
 *
 * No floating point, no precision (.N), no positional arguments.
 * If you hand it an unknown specifier the formatter writes the
 * `%` plus following character verbatim and continues.
 *
 * Output is via SYS_WRITE on fd 1 in chunks of FMT_CHUNK bytes
 * (no allocation); snprintf writes into a caller-supplied buffer.
 * Both return the number of bytes that *would* have been written
 * had the destination been infinite (C99 semantics for snprintf,
 * extended to printf for symmetry).
 */
#ifndef USER_PRINTF_H
#define USER_PRINTF_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "syscall.h"

#define FMT_CHUNK 128

struct _fmt_sink {
    /* If buf != NULL: write into buf up to cap-1 bytes, NUL-terminate.
     * If buf == NULL: write to stdout via SYS_WRITE in CHUNK-sized
     * batches.  In both cases `total` counts characters that *would*
     * have been emitted with no truncation (C99 semantics). */
    char    *buf;
    size_t   cap;     /* total buffer capacity including the NUL */
    size_t   used;    /* bytes already written to buf            */
    size_t   total;   /* counts EVERY character emitted          */

    char     batch[FMT_CHUNK];
    size_t   batch_len;
};

static inline void _fmt_flush(struct _fmt_sink *s)
{
    if (!s->buf && s->batch_len) {
        write(1, s->batch, s->batch_len);
        s->batch_len = 0;
    }
}

static inline void _fmt_emit(struct _fmt_sink *s, char c)
{
    s->total++;
    if (s->buf) {
        if (s->used + 1 < s->cap) {
            s->buf[s->used++] = c;
        }
    } else {
        s->batch[s->batch_len++] = c;
        if (s->batch_len == FMT_CHUNK) _fmt_flush(s);
    }
}

static inline void _fmt_emit_str(struct _fmt_sink *s, const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) _fmt_emit(s, str[i]);
}

static inline size_t _fmt_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Render `value` (already absolute) in `base` into a stack buffer
 * (most-significant digit first via reversal).  base is 10 or 16. */
static inline size_t _fmt_render_unsigned(uint64_t value, unsigned base,
                                          int upper, char *out, size_t out_cap)
{
    const char *digits_lower = "0123456789abcdef";
    const char *digits_upper = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    size_t n = 0;
    if (value == 0) {
        if (out_cap > 0) out[n++] = '0';
        return n;
    }
    char tmp[32];
    size_t i = 0;
    while (value > 0 && i < sizeof(tmp)) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    while (i > 0 && n < out_cap) out[n++] = tmp[--i];
    return n;
}

static inline void _fmt_emit_padded(struct _fmt_sink *s, const char *body,
                                    size_t body_len, int width,
                                    int left_justify, int zero_pad,
                                    int has_sign, char sign_char)
{
    size_t total = body_len + (has_sign ? 1 : 0);
    size_t pad = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;

    if (!left_justify) {
        if (zero_pad) {
            /* sign goes first when zero-padding so "-0042" not "0-042" */
            if (has_sign) _fmt_emit(s, sign_char);
            for (size_t i = 0; i < pad; i++) _fmt_emit(s, '0');
            _fmt_emit_str(s, body, body_len);
        } else {
            for (size_t i = 0; i < pad; i++) _fmt_emit(s, ' ');
            if (has_sign) _fmt_emit(s, sign_char);
            _fmt_emit_str(s, body, body_len);
        }
    } else {
        if (has_sign) _fmt_emit(s, sign_char);
        _fmt_emit_str(s, body, body_len);
        for (size_t i = 0; i < pad; i++) _fmt_emit(s, ' ');
    }
}

static inline int _fmt_vformat(struct _fmt_sink *s, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') { _fmt_emit(s, *fmt++); continue; }
        fmt++;

        /* Flags. */
        int left_justify = 0;
        int zero_pad     = 0;
        for (;;) {
            if (*fmt == '-') { left_justify = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else break;
        }
        if (left_justify) zero_pad = 0;  /* '-' overrides '0' per C99 */

        /* Width (digit run). */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifier: l, ll, z, h are accepted; we treat
         * everything as 64-bit on AArch64 (LP64), so they're
         * basically informational for us. */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') fmt++;       /* "ll" => still 64-bit */
        } else if (*fmt == 'z') {
            is_long = 1;
            fmt++;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;        /* "hh" no-op */
        }

        char spec = *fmt;
        if (spec == '\0') break;
        fmt++;

        char body[40];
        size_t body_len = 0;
        int  has_sign = 0;
        char sign_char = '\0';

        switch (spec) {
        case 'd': case 'i': {
            int64_t v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            uint64_t mag;
            if (v < 0) { has_sign = 1; sign_char = '-'; mag = (uint64_t)(-v); }
            else      { mag = (uint64_t)v; }
            body_len = _fmt_render_unsigned(mag, 10, 0, body, sizeof(body));
            _fmt_emit_padded(s, body, body_len, width, left_justify, zero_pad,
                             has_sign, sign_char);
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, unsigned long)
                                 : (unsigned long)va_arg(ap, unsigned int);
            body_len = _fmt_render_unsigned(v, 10, 0, body, sizeof(body));
            _fmt_emit_padded(s, body, body_len, width, left_justify, zero_pad,
                             0, 0);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = is_long ? va_arg(ap, unsigned long)
                                 : (unsigned long)va_arg(ap, unsigned int);
            body_len = _fmt_render_unsigned(v, 16, spec == 'X', body, sizeof(body));
            _fmt_emit_padded(s, body, body_len, width, left_justify, zero_pad,
                             0, 0);
            break;
        }
        case 'p': {
            /* Print as 0x%016lx (always full 64-bit, hex prefix). */
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            body_len = _fmt_render_unsigned((uint64_t)v, 16, 0, body, sizeof(body));
            /* Force "0x" prefix, then zero-pad body to 16 chars. */
            _fmt_emit(s, '0');
            _fmt_emit(s, 'x');
            for (size_t i = body_len; i < 16; i++) _fmt_emit(s, '0');
            _fmt_emit_str(s, body, body_len);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            _fmt_emit_padded(s, &c, 1, width, left_justify, 0, 0, 0);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            size_t l = _fmt_strlen(str);
            _fmt_emit_padded(s, str, l, width, left_justify, 0, 0, 0);
            break;
        }
        case '%': {
            _fmt_emit(s, '%');
            break;
        }
        default: {
            /* Unknown specifier: emit verbatim and keep going. */
            _fmt_emit(s, '%');
            _fmt_emit(s, spec);
            break;
        }
        }
    }

    _fmt_flush(s);
    if (s->buf && s->cap > 0) {
        size_t at = s->used < s->cap ? s->used : s->cap - 1;
        s->buf[at] = '\0';
    }
    return (int)s->total;
}

static inline int vprintf(const char *fmt, va_list ap)
{
    struct _fmt_sink s;
    s.buf = NULL; s.cap = 0; s.used = 0; s.total = 0; s.batch_len = 0;
    return _fmt_vformat(&s, fmt, ap);
}

static inline int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

static inline int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct _fmt_sink s;
    s.buf = buf; s.cap = cap; s.used = 0; s.total = 0; s.batch_len = 0;
    int n = _fmt_vformat(&s, fmt, ap);
    return n;
}

static inline int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

#endif /* USER_PRINTF_H */
