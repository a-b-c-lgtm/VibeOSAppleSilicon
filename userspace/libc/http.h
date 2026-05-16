/*
 * userspace/libc/http.h — header-only HTTP/1.1 response parser.
 *
 * Single-translation-unit convention.  No allocation; the parser
 * works in-place on a caller-owned buffer.  Designed for the
 * "drain to EOF, then parse" pattern that httpget uses today.
 *
 * Capabilities:
 *  - Parse the status line ("HTTP/1.1 200 OK\r\n").
 *  - Iterate name/value header pairs, case-insensitive lookup.
 *  - Decode the body for the three common framings:
 *      * Content-Length: N        — body is exactly N bytes
 *      * Transfer-Encoding: chunked — decoded in place
 *      * Connection: close (no length, no chunked) — body runs
 *        from end-of-headers to end-of-buffer (caller drained
 *        to EOF, so this is correct)
 *  - Surface the most common headers we need now: Location (for
 *    redirects), Content-Type, Content-Length.
 *
 * What it does NOT do:
 *  - HTTP/0.9, HTTP/2, HTTP/3.
 *  - Multi-line (folded) header values — RFC 7230 deprecated.
 *  - Compressed bodies (gzip, deflate, br).  The browser will
 *    advertise no Accept-Encoding, so most servers send identity.
 *  - Trailers after a chunked body (we ignore them).
 *  - Validate UTF-8 / charset; the caller deals with that.
 *
 * Buffer ownership: the caller passes a writable buffer that holds
 * the entire raw response.  On success, http_parse() leaves headers
 * intact (so further header lookups still work) and the decoded
 * body is a (ptr, len) slice into that same buffer.  For chunked
 * responses the slice points at the *first* chunk's payload and
 * subsequent chunks have been compacted to be contiguous with it.
 */
#ifndef USER_HTTP_H
#define USER_HTTP_H

#include <stdint.h>
#include <stddef.h>

#define HTTP_HEADERS_MAX  64

struct http_header {
    /* Pointers into the caller's buffer.  Both ranges are NOT
     * NUL-terminated; use len for bounds.  The header parser
     * writes ASCII lowercase versions of the header names back
     * into the buffer (in-place) so case-insensitive lookups
     * are a plain memcmp. */
    const char *name;   size_t name_len;
    const char *value;  size_t value_len;
};

struct http_response {
    int          status;            /* e.g. 200, 302, 404         */
    int          minor_version;     /* 0 or 1 (HTTP/1.x)          */
    const char  *reason;            /* "OK", "Not Found", etc.    */
    size_t       reason_len;
    struct http_header  headers[HTTP_HEADERS_MAX];
    size_t       header_count;
    /* Body slice, decoded if needed.  May be empty (len == 0). */
    char        *body;
    size_t       body_len;
};

/* Lower one ASCII byte. */
static unsigned char http_tolower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

/* Case-insensitive byte-range compare.  All bytes in `a` are
 * compared as already-lowercased; `b` is lowered on the fly.
 * The header parser pre-lowers names so this is correct. */
static int http_name_eq(const char *a, size_t alen, const char *b)
{
    size_t blen = 0; while (b[blen]) blen++;
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        if ((unsigned char)a[i] != http_tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* Look up a header by case-insensitive name.  Returns a pointer
 * to the value range (not NUL-terminated) and writes the length
 * to *out_len, or NULL if absent. */
static const char *http_get_header(const struct http_response *r,
                                   const char *name, size_t *out_len)
{
    for (size_t i = 0; i < r->header_count; i++) {
        if (http_name_eq(r->headers[i].name, r->headers[i].name_len, name)) {
            if (out_len) *out_len = r->headers[i].value_len;
            return r->headers[i].value;
        }
    }
    if (out_len) *out_len = 0;
    return NULL;
}

/* Tiny dec parser for Content-Length.  Returns -1 on overflow or
 * non-digit; otherwise the unsigned value. */
static long http_parse_size_dec(const char *s, size_t n)
{
    if (n == 0) return -1;
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        unsigned long nv = v * 10u + (unsigned long)(s[i] - '0');
        if (nv < v) return -1;       /* overflow */
        v = nv;
    }
    return (long)v;
}

/* Tiny hex parser for chunk sizes.  Stops at the first non-hex
 * char (typically ';' for chunk-extensions or '\r' / '\n').
 * Returns -1 if no hex digits at all. */
static long http_parse_size_hex(const char *s, size_t n, size_t *consumed)
{
    unsigned long v = 0;
    size_t i = 0;
    int seen = 0;
    while (i < n) {
        char c = s[i];
        unsigned d;
        if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        unsigned long nv = (v << 4) | d;
        if (nv < v) return -1;
        v = nv;
        i++; seen = 1;
    }
    if (!seen) return -1;
    if (consumed) *consumed = i;
    return (long)v;
}

/* In-place chunked decode.  `body` points at the first byte after
 * the headers; `n` is the number of bytes available there
 * (i.e. total - header_block_size).  On success, writes the
 * decoded length to *out_len and returns 0.  The decoded bytes
 * occupy [body, body + *out_len) and the original chunk frames
 * are clobbered.  Returns -1 on malformed input. */
static int http_decode_chunked(char *body, size_t n, size_t *out_len)
{
    size_t r = 0;     /* read cursor  */
    size_t w = 0;     /* write cursor (always <= r) */
    while (r < n) {
        size_t consumed = 0;
        long sz = http_parse_size_hex(body + r, n - r, &consumed);
        if (sz < 0) return -1;
        r += consumed;
        /* Skip any chunk-extension up to CRLF. */
        while (r < n && body[r] != '\n') r++;
        if (r >= n) return -1;
        r++;  /* past '\n' */
        if (sz == 0) {
            /* Last-chunk; trailers (if any) follow until CRLF. */
            *out_len = w;
            return 0;
        }
        if (r + (size_t)sz > n) return -1;
        /* Copy chunk bytes down. */
        for (long i = 0; i < sz; i++) body[w++] = body[r++];
        /* Skip the trailing CRLF after each chunk. */
        if (r + 2 > n) return -1;
        if (body[r] != '\r' || body[r + 1] != '\n') return -1;
        r += 2;
    }
    /* Ran out of bytes without seeing the zero-chunk terminator.
     * If the server closed the connection mid-stream we treat the
     * decoded prefix as valid — strict mode would return -1 here. */
    *out_len = w;
    return 0;
}

/* Find "\r\n\r\n" in [buf, buf+n).  Returns offset of the first
 * byte AFTER the blank line, or 0 if not found.  We also accept
 * bare-LF separators ("\n\n"), some primitive servers emit them.
 *
 * Currently exposed for callers that want to detect "headers
 * complete" before draining the whole body (e.g. a streaming
 * variant we may add later).  http_parse() does its own walk
 * and doesn't depend on this. */
__attribute__((unused))
static size_t http_find_eoh(const char *buf, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (buf[i] == '\n' && buf[i+1] == '\n') return i + 2;
    }
    return 0;
}

/* Parse a complete (drained-to-EOF) HTTP/1.x response in `buf`.
 * Returns 0 on success, -1 on malformed input.  On success, *out
 * points its name/value/body slices into `buf` — buf must outlive
 * any use of *out. */
static int http_parse(char *buf, size_t n, struct http_response *out)
{
    if (!buf || !out || n < 12) return -1;

    out->header_count = 0;
    out->body = NULL; out->body_len = 0;

    /* Status line: "HTTP/1.x SSS reason\r\n" */
    if (buf[0] != 'H' || buf[1] != 'T' || buf[2] != 'T' || buf[3] != 'P' ||
        buf[4] != '/' || buf[5] != '1' || buf[6] != '.')
        return -1;
    if (buf[7] != '0' && buf[7] != '1') return -1;
    out->minor_version = buf[7] - '0';
    if (buf[8] != ' ') return -1;
    if (buf[9] < '0' || buf[9] > '9' ||
        buf[10] < '0' || buf[10] > '9' ||
        buf[11] < '0' || buf[11] > '9') return -1;
    out->status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');

    size_t i = 12;
    if (i < n && buf[i] == ' ') i++;
    out->reason = buf + i;
    while (i < n && buf[i] != '\r' && buf[i] != '\n') i++;
    out->reason_len = (size_t)((buf + i) - out->reason);
    if (i >= n) return -1;
    /* skip CRLF or LF */
    if (buf[i] == '\r') { i++; if (i >= n || buf[i] != '\n') return -1; }
    i++;

    /* Headers, until blank line. */
    while (i < n) {
        if (buf[i] == '\r') { if (i + 1 < n && buf[i+1] == '\n') { i += 2; break; } return -1; }
        if (buf[i] == '\n') { i++; break; }

        struct http_header *h;
        if (out->header_count >= HTTP_HEADERS_MAX) return -1;
        h = &out->headers[out->header_count++];
        h->name = buf + i;
        size_t nstart = i;
        while (i < n && buf[i] != ':' && buf[i] != '\r' && buf[i] != '\n') {
            /* lower-case the name in place for cheap lookup later */
            unsigned char c = (unsigned char)buf[i];
            buf[i] = (char)http_tolower(c);
            i++;
        }
        if (i >= n || buf[i] != ':') return -1;
        h->name_len = i - nstart;
        i++;  /* past ':' */
        /* Skip optional whitespace before value. */
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        h->value = buf + i;
        size_t vstart = i;
        while (i < n && buf[i] != '\r' && buf[i] != '\n') i++;
        h->value_len = i - vstart;
        /* Trim trailing whitespace from value. */
        while (h->value_len > 0 &&
               (h->value[h->value_len - 1] == ' ' ||
                h->value[h->value_len - 1] == '\t'))
            h->value_len--;
        if (i >= n) return -1;
        if (buf[i] == '\r') { i++; if (i >= n || buf[i] != '\n') return -1; }
        i++;
    }

    /* Body framing decision. */
    size_t te_len = 0;
    const char *te = http_get_header(out, "transfer-encoding", &te_len);
    /* "chunked" must be the FINAL coding per RFC; we accept it
     * if the value contains the substring (case-insensitive). */
    int chunked = 0;
    if (te) {
        for (size_t k = 0; k + 7 <= te_len; k++) {
            char a[7];
            for (int j = 0; j < 7; j++) a[j] = (char)http_tolower((unsigned char)te[k+j]);
            if (a[0]=='c'&&a[1]=='h'&&a[2]=='u'&&a[3]=='n'&&a[4]=='k'&&a[5]=='e'&&a[6]=='d') {
                chunked = 1; break;
            }
        }
    }

    char *body_start = buf + i;
    size_t body_avail = n - i;

    if (chunked) {
        size_t decoded = 0;
        if (http_decode_chunked(body_start, body_avail, &decoded) < 0) return -1;
        out->body = body_start;
        out->body_len = decoded;
    } else {
        size_t cl_len = 0;
        const char *cl = http_get_header(out, "content-length", &cl_len);
        if (cl) {
            long v = http_parse_size_dec(cl, cl_len);
            if (v < 0) return -1;
            out->body_len = ((size_t)v < body_avail) ? (size_t)v : body_avail;
        } else {
            /* Connection: close framing — caller drained to EOF. */
            out->body_len = body_avail;
        }
        out->body = body_start;
    }
    return 0;
}

#endif /* USER_HTTP_H */
