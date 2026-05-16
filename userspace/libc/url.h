/*
 * userspace/libc/url.h — header-only URL parser for our user libc.
 *
 * Single-translation-unit convention (matches printf.h, malloc.h):
 * include from one .c per binary.  No allocation; the parser
 * splits the input string into ranges and either copies each
 * field into caller-provided fixed-size buffers, or returns
 * pointer/length pairs into the original string.  Caller-buffer
 * mode is the friendly default.
 *
 * Supported scheme set: http, https.  Anything else is rejected
 * with -1; we don't try to be a general RFC-3986 parser.  We
 * accept absolute URLs only:
 *
 *     scheme "://" host [":" port] [path] [ "?" query ] [ "#" frag ]
 *
 * The path always starts with '/' in the parsed form: if the
 * input had no path component, we synthesise "/".  Query and
 * fragment, if present, are appended to path verbatim — this
 * matches what an HTTP/1.1 request-line wants (the browser's
 * URL bar shows the fragment separately, but the on-the-wire
 * GET line is path+query).  We expose `frag` separately so the
 * browser can scroll to an anchor without re-parsing.
 *
 * Hostname rules:
 *  - bracketed IPv6 ("[::1]") is rejected (no IPv6 in our stack).
 *  - IDN/punycode is not handled (caller pre-encodes).
 *  - userinfo ("user:pass@host") is rejected.  No need yet.
 *  - max hostname length: 253 octets (matches DNS).
 *
 * All buffers are NUL-terminated on success.  Returns 0 on
 * success, -1 on any parse failure (no errno distinction; the
 * caller usually only needs to fall back to a literal IP).
 */
#ifndef USER_URL_H
#define USER_URL_H

#include <stdint.h>
#include <stddef.h>

#define URL_HOST_MAX  256
#define URL_PATH_MAX  1024
#define URL_FRAG_MAX  256

struct url {
    /* Parsed scheme: 0 = http, 1 = https.  We surface a tiny
     * enum-shaped int so callers can `switch` on it without
     * stringly-typed comparisons. */
    int      scheme;
    /* Resolved port: filled with the scheme default if the URL
     * omitted ":port" (80 for http, 443 for https). */
    uint16_t port;
    /* Hostname (DNS name or dotted-quad), NUL-terminated.        */
    char     host[URL_HOST_MAX];
    /* Path + query, joined with '?' if present, NUL-terminated.
     * Always begins with '/'; never empty.  This is what we put
     * after "GET " in the request-line. */
    char     path[URL_PATH_MAX];
    /* Fragment without the leading '#'; empty string if absent. */
    char     frag[URL_FRAG_MAX];
};

#define URL_SCHEME_HTTP   0
#define URL_SCHEME_HTTPS  1

/* Lower-case ASCII compare; returns 0 if equal, !=0 otherwise.
 * Stops at the shorter of (NUL, n).  Used for scheme matching
 * which is case-insensitive per RFC. */
static int url_strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 1;
        if (ca == 0)  return 0;
    }
    return 0;
}

/* Copy at most cap-1 bytes from [src, src+len) into dst, then NUL.
 * Returns 0 on success, -1 if it would have truncated.  We treat
 * "would truncate" as a hard error: a path longer than URL_PATH_MAX
 * usually means we're being handed something malicious, and the
 * browser would reject it anyway. */
static int url_copy_n(char *dst, size_t cap, const char *src, size_t len)
{
    if (len + 1 > cap) return -1;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
    return 0;
}

/* Parse `s` (NUL-terminated absolute URL) into *out.  Returns 0
 * on success, -1 on any failure.  *out is left in an unspecified
 * state on failure; don't read it. */
static int url_parse(const char *s, struct url *out)
{
    if (!s || !out) return -1;

    /* ── scheme ── */
    int scheme = -1;
    size_t scheme_len = 0;
    if (url_strncasecmp(s, "http://", 7) == 0) {
        scheme     = URL_SCHEME_HTTP;
        scheme_len = 7;
        out->port  = 80;
    } else if (url_strncasecmp(s, "https://", 8) == 0) {
        scheme     = URL_SCHEME_HTTPS;
        scheme_len = 8;
        out->port  = 443;
    } else {
        return -1;
    }
    out->scheme = scheme;

    const char *p = s + scheme_len;

    /* ── host (and optional :port) ──
     * Reject userinfo ('@') and IPv6 brackets ('['). */
    if (*p == '\0' || *p == '/' || *p == '?' || *p == '#') return -1;
    if (*p == '[' || *p == '@') return -1;

    const char *host_start = p;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') {
        if (*p == '@' || *p == '[') return -1;
        p++;
    }
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len > 253) return -1;
    if (url_copy_n(out->host, sizeof(out->host), host_start, host_len) < 0)
        return -1;

    /* ── optional :port ── */
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            if (v > 65535u) return -1;
            p++;
        }
        if (v == 0) return -1;
        out->port = (uint16_t)v;
    }

    /* ── path + query ──
     * If the URL omits the path, synthesise "/".  Otherwise copy
     * verbatim from the first '/' up to '#' (or end of string).
     * Query string is included; fragment is split off. */
    const char *path_start;
    size_t      path_len;
    if (*p != '/' && *p != '?' && *p != '#' && *p != '\0') {
        /* shouldn't happen — host loop should have consumed it */
        return -1;
    }
    if (*p == '/') {
        path_start = p;
    } else {
        path_start = "/";   /* synthesised */
    }
    /* Walk path+query until '#' or end. */
    const char *q = (path_start == p) ? p : p;  /* clarity */
    if (path_start != p) {
        /* synthesised "/" — path_len = 1, then maybe append query */
        if (*p == '?') {
            /* "?xxx" with no leading path: emit "/?xxx" */
            const char *frag = p;
            while (*frag && *frag != '#') frag++;
            size_t qlen = (size_t)(frag - p);
            if (1 + qlen + 1 > sizeof(out->path)) return -1;
            out->path[0] = '/';
            for (size_t i = 0; i < qlen; i++) out->path[1 + i] = p[i];
            out->path[1 + qlen] = '\0';
            p = frag;
        } else {
            out->path[0] = '/';
            out->path[1] = '\0';
        }
    } else {
        const char *end = q;
        while (*end && *end != '#') end++;
        path_len = (size_t)(end - q);
        if (url_copy_n(out->path, sizeof(out->path), q, path_len) < 0)
            return -1;
        p = end;
    }

    /* ── optional fragment ── */
    out->frag[0] = '\0';
    if (*p == '#') {
        p++;
        const char *fend = p;
        while (*fend) fend++;
        size_t flen = (size_t)(fend - p);
        if (url_copy_n(out->frag, sizeof(out->frag), p, flen) < 0)
            return -1;
    }

    return 0;
}

/* Convenience: tag the parsed URL as needing TLS.  Returns 1 if
 * the scheme is HTTPS, 0 for HTTP.  Provided so callers don't
 * have to memorise the URL_SCHEME_* enum values. */
static int url_is_tls(const struct url *u) { return u->scheme == URL_SCHEME_HTTPS; }

#endif /* USER_URL_H */
