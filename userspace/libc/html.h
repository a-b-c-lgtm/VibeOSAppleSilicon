/*
 * userspace/libc/html.h — header-only HTML5 tokenizer.
 *
 * Single-translation-unit convention (matches printf.h, malloc.h,
 * url.h, http.h): include from one .c per binary.  No allocation
 * performed inside the parser; the caller owns the `struct html_token`
 * scratch buffer that the tokenizer fills on each call.
 *
 * Scope and non-scope:
 *
 *   - We implement the subset of the HTML5 tokenization state machine
 *     (WHATWG §13.2.5) that real-world pages exercise: data, tag-open,
 *     tag-name, end-tag-open, before/in attribute name, before/in
 *     attribute value (double-quoted, single-quoted, unquoted),
 *     self-closing-start, comment, doctype, and the "raw text"
 *     branches for <script> and <style>.
 *
 *   - We do NOT implement the parse-error machinery (which the spec
 *     uses primarily for browser conformance testing), nor CDATA
 *     sections, nor the foreign-content handling for SVG/MathML.
 *     Any malformed input that falls through our happy paths is
 *     treated as best-effort: we recover at the next '<' rather
 *     than reject the document.
 *
 *   - Character references (entities) are decoded for the named set
 *     { amp, lt, gt, quot, apos, nbsp } plus numeric forms &#NN; and
 *     &#xHH;.  Anything else is passed through verbatim — exactly
 *     how every real browser handles unknown entities once the
 *     trailing semicolon is missing.
 *
 * Token model:
 *
 *   The tokenizer is pull-style.  Each call to html_tok_next() fills
 *   the caller's `struct html_token` and returns 1 on success, 0 on
 *   EOF, or -1 on internal error (only happens if buffers overflow,
 *   which the tokenizer treats as a hard stop).
 *
 *   Character-data runs (TOK_CHARS) are coalesced as far as the
 *   `data[]` capacity allows: a long run of plain text becomes one
 *   token, or several back-to-back tokens if it would not fit.  This
 *   keeps the next layer (DOM) from having to merge text nodes.
 *
 * Memory:
 *
 *   `struct html_token` is ~12 KiB.  Heap-allocate it; do not put it
 *   on the userspace stack.  See url/http parser chapter for why.
 *
 *   The tokenizer state struct itself is small (< 64 bytes) and is
 *   safe on the stack.
 */
#ifndef USER_HTML_H
#define USER_HTML_H

#include <stdint.h>
#include <stddef.h>

/* ---------- token shapes ---------- */

#define HTML_TAG_NAME_MAX     64
#define HTML_ATTR_NAME_MAX    64
#define HTML_ATTR_VALUE_MAX   512
#define HTML_DATA_MAX         4096
#define HTML_MAX_ATTRS        16

enum html_tok_type {
    HTML_TOK_NONE     = 0,
    HTML_TOK_CHARS    = 1,    /* text run (entity-decoded) */
    HTML_TOK_START    = 2,    /* <tag attr=...>  (or <tag .../> with self_closing=1) */
    HTML_TOK_END      = 3,    /* </tag> */
    HTML_TOK_COMMENT  = 4,    /* <!-- ... --> */
    HTML_TOK_DOCTYPE  = 5,    /* <!DOCTYPE html> (we keep just the name slug) */
    HTML_TOK_EOF      = 6,
};

struct html_attr {
    char  name[HTML_ATTR_NAME_MAX];
    size_t name_len;
    char  value[HTML_ATTR_VALUE_MAX];
    size_t value_len;
};

struct html_token {
    int    type;                          /* enum html_tok_type */
    /* TAG: lower-cased tag name, NUL-terminated. */
    char   tag_name[HTML_TAG_NAME_MAX];
    size_t tag_name_len;
    int    self_closing;                  /* 1 if <foo .../> */
    int    n_attrs;
    struct html_attr attrs[HTML_MAX_ATTRS];
    /* CHARS / COMMENT / DOCTYPE: decoded text, NOT NUL-terminated. */
    char   data[HTML_DATA_MAX];
    size_t data_len;
};

/* ---------- tokenizer state ---------- */

enum {
    HTML_S_DATA = 0,        /* outside tags; emit chars / start tag-open */
    HTML_S_RAWTEXT,         /* inside <script>/<style>; only </name> ends it */
    HTML_S_DONE,            /* EOF reached */
};

struct html_tokenizer {
    const char *src;
    size_t      src_len;
    size_t      pos;
    int         state;
    /* When state == HTML_S_RAWTEXT, ends_with[0..ends_len-1] is the
     * lowercase end-tag we are scanning for, e.g. "script". */
    char        ends_with[16];
    size_t      ends_len;
};

/* ---------- helpers ---------- */

static int htm_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int htm_is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static char htm_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Append one byte to (buf,len,cap).  Returns 1 on success, 0 on
 * overflow.  We treat overflow as silent truncation for character
 * data (so a giant <pre> block does not break tokenization), and
 * the parser explicitly checks for it on names/attrs. */
static int htm_putc(char *buf, size_t *len, size_t cap, char c)
{
    if (*len >= cap) return 0;
    buf[(*len)++] = c;
    return 1;
}

/* Append a NUL-terminated string. */
static int htm_puts(char *buf, size_t *len, size_t cap, const char *s)
{
    while (*s) {
        if (!htm_putc(buf, len, cap, *s)) return 0;
        s++;
    }
    return 1;
}

/* Decode one character reference starting at src[*pos] which is the
 * '&'.  On success advances *pos past the reference and writes one
 * or more bytes into (buf,len,cap).  On failure (not a recognised
 * reference) emits the literal '&' and returns; caller should keep
 * scanning from the next char. */
static void htm_decode_entity(const char *src, size_t src_len, size_t *pos,
                              char *buf, size_t *len, size_t cap)
{
    /* Already at '&'.  We support the named set used in real HTML
     * 99% of the time, plus &#NN; and &#xHH;. */
    size_t p = *pos;
    if (p >= src_len || src[p] != '&') return;

    /* Numeric? */
    if (p + 2 < src_len && src[p + 1] == '#') {
        size_t q = p + 2;
        unsigned long v = 0;
        int hex = 0;
        if (q < src_len && (src[q] == 'x' || src[q] == 'X')) { hex = 1; q++; }
        size_t start = q;
        while (q < src_len) {
            char c = src[q];
            if (hex) {
                if      (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned)(c - 'A' + 10);
                else break;
            } else {
                if (c >= '0' && c <= '9') v = v * 10 + (unsigned)(c - '0');
                else break;
            }
            q++;
        }
        if (q == start) {
            /* No digits — emit the literal '&' and bail. */
            htm_putc(buf, len, cap, '&');
            *pos = p + 1;
            return;
        }
        if (q < src_len && src[q] == ';') q++;   /* swallow optional ';' */
        /* ASCII subset only: anything < 0x80 we emit as one byte;
         * 0xA0 (nbsp) we emit as a regular space; anything else we
         * emit as '?' rather than mishandling UTF-8 in our text
         * mode renderer.  The browser layer can revisit this when
         * we have UTF-8 in the font. */
        if (v < 0x80)            htm_putc(buf, len, cap, (char)v);
        else if (v == 0xA0)      htm_putc(buf, len, cap, ' ');
        else                     htm_putc(buf, len, cap, '?');
        *pos = q;
        return;
    }

    /* Named: scan name into a tiny local buffer. */
    char name[12];
    size_t n = 0;
    size_t q = p + 1;
    while (q < src_len && n < sizeof(name) - 1) {
        char c = src[q];
        if (htm_is_alpha(c)) { name[n++] = htm_lower(c); q++; }
        else break;
    }
    name[n] = '\0';

    /* Recognised set.  Match before requiring ';' so we are robust
     * to the "&amp text" pattern many static-HTML generators emit.
     * Multi-byte expansions (mdash, hellip, etc.) decode to ASCII
     * approximations because our renderer's font is ASCII-only. */
    int matched = 1;
    const char *emit_s = 0;
    char emit[2] = {0,0};
    int  emit_len = 1;
    if      (n == 3 && name[0] == 'a' && name[1] == 'm' && name[2] == 'p') emit[0] = '&';
    else if (n == 2 && name[0] == 'l' && name[1] == 't')                   emit[0] = '<';
    else if (n == 2 && name[0] == 'g' && name[1] == 't')                   emit[0] = '>';
    else if (n == 4 && name[0] == 'q' && name[1] == 'u' && name[2] == 'o' && name[3] == 't') emit[0] = '"';
    else if (n == 4 && name[0] == 'a' && name[1] == 'p' && name[2] == 'o' && name[3] == 's') emit[0] = '\'';
    else if (n == 4 && name[0] == 'n' && name[1] == 'b' && name[2] == 's' && name[3] == 'p') emit[0] = ' ';
    /* Arrows (ASCII approximation). */
    else if (n == 4 && name[0] == 'r' && name[1] == 'a' && name[2] == 'r' && name[3] == 'r') emit_s = "->";
    else if (n == 4 && name[0] == 'l' && name[1] == 'a' && name[2] == 'r' && name[3] == 'r') emit_s = "<-";
    else if (n == 4 && name[0] == 'u' && name[1] == 'a' && name[2] == 'r' && name[3] == 'r') emit[0] = '^';
    else if (n == 4 && name[0] == 'd' && name[1] == 'a' && name[2] == 'r' && name[3] == 'r') emit[0] = 'v';
    /* Dashes / ellipsis. */
    else if (n == 5 && name[0] == 'm' && name[1] == 'd' && name[2] == 'a' && name[3] == 's' && name[4] == 'h') emit_s = "--";
    else if (n == 5 && name[0] == 'n' && name[1] == 'd' && name[2] == 'a' && name[3] == 's' && name[4] == 'h') emit[0] = '-';
    else if (n == 6 && name[0] == 'h' && name[1] == 'e' && name[2] == 'l' && name[3] == 'l' && name[4] == 'i' && name[5] == 'p') emit_s = "...";
    /* Typography / punctuation. */
    else if (n == 4 && name[0] == 'c' && name[1] == 'o' && name[2] == 'p' && name[3] == 'y') emit_s = "(c)";
    else if (n == 3 && name[0] == 'r' && name[1] == 'e' && name[2] == 'g') emit_s = "(R)";
    else if (n == 5 && name[0] == 't' && name[1] == 'r' && name[2] == 'a' && name[3] == 'd' && name[4] == 'e') emit_s = "(TM)";
    else if (n == 5 && name[0] == 'l' && name[1] == 'a' && name[2] == 'q' && name[3] == 'u' && name[4] == 'o') emit_s = "<<";
    else if (n == 5 && name[0] == 'r' && name[1] == 'a' && name[2] == 'q' && name[3] == 'u' && name[4] == 'o') emit_s = ">>";
    else if (n == 4 && name[0] == 'b' && name[1] == 'u' && name[2] == 'l' && name[3] == 'l') emit[0] = '*';
    else if (n == 6 && name[0] == 'm' && name[1] == 'i' && name[2] == 'd' && name[3] == 'd' && name[4] == 'o' && name[5] == 't') emit[0] = '*';
    else if (n == 5 && name[0] == 't' && name[1] == 'i' && name[2] == 'm' && name[3] == 'e' && name[4] == 's') emit[0] = 'x';
    else if (n == 6 && name[0] == 'd' && name[1] == 'i' && name[2] == 'v' && name[3] == 'i' && name[4] == 'd' && name[5] == 'e') emit[0] = '/';
    else if (n == 3 && name[0] == 'd' && name[1] == 'e' && name[2] == 'g') emit[0] = '*';
    else matched = 0;

    if (!matched) {
        /* Pass through verbatim, advance past '&' only.  The
         * caller's outer loop will copy 'a' 'm' 'p' next. */
        htm_putc(buf, len, cap, '&');
        *pos = p + 1;
        return;
    }
    if (emit_s) {
        for (const char *e = emit_s; *e; e++) htm_putc(buf, len, cap, *e);
    } else {
        for (int i = 0; i < emit_len; i++) htm_putc(buf, len, cap, emit[i]);
    }
    if (q < src_len && src[q] == ';') q++;
    *pos = q;
}

/* ---------- public API ---------- */

static void html_tok_init(struct html_tokenizer *tz,
                          const char *src, size_t n)
{
    tz->src     = src;
    tz->src_len = n;
    tz->pos     = 0;
    tz->state   = HTML_S_DATA;
    tz->ends_len = 0;
    tz->ends_with[0] = '\0';
}

/* Reset all token fields.  We zero-by-field rather than memset
 * because freestanding GCC sometimes turns memset into an out-of-line
 * call we don't have. */
static void htm_token_reset(struct html_token *t)
{
    t->type            = HTML_TOK_NONE;
    t->tag_name_len    = 0;
    t->tag_name[0]     = '\0';
    t->self_closing    = 0;
    t->n_attrs         = 0;
    t->data_len        = 0;
    /* Don't bother clearing the attrs[] array — n_attrs gates use. */
}

/* True if the upcoming bytes form the closing tag we're scanning for.
 * The HTML spec compares case-insensitively. */
static int htm_rawtext_end_at(const struct html_tokenizer *tz, size_t p)
{
    /* Match "</name" followed by a tag-terminator (>, /, whitespace,
     * or EOF).  We do NOT require the closing '>' here — the outer
     * loop will re-enter the data state and eat the rest of the tag. */
    if (p + 1 >= tz->src_len) return 0;
    if (tz->src[p] != '<' || tz->src[p + 1] != '/') return 0;
    size_t q = p + 2;
    if (q + tz->ends_len > tz->src_len) return 0;
    for (size_t i = 0; i < tz->ends_len; i++) {
        if (htm_lower(tz->src[q + i]) != tz->ends_with[i]) return 0;
    }
    size_t after = q + tz->ends_len;
    if (after >= tz->src_len) return 1;
    char c = tz->src[after];
    return (c == '>' || c == '/' || htm_is_space(c));
}

/* Parse a tag (start, end, comment, or doctype) starting at src[*pos]
 * which is the '<'.  Fills *out and advances *pos past the closing '>'.
 * Returns 1 on success.  On malformed input we recover by emitting a
 * literal '<' as text and advancing one byte. */
static int htm_parse_tag(struct html_tokenizer *tz, struct html_token *out)
{
    const char *src  = tz->src;
    size_t      n    = tz->src_len;
    size_t      p    = tz->pos;

    if (p >= n || src[p] != '<') return 0;
    if (p + 1 >= n) {
        /* Lone trailing '<' — emit as text. */
        out->type = HTML_TOK_CHARS;
        htm_putc(out->data, &out->data_len, HTML_DATA_MAX, '<');
        tz->pos = n;
        return 1;
    }

    char c = src[p + 1];

    /* Comment / doctype / CDATA-ish: '<!' */
    if (c == '!') {
        /* "<!--" comment. */
        if (p + 4 <= n && src[p+2] == '-' && src[p+3] == '-') {
            size_t q = p + 4;
            out->type = HTML_TOK_COMMENT;
            while (q + 2 < n) {
                if (src[q] == '-' && src[q+1] == '-' && src[q+2] == '>') {
                    q += 3;
                    tz->pos = q;
                    return 1;
                }
                htm_putc(out->data, &out->data_len, HTML_DATA_MAX, src[q]);
                q++;
            }
            /* Unterminated comment — consume to EOF. */
            while (q < n) {
                htm_putc(out->data, &out->data_len, HTML_DATA_MAX, src[q]);
                q++;
            }
            tz->pos = q;
            return 1;
        }
        /* "<!doctype ...>" — case-insensitive.  We only keep the
         * root-element name token (typically "html"). */
        if (p + 9 <= n &&
            (htm_lower(src[p+2]) == 'd') &&
            (htm_lower(src[p+3]) == 'o') &&
            (htm_lower(src[p+4]) == 'c') &&
            (htm_lower(src[p+5]) == 't') &&
            (htm_lower(src[p+6]) == 'y') &&
            (htm_lower(src[p+7]) == 'p') &&
            (htm_lower(src[p+8]) == 'e')) {
            size_t q = p + 9;
            while (q < n && htm_is_space(src[q])) q++;
            out->type = HTML_TOK_DOCTYPE;
            while (q < n && !htm_is_space(src[q]) && src[q] != '>') {
                htm_putc(out->data, &out->data_len, HTML_DATA_MAX,
                         htm_lower(src[q]));
                q++;
            }
            /* Skip remainder up to '>'. */
            while (q < n && src[q] != '>') q++;
            if (q < n) q++;
            tz->pos = q;
            return 1;
        }
        /* Other "<!..." forms: treat as bogus comment, swallow to '>'. */
        size_t q = p + 2;
        out->type = HTML_TOK_COMMENT;
        while (q < n && src[q] != '>') {
            htm_putc(out->data, &out->data_len, HTML_DATA_MAX, src[q]);
            q++;
        }
        if (q < n) q++;
        tz->pos = q;
        return 1;
    }

    /* End tag: "</name>" */
    if (c == '/') {
        if (p + 2 >= n || !htm_is_alpha(src[p + 2])) {
            /* "</" + non-letter — bogus, emit literal '<' and recover. */
            out->type = HTML_TOK_CHARS;
            htm_putc(out->data, &out->data_len, HTML_DATA_MAX, '<');
            tz->pos = p + 1;
            return 1;
        }
        size_t q = p + 2;
        out->type = HTML_TOK_END;
        while (q < n && !htm_is_space(src[q]) && src[q] != '>' && src[q] != '/') {
            if (out->tag_name_len < HTML_TAG_NAME_MAX - 1) {
                out->tag_name[out->tag_name_len++] = htm_lower(src[q]);
            }
            q++;
        }
        out->tag_name[out->tag_name_len] = '\0';
        /* Skip to '>'. */
        while (q < n && src[q] != '>') q++;
        if (q < n) q++;
        tz->pos = q;
        return 1;
    }

    /* Start tag: "<name attr=value ...>" or "<name .../>" */
    if (htm_is_alpha(c)) {
        size_t q = p + 1;
        out->type = HTML_TOK_START;
        while (q < n && !htm_is_space(src[q]) && src[q] != '>' &&
               src[q] != '/' && src[q] != '=') {
            if (out->tag_name_len < HTML_TAG_NAME_MAX - 1) {
                out->tag_name[out->tag_name_len++] = htm_lower(src[q]);
            }
            q++;
        }
        out->tag_name[out->tag_name_len] = '\0';

        /* Attributes loop. */
        while (q < n && src[q] != '>' && src[q] != '/') {
            while (q < n && htm_is_space(src[q])) q++;
            if (q >= n || src[q] == '>' || src[q] == '/') break;

            struct html_attr *a = NULL;
            if (out->n_attrs < HTML_MAX_ATTRS) {
                a = &out->attrs[out->n_attrs];
                a->name_len  = 0;
                a->value_len = 0;
                a->name[0]   = '\0';
                a->value[0]  = '\0';
            }

            /* Attribute name (lowercased). */
            while (q < n && !htm_is_space(src[q]) &&
                   src[q] != '=' && src[q] != '>' && src[q] != '/') {
                if (a && a->name_len < HTML_ATTR_NAME_MAX - 1) {
                    a->name[a->name_len++] = htm_lower(src[q]);
                }
                q++;
            }
            if (a) a->name[a->name_len] = '\0';

            /* Optional value. */
            while (q < n && htm_is_space(src[q])) q++;
            if (q < n && src[q] == '=') {
                q++;
                while (q < n && htm_is_space(src[q])) q++;
                if (q < n && (src[q] == '"' || src[q] == '\'')) {
                    char quote = src[q];
                    q++;
                    while (q < n && src[q] != quote) {
                        if (src[q] == '&') {
                            size_t before = a ? a->value_len : 0;
                            (void)before;
                            if (a) htm_decode_entity(src, n, &q,
                                                    a->value, &a->value_len,
                                                    HTML_ATTR_VALUE_MAX);
                            else { q++; }
                        } else {
                            if (a && a->value_len < HTML_ATTR_VALUE_MAX - 1)
                                a->value[a->value_len++] = src[q];
                            q++;
                        }
                    }
                    if (q < n) q++;          /* closing quote */
                    if (a) a->value[a->value_len] = '\0';
                } else {
                    /* Unquoted: until whitespace, '>', or '/'. */
                    while (q < n && !htm_is_space(src[q]) &&
                           src[q] != '>' && src[q] != '/') {
                        if (src[q] == '&') {
                            if (a) htm_decode_entity(src, n, &q,
                                                    a->value, &a->value_len,
                                                    HTML_ATTR_VALUE_MAX);
                            else { q++; }
                        } else {
                            if (a && a->value_len < HTML_ATTR_VALUE_MAX - 1)
                                a->value[a->value_len++] = src[q];
                            q++;
                        }
                    }
                    if (a) a->value[a->value_len] = '\0';
                }
            }

            if (a && a->name_len > 0) out->n_attrs++;
        }

        if (q < n && src[q] == '/') {
            out->self_closing = 1;
            q++;
        }
        if (q < n && src[q] == '>') q++;
        tz->pos = q;

        /* If this was a raw-text element (script/style), arm the
         * rawtext state so the next call yields the inner text as
         * a single CHARS token without trying to find tags inside. */
        if (!out->self_closing) {
            const char *tn = out->tag_name;
            int is_script = (out->tag_name_len == 6 &&
                             tn[0]=='s'&&tn[1]=='c'&&tn[2]=='r'&&
                             tn[3]=='i'&&tn[4]=='p'&&tn[5]=='t');
            int is_style  = (out->tag_name_len == 5 &&
                             tn[0]=='s'&&tn[1]=='t'&&tn[2]=='y'&&
                             tn[3]=='l'&&tn[4]=='e');
            if (is_script || is_style) {
                tz->state = HTML_S_RAWTEXT;
                if (is_script) { tz->ends_len = 6; htm_puts(tz->ends_with, &tz->ends_len, sizeof(tz->ends_with), ""); /* no-op */
                                 tz->ends_with[0]='s';tz->ends_with[1]='c';tz->ends_with[2]='r';
                                 tz->ends_with[3]='i';tz->ends_with[4]='p';tz->ends_with[5]='t';
                                 tz->ends_with[6]='\0'; tz->ends_len = 6; }
                else           { tz->ends_with[0]='s';tz->ends_with[1]='t';tz->ends_with[2]='y';
                                 tz->ends_with[3]='l';tz->ends_with[4]='e';
                                 tz->ends_with[5]='\0'; tz->ends_len = 5; }
            }
        }
        return 1;
    }

    /* Unknown lead char — treat the '<' as literal text. */
    out->type = HTML_TOK_CHARS;
    htm_putc(out->data, &out->data_len, HTML_DATA_MAX, '<');
    tz->pos = p + 1;
    return 1;
}

/* Pull the next token.  Returns:
 *    1 — out was filled with a real token
 *    0 — EOF (caller may stop)
 *   -1 — only on truly catastrophic input (currently never; we
 *        always recover at the next byte). */
static int html_tok_next(struct html_tokenizer *tz, struct html_token *out)
{
    htm_token_reset(out);

    if (tz->pos >= tz->src_len || tz->state == HTML_S_DONE) {
        out->type = HTML_TOK_EOF;
        tz->state = HTML_S_DONE;
        return 0;
    }

    if (tz->state == HTML_S_RAWTEXT) {
        /* Slurp text until we see "</ends_with" (case-insensitive). */
        out->type = HTML_TOK_CHARS;
        size_t p = tz->pos;
        while (p < tz->src_len) {
            if (tz->src[p] == '<' && htm_rawtext_end_at(tz, p)) break;
            htm_putc(out->data, &out->data_len, HTML_DATA_MAX, tz->src[p]);
            p++;
        }
        tz->pos   = p;
        tz->state = HTML_S_DATA;
        tz->ends_len = 0;
        if (out->data_len == 0) {
            /* Empty raw block — recurse to immediately yield the </tag>. */
            return html_tok_next(tz, out);
        }
        return 1;
    }

    /* Data state. */
    char c = tz->src[tz->pos];
    if (c == '<') {
        return htm_parse_tag(tz, out);
    }

    /* Coalesce a run of plain text, decoding entities. */
    out->type = HTML_TOK_CHARS;
    size_t p = tz->pos;
    while (p < tz->src_len && tz->src[p] != '<') {
        if (out->data_len >= HTML_DATA_MAX - 1) break;
        if (tz->src[p] == '&') {
            htm_decode_entity(tz->src, tz->src_len, &p,
                              out->data, &out->data_len, HTML_DATA_MAX);
        } else {
            out->data[out->data_len++] = tz->src[p];
            p++;
        }
    }
    tz->pos = p;
    return 1;
}

#endif /* USER_HTML_H */
