/*
 * userspace/httpd/httpd.c -- chapters 105 + 106a HTTP server.
 *
 * Chapter 105 / M94: single-threaded accept loop on top of the
 * chapter-104 `socket_listen` / `socket_accept` syscalls.  Serves
 * arbitrary paths out of the VFS read-only: the URL
 * `GET /mnt/hello.txt` opens the kernel path `/mnt/hello.txt` and
 * streams the bytes back as the response body.
 *
 * Chapter 106a / M96: when a request comes in for a path that is
 * NOT a local VFS prefix (/mnt/, /data/, /proc/), httpd opens a
 * TCP connection to a configurable upstream (default
 * 10.0.2.2:8080 -- scripts/https_proxy.py on the host) and
 * splices the response bytes straight back to the client.  The
 * middle stays dumb: httpd never parses the upstream response,
 * never rewrites headers, never knows TLS exists.  See chapter
 * 106a for the "be a dumb pipe" lesson and the chapter-106
 * loopback prerequisites that make a fresh outbound TCP
 * connection from inside an inbound TCP handler safe.
 *
 * Speaks HTTP/1.0 with `Connection: close` -- the simplest legal
 * dialect for a static-file server.  Connection: close means we
 * never have to know the file size in advance: the response ends
 * when we close the TCP connection, and the client (which already
 * understands this framing via libc/http.h) drains until peer FIN.
 * On the forward path the same property holds end-to-end:
 * upstream closes -> we drain to EOF -> we close the client fd.
 *
 * Usage:
 *   httpd                -- listen on port 8080
 *   httpd <port>         -- listen on a specific port
 *   httpd <port> --once  -- accept exactly one connection, then exit
 *
 * Environment:
 *   HTTPD_UPSTREAM=host:port -- override the chapter-106a forward
 *                               target (default 10.0.2.2:8080).
 *                               Hostnames are resolved via DNS.
 *
 * `--once` exists so the regression harness can spin httpd up,
 * issue one GET, and let it tear itself down -- we don't have
 * Ctrl-C or signals from the test side yet.  Same pattern as
 * chapter 104's echod.
 *
 * What this server intentionally does NOT do:
 *
 *   - keep-alive (chapter 105 explicitly chose HTTP/1.0 + close)
 *   - chunked transfer encoding (not needed without keep-alive)
 *   - POST / PUT / DELETE (we serve, we don't accept)
 *   - directory listings (404 on directories)
 *   - Range:  (no byte-range serving)
 *   - virtual hosts / Host: based routing
 *   - logging beyond a single console line per request
 *   - multiple in-flight connections (single-threaded)
 *   - response rewriting on the forward path (chapter 106a is
 *     deliberately a transparent byte pipe; rewriting belongs
 *     in scripts/https_proxy.py)
 *
 * Each of those would be a real extension, not a TODO.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/errno.h"
#include "../libc/env.h"

#define HTTPD_VERSION       "osdev/1.0"
/* 4 KiB request cap -- big enough for a curl request with all
 * the default headers (User-Agent, Accept, etc.) plus a generous
 * margin.  Chapter 105 sized this at 2 KiB; chapter 106a bumped
 * it because the forward path replays whatever headers the
 * client sent us, and real-world clients are chattier than our
 * own test harness. */
#define HTTPD_REQ_CAP       4096
#define HTTPD_PATH_CAP       512    /* path cap (RFC-style 2 KiB is
                                     * overkill for our workload)   */
/* Per-iteration body slice for both serve_get (chapter 105) and
 * serve_forward (chapter 106a).  Chapter 105 sized this at 1 KiB
 * because every test file fit in a single chunk and the syscall
 * overhead was invisible against the boot-time cost.  Chapter 106b
 * bumped it to 16 KiB: forwarding a real HN homepage (~50 KiB
 * body) at 1 KiB/iter takes 50 read+write pairs through the
 * scheduler, which combined with chapter 106's loopback queue
 * (16 frames, drained per net_poll) was producing minute-long
 * page loads.  16 KiB matches the TCP_BUF_SIZE / 2 sweet spot --
 * one read fills half the ring, the write drains it, no
 * back-pressure stalls.  The chunk lives on the per-connection
 * stack, so the bump costs 15 KiB of stack space per handler. */
#define HTTPD_SEND_CHUNK    16384

/* Chapter 106a default upstream proxy: SLIRP's gateway-of-the-
 * guest IP, port 8080.  That's where `scripts/https_proxy.py`
 * listens on the developer's host.  Override via the
 * HTTPD_UPSTREAM env var. */
#define HTTPD_DEFAULT_UPSTREAM_IP    IP4(10, 0, 2, 2)
#define HTTPD_DEFAULT_UPSTREAM_PORT  8080

/* ----------------------------------------------------------------
 * Small string helpers (no libc).
 * ---------------------------------------------------------------- */

static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static size_t s_len(const char *s)
{
    size_t n = 0; while (s[n]) n++; return n;
}

/* Chapter 106a: prefix match.  Used by handle_one to decide
 * whether a request goes to the local serve_get path or to
 * serve_forward.  Returns 1 on match, 0 otherwise. */
static int s_starts_with(const char *s, const char *pre)
{
    while (*pre) {
        if (*s != *pre) return 0;
        s++; pre++;
    }
    return 1;
}

/* Parse "A.B.C.D" into a packed BE IPv4 address.  Copy of the
 * helper in userspace/httpget/httpget.c -- chapter 106a uses it
 * to interpret HTTPD_UPSTREAM=10.0.2.2:8080 without having to
 * round-trip through DNS for the common case. */
static int s_parse_dotted(const char *s, uint32_t *out_be)
{
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0, seen_digit = 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            parts[idx] = parts[idx] * 10u + (uint32_t)(*s - '0');
            if (parts[idx] > 255u) return -1;
            seen_digit = 1;
        } else if (*s == '.') {
            if (!seen_digit) return -1;
            idx++; seen_digit = 0;
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

/* Case-insensitive suffix match on a NUL-terminated path.  Used
 * for content-type sniffing.  ASCII-only -- our filenames don't
 * carry locale-sensitive case. */
static int s_endswith_ci(const char *s, const char *suf)
{
    size_t ls = s_len(s), lf = s_len(suf);
    if (lf > ls) return 0;
    const char *p = s + (ls - lf);
    for (size_t i = 0; i < lf; i++) {
        char a = p[i],   b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

/* Pretty-print a packed BE IPv4 address as a.b.c.d on stdout.
 * Used for the per-request log line.  Same shape as echod. */
static void print_ip(uint32_t ip_be)
{
    printf("%d.%d.%d.%d",
           (int)((ip_be >> 24) & 0xff),
           (int)((ip_be >> 16) & 0xff),
           (int)((ip_be >>  8) & 0xff),
           (int)( ip_be        & 0xff));
}

/* Decimal port parser.  Same one echod uses; copy is cheap. */
static int parse_port(const char *s, uint16_t *out)
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
    if (!seen || v == 0) return -1;
    *out = (uint16_t)v;
    return 0;
}

/* ----------------------------------------------------------------
 * Chapter 106a: upstream proxy configuration.
 *
 * The forward path (see serve_forward) opens a fresh outbound
 * TCP connection to (g_upstream_ip, g_upstream_port) for every
 * request whose path isn't a local VFS prefix.  Defaults point
 * at scripts/https_proxy.py running on the developer's host via
 * SLIRP's gateway-of-the-guest magic IP (10.0.2.2).
 *
 * `HTTPD_UPSTREAM=host:port` overrides at startup.  Both halves
 * are optional: `HTTPD_UPSTREAM=10.0.2.5` keeps the default
 * port; `HTTPD_UPSTREAM=:9090` would parse the host as the
 * empty string and fail.  Hostnames go through the chapter-57
 * `resolve` syscall.
 * ---------------------------------------------------------------- */

static uint32_t g_upstream_ip   = HTTPD_DEFAULT_UPSTREAM_IP;
static uint16_t g_upstream_port = HTTPD_DEFAULT_UPSTREAM_PORT;

/* Parse "host[:port]" into (*out_ip, *out_port).  Returns 0 on
 * success, -1 on parse failure.  Resolves hostnames via DNS. */
static int parse_upstream(const char *spec,
                          uint32_t *out_ip, uint16_t *out_port)
{
    /* Find the LAST ':' so IPv6-literal-like inputs don't break.
     * (We don't actually support IPv6, but if someone types
     * `[::1]:8080` we'd rather fail cleanly than misparse.) */
    char host[64];
    int  hi = 0;
    int  colon = -1;
    for (int i = 0; spec[i]; i++) {
        if (spec[i] == ':') colon = i;
    }
    int host_len = (colon >= 0) ? colon : (int)s_len(spec);
    if (host_len <= 0 || host_len >= (int)sizeof(host)) return -1;
    for (int i = 0; i < host_len; i++) host[hi++] = spec[i];
    host[hi] = 0;

    /* IPv4 dotted-quad fast path; else DNS. */
    uint32_t ip = 0;
    if (s_parse_dotted(host, &ip) < 0) {
        if (resolve(host, &ip) < 0) return -1;
    }
    *out_ip = ip;

    if (colon >= 0) {
        uint16_t p = 0;
        if (parse_port(spec + colon + 1, &p) < 0) return -1;
        *out_port = p;
    }
    /* If no ':' was supplied, leave *out_port untouched (caller
     * seeded it with the default). */
    return 0;
}

/* Apply HTTPD_UPSTREAM if set; otherwise leave defaults intact.
 * Always runs before socket_listen so a misconfigured upstream
 * is loud at startup. */
static void load_upstream_from_env(void)
{
    const char *tmp = getenv("HTTPD_UPSTREAM");
    if (!tmp || !tmp[0]) return;       /* unset -> defaults */
    uint32_t ip   = g_upstream_ip;
    uint16_t port = g_upstream_port;
    if (parse_upstream(tmp, &ip, &port) < 0) {
        printf("httpd: bad HTTPD_UPSTREAM=\"%s\"; using default\n", tmp);
        return;
    }
    g_upstream_ip   = ip;
    g_upstream_port = port;
}

/* ----------------------------------------------------------------
 * Wire I/O.  write() may short-write under flow control; we always
 * loop until the slice is fully drained or the kernel returns an
 * error.  Pre-`socket_listen` chapters use the same idiom.
 * ---------------------------------------------------------------- */

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        long w = write(fd, p + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int write_str(int fd, const char *s)
{
    return write_all(fd, s, s_len(s));
}

/* ----------------------------------------------------------------
 * Content-Type sniffing.  Extension-based, in priority order: the
 * first match wins.  We bias toward the formats our in-tree
 * browser already understands (chapter 60+ html, chapter 61 css,
 * chapter 97 png) so a self-served page renders correctly.
 * ---------------------------------------------------------------- */

static const char *content_type_for(const char *path)
{
    if (s_endswith_ci(path, ".html")) return "text/html; charset=utf-8";
    if (s_endswith_ci(path, ".htm" )) return "text/html; charset=utf-8";
    if (s_endswith_ci(path, ".css" )) return "text/css; charset=utf-8";
    if (s_endswith_ci(path, ".js"  )) return "application/javascript";
    if (s_endswith_ci(path, ".txt" )) return "text/plain; charset=utf-8";
    if (s_endswith_ci(path, ".md"  )) return "text/plain; charset=utf-8";
    if (s_endswith_ci(path, ".png" )) return "image/png";
    if (s_endswith_ci(path, ".jpg" )) return "image/jpeg";
    if (s_endswith_ci(path, ".jpeg")) return "image/jpeg";
    if (s_endswith_ci(path, ".gif" )) return "image/gif";
    if (s_endswith_ci(path, ".svg" )) return "image/svg+xml";
    if (s_endswith_ci(path, ".json")) return "application/json";
    /* Unknown extension: stream as opaque bytes and let the
     * client decide.  Same default Apache picks. */
    return "application/octet-stream";
}

/* ----------------------------------------------------------------
 * Request parsing.  We only need the request line:
 *
 *   METHOD SP TARGET SP HTTP/VERSION CRLF
 *
 * The headers are read-and-discarded -- nothing in serve_get
 * conditions its behaviour on a header.  We DO stop reading at
 * the blank line that ends the header block so the client knows
 * its request was fully received before we start streaming the
 * body back.
 *
 * Chapter 106a: the raw bytes that make up the request (from
 * the first method letter through the trailing "\r\n\r\n") are
 * preserved in the caller's `buf` and the byte count is written
 * to *raw_len_out.  The forward path replays those bytes verbatim
 * to the upstream proxy -- we don't reframe headers, we don't
 * rewrite the Host:, we don't add or strip a Connection: close.
 * The upstream proxy decides what to do with each header.
 *
 * Returns 0 on a well-formed GET / HEAD, with `method` set to
 * 'G' or 'H' and the request target NUL-terminated into `target`.
 * Returns a negative HTTP status code on failure (e.g. -400 for
 * a malformed request line, -405 for an unsupported method).
 * ---------------------------------------------------------------- */

static int read_request(int cfd,
                        char *buf, size_t bcap, size_t *raw_len_out,
                        char *target, size_t tcap, char *method_out)
{
    size_t off = 0;
    *raw_len_out = 0;

    /* Pump bytes until we see "\r\n\r\n" or fill the buffer.  A
     * well-behaved client sends the whole request in one segment,
     * but TCP is allowed to fragment so we loop. */
    int    saw_blank_line = 0;
    size_t blank_end      = 0;       /* offset just past "\r\n\r\n" */
    while (off < bcap - 1) {
        long n = read(cfd, buf + off, bcap - 1 - off);
        if (n <= 0) return -400;     /* peer hung up or read error */
        off += (size_t)n;
        buf[off] = 0;
        /* Probe for the end-of-headers marker in the freshly
         * received tail.  Easiest is a linear scan from the start
         * each iteration -- the buffer is tiny. */
        for (size_t i = 3; i < off; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n' &&
                buf[i-1] == '\r' && buf[i  ] == '\n') {
                saw_blank_line = 1;
                blank_end = i + 1;   /* include the final '\n' */
                break;
            }
        }
        if (saw_blank_line) break;
    }
    if (!saw_blank_line) return -400;
    *raw_len_out = blank_end;

    /* Parse the request line: METHOD SP TARGET SP HTTP/VER CRLF. */
    size_t i = 0;
    /* method */
    char method[8] = {0};
    size_t mn = 0;
    while (i < off && buf[i] != ' ' && mn + 1 < sizeof(method))
        method[mn++] = buf[i++];
    if (i >= off || buf[i] != ' ') return -400;
    method[mn] = 0;
    i++;

    /* request target */
    size_t tn = 0;
    while (i < off && buf[i] != ' ' && tn + 1 < tcap)
        target[tn++] = buf[i++];
    if (i >= off || buf[i] != ' ') return -400;
    target[tn] = 0;
    i++;

    /* version: HTTP/1.0 or HTTP/1.1 are both fine.  We *respond*
     * in HTTP/1.0, which is a legal downgrade -- 1.1 clients are
     * required to handle a 1.0 response, including the implicit
     * close-delimited body framing. */
    if (i + 8 > off) return -400;
    if (buf[i+0] != 'H' || buf[i+1] != 'T' || buf[i+2] != 'T' ||
        buf[i+3] != 'P' || buf[i+4] != '/' || buf[i+5] != '1' ||
        buf[i+6] != '.' || (buf[i+7] != '0' && buf[i+7] != '1'))
        return -400;

    /* Method dispatch.  GET is the only one we serve directly.
     * HEAD is a trivial extension (send headers, skip body), but
     * until a caller needs it we don't pay the code complexity.
     * On the chapter-106a forward path the upstream proxy gets
     * any method we pass through; this 405 only fires for the
     * local /mnt|/data|/proc dispatch. */
    if (s_eq(method, "GET"))  { *method_out = 'G'; return 0; }
    if (s_eq(method, "HEAD")) { *method_out = 'H'; return 0; }
    /* Everything else: POST, PUT, DELETE, PATCH, OPTIONS, ... */
    return -405;
}

/* ----------------------------------------------------------------
 * Path safety.  The request target arrives URL-encoded in real
 * HTTP, but we don't implement %-decoding -- our test paths are
 * ASCII-clean.  The big risk is path traversal: a malicious
 * client sending `GET /../../etc/passwd` could escape the
 * intended doc root if we passed the target straight to open().
 *
 * The rule we enforce is: every path component must be a non-
 * empty string of [A-Za-z0-9._-/] and must not equal "..".  The
 * path must start with '/'.  That's strictly safer than what
 * Apache does by default and is enough for our test corpus.
 *
 * Returns 0 if safe, -1 if not.
 * ---------------------------------------------------------------- */

static int path_is_safe(const char *p)
{
    if (p[0] != '/') return -1;
    size_t i = 0;
    /* Walk each '/'-delimited segment. */
    while (p[i]) {
        size_t start = i + 1;          /* skip the leading '/' */
        i = start;
        while (p[i] && p[i] != '/') i++;
        size_t seg_len = i - start;
        if (seg_len == 0) {
            /* Empty segment ("//") is allowed only at the very
             * end (trailing slash) -- collapse to single slash. */
            if (p[i] == 0) break;
            return -1;
        }
        if (seg_len == 2 && p[start] == '.' && p[start+1] == '.')
            return -1;
        for (size_t j = start; j < i; j++) {
            char c = p[j];
            int ok = (c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') ||
                     c == '.' || c == '_' || c == '-';
            if (!ok) return -1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Response helpers.  All of them emit HTTP/1.0 + Connection: close
 * so the framing is implicit (body ends at TCP FIN).
 * ---------------------------------------------------------------- */

static void send_status_line(int cfd, int code, const char *reason)
{
    char line[64];
    /* snprintf-style: assemble by hand to avoid pulling in
     * libc/printf.h's varargs into a hot inner loop. */
    char *p = line;
    *p++ = 'H'; *p++ = 'T'; *p++ = 'T'; *p++ = 'P';
    *p++ = '/'; *p++ = '1'; *p++ = '.'; *p++ = '0'; *p++ = ' ';
    *p++ = (char)('0' + (code / 100) % 10);
    *p++ = (char)('0' + (code /  10) % 10);
    *p++ = (char)('0' + (code      ) % 10);
    *p++ = ' ';
    for (const char *r = reason; *r && (p - line) < (long)sizeof(line) - 3; r++)
        *p++ = *r;
    *p++ = '\r'; *p++ = '\n';
    (void)write_all(cfd, line, (size_t)(p - line));
}

static void send_common_headers(int cfd, const char *content_type)
{
    /* No Content-Length -- body framing is "until close" for
     * HTTP/1.0 + Connection: close.  This is RFC 1945 5.3. */
    (void)write_str(cfd, "Server: " HTTPD_VERSION "\r\n");
    (void)write_str(cfd, "Connection: close\r\n");
    (void)write_str(cfd, "Content-Type: ");
    (void)write_str(cfd, content_type);
    (void)write_str(cfd, "\r\n\r\n");
}

static void send_error(int cfd, int code, const char *reason)
{
    send_status_line(cfd, code, reason);
    send_common_headers(cfd, "text/plain; charset=utf-8");
    /* Tiny human-readable body -- helps debugging when poking at
     * the server with curl. */
    char body[64];
    char *p = body;
    *p++ = (char)('0' + (code / 100) % 10);
    *p++ = (char)('0' + (code /  10) % 10);
    *p++ = (char)('0' + (code      ) % 10);
    *p++ = ' ';
    for (const char *r = reason; *r; r++) *p++ = *r;
    *p++ = '\n';
    (void)write_all(cfd, body, (size_t)(p - body));
}

/* ----------------------------------------------------------------
 * GET serving.  Open, stream the body in HTTPD_SEND_CHUNK slices,
 * close.  Returns the number of body bytes sent (purely for the
 * log line).
 * ---------------------------------------------------------------- */

static long serve_get(int cfd, const char *path, char method)
{
    int fd = open(path, 0);
    if (fd < 0) {
        /* -ENOENT is the common case; everything else falls
         * back to 500.  Both responses still travel back to
         * the client.  Chapter 116d: read errno not -fd. */
        if (errno == ENOENT) {
            send_error(cfd, 404, "Not Found");
            return -404;
        }
        send_error(cfd, 500, "Internal Server Error");
        return -500;
    }

    send_status_line(cfd, 200, "OK");
    send_common_headers(cfd, content_type_for(path));

    /* HEAD: headers only, no body.  We still had to open() the
     * file to be sure it exists -- a 200 on a missing file would
     * be a lie. */
    if (method == 'H') { close(fd); return 0; }

    /* Chapter 106b: HTTPD_SEND_CHUNK is now 16 KiB.  User stack
     * is 64 KiB; putting a 16 KiB buffer here AND another in
     * serve_forward's stack frame would push the deepest call
     * chain (main -> for -> handle_one -> serve_get) too close
     * to the guard page.  malloc keeps the stack shallow. */
    char *chunk = (char *)malloc(HTTPD_SEND_CHUNK);
    if (!chunk) { close(fd); return -500; }
    long total = 0;
    for (;;) {
        long n = read(fd, chunk, HTTPD_SEND_CHUNK);
        if (n <  0) break;     /* read error mid-body: just close */
        if (n == 0) break;     /* EOF */
        if (write_all(cfd, chunk, (size_t)n) < 0) break;
        total += n;
    }
    free(chunk);
    close(fd);
    return total;
}

/* ----------------------------------------------------------------
 * Chapter 106a -- forwarding proxy ("TLS bridge").
 *
 * Request paths that aren't a local VFS prefix get forwarded to
 * the configured upstream proxy.  The default upstream is
 * `scripts/https_proxy.py` running on the developer's host;
 * that script turns `GET /news.ycombinator.com/...` into a
 * real HTTPS fetch and returns the body verbatim.
 *
 * The big design choice is **be a dumb pipe**.  serve_forward
 * never parses the upstream response.  It doesn't reframe
 * headers, doesn't compute Content-Length, doesn't decode
 * Transfer-Encoding: chunked, doesn't even peek at the status
 * line.  The client's request bytes go up; the upstream's
 * response bytes come back.  Three FINs, one chain:
 *
 *     client  --  FIN  -->  httpd  --  FIN  -->  upstream
 *     client  <--  FIN  --  httpd  <--  FIN  --  upstream
 *
 * The same `Connection: close` framing the upstream uses (every
 * Python `http.server` response, every chapter-105 response,
 * and `https_proxy.py` by explicit choice) tells the client
 * when the body ends.  Because we never parse, we never need
 * to know.
 *
 * Returns the number of body bytes spliced (for the log line),
 * or a negative HTTP status code if we couldn't connect to the
 * upstream at all.
 *
 * Loopback prerequisite: chapter 106 made the OUTBOUND
 * socket_connect from inside an INBOUND handler safe even when
 * inbound and outbound share the same 4-tuple namespace.  Before
 * that change, both sides would have raced over the single
 * virtio-net TX queue and the inbound side could stall.
 * ---------------------------------------------------------------- */

static long serve_forward(int cfd, const char *req_buf, size_t req_len)
{
    int up = socket_connect(g_upstream_ip, g_upstream_port);
    if (up < 0) {
        /* Upstream unreachable -- emit a real 502 so the client
         * can tell the difference between "upstream said 404" and
         * "I couldn't even reach upstream."  This is the only
         * place serve_forward generates HTTP itself. */
        send_error(cfd, 502, "Bad Gateway");
        return -502;
    }

    /* 1. Replay the client's request bytes verbatim.  Headers,
     *    spacing, capitalization, all of it.  The upstream gets
     *    to make its own decisions. */
    if (write_all(up, req_buf, req_len) < 0) {
        close(up);
        send_error(cfd, 502, "Bad Gateway");
        return -502;
    }

    /* Tell the upstream we're done sending so it can start
     * streaming its response without waiting for a request body
     * that isn't coming.  socket_shutdown sends FIN but leaves
     * the read side open. */
    (void)socket_shutdown(up);

    /* 2. Splice response: upstream -> client until upstream
     *    sends FIN.  Per chapter 106's vfs.c fix, read() returns
     *    0 (not -EIO) once the conn is fully closed and drained.
     *
     * Chapter 106b: chunk is malloc'd rather than stack-allocated
     * for the same reason as serve_get -- HTTPD_SEND_CHUNK = 16
     * KiB and the user stack is 64 KiB. */
    char *chunk = (char *)malloc(HTTPD_SEND_CHUNK);
    if (!chunk) {
        close(up);
        send_error(cfd, 502, "Bad Gateway");
        return -502;
    }
    long total_read    = 0;
    long total_written = 0;
    int  iter          = 0;
    for (;;) {
        long n = read(up, chunk, HTTPD_SEND_CHUNK);
        if (n <  0) break;     /* upstream error: best-effort close */
        if (n == 0) break;     /* upstream FIN, drained */
        total_read += n;
        if (write_all(cfd, chunk, (size_t)n) < 0) break;
        total_written += n;
        iter++;
    }
    free(chunk);
    close(up);
    /* Chapter 106b diag: log both upstream-read and client-written
     * totals.  If they diverge we know the splice failed mid-stream;
     * if they're equal but the client claims more, the kernel TCP
     * path is duplicating data. */
    printf("[httpd] serve_forward: read=%ld wrote=%ld iters=%d\n",
           total_read, total_written, iter);
    return total_written;
}

/* ----------------------------------------------------------------
 * Chapter 110 -- cookie test endpoints.
 *
 * These exist so test_browser_cookies.py can exercise the cookie
 * round-trip end-to-end without depending on any external server.
 * They are NOT general-purpose: just enough surface area to
 * assert "Set-Cookie creates a jar entry on disk" and "Cookie:
 * gets echoed back through whoami".
 *
 *   GET /cookie/set     -> sets hobbyos_session=alice (Max-Age=3600)
 *   GET /cookie/whoami  -> "hello <name>" or "anonymous"
 *   GET /cookie/clear   -> sets hobbyos_session= with Max-Age=0
 *
 * These run BEFORE is_local_path() so the strings /cookie/...
 * never touch the VFS path validator, never need a file under
 * /mnt or /data, and never need a hostname rewrite at the
 * upstream proxy.
 * ---------------------------------------------------------------- */

/* Locate a header value in the raw request buffer.  Case-insensitive
 * name match.  Returns the start of the value (no leading spaces),
 * and writes the length to *out_len.  Returns 0 if not found. */
static const char *find_header(const char *raw, size_t raw_len,
                                const char *name, size_t *out_len)
{
    /* Skip the request line (METHOD SP TARGET SP VER CRLF). */
    size_t i = 0;
    while (i + 1 < raw_len && !(raw[i] == '\r' && raw[i+1] == '\n')) i++;
    if (i + 2 > raw_len) return 0;
    i += 2;

    size_t nlen = s_len(name);
    while (i < raw_len) {
        if (i + 1 < raw_len && raw[i] == '\r' && raw[i+1] == '\n') break;
        size_t le = i;
        while (le + 1 < raw_len &&
               !(raw[le] == '\r' && raw[le+1] == '\n')) le++;
        if (le > i + nlen && raw[i + nlen] == ':') {
            int match = 1;
            for (size_t k = 0; k < nlen; k++) {
                char a = raw[i + k];
                char b = name[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match) {
                size_t v = i + nlen + 1;
                while (v < le && (raw[v] == ' ' || raw[v] == '\t')) v++;
                *out_len = le - v;
                return raw + v;
            }
        }
        i = le + 2;
    }
    return 0;
}

/* Status line + common headers + caller-supplied extra header
 * block (must end with \r\n, or be NULL) + Content-Type + blank
 * line.  Mirrors send_common_headers but lets cookie endpoints
 * slip Set-Cookie: in between Connection: close and Content-Type. */
static void send_status_with_extra(int cfd, int code, const char *reason,
                                    const char *extra_headers,
                                    const char *content_type)
{
    send_status_line(cfd, code, reason);
    (void)write_str(cfd, "Server: " HTTPD_VERSION "\r\n");
    (void)write_str(cfd, "Connection: close\r\n");
    if (extra_headers) (void)write_str(cfd, extra_headers);
    (void)write_str(cfd, "Content-Type: ");
    (void)write_str(cfd, content_type);
    (void)write_str(cfd, "\r\n\r\n");
}

static long serve_cookie_set(int cfd)
{
    const char *extra =
        "Set-Cookie: hobbyos_session=alice; Path=/; Max-Age=3600\r\n";
    send_status_with_extra(cfd, 200, "OK", extra,
                           "text/plain; charset=utf-8");
    const char *body = "session=alice\n";
    (void)write_str(cfd, body);
    return (long)s_len(body);
}

static long serve_cookie_clear(int cfd)
{
    const char *extra =
        "Set-Cookie: hobbyos_session=; Path=/; Max-Age=0\r\n";
    send_status_with_extra(cfd, 200, "OK", extra,
                           "text/plain; charset=utf-8");
    const char *body = "cleared\n";
    (void)write_str(cfd, body);
    return (long)s_len(body);
}

static long serve_cookie_whoami(int cfd, const char *raw, size_t raw_len)
{
    size_t cookie_len = 0;
    const char *cookie = find_header(raw, raw_len, "Cookie", &cookie_len);

    char name[64];
    int  named = 0;
    if (cookie && cookie_len > 0) {
        const char *needle = "hobbyos_session=";
        size_t nlen = s_len(needle);
        for (size_t k = 0; k + nlen <= cookie_len; k++) {
            /* Each candidate match must be at the start of a
             * cookie -- i.e. preceded by ';', ' ', or start. */
            if (k > 0 && cookie[k - 1] != ' ' && cookie[k - 1] != ';')
                continue;
            int match = 1;
            for (size_t j = 0; j < nlen; j++) {
                if (cookie[k + j] != needle[j]) { match = 0; break; }
            }
            if (!match) continue;
            size_t v = k + nlen, e = v;
            while (e < cookie_len && cookie[e] != ';') e++;
            size_t cap = sizeof(name) - 1;
            size_t got = (e - v < cap) ? (e - v) : cap;
            for (size_t i = 0; i < got; i++) name[i] = cookie[v + i];
            name[got] = '\0';
            if (got > 0) named = 1;
            break;
        }
    }

    char body[128];
    int  bl;
    if (named) bl = snprintf(body, sizeof(body), "hello %s\n", name);
    else       bl = snprintf(body, sizeof(body), "anonymous\n");
    send_status_with_extra(cfd, 200, "OK", 0,
                           "text/plain; charset=utf-8");
    (void)write_all(cfd, body, (size_t)bl);
    return bl;
}

/* ----------------------------------------------------------------
 * Per-connection handler.  Reads one request, dispatches to either
 * the chapter-105 local-file path or the chapter-106a forwarding
 * path, closes the connection.  HTTP/1.0 + Connection: close means
 * there is exactly one request per accept.
 * ---------------------------------------------------------------- */

/* Local VFS prefixes that serve_get handles directly.  Any path
 * NOT starting with one of these goes to serve_forward.  Add a
 * new mount type here when you want it reachable via httpd; the
 * VFS already dispatches by mount inside open().  Chapter 105
 * shipped exactly these three. */
static int is_local_path(const char *target)
{
    return s_starts_with(target, "/mnt/")  ||
           s_starts_with(target, "/data/") ||
           s_starts_with(target, "/proc/");
}

static void log_request(uint32_t peer_ip, uint16_t peer_port,
                        const char *kind, char method,
                        const char *target, int code, long bytes)
{
    printf("[httpd] ");
    print_ip(peer_ip);
    printf(":%d %s %s %s -> %d",
           (int)peer_port,
           kind,
           (method == 'H') ? "HEAD" : "GET",
           target, code);
    if (bytes >= 0) printf(" (%ld bytes)\n", bytes);
    else            printf("\n");
}

static void handle_one(int cfd, uint32_t peer_ip, uint16_t peer_port)
{
    char raw[HTTPD_REQ_CAP];
    size_t raw_len = 0;
    char target[HTTPD_PATH_CAP];
    char method = 0;
    int rc = read_request(cfd, raw, sizeof(raw), &raw_len,
                          target, sizeof(target), &method);
    if (rc < 0) {
        int code = -rc;
        const char *reason = (code == 405) ? "Method Not Allowed"
                                           : "Bad Request";
        send_error(cfd, code, reason);
        printf("[httpd] ");
        print_ip(peer_ip);
        printf(":%d -- %d %s\n", (int)peer_port, code, reason);
        return;
    }

    if (s_starts_with(target, "/cookie/")) {
        /* Chapter 110 test endpoints.  Dispatched before
         * is_local_path so the strings never touch the VFS or
         * the upstream proxy. */
        long n = -404;
        if (s_eq(target, "/cookie/set")) {
            n = serve_cookie_set(cfd);
        } else if (s_eq(target, "/cookie/whoami")) {
            n = serve_cookie_whoami(cfd, raw, raw_len);
        } else if (s_eq(target, "/cookie/clear")) {
            n = serve_cookie_clear(cfd);
        } else {
            send_error(cfd, 404, "Not Found");
        }
        int code = (n >= 0) ? 200 : (int)(-n);
        log_request(peer_ip, peer_port, "cookie", method, target, code, n);
        return;
    }

    if (is_local_path(target)) {
        /* Chapter 105 local-file path.  path_is_safe only matters
         * here -- the forward path never calls open().
         *
         * Chapter 109: strip the optional ?query suffix before
         * validating + opening.  GET form submits round-trip
         * their inputs as query strings; without this they'd
         * always trip path_is_safe's "no ? in segments" rule
         * and 400.  We keep the original `target` only for the
         * log line so URL queries are still visible in serial. */
        char path_only[HTTPD_PATH_CAP];
        size_t pi = 0;
        for (; target[pi] && target[pi] != '?' &&
                pi + 1 < sizeof(path_only); pi++) {
            path_only[pi] = target[pi];
        }
        path_only[pi] = '\0';

        if (path_is_safe(path_only) < 0) {
            send_error(cfd, 400, "Bad Request");
            printf("[httpd] ");
            print_ip(peer_ip);
            printf(":%d %s -- 400 (unsafe path)\n",
                   (int)peer_port, target);
            return;
        }
        long n = serve_get(cfd, path_only, method);
        int code = (n >= 0) ? 200 : (int)(-n);
        log_request(peer_ip, peer_port, "local", method, target, code, n);
    } else {
        /* Chapter 106a forward path.  We don't validate the path:
         * the upstream proxy is the source of truth for what
         * URLs it accepts.  We DO send the raw request bytes
         * verbatim (replay attack? sure -- but the upstream is
         * us, on our own host, behind SLIRP NAT). */
        long n = serve_forward(cfd, raw, raw_len);
        int code = (n >= 0) ? 200 : (int)(-n);
        log_request(peer_ip, peer_port, "forward", method, target, code, n);
    }
}

int main(int argc, char **argv)
{
    uint16_t port = 8080;
    int once = 0;

    if (argc >= 2) {
        if (parse_port(argv[1], &port) < 0) {
            printf("httpd: bad port \"%s\"\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        const char *flag = argv[2];
        if (s_eq(flag, "--once")) once = 1;
        else {
            printf("httpd: unknown arg \"%s\"\n", flag);
            return 1;
        }
    }

    int lfd = socket_listen(port, 4);
    if (lfd < 0) {
        printf("httpd: listen failed: %d\n", lfd);
        return 1;
    }
    /* Chapter 106a: pick up the optional upstream override BEFORE
     * we start logging the listen line, so a misconfigured proxy
     * is visible in the serial transcript next to the bind. */
    load_upstream_from_env();
    printf("httpd: listening on port %d (once=%d)\n", (int)port, once);
    printf("httpd: forward upstream ");
    print_ip(g_upstream_ip);
    printf(":%d\n", (int)g_upstream_port);

    for (;;) {
        uint32_t peer_ip = 0;
        uint16_t peer_port = 0;
        int cfd = socket_accept(lfd, &peer_ip, &peer_port);
        if (cfd < 0) {
            printf("httpd: accept failed: %d\n", cfd);
            close(lfd);
            return 1;
        }

        handle_one(cfd, peer_ip, peer_port);
        /* HTTP/1.0 + Connection: close: drop the conn after each
         * request.  The peer's read() will return 0 once our FIN
         * lands and our buffered body has been drained. */
        close(cfd);

        if (once) break;
    }

    close(lfd);
    printf("httpd: done\n");
    return 0;
}
