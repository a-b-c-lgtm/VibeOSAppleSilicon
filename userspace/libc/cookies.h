/*
 * userspace/libc/cookies.h -- chapter 120 cookie jar.
 *
 * Header-only.  Include from one .c per binary, same convention
 * as printf.h / malloc.h / url.h.  Depends on syscall.h (open /
 * read / write / fsync / mkdir / time), printf.h (snprintf for
 * the disk format), and string-ish helpers that we inline here
 * to avoid pulling in stdlib we don't have.
 *
 * Disk layout
 * -----------
 * One file per host: /data/cookies/<host>.  The directory
 * /data/cookies/ is created lazily on first write.  No
 * subdirectories or extended attributes: the host string is
 * used verbatim as the filename, with character sanitisation
 * (only [A-Za-z0-9._-] survives, anything else becomes '_').
 *
 * One cookie per line:
 *
 *     name<TAB>value<TAB>expires_epoch<TAB>path<LF>
 *
 * - `expires_epoch` is a decimal POSIX time_t (seconds since 1970).
 *   0 means "session cookie" -- we treat that as "expires never"
 *   for simplicity (no concept of browser session yet).
 * - `path` defaults to "/" if the Set-Cookie didn't say.
 *
 * The format is intentionally cat-friendly: `cat /data/cookies/host`
 * shows the jar contents in plain text.  When you replace an
 * existing cookie, the file is rewritten with the new line in
 * place of the old (append-only would grow unbounded under churn).
 *
 * What we honour from the RFC 6265 Set-Cookie grammar
 * ----------------------------------------------------
 * - `name=value`              required
 * - `Max-Age=<seconds>`       parsed; absolute expiry = now + N
 * - `Path=<path>`             stored; outgoing Cookie: filtered
 *                             so we only send if request path
 *                             starts with stored path
 * - `Expires=...`             IGNORED -- no HTTP-date parser yet
 * - `Domain=...`              IGNORED -- host-only cookies only,
 *                             which is the modern recommended
 *                             default anyway
 * - `Secure`, `HttpOnly`,
 *   `SameSite=...`            IGNORED with a one-shot log on
 *                             first observation; we don't have
 *                             TLS, no JS DOM API, and no third-
 *                             party iframes, so these are no-ops
 *                             in this OS.
 *
 * Same-Origin Policy (SOP) -- cookie side
 * ---------------------------------------
 * Cookies are keyed by exact host string.  cookie_store_get
 * for host "b.example.com" never returns cookies stored under
 * "a.example.com" or "example.com".  This is stricter than real
 * browsers (which honour Domain=.example.com) but it is correct,
 * boring, and matches our "host-only cookies only" stance above.
 *
 * Capacity
 * --------
 * 64 cookies per host max.  Anything beyond is dropped with a
 * log message; in practice a single host shouldn't be setting
 * dozens of cookies on a hobby OS.  Cookie name/value/path
 * together must fit in COOKIE_LINE_MAX = 512 bytes.
 */

#ifndef OSDEV_LIBC_COOKIES_H
#define OSDEV_LIBC_COOKIES_H

#include "syscall.h"
#include "printf.h"
#include "malloc.h"
#include "freestanding.h"   /* memcpy / memset shims -- see header */
#define COOKIE_NAME_MAX   64
#define COOKIE_VALUE_MAX  256
#define COOKIE_PATH_MAX   64
#define COOKIE_HOST_MAX   128
#define COOKIE_LINE_MAX   512
#define COOKIE_MAX_PER_HOST 64
#define COOKIE_DIR        "/data/cookies"

struct cookie_attr {
    char     name [COOKIE_NAME_MAX];
    char     value[COOKIE_VALUE_MAX];
    char     path [COOKIE_PATH_MAX];
    time_t   expires;   /* 0 = session (treated as "never expires") */
};

/* ──────────────────────── tiny string helpers ───────────────── */

static inline int ck_ieq_ch(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

/* Case-insensitive prefix match: does `s` start with literal `pfx`? */
static inline int ck_ieq_prefix(const char *s, size_t slen, const char *pfx)
{
    size_t i = 0;
    while (pfx[i]) {
        if (i >= slen) return 0;
        if (!ck_ieq_ch(s[i], pfx[i])) return 0;
        i++;
    }
    return 1;
}

static inline size_t ck_strlen(const char *s)
{
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}

static inline void ck_copy_clamped(char *dst, size_t cap,
                                   const char *src, size_t n)
{
    size_t lim = (n < cap - 1) ? n : cap - 1;
    for (size_t i = 0; i < lim; i++) dst[i] = src[i];
    dst[lim] = '\0';
}

static inline int ck_digit(char c) { return c >= '0' && c <= '9'; }

/* Parse a decimal integer up to `max_chars` long.  Returns -1 if
 * no digits or out-of-range; otherwise sets *out and returns the
 * number of digits consumed. */
static inline int ck_parse_decimal(const char *s, size_t max_chars,
                                   long long *out)
{
    long long v = 0;
    size_t i = 0;
    int neg = 0;
    if (i < max_chars && s[i] == '-') { neg = 1; i++; }
    size_t start = i;
    while (i < max_chars && ck_digit(s[i])) {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    if (i == start) return -1;
    *out = neg ? -v : v;
    return (int)i;
}

/* Sanitise a host string into a filename-safe form.  Allowed:
 * [A-Za-z0-9._-].  Everything else becomes '_'.  This means
 * "127.0.0.1" -> "127.0.0.1" (unchanged) and most DNS names
 * pass through unchanged. */
static inline void ck_sanitize_host(const char *host, char *out, size_t cap)
{
    size_t i = 0;
    for (; host[i] && i + 1 < cap; i++) {
        char c = host[i];
        int ok = (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-';
        out[i] = ok ? c : '_';
    }
    out[i] = '\0';
}

/* Build the per-host jar path: "/data/cookies/<sanitized-host>". */
static inline void ck_jar_path(const char *host, char *out, size_t cap)
{
    char safe[COOKIE_HOST_MAX];
    ck_sanitize_host(host, safe, sizeof(safe));
    (void)snprintf(out, cap, "%s/%s", COOKIE_DIR, safe);
}

/* ─────────────────────────── disk helpers ───────────────────── */

/* Open a /data/cookies/<host> file for reading.  Returns the fd
 * (>=0) or a negative errno-like value.  -2 (-ENOENT) is the
 * common case "no cookies for this host yet" and is not an error
 * worth logging. */
static inline int ck_open_read(const char *host)
{
    char path[COOKIE_HOST_MAX + 32];
    ck_jar_path(host, path, sizeof(path));
    return open(path, 0 /* O_RDONLY */);
}

/* Read the whole jar file into a freshly-malloc'd buffer.  On
 * success *out_len is set and the caller frees *out.  Returns 0
 * on success, -1 on any failure (caller treats as "empty jar"). */
static inline int ck_slurp(const char *host, char **out, size_t *out_len)
{
    *out = 0; *out_len = 0;
    int fd = ck_open_read(host);
    if (fd < 0) return -1;
    /* Cap at 64 KiB -- a single host with more than that has gone
     * rogue and we'd rather drop the tail than OOM. */
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return -1; }
    for (;;) {
        if (len == cap) {
            if (cap >= 64 * 1024) break;
            size_t ncap = cap * 2;
            char *nb = (char *)malloc(ncap);
            if (!nb) { free(buf); close(fd); return -1; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf); buf = nb; cap = ncap;
        }
        long n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    *out = buf; *out_len = len;
    return 0;
}

/* Ensure /data/cookies exists.  Tolerates "already exists"
 * (-EEXIST = -17). */
static inline void ck_ensure_dir(void)
{
    int rc = mkdir(COOKIE_DIR, 0700);
    (void)rc;  /* -17 EEXIST is the steady-state case */
}

/* ─────────────────────────── parsing ────────────────────────── */

/* Find the position of `needle` (single char) in [s, s+n), or
 * return n if not found. */
static inline size_t ck_findc(const char *s, size_t n, char needle)
{
    for (size_t i = 0; i < n; i++) if (s[i] == needle) return i;
    return n;
}

static inline void ck_skip_ws(const char *s, size_t *i, size_t n)
{
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t')) (*i)++;
}

/* Parse one Set-Cookie value (just the right-hand side -- the
 * "Set-Cookie:" name has already been stripped by the HTTP
 * parser).  `now` is the current wall-clock time; Max-Age is
 * resolved to an absolute expiry against it.  Returns 0 on
 * success, -1 on malformed (no '=' before the first ';'). */
static inline int cookie_parse_set(const char *value, size_t vlen,
                                   time_t now, struct cookie_attr *out)
{
    /* Reset to known defaults. */
    out->name[0]  = '\0';
    out->value[0] = '\0';
    out->path[0]  = '/'; out->path[1] = '\0';
    out->expires  = 0;

    size_t i = 0;
    ck_skip_ws(value, &i, vlen);

    /* First attribute is `name=value`.  No '=' before ';' = bad. */
    size_t eq = i;
    while (eq < vlen && value[eq] != '=' && value[eq] != ';') eq++;
    if (eq >= vlen || value[eq] != '=') return -1;

    /* Trim trailing whitespace from name. */
    size_t name_end = eq;
    while (name_end > i && (value[name_end - 1] == ' ' || value[name_end - 1] == '\t'))
        name_end--;
    ck_copy_clamped(out->name, sizeof(out->name),
                    value + i, name_end - i);
    if (out->name[0] == '\0') return -1;

    /* Cookie value: from after '=' to first ';' or end. */
    size_t vstart = eq + 1;
    ck_skip_ws(value, &vstart, vlen);
    size_t vend = vstart;
    while (vend < vlen && value[vend] != ';') vend++;
    /* Trim trailing whitespace. */
    while (vend > vstart && (value[vend - 1] == ' ' || value[vend - 1] == '\t'))
        vend--;
    ck_copy_clamped(out->value, sizeof(out->value),
                    value + vstart, vend - vstart);

    /* Walk remaining ';'-separated attributes. */
    i = vend;
    while (i < vlen && value[i] == ';') {
        i++;
        ck_skip_ws(value, &i, vlen);
        if (i >= vlen) break;

        /* Attribute name runs until '=' or ';' or end. */
        size_t a_start = i;
        while (i < vlen && value[i] != '=' && value[i] != ';') i++;
        size_t a_end = i;
        while (a_end > a_start && (value[a_end - 1] == ' ' || value[a_end - 1] == '\t'))
            a_end--;
        size_t a_len = a_end - a_start;

        /* Optional `= value`. */
        size_t v_start = i, v_end = i;
        if (i < vlen && value[i] == '=') {
            i++;
            ck_skip_ws(value, &i, vlen);
            v_start = i;
            while (i < vlen && value[i] != ';') i++;
            v_end = i;
            while (v_end > v_start && (value[v_end - 1] == ' ' || value[v_end - 1] == '\t'))
                v_end--;
        }
        size_t v_len = v_end - v_start;

        if (ck_ieq_prefix(value + a_start, a_len, "max-age") && a_len == 7) {
            long long secs = 0;
            if (ck_parse_decimal(value + v_start, v_len, &secs) > 0) {
                if (secs <= 0) {
                    /* Negative / zero Max-Age means "expire now" --
                     * record a past-time so the next call to
                     * cookie_store_get drops it. */
                    out->expires = 1;
                } else {
                    out->expires = (time_t)((long long)now + secs);
                }
            }
        } else if (ck_ieq_prefix(value + a_start, a_len, "path") && a_len == 4) {
            if (v_len > 0 && value[v_start] == '/') {
                ck_copy_clamped(out->path, sizeof(out->path),
                                value + v_start, v_len);
            }
        }
        /* Domain / Expires / Secure / HttpOnly / SameSite: silently
         * ignored.  See the header banner for why. */
    }
    return 0;
}

/* ─────────────────────────── on-disk merge ──────────────────── */

/* Walk `buf` line by line, populating `out[]` with up to `cap`
 * cookies (silently dropping the rest).  Each line is the format
 * "name\tvalue\texpires\tpath\n".  Malformed lines are skipped.
 * Returns the number of cookies parsed. */
static inline int ck_load_lines(const char *buf, size_t blen,
                                 struct cookie_attr *out, int cap)
{
    int n = 0;
    size_t i = 0;
    while (i < blen && n < cap) {
        size_t le = ck_findc(buf + i, blen - i, '\n') + i;
        size_t fields[4];   /* tab positions, then end */
        int nf = 0;
        size_t j = i;
        while (j < le && nf < 3) {
            if (buf[j] == '\t') fields[nf++] = j;
            j++;
        }
        if (nf == 3) {
            fields[3] = le;
            struct cookie_attr *c = &out[n];
            ck_copy_clamped(c->name,  sizeof(c->name),
                            buf + i,            fields[0] - i);
            ck_copy_clamped(c->value, sizeof(c->value),
                            buf + fields[0] + 1, fields[1] - fields[0] - 1);
            long long exp = 0;
            (void)ck_parse_decimal(buf + fields[1] + 1,
                                   fields[2] - fields[1] - 1, &exp);
            c->expires = (time_t)exp;
            ck_copy_clamped(c->path, sizeof(c->path),
                            buf + fields[2] + 1, fields[3] - fields[2] - 1);
            if (c->path[0] == '\0') { c->path[0] = '/'; c->path[1] = '\0'; }
            if (c->name[0] != '\0') n++;
        }
        i = le + 1;
    }
    return n;
}

/* Rewrite /data/cookies/<host> from an in-memory cookie array.
 * Returns 0 / -1. */
static inline int ck_write_jar(const char *host,
                                const struct cookie_attr *arr, int n)
{
    ck_ensure_dir();
    char path[COOKIE_HOST_MAX + 32];
    ck_jar_path(host, path, sizeof(path));

    int fd = open(path, 577 /* O_WRONLY|O_CREAT|O_TRUNC */);
    if (fd < 0) return -1;

    char line[COOKIE_LINE_MAX];
    for (int k = 0; k < n; k++) {
        const struct cookie_attr *c = &arr[k];
        if (c->name[0] == '\0') continue;
        int len = snprintf(line, sizeof(line),
                           "%s\t%s\t%lld\t%s\n",
                           c->name, c->value,
                           (long long)c->expires,
                           c->path[0] ? c->path : "/");
        if (len <= 0) continue;
        if (len > (int)sizeof(line)) len = (int)sizeof(line);
        (void)write(fd, line, (size_t)len);
    }
    (void)fsync(fd);
    close(fd);
    return 0;
}

/* ─────────────────────────── public API ─────────────────────── */

/* Merge `c` into the on-disk jar for `host`.  If a cookie with
 * the same (name, path) already exists, it is replaced.  Returns
 * 0 / -1. */
static inline int cookie_store_set(const char *host,
                                    const struct cookie_attr *c)
{
    if (!host || !c || !c->name[0]) return -1;

    struct cookie_attr jar[COOKIE_MAX_PER_HOST];
    int n = 0;

    char *raw = 0; size_t raw_len = 0;
    if (ck_slurp(host, &raw, &raw_len) == 0 && raw) {
        n = ck_load_lines(raw, raw_len, jar, COOKIE_MAX_PER_HOST);
        free(raw);
    }

    /* Find an existing slot with the same name+path, else append. */
    int slot = -1;
    for (int i = 0; i < n; i++) {
        if (jar[i].name[0] &&
            ck_strlen(jar[i].name) == ck_strlen(c->name) &&
            ck_ieq_prefix(jar[i].name, ck_strlen(c->name), c->name) &&
            ck_strlen(jar[i].path) == ck_strlen(c->path) &&
            ck_ieq_prefix(jar[i].path, ck_strlen(c->path), c->path)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (n >= COOKIE_MAX_PER_HOST) {
            printf("[cookies] %s: full (drop %s)\n", host, c->name);
            return -1;
        }
        slot = n++;
    }
    /* Explicit field copy -- struct assignment compiles to an
     * implicit memcpy() call for arrays this size.  Even with the
     * fallback memcpy() above, hand-copying makes intent visible
     * and saves a noinline indirection on a hot path. */
    {
        struct cookie_attr *d = &jar[slot];
        for (size_t i = 0; i < sizeof(d->name);  i++) d->name[i]  = c->name[i];
        for (size_t i = 0; i < sizeof(d->value); i++) d->value[i] = c->value[i];
        for (size_t i = 0; i < sizeof(d->path);  i++) d->path[i]  = c->path[i];
        d->expires = c->expires;
    }

    return ck_write_jar(host, jar, n);
}

/* Build the value for an outgoing "Cookie:" header for the given
 * (host, path) at time `now`.  Cookies whose path is not a prefix
 * of `path`, or whose expiry is in the past, are skipped.  Writes
 * into `out` (NUL-terminated).  out[0] is set to '\0' if no
 * cookies apply.  Returns the number of cookies emitted, or -1
 * on buffer overflow. */
static inline int cookie_store_get(const char *host, const char *path,
                                    time_t now, char *out, size_t cap)
{
    if (!host || !path || !out || cap == 0) return -1;
    out[0] = '\0';

    char *raw = 0; size_t raw_len = 0;
    if (ck_slurp(host, &raw, &raw_len) < 0 || !raw) return 0;

    struct cookie_attr jar[COOKIE_MAX_PER_HOST];
    int n = ck_load_lines(raw, raw_len, jar, COOKIE_MAX_PER_HOST);
    free(raw);

    size_t off = 0;
    int emitted = 0;
    size_t path_len = ck_strlen(path);

    for (int i = 0; i < n; i++) {
        const struct cookie_attr *c = &jar[i];
        if (!c->name[0]) continue;
        if (c->expires != 0 && c->expires <= now) continue;

        /* Path prefix match.  Stored "/" matches any request
         * path; stored "/foo" matches "/foo" and "/foo/bar". */
        size_t cplen = ck_strlen(c->path);
        if (cplen > path_len) continue;
        int match = 1;
        for (size_t k = 0; k < cplen; k++) {
            if (c->path[k] != path[k]) { match = 0; break; }
        }
        if (!match) continue;
        /* Boundary: "/foo" must not match "/foobar".  Stored
         * "/foo" matches "/foo" or "/foo/...". */
        if (cplen > 0 && c->path[cplen - 1] != '/' &&
            path_len > cplen && path[cplen] != '/') continue;

        int wrote = snprintf(out + off, cap - off,
                             emitted ? "; %s=%s" : "%s=%s",
                             c->name, c->value);
        if (wrote < 0 || (size_t)wrote >= cap - off) {
            out[off] = '\0';
            return -1;
        }
        off += (size_t)wrote;
        emitted++;
    }
    return emitted;
}

#endif /* OSDEV_LIBC_COOKIES_H */
