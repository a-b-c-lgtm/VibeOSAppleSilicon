/*
 * userspace/httpget/httpget.c -- HTTP/1.x fetch client.
 *
 * Two invocation forms:
 *
 *   httpget <url>
 *       URL form (M58).  <url> is parsed by url_parse(); the
 *       response is parsed by http_parse() so we can print a
 *       structured summary, follow one Location: redirect, and
 *       handle Content-Length and Transfer-Encoding: chunked.
 *
 *   httpget <host-or-ip> <port> [path]
 *       Legacy form (M56-M57).  Still supported because the
 *       existing socket tests grep raw response bytes.
 *
 * No HTTPS yet -- TLS is parked for a later milestone.  An
 * https:// URL prints a clear error and exits non-zero.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/url.h"
#include "../libc/http.h"

static int parse_dotted(const char *s, uint32_t *out_be)
{
    uint32_t parts[4] = {0,0,0,0};
    int idx = 0;
    int seen_digit = 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            parts[idx] = parts[idx] * 10u + (uint32_t)(*s - '0');
            if (parts[idx] > 255u) return -1;
            seen_digit = 1;
        } else if (*s == '.') {
            if (!seen_digit) return -1;
            idx++;
            seen_digit = 0;
        } else {
            return -1;
        }
        s++;
    }
    if (idx != 3 || !seen_digit) return -1;
    *out_be = (parts[0] << 24) | (parts[1] << 16) |
              (parts[2] <<  8) |  parts[3];
    return 0;
}

static int parse_uint(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int seen = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 65535u) return -1;
        seen = 1;
        s++;
    }
    if (!seen) return -1;
    *out = v;
    return 0;
}

/* Print a length-bounded slice (our printf has no '%.*s'). */
static void print_slice(const char *s, size_t n)
{
    write(1, s, (unsigned long)n);
}

/* Drain a connection into a malloc'd buffer that grows on demand.
 * Returns the buffer (caller frees) and writes the byte count to
 * *out_len.  On error returns 0 with an error message printed. */
static char *drain_to_buf(int fd, size_t *out_len)
{
    size_t cap = 8 * 1024;
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("httpget: out of memory\n"); return 0; }
    char  scratch[1024];
    for (;;) {
        long r = read(fd, scratch, sizeof(scratch));
        if (r < 0) { printf("httpget: read error %ld\n", r); free(buf); return 0; }
        if (r == 0) break;
        if (len + (size_t)r > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)r) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) { printf("httpget: oom at %lu\n", (unsigned long)newcap); free(buf); return 0; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf); buf = nb; cap = newcap;
        }
        for (long i = 0; i < r; i++) buf[len + (size_t)i] = scratch[i];
        len += (size_t)r;
    }
    *out_len = len;
    return buf;
}

/* ---- URL form (M58) ---- */

/* Print a NUL-terminated literal via raw write().  We use this for
 * fixed banners that do not need any %-formatting — sidesteps any
 * stack pressure printf might add to a function that's already
 * holding several large heap pointers and a struct url. */
static void write_cstr(const char *s)
{
    size_t n = 0; while (s[n]) n++;
    if (n) write(1, s, (unsigned long)n);
}

/* Single-shot HTTP fetch.  Internal helper; fetch_url is the
 * public entry point that adds a one-hop redirect chase on top.
 *
 * Returns the HTTP status code on success, or a negative error.
 * On a 3xx response with a captured Location header, *out_redirect
 * is set to a malloc'd absolute URL the caller should fetch next.
 * Otherwise *out_redirect is left NULL.
 *
 * fetch_one keeps its own stack frame small by malloc-ing every
 * large local: the request buffer (~1.4 KiB), the response struct
 * (~2 KiB headers table), and the captured redirect target (~1.3
 * KiB).  Combined with the iterative redirect loop in fetch_url
 * this caps the userspace stack footprint of a fetch at well
 * under one page even when pages contain a few hundred bytes of
 * URL/host data. */
static int fetch_one(const char *raw_url, char **out_redirect)
{
    *out_redirect = NULL;

    struct url u;
    if (url_parse(raw_url, &u) < 0) {
        printf("httpget: cannot parse URL '%s'\n", raw_url);
        return -1;
    }
    if (url_is_tls(&u)) {
        printf("httpget: https:// is not yet supported in this build.\n");
        printf("        TLS is parked behind a future milestone.\n");
        return -1;
    }

    /* Resolve. */
    uint32_t ip_be = 0;
    if (parse_dotted(u.host, &ip_be) < 0) {
        int rc = resolve(u.host, &ip_be);
        if (rc < 0) {
            printf("httpget: cannot resolve '%s' (%d)\n", u.host, rc);
            return -1;
        }
        printf("httpget: resolved %s -> %u.%u.%u.%u\n", u.host,
               (unsigned)((ip_be >> 24) & 0xFF),
               (unsigned)((ip_be >> 16) & 0xFF),
               (unsigned)((ip_be >>  8) & 0xFF),
               (unsigned)( ip_be        & 0xFF));
    }
    int fd = socket_connect(ip_be, u.port);
    if (fd < 0) {
        printf("httpget: connect to %s:%u failed (%d)\n", u.host, u.port, fd);
        return -1;
    }

    /* Build HTTP/1.1 request in a heap buffer. */
    size_t req_cap = URL_PATH_MAX + URL_HOST_MAX + 128;
    char  *req     = (char *)malloc(req_cap);
    if (!req) { printf("httpget: oom (req)\n"); close(fd); return -1; }
    int n = 0;
    n += snprintf(req + n, req_cap - (size_t)n,
                  "GET %s HTTP/1.1\r\n", u.path);
    if (u.port == 80)
        n += snprintf(req + n, req_cap - (size_t)n,
                      "Host: %s\r\n", u.host);
    else
        n += snprintf(req + n, req_cap - (size_t)n,
                      "Host: %s:%u\r\n", u.host, u.port);
    n += snprintf(req + n, req_cap - (size_t)n,
                  "User-Agent: hobbyos-httpget/1.0\r\n"
                  "Accept: */*\r\n"
                  "Connection: close\r\n\r\n");
    long wr = write(fd, req, (unsigned long)n);
    free(req);
    if (wr < 0) {
        printf("httpget: write failed\n"); close(fd); return -1;
    }

    /* Drain everything, then parse. */
    size_t total = 0;
    char  *buf   = drain_to_buf(fd, &total);
    close(fd);
    if (!buf) return -1;

    struct http_response *resp =
        (struct http_response *)malloc(sizeof(*resp));
    if (!resp) {
        printf("httpget: oom (resp)\n"); free(buf); return -1;
    }
    if (http_parse(buf, total, resp) < 0) {
        printf("httpget: malformed HTTP response (%lu raw bytes)\n",
               (unsigned long)total);
        free(resp); free(buf); return -1;
    }

    /* Structured summary. */
    size_t ct_len = 0;
    const char *ct = http_get_header(resp, "content-type", &ct_len);
    printf("[httpget] HTTP/1.%d %d ", resp->minor_version, resp->status);
    print_slice(resp->reason, resp->reason_len);
    write(1, " (", 2);
    if (ct) print_slice(ct, ct_len); else write_cstr("no content-type");
    printf(", body=%lu bytes)\n", (unsigned long)resp->body_len);

    /* Capture Location header (if any) on the heap. */
    int status = resp->status;
    if (status >= 300 && status < 400) {
        size_t loc_len = 0;
        const char *loc = http_get_header(resp, "location", &loc_len);
        if (loc && loc_len > 0 && loc_len < URL_HOST_MAX + URL_PATH_MAX) {
            char *next = (char *)malloc(loc_len + 1);
            if (next) {
                for (size_t i = 0; i < loc_len; i++) next[i] = loc[i];
                next[loc_len] = '\0';
                *out_redirect = next;
            }
        }
    }

    /* Body. */
    if (resp->body_len > 0) print_slice(resp->body, resp->body_len);
    if (resp->body_len > 0 && resp->body[resp->body_len - 1] != '\n')
        write(1, "\n", 1);

    free(resp);
    free(buf);
    return status;
}

/* Public URL-form entry point.  Fetches the given URL and follows
 * up to one HTTP 3xx redirect.  Iterative — no recursion — so the
 * userspace stack stays shallow even on chained redirects. */
static int fetch_url(const char *raw_url)
{
    char *owned = NULL;          /* malloc'd copy of raw_url across hops */
    const char *target = raw_url;
    int status = -1;

    for (int hop = 0; hop < 2; hop++) {
        char *next = NULL;
        status = fetch_one(target, &next);

        /* Free the previous hop's URL copy before we overwrite it. */
        if (owned) { free(owned); owned = NULL; }

        if (!next) break;

        printf("[httpget] following redirect -> %s\n", next);

        owned  = next;
        target = next;
    }

    if (owned) free(owned);
    return status;
}

/* ---- Legacy 3-arg form (kept verbatim for M56/M57 tests) ---- */

static int run_legacy(const char *host, const char *port_s, const char *path)
{
    uint32_t ip_be = 0, port = 0;
    if (parse_dotted(host, &ip_be) < 0) {
        int rc = resolve(host, &ip_be);
        if (rc < 0) {
            printf("httpget: cannot resolve '%s' (%d)\n", host, rc);
            return 1;
        }
        printf("httpget: resolved %s -> %u.%u.%u.%u\n", host,
               (unsigned)((ip_be >> 24) & 0xFF),
               (unsigned)((ip_be >> 16) & 0xFF),
               (unsigned)((ip_be >>  8) & 0xFF),
               (unsigned)( ip_be        & 0xFF));
    }
    if (parse_uint(port_s, &port) < 0 || port == 0) {
        printf("httpget: invalid port '%s'\n", port_s);
        return 1;
    }

    int fd = socket_connect(ip_be, (uint16_t)port);
    if (fd < 0) {
        printf("httpget: connect failed (%d)\n", fd);
        return 2;
    }

    char req[256];
    int n = 0;
    const char *prefix = "GET ";
    while (*prefix && n < (int)sizeof(req)) req[n++] = *prefix++;
    const char *p = path;
    while (*p && n < (int)sizeof(req)) req[n++] = *p++;
    const char *suffix = " HTTP/1.0\r\nHost: ";
    while (*suffix && n < (int)sizeof(req)) req[n++] = *suffix++;
    const char *h = host;
    while (*h && n < (int)sizeof(req)) req[n++] = *h++;
    const char *tail = "\r\nConnection: close\r\n\r\n";
    while (*tail && n < (int)sizeof(req)) req[n++] = *tail++;
    if (write(fd, req, (unsigned long)n) < 0) {
        printf("httpget: write failed\n"); close(fd); return 3;
    }

    unsigned long total = 0;
    char buf[512];
    for (;;) {
        long r = read(fd, buf, sizeof(buf));
        if (r < 0) { printf("httpget: read error %ld\n", r); close(fd); return 4; }
        if (r == 0) break;
        write(1, buf, (unsigned long)r);
        total += (unsigned long)r;
    }
    close(fd);
    printf("\n[httpget] received %lu bytes\n", total);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        int s = fetch_url(argv[1]);
        return (s < 0) ? 2 : 0;
    }
    if (argc >= 3) {
        const char *path = (argc >= 4) ? argv[3] : "/";
        return run_legacy(argv[1], argv[2], path);
    }
    printf("usage:\n");
    printf("  httpget <url>                          (URL form)\n");
    printf("  httpget <host-or-ip> <port> [path]     (legacy form)\n");
    printf("  schemes: http (https not yet supported)\n");
    return 1;
}
