/*
 * userspace/browser/browser.c — milestone-63 browser driver.
 *
 * The capstone of Part VIII: a userspace program that fetches an
 * HTML document (from disk or via HTTP), runs the full milestone-
 * 59-through-62 pipeline, and renders the resulting paint stream
 * to a character grid on stdout.  The text-mode renderer maps the
 * 800-px-wide viewport onto a fixed 8x16-px cell grid (matching
 * our kernel font), so a default page is roughly 100 cols × N rows.
 *
 * Pipeline:
 *
 *   argv[1]  ──▶  fetch (file slurp / HTTP/1.1 GET)
 *                   │
 *                   ▼
 *               raw HTML bytes
 *                   │
 *                   ▼
 *           html.h  (tokeniser)
 *                   ▼
 *           dom.h   (tree builder)
 *                   ▼
 *           layout.h cascade  (UA + author <style> + inline style="…")
 *                   ▼
 *           layout.h box tree  +  block / inline layout
 *                   ▼
 *           layout.h paint command stream
 *                   ▼
 *           text-mode renderer  →  char grid  →  stdout
 *
 * Usage:
 *
 *   browser <url-or-path> [viewport]    plain text rendering
 *   browser --paint <…>                 dump paint stream (debug)
 *   browser --ansi  <…>                 colour + underline via ANSI
 *   browser --gui   <…>                 open a window and render the
 *                                       full paint stream with
 *                                       scroll support (arrow keys,
 *                                       PgUp/PgDn = space/'b',
 *                                       Home/End, ESC = quit)
 *
 * Examples:
 *
 *   browser /mnt/test_layout.html
 *   browser http://example.com/ 700
 *   browser --ansi /mnt/test_layout.html 800
 *   browser --gui  /mnt/test_layout.html 900
 *
 * No HTTPS yet (TLS is parked behind a future milestone).  An
 * https:// URL prints a clear error and exits non-zero.
 *
 * The plain / ANSI text renderers don't try to be a pixel-perfect
 * representation of the layout: they care about reading order and
 * structural boundaries, not graphical fidelity.  The GUI renderer
 * IS pixel-faithful: it walks the paint stream and issues one
 * gui_fill_rect / gui_draw_text per command, so the on-screen
 * picture matches what M64+ will get from a framebuffer renderer.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/url.h"
#include "../libc/http.h"
#include "../libc/html.h"
#include "../libc/dom.h"
#include "../libc/css.h"
#include "../libc/layout.h"
#include "../libc/thread.h"
#include "../libc/png.h"

/* ----------------------------------------------------------------
 * Tiny utilities (no libc).
 * ---------------------------------------------------------------- */

static int br_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int br_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

static int br_atoi(const char *s)
{
    int v = 0, sign = 1;
    if (!s) return 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

static size_t br_strlen(const char *s)
{
    size_t n = 0; while (s[n]) n++; return n;
}

static int parse_dotted(const char *s, uint32_t *out_be)
{
    uint32_t parts[4] = {0,0,0,0};
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

/* ----------------------------------------------------------------
 * Fetch.  Two transports:
 *
 *   1. http://host[:port]/path  -- HTTP/1.1 GET via socket_connect
 *   2. anything else            -- file path on the local FS
 *
 * Both return a malloc'd buffer the caller must free, and write
 * the byte count to *out_len.  On error, return NULL with a
 * message printed.
 * ---------------------------------------------------------------- */

/* File slurp.  Read the whole file into a malloc'd buffer that
 * grows on demand. */
static char *slurp_file(const char *path, size_t *out_len)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("browser: cannot open '%s': errno=%d\n", path, -fd);
        return 0;
    }
    size_t cap = 4096, len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("browser: oom\n"); close(fd); return 0; }
    char tmp[1024];
    long n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)n) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) {
                printf("browser: oom (grow)\n");
                free(buf); close(fd); return 0;
            }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf);
            buf = nb; cap = newcap;
        }
        for (long i = 0; i < n; i++) buf[len + (size_t)i] = tmp[i];
        len += (size_t)n;
    }
    close(fd);
    if (n < 0) {
        printf("browser: read error %ld on '%s'\n", n, path);
        free(buf); return 0;
    }
    *out_len = len;
    return buf;
}

/* Drain a TCP fd into a malloc'd buffer that grows on demand. */
static char *drain_fd(int fd, size_t *out_len)
{
    size_t cap = 16 * 1024, len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("browser: oom (drain)\n"); return 0; }
    /* Per-call read chunk: sized to match the kernel's per-conn
     * RX ring (TCP_BUF_SIZE = 32 KB).  A larger chunk means fewer
     * round trips through the syscall + scheduler, which matters
     * when many other GUI threads are also calling yield().  We
     * malloc this rather than putting it on the stack: the user
     * thread stack is only 16 KiB. */
    size_t scratch_cap = 8 * 1024;
    char  *scratch = (char *)malloc(scratch_cap);
    if (!scratch) { free(buf); printf("browser: oom (scratch)\n"); return 0; }
    for (;;) {
        long r = read(fd, scratch, scratch_cap);
        if (r < 0) {
            printf("browser: read error %ld\n", r);
            free(scratch); free(buf); return 0;
        }
        if (r == 0) break;
        if (len + (size_t)r > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)r) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) {
                printf("browser: oom (grow %lu)\n", (unsigned long)newcap);
                free(scratch); free(buf); return 0;
            }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf); buf = nb; cap = newcap;
        }
        for (long i = 0; i < r; i++) buf[len + (size_t)i] = scratch[i];
        len += (size_t)r;
    }
    free(scratch);
    *out_len = len;
    return buf;
}

/* Single HTTP/1.1 fetch.  On 3xx with a Location header, copies
 * the redirect target into out_redirect (caller frees) so a
 * higher-level loop can follow it. */
static char *http_fetch_one(const char *raw_url, size_t *out_html_len,
                             char **out_redirect)
{
    *out_redirect = 0;

    struct url u;
    if (url_parse(raw_url, &u) < 0) {
        printf("browser: cannot parse URL '%s'\n", raw_url);
        return 0;
    }
    if (url_is_tls(&u)) {
        printf("browser: https:// is not yet supported in this build.\n");
        printf("        TLS is parked behind a future milestone.\n");
        return 0;
    }

    /* Resolve. */
    uint32_t ip_be = 0;
    if (parse_dotted(u.host, &ip_be) < 0) {
        int rc = resolve(u.host, &ip_be);
        if (rc < 0) {
            printf("browser: cannot resolve '%s' (%d)\n", u.host, rc);
            return 0;
        }
        printf("[browser] resolved %s -> %u.%u.%u.%u\n", u.host,
               (unsigned)((ip_be >> 24) & 0xFF),
               (unsigned)((ip_be >> 16) & 0xFF),
               (unsigned)((ip_be >>  8) & 0xFF),
               (unsigned)( ip_be        & 0xFF));
    }

    int fd = socket_connect(ip_be, u.port);
    if (fd < 0) {
        printf("browser: connect %s:%u failed (%d)\n", u.host, u.port, fd);
        return 0;
    }

    /* Build HTTP/1.1 request.  Identify as a real browser so we
     * get HTML rather than a "no JavaScript supported" page from
     * sites that sniff the UA. */
    size_t req_cap = URL_PATH_MAX + URL_HOST_MAX + 256;
    char  *req = (char *)malloc(req_cap);
    if (!req) { printf("browser: oom (req)\n"); close(fd); return 0; }
    int n = 0;
    n += snprintf(req + n, req_cap - (size_t)n, "GET %s HTTP/1.1\r\n", u.path);
    if (u.port == 80)
        n += snprintf(req + n, req_cap - (size_t)n, "Host: %s\r\n", u.host);
    else
        n += snprintf(req + n, req_cap - (size_t)n, "Host: %s:%u\r\n", u.host, u.port);
    n += snprintf(req + n, req_cap - (size_t)n,
                  "User-Agent: hobbyos-browser/1.0 (M63)\r\n"
                  "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.5\r\n"
                  "Accept-Encoding: identity\r\n"
                  "Connection: close\r\n\r\n");
    long wr = write(fd, req, (unsigned long)n);
    free(req);
    if (wr < 0) {
        printf("browser: HTTP write failed\n");
        close(fd); return 0;
    }

    size_t total = 0;
    char  *raw   = drain_fd(fd, &total);
    close(fd);
    if (!raw) return 0;

    struct http_response *resp = (struct http_response *)malloc(sizeof(*resp));
    if (!resp) {
        printf("browser: oom (resp)\n"); free(raw); return 0;
    }
    if (http_parse(raw, total, resp) < 0) {
        printf("browser: malformed HTTP response (%lu raw bytes)\n",
               (unsigned long)total);
        free(resp); free(raw); return 0;
    }

    size_t ct_len = 0;
    const char *ct = http_get_header(resp, "content-type", &ct_len);
    printf("[browser] HTTP/1.%d %d", resp->minor_version, resp->status);
    write(1, " ", 1);
    if (resp->reason_len) write(1, resp->reason, (unsigned long)resp->reason_len);
    printf(" (");
    if (ct) write(1, ct, (unsigned long)ct_len); else printf("no content-type");
    printf(", body=%lu bytes)\n", (unsigned long)resp->body_len);

    /* Capture redirect target (if any). */
    if (resp->status >= 300 && resp->status < 400) {
        size_t loc_len = 0;
        const char *loc = http_get_header(resp, "location", &loc_len);
        if (loc && loc_len > 0 && loc_len < URL_HOST_MAX + URL_PATH_MAX) {
            char *next = (char *)malloc(loc_len + 1);
            if (next) {
                for (size_t i = 0; i < loc_len; i++) next[i] = loc[i];
                next[loc_len] = 0;
                *out_redirect = next;
            }
        }
    }

    /* Hand the body slice back to the caller — but we have to copy
     * it out before freeing the raw buffer. */
    char *html = 0;
    if (resp->body_len > 0) {
        html = (char *)malloc(resp->body_len + 1);
        if (html) {
            for (size_t i = 0; i < resp->body_len; i++) html[i] = resp->body[i];
            html[resp->body_len] = 0;
            *out_html_len = resp->body_len;
        } else {
            printf("browser: oom copying body (%lu bytes)\n",
                   (unsigned long)resp->body_len);
        }
    } else {
        html = (char *)malloc(1);
        if (html) { html[0] = 0; *out_html_len = 0; }
    }

    free(resp); free(raw);
    return html;
}

/* Fetch with up-to-2 redirect hops.  Iterative — never recurses,
 * so the userspace stack stays shallow. */
static char *http_fetch(const char *url0, size_t *out_html_len)
{
    char       *owned  = 0;
    const char *target = url0;
    char       *html   = 0;
    for (int hop = 0; hop < 3; hop++) {
        char *redir = 0;
        html = http_fetch_one(target, out_html_len, &redir);
        if (owned) { free(owned); owned = 0; }
        if (!redir) break;
        if (html) { free(html); html = 0; }
        printf("[browser] following redirect -> %s\n", redir);
        owned  = redir;
        target = redir;
    }
    if (owned) free(owned);
    return html;
}

/* Top-level dispatcher.  Picks the right transport based on the
 * presence of a "://" scheme separator. */
static char *fetch(const char *src, size_t *out_len, char **out_origin_url)
{
    *out_origin_url = 0;
    if (br_starts(src, "http://")) {
        char *u = (char *)malloc(br_strlen(src) + 1);
        if (u) {
            for (size_t i = 0; src[i]; i++) u[i] = src[i];
            u[br_strlen(src)] = 0;
            *out_origin_url = u;
        }
        return http_fetch(src, out_len);
    }
    if (br_starts(src, "https://")) {
        printf("browser: https:// not yet supported (no TLS).\n");
        printf("        run scripts/https_proxy.py on the host and use\n");
        printf("        http://10.0.2.2:8080/<host><path> instead.\n");
        return 0;
    }
    /* Treat as file path. */
    return slurp_file(src, out_len);
}

/* ----------------------------------------------------------------
 *   External stylesheet support
 *
 *   Walk the DOM for <link rel="stylesheet" href="..."> elements,
 *   resolve each href against the page's origin URL, fetch the
 *   body via HTTP, and concatenate the bodies into one big CSS
 *   string ready to be appended to the inline-<style> blob.
 *
 *   Limits (sane defaults — mirrors browser.c's redirect cap):
 *     - up to 8 stylesheets per page (HN ships 1; most sites <4)
 *     - each individual sheet capped at 256 KiB (HN's news.css is ~12 KiB)
 *     - skip sheets we can't fetch (404, parse error, OOM); the page
 *       still renders with whatever CSS we could grab + UA defaults
 *
 *   `origin_url` is the URL of the page itself (e.g. "http://news
 *   .ycombinator.com/"); if NULL (i.e. file:// load), external CSS
 *   fetching is skipped.
 * ---------------------------------------------------------------- */

#define BR_MAX_LINK_SHEETS  8
#define BR_LINK_SHEET_MAX   (256u * 1024u)

/* Resolve a relative href against base_url.  Handles:
 *   1. ref starts with "http://" / "https://"   -> verbatim copy
 *   2. ref starts with "//host/path" (protocol-relative)
 *                                                -> base_scheme + ref
 *   3. ref starts with "/"                       -> scheme://host + ref
 *   4. otherwise (bare relative)                 -> base_dir + ref
 *
 * out is caller-allocated (cap bytes).  Returns 0 on success, -1
 * on overflow or malformed base.  Lightweight; doesn't handle
 * "../" segment compression or fragment / query stripping. */
static int resolve_url(const char *base_url, const char *ref,
                        char *out, size_t cap)
{
    if (!ref || !out || cap < 2) return -1;
    if (br_starts(ref, "http://") || br_starts(ref, "https://")) {
        size_t n = br_strlen(ref);
        if (n + 1 > cap) return -1;
        for (size_t i = 0; i <= n; i++) out[i] = ref[i];
        return 0;
    }
    if (!base_url) return -1;
    /* Find scheme://host/ split in base_url. */
    const char *p = base_url;
    int scheme_len = 0;
    if (br_starts(p, "http://"))       { p += 7; scheme_len = 7; }
    else if (br_starts(p, "https://")) { p += 8; scheme_len = 8; }
    else return -1;
    /* Protocol-relative `//other/path`: combine with base scheme. */
    if (ref[0] == '/' && ref[1] == '/') {
        size_t rl = br_strlen(ref);
        if ((size_t)scheme_len + rl - 2 + 1 > cap) return -1;
        for (int i = 0; i < scheme_len; i++) out[i] = base_url[i];
        for (size_t i = 0; i < rl - 2; i++) out[scheme_len + i] = ref[i + 2];
        out[scheme_len + rl - 2] = '\0';
        return 0;
    }
    /* `p` now points at host; advance to first '/' (start of path). */
    const char *path_start = p;
    while (*path_start && *path_start != '/' && *path_start != '?'
                       && *path_start != '#') path_start++;
    size_t scheme_host_len = (size_t)(path_start - base_url);

    if (ref[0] == '/') {
        /* Root-relative. */
        size_t rl = br_strlen(ref);
        if (scheme_host_len + rl + 1 > cap) return -1;
        for (size_t i = 0; i < scheme_host_len; i++) out[i] = base_url[i];
        for (size_t i = 0; i < rl; i++)              out[scheme_host_len + i] = ref[i];
        out[scheme_host_len + rl] = '\0';
        return 0;
    }
    /* Path-relative.  Use the base path up to and including its
     * last '/' (or "/" if base path was empty/"/foo"). */
    const char *path_end = path_start;
    while (*path_end && *path_end != '?' && *path_end != '#') path_end++;
    /* Find the last '/' in [path_start, path_end). */
    const char *last_slash = path_start;
    for (const char *q = path_start; q < path_end; q++) {
        if (*q == '/') last_slash = q;
    }
    /* Base path prefix is [base_url, last_slash], inclusive of '/'. */
    size_t prefix_len;
    if (last_slash >= path_start) {
        prefix_len = (size_t)(last_slash - base_url) + 1;
    } else {
        /* No path in base; synthesise "/". */
        if (scheme_host_len + 1 > cap) return -1;
        for (size_t i = 0; i < scheme_host_len; i++) out[i] = base_url[i];
        out[scheme_host_len] = '/';
        prefix_len = scheme_host_len + 1;
    }
    if (prefix_len > 0 && last_slash >= path_start) {
        for (size_t i = 0; i < prefix_len; i++) out[i] = base_url[i];
    }
    size_t rl = br_strlen(ref);
    if (prefix_len + rl + 1 > cap) return -1;
    for (size_t i = 0; i < rl; i++) out[prefix_len + i] = ref[i];
    out[prefix_len + rl] = '\0';
    return 0;
}

/* Walk the DOM and fill `hrefs[]` with up to `cap` <link
 * rel="stylesheet" href="..."> values (pointers borrowed into
 * the DOM; do not free).  Returns the count actually filled. */
static int collect_link_hrefs(const struct dom_node *root,
                               const char **hrefs, int cap)
{
    if (!root || cap <= 0) return 0;
    int n = 0;
    /* Iterative DFS with a fixed-depth stack.  256 should comfortably
     * cover any sane page; if it overflows we just stop walking. */
    const struct dom_node *stack[256];
    int top = 0;
    stack[top++] = root;
    while (top > 0 && n < cap) {
        const struct dom_node *cur = stack[--top];
        if (!cur) continue;
        if (cur->type == DOM_NODE_ELEMENT && cur->tag) {
            /* Case-insensitive "link" match.  dom_node_attr is
             * already case-insensitive on the attribute name. */
            const char *t = cur->tag;
            int is_link = (t[0] == 'l' || t[0] == 'L') &&
                          (t[1] == 'i' || t[1] == 'I') &&
                          (t[2] == 'n' || t[2] == 'N') &&
                          (t[3] == 'k' || t[3] == 'K') && t[4] == '\0';
            if (is_link) {
                const char *rel  = dom_node_attr(cur, "rel");
                const char *href = dom_node_attr(cur, "href");
                if (rel && href) {
                    /* Accept "stylesheet" anywhere in the rel value
                     * (could be "stylesheet alternate"). */
                    int found = 0;
                    for (int i = 0; rel[i]; i++) {
                        const char *target = "stylesheet";
                        int k = 0;
                        while (target[k]) {
                            char a = rel[i + k];
                            char b = target[k];
                            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                            if (a != b) break;
                            k++;
                        }
                        if (!target[k]) { found = 1; break; }
                    }
                    if (found) hrefs[n++] = href;
                }
            }
        }
        for (struct dom_node *c = cur->first_child; c && top < 256; c = c->next_sibling) {
            stack[top++] = c;
        }
    }
    return n;
}

/* Fetch every <link rel="stylesheet"> sheet referenced by the
 * document and return them concatenated as one freshly-malloc'd
 * NUL-terminated string.  *out_len gets the length (excluding NUL).
 *
 * Returns NULL when there are no link sheets, or when none could
 * be successfully fetched.  Either way the caller's existing
 * inline-<style> blob is still authoritative — link sheets are
 * appended to it, not a replacement. */
static char *fetch_external_stylesheets(const struct dom_node *root,
                                         const char *origin_url,
                                         size_t *out_len)
{
    *out_len = 0;
    if (!origin_url) return 0;     /* file:// load: nothing to do */

    const char *hrefs[BR_MAX_LINK_SHEETS];
    int n = collect_link_hrefs(root, hrefs, BR_MAX_LINK_SHEETS);
    if (n == 0) return 0;

    /* Two-pass: fetch each sheet into bodies[], then concat. */
    char   *bodies[BR_MAX_LINK_SHEETS];
    size_t  blens [BR_MAX_LINK_SHEETS];
    for (int i = 0; i < n; i++) { bodies[i] = 0; blens[i] = 0; }

    size_t total = 0;
    char abs[URL_PATH_MAX + URL_HOST_MAX + 16];
    for (int i = 0; i < n; i++) {
        if (resolve_url(origin_url, hrefs[i], abs, sizeof(abs)) < 0) {
            printf("[browser] skip sheet (cannot resolve): %s\n", hrefs[i]);
            continue;
        }
        if (br_starts(abs, "https://")) {
            printf("[browser] skip sheet (https not supported): %s\n", abs);
            continue;
        }
        if (!br_starts(abs, "http://")) {
            printf("[browser] skip sheet (unsupported scheme): %s\n", abs);
            continue;
        }
        size_t blen = 0;
        printf("[browser] fetching stylesheet %s\n", abs);
        char *body = http_fetch(abs, &blen);
        if (!body || blen == 0) {
            printf("[browser]   -> fetch failed\n");
            if (body) free(body);
            continue;
        }
        if (blen > BR_LINK_SHEET_MAX) {
            printf("[browser]   -> %u bytes exceeds %u-byte cap; truncating\n",
                   (unsigned)blen, (unsigned)BR_LINK_SHEET_MAX);
            blen = BR_LINK_SHEET_MAX;
        }
        bodies[i] = body;
        blens [i] = blen;
        total    += blen + 2;     /* "\n" between sheets */
    }

    if (total == 0) return 0;

    char *out = (char *)malloc(total + 1);
    if (!out) {
        for (int i = 0; i < n; i++) if (bodies[i]) free(bodies[i]);
        return 0;
    }
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        if (!bodies[i]) continue;
        for (size_t k = 0; k < blens[i]; k++) out[off++] = bodies[i][k];
        out[off++] = '\n';
        out[off++] = '\n';
        free(bodies[i]);
    }
    out[off] = '\0';
    *out_len = off;
    return out;
}

/* ----------------------------------------------------------------
 * Paint-stream debug dumper (browser --paint).  Mirrors the format
 * /bin/layout uses, so the same eyeballs work for both binaries.
 * ---------------------------------------------------------------- */

static void print_color(unsigned int c)
{
    static const char *hex = "0123456789ABCDEF";
    char out[9];
    for (int i = 7; i >= 0; i--) { out[i] = hex[c & 0xF]; c >>= 4; }
    out[8] = 0;
    printf("#%s", out);
}

static void print_quoted(const char *s, int n)
{
    printf("\"");
    if (s) for (int i = 0; i < n; i++) {
        char c = s[i];
        if      (c == '"')  printf("\\\"");
        else if (c == '\\') printf("\\\\");
        else if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c == '\r') printf("\\r");
        else if ((unsigned char)c < 0x20) printf("?");
        else printf("%c", c);
    }
    printf("\"");
}

static void dump_paints(const struct layout_doc *d, const struct layout_paint_buf *p)
{
    printf("[BROWSER-PAINT] viewport=%d height=%d paints=%d\n",
           d->doc_width_px, d->doc_height_px, p->n);
    for (int i = 0; i < p->n; i++) {
        const struct layout_paint_cmd *c = &p->cmds[i];
        const char *kind = c->kind == LAY_PAINT_RECT      ? "RECT" :
                           c->kind == LAY_PAINT_TEXT      ? "TEXT" :
                           c->kind == LAY_PAINT_UNDERLINE ? "UNDERLINE" : "?";
        printf("[PAINT#%d] %s x=%d y=%d w=%d h=%d color=", i, kind,
               c->x, c->y, c->w, c->h);
        print_color(c->color);
        if (c->kind == LAY_PAINT_TEXT) {
            printf(" fs=%d fw=%d fst=%d ", c->font_size_px, c->font_weight, c->font_style);
            print_quoted(c->text, c->text_len);
        }
        printf("\n");
    }
}

/* ----------------------------------------------------------------
 * Text-mode renderer.
 *
 * The grid maps the layout viewport onto an 8x16-px character cell.
 * For the default 800-px viewport that gives 100 columns; the row
 * count comes from the laid-out document height.
 *
 * Each paint command is mapped as follows:
 *
 *   RECT (background, w>2 AND h>2)     skipped — fill would
 *                                       clutter the text.
 *   RECT (border, w<=2 XOR h<=2)       drawn as a horizontal
 *                                       '─' or vertical '│' span.
 *   TEXT                               each character placed at
 *                                       col = (x + i*glyph_w)/8,
 *                                       row = y/16.
 *   UNDERLINE                          skipped in plain mode;
 *                                       rendered with ANSI in
 *                                       --ansi mode.
 *
 * Later paints overwrite earlier paints in the same cell, so text
 * "wins" over a coincident border (matching what the framebuffer
 * does — child content paints over parent borders).
 *
 * The grid uses one byte per cell for the glyph (multibyte UTF-8
 * is supported up to 4 bytes per logical character via the
 * `glyphs` overflow table).  ANSI mode adds a parallel attrs[]
 * table holding fg-colour-code + underline-bit per cell.
 * ---------------------------------------------------------------- */

#define BR_CELL_W   8     /* px per character cell (kernel font 8x16) */
#define BR_CELL_H  16

/* Per-cell attributes (ANSI mode). */
struct br_attr {
    uint32_t fg;          /* 0xAARRGGBB; 0 means "default"          */
    uint32_t bg;          /* 0xAARRGGBB; 0 means "default"          */
    uint8_t  underline;   /* 0 / 1                                  */
    uint8_t  bold;
    uint8_t  italic;
    uint8_t  pad;
};

struct br_grid {
    int cols, rows;
    /* `cells` is one byte per cell.  ASCII chars land here.  For
     * the bullet glyph "•" (U+2022 = 0xE2 0x80 0xA2) we set
     * cells[i] = 0x01 (a sentinel) and write the UTF-8 bytes into
     * the overflow buffer at glyphs[i*4..i*4+3]. */
    char           *cells;
    char           *glyphs;     /* 4 bytes per cell, 0 if unused      */
    struct br_attr *attrs;      /* per-cell attribute (NULL in plain) */
};

static int br_grid_init(struct br_grid *g, int cols, int rows, int with_attrs)
{
    g->cols = cols; g->rows = rows;
    size_t total = (size_t)cols * (size_t)rows;
    if (total == 0) { g->cells = 0; g->glyphs = 0; g->attrs = 0; return 0; }
    g->cells  = (char *)malloc(total);
    g->glyphs = (char *)malloc(total * 4);
    g->attrs  = with_attrs ? (struct br_attr *)malloc(total * sizeof(struct br_attr)) : 0;
    if (!g->cells || !g->glyphs || (with_attrs && !g->attrs)) {
        if (g->cells)  free(g->cells);
        if (g->glyphs) free(g->glyphs);
        if (g->attrs)  free(g->attrs);
        g->cells = g->glyphs = 0; g->attrs = 0;
        return -1;
    }
    for (size_t i = 0; i < total; i++)        g->cells[i]  = ' ';
    for (size_t i = 0; i < total * 4; i++)    g->glyphs[i] = 0;
    if (g->attrs) for (size_t i = 0; i < total; i++) {
        g->attrs[i].fg = 0; g->attrs[i].bg = 0;
        g->attrs[i].underline = 0; g->attrs[i].bold = 0;
        g->attrs[i].italic = 0; g->attrs[i].pad = 0;
    }
    return 0;
}

static void br_grid_destroy(struct br_grid *g)
{
    if (g->cells)  free(g->cells);
    if (g->glyphs) free(g->glyphs);
    if (g->attrs)  free(g->attrs);
    g->cells = g->glyphs = 0; g->attrs = 0;
}

/* Place an ASCII byte into a cell, no-op on out-of-range. */
static __attribute__((unused)) void br_put(struct br_grid *g, int col, int row, char ch)
{
    if (col < 0 || row < 0 || col >= g->cols || row >= g->rows) return;
    int i = row * g->cols + col;
    g->cells[i] = ch;
    g->glyphs[i * 4] = 0;
}

/* Place a UTF-8 multi-byte character into a cell.  Caller passes
 * the bytes and the byte count (1..4).  ASCII goes through
 * cells[]; multibyte goes through glyphs[]. */
static void br_put_utf8(struct br_grid *g, int col, int row,
                         const char *bytes, int nbytes)
{
    if (col < 0 || row < 0 || col >= g->cols || row >= g->rows) return;
    if (nbytes <= 0) return;
    int i = row * g->cols + col;
    if (nbytes == 1 && (unsigned char)bytes[0] < 0x80) {
        g->cells[i] = bytes[0];
        g->glyphs[i * 4] = 0;
        return;
    }
    /* Multi-byte glyph.  Sentinel '\x01' in cells[] means "look in
     * glyphs[] for the real bytes". */
    g->cells[i] = 0x01;
    int copy = nbytes > 4 ? 4 : nbytes;
    for (int k = 0; k < copy; k++) g->glyphs[i * 4 + k] = bytes[k];
    for (int k = copy; k < 4; k++) g->glyphs[i * 4 + k] = 0;
}

/* Decode one UTF-8 code point from s[0..n).  Returns the number of
 * bytes consumed (1..4), or 1 on a decode error (the byte itself).
 * Outputs nothing — we only need the byte count to advance through
 * the source string a code-point at a time. */
static int utf8_chunk_len(const char *s, int n)
{
    if (n <= 0) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return n >= 2 ? 2 : 1;
    if ((c & 0xF0) == 0xE0) return n >= 3 ? 3 : 1;
    if ((c & 0xF8) == 0xF0) return n >= 4 ? 4 : 1;
    return 1;
}

/* Plot a single TEXT paint command into the grid.
 *
 * Text-mode is character-cell.  Layout's glyph advance is now
 * floored at one cell (8 px) — see layout_glyph_width — so
 * stepping `xoff += gw` lands every codepoint in a distinct cell
 * and we never need the inter-glyph collision handling we used
 * before that floor was in place. */
static void render_text(struct br_grid *g, const struct layout_paint_cmd *c)
{
    if (!c->text || c->text_len <= 0) return;
    int fs = c->font_size_px > 0 ? c->font_size_px : 16;
    int gw = fs / 2;
    if (gw < BR_CELL_W) gw = BR_CELL_W;
    int row = c->y / BR_CELL_H;
    int xoff = 0;
    int i = 0;
    while (i < c->text_len) {
        int chunk = utf8_chunk_len(c->text + i, c->text_len - i);
        int col = (c->x + xoff) / BR_CELL_W;
        br_put_utf8(g, col, row, c->text + i, chunk);
        if (g->attrs && col >= 0 && col < g->cols && row >= 0 && row < g->rows) {
            int idx = row * g->cols + col;
            g->attrs[idx].fg     = c->color;
            g->attrs[idx].bold   = (c->font_weight >= 600);
            g->attrs[idx].italic = (c->font_style != 0);
        }
        i    += chunk;
        xoff += gw;
    }
}

/* Plot a thin border RECT into the grid as a horizontal or
 * vertical run of box-drawing characters.  Wide-and-tall rects
 * are background fills and skipped. */
static void render_rect(struct br_grid *g, const struct layout_paint_cmd *c)
{
    int thin_h = (c->h <= 4 && c->w >= 8);   /* horizontal edge   */
    int thin_v = (c->w <= 4 && c->h >= 8);   /* vertical edge     */
    if (!thin_h && !thin_v) {
        /* In ANSI mode, paint background as a per-cell colour. */
        if (g->attrs && (c->color >> 24) != 0 &&
            c->w >= BR_CELL_W && c->h >= BR_CELL_H) {
            int col0 = c->x / BR_CELL_W;
            int row0 = c->y / BR_CELL_H;
            int col1 = (c->x + c->w) / BR_CELL_W;
            int row1 = (c->y + c->h) / BR_CELL_H;
            if (col1 > g->cols) col1 = g->cols;
            if (row1 > g->rows) row1 = g->rows;
            for (int r = row0; r < row1; r++)
                for (int co = col0; co < col1; co++) {
                    if (r < 0 || co < 0) continue;
                    g->attrs[r * g->cols + co].bg = c->color;
                }
        }
        return;
    }
    if (thin_h) {
        int row = c->y / BR_CELL_H;
        int col0 = c->x / BR_CELL_W;
        int col1 = (c->x + c->w) / BR_CELL_W;
        for (int co = col0; co < col1; co++) {
            /* '─' = U+2500 = 0xE2 0x94 0x80 */
            char b[3] = {(char)0xE2, (char)0x94, (char)0x80};
            br_put_utf8(g, co, row, b, 3);
        }
    }
    if (thin_v) {
        int col = c->x / BR_CELL_W;
        int row0 = c->y / BR_CELL_H;
        int row1 = (c->y + c->h) / BR_CELL_H;
        for (int ro = row0; ro < row1; ro++) {
            /* '│' = U+2502 = 0xE2 0x94 0x82 */
            char b[3] = {(char)0xE2, (char)0x94, (char)0x82};
            br_put_utf8(g, col, ro, b, 3);
        }
    }
}

/* Plot UNDERLINE.  In plain mode skipped; in ANSI mode it sets
 * the underline attribute on all covered cells (so the renderer
 * will emit ESC[4m around the run). */
static void render_underline(struct br_grid *g, const struct layout_paint_cmd *c)
{
    if (!g->attrs) return;
    /* Underline y is just below the glyph baseline; map to the
     * same row as the text by stepping up one cell. */
    int row = (c->y - 1) / BR_CELL_H;
    if (row < 0) row = 0;
    int col0 = c->x / BR_CELL_W;
    int col1 = (c->x + c->w + BR_CELL_W - 1) / BR_CELL_W;
    if (row >= g->rows) return;
    if (col1 > g->cols) col1 = g->cols;
    for (int co = col0; co < col1; co++) {
        if (co < 0) continue;
        g->attrs[row * g->cols + co].underline = 1;
    }
}

static void render_paints(struct br_grid *g, const struct layout_paint_buf *p)
{
    /* Two-pass: backgrounds + borders first, then text on top, so
     * our "later wins" rule lets text show through borders.  The
     * paint stream is already in the right back-to-front order
     * (parents before children), so a single pass works fine — we
     * keep it as one for clarity. */
    for (int i = 0; i < p->n; i++) {
        const struct layout_paint_cmd *c = &p->cmds[i];
        switch (c->kind) {
        case LAY_PAINT_RECT:      render_rect(g, c);      break;
        case LAY_PAINT_TEXT:      render_text(g, c);      break;
        case LAY_PAINT_UNDERLINE: render_underline(g, c); break;
        default: break;
        }
    }
}

/* Decide which rows actually contain non-blank content.  We prune
 * trailing all-blank rows so an HTML page with a tall background
 * doesn't spew dozens of empty lines at the end. */
static int last_used_row(const struct br_grid *g)
{
    int last = -1;
    for (int r = 0; r < g->rows; r++) {
        int row_used = 0;
        for (int co = 0; co < g->cols; co++) {
            char ch = g->cells[r * g->cols + co];
            if (ch != ' ' && ch != 0) { row_used = 1; break; }
        }
        if (g->attrs && !row_used) {
            for (int co = 0; co < g->cols; co++) {
                struct br_attr *a = &g->attrs[r * g->cols + co];
                if (a->bg != 0) { row_used = 1; break; }
            }
        }
        if (row_used) last = r;
    }
    return last;
}

/* Print one cell.  In ANSI mode wrap with escape sequences. */
static void print_cell_plain(const struct br_grid *g, int col, int row)
{
    int i = row * g->cols + col;
    char ch = g->cells[i];
    if (ch == 0x01) {
        /* Multi-byte glyph in overflow buffer. */
        int j = i * 4;
        int n = 0;
        for (; n < 4 && g->glyphs[j + n]; n++) {}
        if (n == 0) { write(1, " ", 1); }
        else        { write(1, g->glyphs + j, (unsigned long)n); }
    } else if (ch == 0) {
        write(1, " ", 1);
    } else {
        write(1, &ch, 1);
    }
}

/* ANSI 24-bit colour escape: ESC [ 38;2;R;G;B m for fg,
 *                            ESC [ 48;2;R;G;B m for bg. */
static void emit_fg_24(uint32_t c)
{
    int r = (c >> 16) & 0xFF, gg = (c >> 8) & 0xFF, b = c & 0xFF;
    printf("\x1b[38;2;%d;%d;%dm", r, gg, b);
}
static void emit_bg_24(uint32_t c)
{
    int r = (c >> 16) & 0xFF, gg = (c >> 8) & 0xFF, b = c & 0xFF;
    printf("\x1b[48;2;%d;%d;%dm", r, gg, b);
}
static void emit_reset(void) { printf("\x1b[0m"); }

static void render_grid_plain(const struct br_grid *g)
{
    int last = last_used_row(g);
    if (last < 0) {
        printf("[browser] (page has no visible text)\n");
        return;
    }
    /* Top border. */
    write(1, "+", 1);
    for (int co = 0; co < g->cols; co++) write(1, "-", 1);
    write(1, "+\n", 2);
    for (int r = 0; r <= last; r++) {
        write(1, "|", 1);
        for (int co = 0; co < g->cols; co++)
            print_cell_plain(g, co, r);
        write(1, "|\n", 2);
    }
    write(1, "+", 1);
    for (int co = 0; co < g->cols; co++) write(1, "-", 1);
    write(1, "+\n", 2);
}

static void render_grid_ansi(const struct br_grid *g)
{
    int last = last_used_row(g);
    if (last < 0) {
        printf("[browser] (page has no visible text)\n");
        return;
    }
    /* No frame in ANSI mode — let the colors carry the page. */
    for (int r = 0; r <= last; r++) {
        uint32_t cur_fg = 0, cur_bg = 0;
        int      cur_ul = 0, cur_b = 0, cur_i = 0;
        int      attr_active = 0;
        for (int co = 0; co < g->cols; co++) {
            struct br_attr *a = &g->attrs[r * g->cols + co];
            int need_change = (a->fg != cur_fg) || (a->bg != cur_bg) ||
                              (a->underline != cur_ul) ||
                              (a->bold != cur_b) || (a->italic != cur_i);
            if (need_change) {
                if (attr_active) emit_reset();
                attr_active = 0;
                if (a->bg) { emit_bg_24(a->bg); attr_active = 1; }
                if (a->fg) { emit_fg_24(a->fg); attr_active = 1; }
                if (a->underline) { printf("\x1b[4m"); attr_active = 1; }
                if (a->bold)      { printf("\x1b[1m"); attr_active = 1; }
                if (a->italic)    { printf("\x1b[3m"); attr_active = 1; }
                cur_fg = a->fg; cur_bg = a->bg;
                cur_ul = a->underline; cur_b = a->bold; cur_i = a->italic;
            }
            print_cell_plain(g, co, r);
        }
        if (attr_active) emit_reset();
        write(1, "\n", 1);
    }
}

/* ----------------------------------------------------------------
 * GUI renderer.
 *
 * Opens a real window via the M40-era GUI syscalls and walks the
 * paint stream issuing one gui_fill_rect / gui_draw_text per
 * command, translated by the current scroll offset and clipped to
 * the visible viewport.  This is the ONLY renderer in the codebase
 * that produces pixel-faithful output of what layout.h thinks the
 * page should look like — the plain / ANSI text renderers are
 * informational, this one is the real thing.
 *
 * Scrolling: arrow keys (16 px), space / 'b' (page), Home / End,
 * ESC or window-close to quit.  Repaint on every scroll change.
 *
 * Limitations honoured from M62:
 *   - kernel font is fixed 8x16; large text reads as plain 16-px
 *     text but with the layout-defined extra spacing around it.
 *   - non-ASCII bullet glyphs (U+2022 = "\xE2\x80\xA2") are folded
 *     to '*' so they render legibly under the kernel font.
 * ---------------------------------------------------------------- */

/* Convert layout's 0xAARRGGBB to the GUI's BGRA pixel word
 * (low byte = B, then G, then R, ignored A).  We drop alpha
 * entirely — the GUI surface is opaque and the WM owns all
 * compositing decisions. */
static uint32_t color_to_bgra(uint32_t aarrggbb)
{
    uint32_t r = (aarrggbb >> 16) & 0xFF;
    uint32_t g = (aarrggbb >>  8) & 0xFF;
    uint32_t b =  aarrggbb        & 0xFF;
    return GUI_BGRA(r, g, b);
}

/* Copy one TEXT paint into a small NUL-terminated buffer for
 * gui_draw_text, folding any non-ASCII byte to '*' (the kernel
 * font is 8-bit ASCII).  Return number of glyph cells written.
 * Bytes that are part of a UTF-8 multi-byte sequence are folded
 * collectively: leading byte → '*', continuation bytes → skipped. */
static int copy_text_for_gui(const struct layout_paint_cmd *c,
                              char *out, int out_cap)
{
    int n = 0;
    int i = 0;
    while (i < c->text_len && n < out_cap - 1) {
        unsigned char b = (unsigned char)c->text[i];
        if (b < 0x80) {
            /* Drop control bytes (incl. NUL) — never legal page text. */
            if (b >= 0x20 || b == '\t') out[n++] = (char)b;
            i++;
        } else {
            /* Leading byte of a multibyte sequence; emit '*' for it
             * and skip continuation bytes (10xxxxxx). */
            out[n++] = '*';
            i++;
            while (i < c->text_len &&
                   ((unsigned char)c->text[i] & 0xC0) == 0x80) i++;
        }
    }
    out[n] = '\0';
    return n;
}

/* ----------------------------------------------------------------
 * Toolbar layout (milestone-64).
 *
 * The toolbar is the strip across the top of the window.  It
 * contains, left-to-right:
 *
 *   [<]  back button     (BACK_X .. BACK_X + BTN_W)
 *   [>]  forward button  (FWD_X  .. FWD_X  + BTN_W)
 *   [O]  reload button   (RELOAD_X .. RELOAD_X + BTN_W)
 *   URL field             (URL_X .. URL_RIGHT_END)
 *   scroll % indicator    (right-anchored)
 *
 * Heights / widths are fixed.  The URL field stretches to fill
 * the gap between the reload button and the scroll indicator.
 *
 * BR_GUI_STATUS_H is the strip's pixel height; everything below
 * it is page content.
 * ---------------------------------------------------------------- */

#define BR_GUI_STATUS_H   28
#define BR_GUI_DEFAULT_H  720

#define BR_TB_PAD          4
#define BR_TB_BTN_W       24
#define BR_TB_BTN_H       20
#define BR_TB_BTN_Y        4
#define BR_TB_BACK_X       BR_TB_PAD
#define BR_TB_FWD_X       (BR_TB_BACK_X + BR_TB_BTN_W + 2)
#define BR_TB_RELOAD_X    (BR_TB_FWD_X  + BR_TB_BTN_W + 2)
#define BR_TB_URL_X       (BR_TB_RELOAD_X + BR_TB_BTN_W + 6)
/* Width of the right-anchored "0%/0%" indicator in characters
 * including '/' and final '%'.  Each glyph is 8 px wide so the
 * pixel reservation is BR_TB_IND_CHARS * 8 + small margin. */
#define BR_TB_IND_CHARS   10

/* Render a button.  Filled rect + 1-px border + centered glyph(s).
 * `enabled` controls the foreground color so disabled buttons
 * are visibly dim. */
static void render_tb_button(int win_id, int x, int y,
                              int w, int h,
                              const char *label,
                              int enabled, int hover)
{
    uint32_t face = hover ? GUI_BGRA(80, 80, 110)
                          : GUI_BGRA(56, 56, 80);
    uint32_t border = GUI_BGRA(120, 120, 160);
    uint32_t fg = enabled ? GUI_BGRA(230, 230, 245)
                          : GUI_BGRA(110, 110, 140);
    gui_fill_rect(win_id, (uint32_t)x, (uint32_t)y,
                  (uint32_t)w, (uint32_t)h, face);
    /* 1-px border drawn as four thin rects. */
    gui_fill_rect(win_id, (uint32_t)x, (uint32_t)y,
                  (uint32_t)w, 1, border);
    gui_fill_rect(win_id, (uint32_t)x, (uint32_t)(y + h - 1),
                  (uint32_t)w, 1, border);
    gui_fill_rect(win_id, (uint32_t)x, (uint32_t)y,
                  1, (uint32_t)h, border);
    gui_fill_rect(win_id, (uint32_t)(x + w - 1), (uint32_t)y,
                  1, (uint32_t)h, border);
    /* Center the label horizontally; vertical center fixed for an
     * 8x16 glyph inside a 20-px-tall button (pad ~2 above). */
    int n = 0; while (label[n]) n++;
    int tx = x + (w - n * 8) / 2;
    int ty = y + (h - 16) / 2;
    if (tx < x + 1) tx = x + 1;
    if (ty < y + 1) ty = y + 1;
    gui_draw_text(win_id, (uint32_t)tx, (uint32_t)ty, label,
                  fg, face, 0);
}

/* Render the toolbar: back/fwd/reload buttons + editable URL bar
 * + right-anchored scroll-position indicator.
 *
 * url_buf / url_len are the address bar contents; cursor is the
 * insertion-point index when focused (-1 = not focused, no caret).
 * back_ok / fwd_ok control button enable state.  hover_btn tells
 * which button (0..2) the mouse is over, or -1 for none. */
static void render_toolbar(int win_id, int win_w,
                            const char *url_buf, int url_len, int cursor,
                            int back_ok, int fwd_ok,
                            int hover_btn,
                            int scroll_x, int scroll_y,
                            int doc_w,    int doc_h,
                            int viewport_w, int viewport_h)
{
    uint32_t bar_bg  = GUI_BGRA(  32,  32,  48);
    uint32_t url_bg  = GUI_BGRA( 245, 245, 250);
    uint32_t url_fg  = GUI_BGRA(  20,  20,  40);
    uint32_t url_brd = GUI_BGRA( 120, 120, 160);
    uint32_t bar_dim = GUI_BGRA(160, 160, 200);

    gui_fill_rect(win_id, 0, 0, (uint32_t)win_w, BR_GUI_STATUS_H, bar_bg);

    render_tb_button(win_id, BR_TB_BACK_X,   BR_TB_BTN_Y,
                     BR_TB_BTN_W, BR_TB_BTN_H, "<", back_ok,
                     hover_btn == 0);
    render_tb_button(win_id, BR_TB_FWD_X,    BR_TB_BTN_Y,
                     BR_TB_BTN_W, BR_TB_BTN_H, ">", fwd_ok,
                     hover_btn == 1);
    render_tb_button(win_id, BR_TB_RELOAD_X, BR_TB_BTN_Y,
                     BR_TB_BTN_W, BR_TB_BTN_H, "R", 1,
                     hover_btn == 2);

    /* Scroll indicator on the right (same format as the old
     * status bar so the on-screen vocabulary doesn't change). */
    char ind[16]; int ip = 0;
    int max_x = doc_w - viewport_w; if (max_x < 0) max_x = 0;
    int max_y = doc_h - viewport_h; if (max_y < 0) max_y = 0;
    int axes[2] = { max_x ? (int)(((long)scroll_x * 100L) / (long)max_x) : -1,
                    max_y ? (int)(((long)scroll_y * 100L) / (long)max_y) : -1 };
    for (int i = 0; i < 2; i++) {
        if (i) ind[ip++] = '/';
        int p = axes[i];
        if (p < 0) {
            ind[ip++] = '-';
        } else {
            if (p < 0)   p = 0;
            if (p > 100) p = 100;
            if (p == 100)      { ind[ip++] = '1'; ind[ip++] = '0'; ind[ip++] = '0'; }
            else if (p >= 10)  { ind[ip++] = (char)('0' + p / 10); ind[ip++] = (char)('0' + p % 10); }
            else               { ind[ip++] = (char)('0' + p); }
            ind[ip++] = '%';
        }
    }
    ind[ip] = '\0';
    int ind_px_w = ip * 8;
    int ind_x   = win_w - ind_px_w - BR_TB_PAD;
    if (ind_x < BR_TB_URL_X + 32) ind_x = BR_TB_URL_X + 32;
    gui_draw_text(win_id, (uint32_t)ind_x, (BR_GUI_STATUS_H - 16) / 2,
                  ind, bar_dim, bar_bg, 0);

    /* URL field: ranges from BR_TB_URL_X to (ind_x - small gap). */
    int url_field_x = BR_TB_URL_X;
    int url_field_w = ind_x - BR_TB_PAD - url_field_x;
    if (url_field_w < 80) url_field_w = 80;
    int url_field_y = BR_TB_BTN_Y;
    int url_field_h = BR_TB_BTN_H;

    /* Field background + border. */
    gui_fill_rect(win_id, (uint32_t)url_field_x, (uint32_t)url_field_y,
                  (uint32_t)url_field_w, (uint32_t)url_field_h, url_bg);
    gui_fill_rect(win_id, (uint32_t)url_field_x, (uint32_t)url_field_y,
                  (uint32_t)url_field_w, 1, url_brd);
    gui_fill_rect(win_id, (uint32_t)url_field_x, (uint32_t)(url_field_y + url_field_h - 1),
                  (uint32_t)url_field_w, 1, url_brd);
    gui_fill_rect(win_id, (uint32_t)url_field_x, (uint32_t)url_field_y,
                  1, (uint32_t)url_field_h, url_brd);
    gui_fill_rect(win_id, (uint32_t)(url_field_x + url_field_w - 1), (uint32_t)url_field_y,
                  1, (uint32_t)url_field_h, url_brd);

    /* Visible window: scroll horizontally so the cursor stays in
     * view.  Each glyph is 8 px wide; field interior is
     * (url_field_w - 6) px. */
    int interior_w   = url_field_w - 6;
    int max_visible  = interior_w / 8;
    if (max_visible < 1) max_visible = 1;
    int focus = (cursor >= 0);
    int caret = focus ? cursor : url_len;
    int win_start;
    if (url_len <= max_visible) win_start = 0;
    else if (caret <= max_visible - 1) win_start = 0;
    else if (caret >= url_len) win_start = url_len - max_visible + 1;
    else win_start = caret - (max_visible - 1);
    if (win_start < 0) win_start = 0;
    if (win_start > url_len) win_start = url_len;

    /* Render the visible substring one glyph at a time so we can
     * sanitize non-printable bytes. */
    int x_pen = url_field_x + 3;
    int y_pen = url_field_y + (url_field_h - 16) / 2;
    char one[2]; one[1] = '\0';
    int max_show = url_len - win_start;
    if (max_show > max_visible) max_show = max_visible;
    for (int k = 0; k < max_show; k++) {
        unsigned char b = (unsigned char)url_buf[win_start + k];
        one[0] = (b >= 0x20 && b < 0x7F) ? (char)b : '?';
        gui_draw_text(win_id, (uint32_t)(x_pen + k * 8),
                      (uint32_t)y_pen, one, url_fg, url_bg, 0);
    }
    /* Caret: a 1-px vertical bar at (caret - win_start) glyph pos.
     * Drawn ONLY when the URL field is focused. */
    if (focus) {
        int cx = x_pen + (caret - win_start) * 8;
        if (cx < url_field_x + 1) cx = url_field_x + 1;
        if (cx > url_field_x + url_field_w - 2)
            cx = url_field_x + url_field_w - 2;
        gui_fill_rect(win_id, (uint32_t)cx,
                      (uint32_t)(url_field_y + 2),
                      1, (uint32_t)(url_field_h - 4), url_fg);
    }
}

/* Render a single frame of the page: clear + walk paint stream
 * with translation by -scroll_y + status bar on top + flush. */
/* Render a single frame of the page: clear + walk paint stream
 * with translation by -(scroll_x, scroll_y) + status bar on top
 * + flush.  Items entirely outside the visible viewport are
 * culled; partial overlaps are clipped to the window content
 * rectangle so the WM never sees out-of-bounds coordinates. */

/* CSS rule: <body>'s background color propagates to the canvas
 * (the area outside the body's box).  Walk the layout tree from
 * the root, find the first <body> or <html> with a non-transparent
 * background, and return that color.  Returns 0 when none — caller
 * falls back to white.  This is what makes pages like
 * plaintextworld.com (body { background: black }) look right when
 * the body box is shorter than the viewport. */
static uint32_t br_find_canvas_bg(const struct layout_box *b)
{
    if (!b) return 0;
    if (b->dom && b->dom->tag && b->style &&
        (b->style->background >> 24) != 0) {
        if (css_streq(b->dom->tag, "body") ||
            css_streq(b->dom->tag, "html"))
            return b->style->background;
    }
    for (const struct layout_box *c = b->first_child; c; c = c->next_sibling) {
        uint32_t bg = br_find_canvas_bg(c);
        if (bg) return bg;
    }
    return 0;
}

static void render_gui_frame(int win_id, int win_w, int win_h,
                              const struct layout_doc *d,
                              const struct layout_paint_buf *pb,
                              int scroll_x, int scroll_y)
{
    int content_top = BR_GUI_STATUS_H;
    int content_h   = win_h - content_top;
    if (content_h < 1) content_h = 1;

    /* Page background: derived from <body>'s computed background
     * (CSS canvas-propagation rule).  Defaults to white when no
     * <body> bg is set so plain pages look like a sheet of paper. */
    uint32_t canvas_color = br_find_canvas_bg(d->root_box);
    uint32_t page_bg = canvas_color
        ? color_to_bgra(canvas_color)
        : GUI_BGRA(255, 255, 255);
    gui_fill_rect(win_id, 0, (uint32_t)content_top,
                  (uint32_t)win_w, (uint32_t)content_h, page_bg);

    char text_buf[256];

    for (int i = 0; i < pb->n; i++) {
        const struct layout_paint_cmd *c = &pb->cmds[i];

        /* Document-coords -> window-coords (translated for scroll
         * and shifted down past the status bar). */
        int x_doc_left  = c->x - scroll_x;
        int x_doc_right = x_doc_left + c->w;
        int y_doc_top    = c->y - scroll_y;
        int y_doc_bottom = y_doc_top + c->h;
        int y_win        = y_doc_top + content_top;

        /* Cull entirely-outside paints. */
        if (y_doc_bottom <= 0)            continue;
        if (y_doc_top   >= content_h)     continue;
        if (x_doc_right <= 0)             continue;
        if (x_doc_left  >= win_w)         continue;

        /* Crop top edge: if the box starts above the visible content
         * (y_doc_top < 0), shorten the top by that much so the WM
         * doesn't have to clip outside the content area. */
        int draw_y = y_win;
        int draw_h = c->h;
        if (y_doc_top < 0) { draw_h += y_doc_top; draw_y = content_top; }
        if (draw_y + draw_h > win_h) draw_h = win_h - draw_y;
        if (draw_h <= 0) continue;

        /* Crop left/right against window edges.  Track how many
         * pixels were trimmed off the LEFT (clip_left) so we can
         * skip whole glyphs that fell entirely outside the window
         * before drawing the visible suffix. */
        int draw_x = x_doc_left;
        int draw_w = c->w;
        int clip_left = 0;
        if (draw_x < 0) {
            clip_left = -draw_x;
            draw_w   -= clip_left;
            draw_x    = 0;
        }
        if (draw_x + draw_w > win_w) draw_w = win_w - draw_x;
        if (draw_w <= 0) continue;

        switch (c->kind) {
        case LAY_PAINT_RECT: {
            /* Skip transparent rects (alpha == 0). */
            if ((c->color >> 24) == 0) break;
            gui_fill_rect(win_id,
                          (uint32_t)draw_x,
                          (uint32_t)draw_y,
                          (uint32_t)draw_w,
                          (uint32_t)draw_h,
                          color_to_bgra(c->color));
            break;
        }
        case LAY_PAINT_TEXT: {
            int n = copy_text_for_gui(c, text_buf, (int)sizeof(text_buf));
            if (n <= 0) break;
            uint32_t fg = color_to_bgra(c->color);

            /* Layout assumed each glyph is `font_size_px / 2` wide
             * when it placed words on the line.  The kernel font,
             * however, is fixed 8 px wide.  When font_size_px != 16
             * a single gui_draw_text() call would advance every
             * character by 8 px, pushing this word's tail past the
             * x where layout placed the next word — characters from
             * adjacent words would then collide ("Smal greyfooter
             * -stylearagraph.").  When the layout-implied glyph
             * pitch differs from 8, fall back to one draw call per
             * character at the layout-implied positions. */
            int pitch = c->font_size_px / 2;
            if (pitch < LAYOUT_BASE_GLYPH_W) pitch = LAYOUT_BASE_GLYPH_W;
            if (pitch == BR_CELL_W && clip_left == 0) {
                /* Whole text fits on the left, fast path. */
                gui_draw_text(win_id,
                              (uint32_t)draw_x,
                              (uint32_t)draw_y,
                              text_buf,
                              fg,
                              0,
                              1);
            } else {
                /* Per-glyph: place each at its layout-implied x,
                 * skipping ones that fall outside the window. */
                int x_off = (pitch > BR_CELL_W) ? (pitch - BR_CELL_W) / 2 : 0;
                int doc_x0 = c->x - scroll_x;     /* unclamped left */
                char one[2]; one[1] = '\0';
                for (int k = 0; k < n; k++) {
                    int gx = doc_x0 + k * pitch + x_off;
                    if (gx + BR_CELL_W <= 0) continue;
                    if (gx       >= win_w) break;
                    int gx_clamped = gx < 0 ? 0 : gx;
                    one[0] = text_buf[k];
                    gui_draw_text(win_id,
                                  (uint32_t)gx_clamped,
                                  (uint32_t)draw_y,
                                  one,
                                  fg,
                                  0,
                                  1);
                }
            }
            break;
        }
        case LAY_PAINT_UNDERLINE: {
            /* 1-px high coloured bar.  Use draw_h capped to 2 to
             * avoid drawing a thick band if the layout produced a
             * tall underline (it shouldn't, but defend against it). */
            int uh = draw_h > 2 ? 2 : draw_h;
            gui_fill_rect(win_id,
                          (uint32_t)draw_x,
                          (uint32_t)draw_y,
                          (uint32_t)draw_w,
                          (uint32_t)uh,
                          color_to_bgra(c->color));
            break;
        }
        case LAY_PAINT_IMAGE: {
            /* Blit a sub-rect of the cached BGRA image at the
             * box's clipped pixel coords.  We always copy through
             * a tightly-packed temporary because gui_present's
             * `src` argument is `w*h*4` contiguous bytes — we
             * can't pass an interior pointer of a wider image
             * (the kernel would walk straight off the end of the
             * row).  For images that fit fully inside the window
             * with no clipping the copy is the entire image, so
             * the cost is one extra w*h*4 traversal per frame.
             *
             * The image's intrinsic size is (image_w, image_h);
             * the layout box's size is (c->w, c->h) which the
             * layout pass usually set to the intrinsic size as
             * well.  We blit at intrinsic size and clip — no
             * scaling.  If a future page sets <img width=...>
             * larger than the intrinsic size the excess gets
             * the page background colour painted later by the
             * adjacent rects (or, more typically, just shows
             * empty since we drew page_bg first). */
            if (!c->image_pixels || c->image_w <= 0 || c->image_h <= 0) break;
            int clip_top = (y_doc_top < 0) ? -y_doc_top : 0;
            int img_w = c->image_w;
            int img_h = c->image_h;
            int blit_w = draw_w;
            int blit_h = draw_h;
            if (clip_left + blit_w > img_w) blit_w = img_w - clip_left;
            if (clip_top  + blit_h > img_h) blit_h = img_h  - clip_top;
            if (blit_w <= 0 || blit_h <= 0) break;
            uint8_t *tmp = (uint8_t *)malloc((size_t)blit_w *
                                              (size_t)blit_h * 4);
            if (!tmp) break;
            const uint8_t *src = (const uint8_t *)c->image_pixels;
            /* Alpha-blend over the page background colour so
             * transparent PNG pixels don't show as opaque black.
             * Composition: out = src.rgb * src.a/255 + bg.rgb *
             * (255 - src.a)/255.  We use page_bg's BGRA bytes
             * directly. */
            uint8_t bg_b = (uint8_t)( page_bg        & 0xFF);
            uint8_t bg_g = (uint8_t)((page_bg >>  8) & 0xFF);
            uint8_t bg_r = (uint8_t)((page_bg >> 16) & 0xFF);
            for (int oy = 0; oy < blit_h; oy++) {
                const uint8_t *srow = src
                    + (size_t)(oy + clip_top) * (size_t)img_w * 4
                    + (size_t)clip_left * 4;
                uint8_t *drow = tmp + (size_t)oy * (size_t)blit_w * 4;
                for (int ox = 0; ox < blit_w; ox++) {
                    uint8_t sb = srow[ox * 4 + 0];
                    uint8_t sg = srow[ox * 4 + 1];
                    uint8_t sr = srow[ox * 4 + 2];
                    uint8_t sa = srow[ox * 4 + 3];
                    if (sa == 0xFF) {
                        drow[ox * 4 + 0] = sb;
                        drow[ox * 4 + 1] = sg;
                        drow[ox * 4 + 2] = sr;
                        drow[ox * 4 + 3] = 0xFF;
                    } else if (sa == 0) {
                        drow[ox * 4 + 0] = bg_b;
                        drow[ox * 4 + 1] = bg_g;
                        drow[ox * 4 + 2] = bg_r;
                        drow[ox * 4 + 3] = 0xFF;
                    } else {
                        int ia = 255 - sa;
                        drow[ox * 4 + 0] = (uint8_t)((sb * sa + bg_b * ia) / 255);
                        drow[ox * 4 + 1] = (uint8_t)((sg * sa + bg_g * ia) / 255);
                        drow[ox * 4 + 2] = (uint8_t)((sr * sa + bg_r * ia) / 255);
                        drow[ox * 4 + 3] = 0xFF;
                    }
                }
            }
            gui_present(win_id,
                        (uint32_t)draw_x,
                        (uint32_t)draw_y,
                        (uint32_t)blit_w,
                        (uint32_t)blit_h,
                        tmp);
            free(tmp);
            break;
        }
        default: break;
        }
    }

    /* Toolbar is drawn separately by the caller (run_gui), since it
     * owns the URL bar text + button hover state.  The caller is
     * also responsible for the trailing gui_flush. */
}

/* When non-zero, print uptime_ms() deltas at every pipeline
 * stage so we can profile cold-start latency end-to-end.  Off
 * by default; enabled with `--timing` or env BROWSER_TIMING=1.
 * Defined here (above run_gui) so the GUI render loop can read
 * it too. */
static int g_timing = 0;

#define BR_TIMING(msg, t0)                                                  \
    do {                                                                    \
        if (g_timing) {                                                     \
            unsigned long _now = uptime_ms();                               \
            printf("[timing] %-22s %lu ms\n", (msg), _now - (t0));         \
            (t0) = _now;                                                    \
        }                                                                   \
    } while (0)

/* ----------------------------------------------------------------
 *   M64 navigation: loaded_page, history, URL canonicalization,
 *   link hit-testing, and the rewritten run_gui event loop.
 * ---------------------------------------------------------------- */

#define BR_HISTORY_CAP   32
#define BR_URL_BUF_CAP   1024

/* Default proxy prefix.  When the user types a bare host like
 * "news.ycombinator.com" we prepend this so the request goes
 * through the M58 https-proxy (scripts/https_proxy.py) rather
 * than out to the real internet (which would need TLS we don't
 * have).  Override at startup by setting the BROWSER_PROXY env
 * variable to something else, e.g. "http://192.168.1.5:9000/". */
#define BR_DEFAULT_PROXY "http://10.0.2.2:8080/"

static char g_proxy_prefix[160] = BR_DEFAULT_PROXY;

/* All the per-page state we own.  Loaded by load_page(), freed by
 * free_page(). */
struct br_img_cache_entry {
    char    *url;            /* canonical URL we fetched the bytes from */
    uint8_t *bgra;            /* png_decode output; freed via png_free */
    int      w, h;
    struct br_img_cache_entry *next;
};

#define BR_IMG_CACHE_MAX_ENTRIES   16
#define BR_IMG_CACHE_MAX_BYTES     (4u * 1024u * 1024u)   /* 4 MiB */

struct loaded_page {
    char  *url;             /* canonical absolute URL we fetched */
    char  *html_buf;
    size_t html_len;
    char  *origin_url;      /* same as url for HTTP, NULL for files */
    struct html_token *scratch;
    struct dom        *dom; /* malloc'd; large struct */
    char  *author_css;
    int    author_len;
    struct layout_doc       ldoc;
    struct layout_paint_buf pb;
    int    doc_built;
    int    pb_built;
    /* For error pages: dom is still built (a tiny synthetic page
     * with the error message), so the GUI can render normally. */

    /* Image cache: lazily-decoded BGRA buffers for every <img> we've
     * been asked to render.  Borrowed pointers into these entries
     * are stuffed into layout_box::replaced_pixels by
     * br_attach_images().  free_page tears them all down.  See
     * chapter 97 for the cap policy. */
    struct br_img_cache_entry *img_cache;
    int   img_cache_count;
    unsigned img_cache_bytes;
};

static char *br_strdup(const char *s)
{
    if (!s) return 0;
    size_t n = br_strlen(s);
    char *o = (char *)malloc(n + 1);
    if (!o) return 0;
    for (size_t i = 0; i <= n; i++) o[i] = s[i];
    return o;
}

static void free_page(struct loaded_page *p)
{
    if (!p) return;
    if (p->pb_built)  { layout_paint_buf_destroy(&p->pb);  p->pb_built  = 0; }
    if (p->doc_built) { layout_doc_destroy(&p->ldoc);      p->doc_built = 0; }
    if (p->author_css) free(p->author_css);
    if (p->dom)        { dom_destroy(p->dom); free(p->dom); }
    if (p->scratch)    free(p->scratch);
    if (p->html_buf)   free(p->html_buf);
    if (p->origin_url) free(p->origin_url);
    if (p->url)        free(p->url);
    /* Tear down image cache. */
    {
        struct br_img_cache_entry *e = p->img_cache;
        while (e) {
            struct br_img_cache_entry *n = e->next;
            if (e->url)  free(e->url);
            if (e->bgra) png_free(e->bgra);
            free(e);
            e = n;
        }
    }
    free(p);
}

/* Build a tiny synthetic HTML document showing an error message,
 * so a failed fetch still produces a viewable page rather than
 * exiting the browser.  Returns malloc'd HTML bytes via *out. */
static char *make_error_html(const char *url, const char *msg, size_t *out_len)
{
    /* Construct: "<html><body><h2>browser: error</h2><p>...url...</p>
     *               <p>...msg...</p></body></html>" */
    static const char *pre  = "<html><body><h2>browser: error</h2><p>";
    static const char *mid  = "</p><p>";
    static const char *post = "</p></body></html>";
    size_t plen = br_strlen(pre);
    size_t mlen = br_strlen(mid);
    size_t qlen = br_strlen(post);
    size_t ulen = url ? br_strlen(url) : 0;
    size_t slen = msg ? br_strlen(msg) : 0;
    size_t total = plen + ulen + mlen + slen + qlen;
    char *buf = (char *)malloc(total + 1);
    if (!buf) return 0;
    size_t off = 0;
    for (size_t i = 0; i < plen; i++) buf[off++] = pre[i];
    for (size_t i = 0; i < ulen; i++) buf[off++] = url[i];
    for (size_t i = 0; i < mlen; i++) buf[off++] = mid[i];
    for (size_t i = 0; i < slen; i++) buf[off++] = msg[i];
    for (size_t i = 0; i < qlen; i++) buf[off++] = post[i];
    buf[off] = '\0';
    *out_len = off;
    return buf;
}

/* Run the parse + layout pipeline on bytes already in memory.
 * Populates p->{scratch,dom,author_css,ldoc,pb,doc_built,pb_built}.
 * Caller must have set p->html_buf, p->html_len, p->origin_url.
 * Returns 0 on success, -1 on OOM (caller frees p). */
static int build_page_from_html(struct loaded_page *p, int viewport)
{
    p->scratch = (struct html_token *)malloc(sizeof(*p->scratch));
    if (!p->scratch) return -1;

    struct html_tokenizer tz;
    html_tok_init(&tz, p->html_buf, p->html_len);

    p->dom = (struct dom *)malloc(sizeof(*p->dom));
    if (!p->dom) return -1;
    if (dom_init(p->dom) < 0) {
        free(p->dom); p->dom = 0;
        return -1;
    }
    int drc = dom_build(p->dom, &tz, p->scratch);
    if (drc < 0)
        printf("[browser] dom_build returned %d (continuing)\n", drc);

    p->author_len = 0;
    p->author_css = layout_collect_inline_styles(dom_root(p->dom),
                                                  &p->author_len);

    size_t link_css_len = 0;
    char *link_css = fetch_external_stylesheets(dom_root(p->dom),
                                                  p->origin_url,
                                                  &link_css_len);
    if (link_css && link_css_len > 0) {
        size_t need = (size_t)p->author_len + link_css_len + 2;
        char *combined = (char *)malloc(need);
        if (combined) {
            size_t off = 0;
            if (p->author_css && p->author_len > 0) {
                for (int i = 0; i < p->author_len; i++)
                    combined[off++] = p->author_css[i];
                combined[off++] = '\n';
            }
            for (size_t i = 0; i < link_css_len; i++)
                combined[off++] = link_css[i];
            combined[off] = '\0';
            if (p->author_css) free(p->author_css);
            p->author_css = combined;
            p->author_len = (int)off;
        }
        free(link_css);
    }

    if (layout_build_and_run(&p->ldoc, dom_root(p->dom),
                              p->author_css, p->author_len, viewport) < 0)
        return -1;
    p->doc_built = 1;
    layout_paint_collect(&p->ldoc, &p->pb);
    p->pb_built = 1;
    return 0;
}

/* ----------------------------------------------------------------
 *   Image cache + <img> resolution (chapter 97).
 *
 *   For every <img src="..."> in the rendered document we want
 *   to hand the layout box a decoded BGRA buffer to blit.  This
 *   walks the box tree, fetches each unique src once, png_decodes
 *   it, and stuffs a borrowed pointer into b->replaced_pixels.
 *   Failures are non-fatal — the layout falls back to its grey
 *   placeholder + alt text.
 *
 *   Resolution rules:
 *     * If the page itself was loaded over HTTP (origin_url set),
 *       resolve_url() handles relative / absolute / root paths.
 *     * If the page was loaded from a file path, treat src as a
 *       file path.  Convention: a leading '/' becomes a path
 *       under /mnt (the OSFS mount), and anything else is
 *       resolved against the page's directory.
 *
 *   Cache cap: BR_IMG_CACHE_MAX_ENTRIES entries OR
 *   BR_IMG_CACHE_MAX_BYTES total, whichever hits first.  When
 *   we hit the cap we just skip newcomers (don't evict) — sites
 *   pulling in 100 PNGs aren't supported yet.
 * ---------------------------------------------------------------- */

static struct br_img_cache_entry *br_img_cache_lookup(struct loaded_page *p,
                                                       const char *url)
{
    for (struct br_img_cache_entry *e = p->img_cache; e; e = e->next) {
        if (e->url && br_streq(e->url, url)) return e;
    }
    return 0;
}

/* Resolve a relative <img src> against the page's URL.  Returns
 * 0 on success (writes NUL-terminated abs URL to out), -1 on
 * overflow / bad input.
 *
 * For a file:// page (origin_url == NULL):
 *   * "http://..." -> verbatim
 *   * "/foo.png"   -> "/mnt/foo.png" (we want OSFS-rooted paths)
 *   * "foo.png"    -> "/mnt/foo.png" (same; we don't keep cwd state)
 * For an http:// page: delegate to resolve_url(). */
static int br_resolve_img_src(const char *page_url,
                              const char *page_origin,
                              const char *src,
                              char *out, size_t cap)
{
    (void)page_url;     /* reserved for future "../foo.png" walks */
    if (!src || !out || cap < 2) return -1;
    if (br_starts(src, "http://") || br_starts(src, "https://")) {
        size_t n = br_strlen(src);
        if (n + 1 > cap) return -1;
        for (size_t i = 0; i <= n; i++) out[i] = src[i];
        return 0;
    }
    if (page_origin) {
        return resolve_url(page_origin, src, out, cap);
    }
    /* file:// page.  Always rewrite into /mnt/<basename-of-src>.
     * We don't track the page's directory yet, so anything that
     * looks like a relative path is interpreted as living under
     * /mnt alongside our test pages. */
    static const char prefix[] = "/mnt/";
    size_t pn = sizeof(prefix) - 1;
    const char *tail = src;
    if (src[0] == '/') tail = src + 1;          /* drop leading '/' */
    size_t tl = br_strlen(tail);
    if (pn + tl + 1 > cap) return -1;
    for (size_t i = 0; i < pn; i++) out[i] = prefix[i];
    for (size_t i = 0; i <= tl; i++) out[pn + i] = tail[i];
    /* Avoid an empty/duplicate "/mnt//mnt/foo.png" if the caller
     * already passed us "/mnt/foo.png" — trivial check on the
     * common case. */
    if (br_starts(tail, "mnt/")) {
        const char *t2 = tail + 4;
        size_t tl2 = br_strlen(t2);
        if (pn + tl2 + 1 > cap) return -1;
        for (size_t i = 0; i <= tl2; i++) out[pn + i] = t2[i];
    }
    return 0;
}

/* Fetch + decode a single image; install in the cache.  On
 * success returns the cache entry; on any failure returns NULL
 * after logging once. */
static struct br_img_cache_entry *br_img_cache_load(struct loaded_page *p,
                                                     const char *url)
{
    if (p->img_cache_count >= BR_IMG_CACHE_MAX_ENTRIES) {
        printf("[browser] image cache full (%d entries); skipping %s\n",
               p->img_cache_count, url);
        return 0;
    }

    /* Fetch the bytes.  We can ignore the origin URL output here
     * — image fetches don't follow chains. */
    size_t blen = 0;
    char *origin = 0;
    char *bytes = fetch(url, &blen, &origin);
    if (origin) free(origin);
    if (!bytes || blen < 8) {
        if (bytes) free(bytes);
        printf("[browser] image fetch failed for %s\n", url);
        return 0;
    }

    uint8_t *bgra = 0;
    int w = 0, h = 0;
    int rc = png_decode((const uint8_t *)bytes, blen, &bgra, &w, &h);
    free(bytes);
    if (rc < 0 || !bgra) {
        printf("[browser] png_decode failed for %s\n", url);
        return 0;
    }
    unsigned bytes_used = (unsigned)w * (unsigned)h * 4u;
    if (p->img_cache_bytes + bytes_used > BR_IMG_CACHE_MAX_BYTES) {
        printf("[browser] image cache size cap (%u + %u > %u); "
               "dropping %s\n",
               p->img_cache_bytes, bytes_used,
               (unsigned)BR_IMG_CACHE_MAX_BYTES, url);
        png_free(bgra);
        return 0;
    }

    struct br_img_cache_entry *e =
        (struct br_img_cache_entry *)malloc(sizeof(*e));
    if (!e) { png_free(bgra); return 0; }
    e->url = br_strdup(url);
    e->bgra = bgra;
    e->w = w; e->h = h;
    e->next = p->img_cache;
    p->img_cache = e;
    p->img_cache_count++;
    p->img_cache_bytes += bytes_used;
    return e;
}

/* Walk the box tree; for each REPLACED <img> with an `src` attr
 * fetch+decode the image (or look it up in the cache) and stuff
 * the BGRA pointer into b->replaced_pixels.
 *
 * `lookup_only` skips br_img_cache_load — used by the parser
 * thread, where mutating the cache would race with the GUI
 * thread.  On resize this is always safe: the GUI's initial
 * load already populated the cache, so every <img> we re-attach
 * is a lookup hit. */
static void br_attach_images_walk(struct loaded_page *p,
                                  struct layout_box *b,
                                  int lookup_only)
{
    if (!b) return;
    if (b->kind == LAY_BOX_REPLACED && b->dom &&
        b->dom->type == DOM_NODE_ELEMENT && b->dom->tag) {
        const char *t = b->dom->tag;
        int is_img = (t[0] == 'i' || t[0] == 'I') &&
                     (t[1] == 'm' || t[1] == 'M') &&
                     (t[2] == 'g' || t[2] == 'G') && t[3] == '\0';
        if (is_img) {
            const char *src = dom_node_attr(b->dom, "src");
            if (src && src[0]) {
                char abs_url[1024];
                if (br_resolve_img_src(p->url, p->origin_url, src,
                                       abs_url, sizeof(abs_url)) == 0) {
                    struct br_img_cache_entry *e =
                        br_img_cache_lookup(p, abs_url);
                    if (!e && !lookup_only)
                        e = br_img_cache_load(p, abs_url);
                    if (e) {
                        b->replaced_pixels = e->bgra;
                        b->replaced_pixels_w = e->w;
                        b->replaced_pixels_h = e->h;
                    }
                }
            }
        }
    }
    for (struct layout_box *c = b->first_child; c; c = c->next_sibling)
        br_attach_images_walk(p, c, lookup_only);
}

static void br_attach_images(struct loaded_page *p)
{
    if (!p->doc_built) return;
    br_attach_images_walk(p, p->ldoc.root_box, /*lookup_only=*/0);
}

/* Cache-lookup-only attach against an externally-owned root box.
 * Safe to call from the parser thread because it never mutates
 * the image cache (so it can't race with the GUI thread's
 * br_img_cache_load / install / evict). */
static void br_attach_images_root(struct loaded_page *p,
                                  struct layout_box *root)
{
    if (!p || !root) return;
    br_attach_images_walk(p, root, /*lookup_only=*/1);
}

/* Layout-side hook: given an <img>'s raw src attribute, return
 * the intrinsic dimensions of the decoded image if it's already
 * in this page's cache.  Called by layout when the HTML omitted
 * an explicit width="" or height="" attribute.  Returns 0 on
 * cache-hit (writes *w / *h), -1 otherwise (layout then falls
 * back to its 16x16 placeholder).
 *
 * Must use the same URL-resolution rules as br_attach_images so
 * the cache key shape matches.  `ud` is the loaded_page * passed
 * to layout_set_img_size_lookup. */
static int br_layout_img_size_cb(const char *src, int *out_w, int *out_h,
                                  void *ud)
{
    struct loaded_page *p = (struct loaded_page *)ud;
    if (!p || !src || !src[0]) return -1;
    char abs_url[1024];
    if (br_resolve_img_src(p->url, p->origin_url, src,
                            abs_url, sizeof(abs_url)) < 0)
        return -1;
    struct br_img_cache_entry *e = br_img_cache_lookup(p, abs_url);
    if (!e) return -1;
    if (out_w) *out_w = e->w;
    if (out_h) *out_h = e->h;
    return 0;
}

/* ----------------------------------------------------------------
 *   Direct-image navigation (chapter 98).
 *
 *   When the user navigates to an image URL the HTTP response is
 *   the raw PNG bytes, not HTML.  Without this path we would feed
 *   those bytes to the HTML tokenizer and render the file dump as
 *   garbled text.  Instead we sniff the PNG signature, install
 *   the bytes into the image cache under the page URL, and
 *   synthesise a tiny HTML wrapper containing a single <img>
 *   referencing that same URL.  The rest of the pipeline runs
 *   unchanged: parser -> layout -> br_attach_images (cache hit)
 *   -> render.
 * ---------------------------------------------------------------- */

static int br_looks_like_png(const char *bytes, size_t len)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!bytes || len < 8) return 0;
    for (int i = 0; i < 8; i++)
        if ((uint8_t)bytes[i] != sig[i]) return 0;
    return 1;
}

/* Pre-decode `png_bytes` (length `png_len`) and install the
 * resulting BGRA in the cache under `url`.  Returns the cache
 * entry on success, NULL on decode failure or cap overflow.
 * Identical bookkeeping to br_img_cache_load but skips the
 * fetch step. */
static struct br_img_cache_entry *br_img_cache_install_bytes(
        struct loaded_page *p, const char *url,
        const uint8_t *png_bytes, size_t png_len)
{
    if (p->img_cache_count >= BR_IMG_CACHE_MAX_ENTRIES) return 0;
    uint8_t *bgra = 0;
    int w = 0, h = 0;
    if (png_decode(png_bytes, png_len, &bgra, &w, &h) < 0 || !bgra)
        return 0;
    unsigned bytes_used = (unsigned)w * (unsigned)h * 4u;
    if (p->img_cache_bytes + bytes_used > BR_IMG_CACHE_MAX_BYTES) {
        png_free(bgra);
        return 0;
    }
    struct br_img_cache_entry *e =
        (struct br_img_cache_entry *)malloc(sizeof(*e));
    if (!e) { png_free(bgra); return 0; }
    e->url = br_strdup(url);
    e->bgra = bgra;
    e->w = w; e->h = h;
    e->next = p->img_cache;
    p->img_cache = e;
    p->img_cache_count++;
    p->img_cache_bytes += bytes_used;
    return e;
}

/* Build a synthetic HTML page wrapping a single <img> at the
 * image's intrinsic size.  Returned buffer is malloc'd (caller
 * frees) with length written to *out_len.  We embed the
 * dimensions as width="" / height="" attrs so the layout
 * reserves a correctly-sized REPLACED box on first pass — we
 * don't have an intrinsic-size feedback loop into layout yet. */
static char *br_synthesize_image_page(const char *img_url,
                                       int img_w, int img_h,
                                       size_t *out_len)
{
    size_t ul = br_strlen(img_url);
    size_t cap = 256 + ul * 2;
    char *buf = (char *)malloc(cap);
    if (!buf) return 0;
    int n = snprintf(buf, cap,
        "<html><body><p>Image: %s (%dx%d)</p>"
        "<img src=\"%s\" width=\"%d\" height=\"%d\" alt=\"image\" />"
        "</body></html>",
        img_url, img_w, img_h, img_url, img_w, img_h);
    if (n < 0 || (size_t)n >= cap) { free(buf); return 0; }
    *out_len = (size_t)n;
    return buf;
}

/* Fetch + parse + layout `url` at the given viewport width.
 * On success returns a populated loaded_page.  On any failure
 * (fetch error, parse OOM, layout OOM) returns a synthetic page
 * showing the error message — never returns NULL.  The caller
 * always gets a valid page to display. */
static int relayout_page(struct loaded_page *p, int viewport);  /* fwd */
static struct loaded_page *load_page(const char *url, int viewport)
{
    struct loaded_page *p = (struct loaded_page *)malloc(sizeof(*p));
    if (!p) return 0;
    /* Zero the fields we care about explicitly to avoid the
     * freestanding `= {0}` -> implicit memset trap. */
    p->url = 0; p->html_buf = 0; p->html_len = 0; p->origin_url = 0;
    p->scratch = 0; p->dom = 0;
    p->author_css = 0; p->author_len = 0;
    p->doc_built = 0; p->pb_built = 0;
    p->img_cache = 0; p->img_cache_count = 0; p->img_cache_bytes = 0;

    p->url = br_strdup(url);
    if (!p->url) { free_page(p); return 0; }

    unsigned long t_stage = uptime_ms();

    p->html_buf = fetch(url, &p->html_len, &p->origin_url);
    if (!p->html_buf) {
        p->html_buf = make_error_html(url,
                                       "Failed to fetch page (no network? "
                                       "bad URL? proxy down?).",
                                       &p->html_len);
        if (!p->html_buf) { free_page(p); return 0; }
    }
    BR_TIMING("fetch", t_stage);

    /* Sniff the response: if the bytes themselves are a PNG (the
     * user navigated directly to an image URL), pre-decode the
     * image into the cache under p->url and synthesise a tiny
     * HTML wrapper.  The parser/layout/render pipeline below
     * runs unchanged; br_attach_images will hit the cache for
     * the synthesised <img>.  If the decode fails (unsupported
     * PNG variant) we show an error page so the user sees a
     * useful message instead of a raw byte dump. */
    if (br_looks_like_png(p->html_buf, p->html_len)) {
        struct br_img_cache_entry *e = br_img_cache_install_bytes(
            p, p->url,
            (const uint8_t *)p->html_buf, p->html_len);
        if (e) {
            size_t new_len = 0;
            char *wrap = br_synthesize_image_page(p->url, e->w, e->h,
                                                   &new_len);
            if (wrap) {
                free(p->html_buf);
                p->html_buf = wrap;
                p->html_len = new_len;
            }
        } else {
            free(p->html_buf);
            p->html_buf = make_error_html(url,
                "Image fetched but decoder rejected it (unsupported "
                "PNG variant — interlaced or 16-bit? Try another image).",
                &p->html_len);
            if (!p->html_buf) { free_page(p); return 0; }
        }
    }

    if (build_page_from_html(p, viewport) < 0) {
        /* Free anything we partially built and replace with an
         * error page synthesised in place. */
        if (p->pb_built)  { layout_paint_buf_destroy(&p->pb);  p->pb_built  = 0; }
        if (p->doc_built) { layout_doc_destroy(&p->ldoc);      p->doc_built = 0; }
        if (p->author_css) { free(p->author_css); p->author_css = 0; p->author_len = 0; }
        if (p->dom)        { dom_destroy(p->dom); free(p->dom); p->dom = 0; }
        if (p->scratch)    { free(p->scratch); p->scratch = 0; }
        free(p->html_buf); p->html_len = 0;
        p->html_buf = make_error_html(url,
                                       "Could not build page (parse / layout OOM).",
                                       &p->html_len);
        if (!p->html_buf || build_page_from_html(p, viewport) < 0) {
            free_page(p);
            return 0;
        }
    }
    BR_TIMING("parse + layout", t_stage);
    /* Decode any <img> referenced by the page and stash the BGRA
     * pointers in their replaced layout boxes.  Layout is already
     * built; we just patch fields it left zero.  Failures are non-
     * fatal (the placeholder + alt-text path renders instead). */
    br_attach_images(p);
    BR_TIMING("image decode", t_stage);
    /* Chapter 98b: if any images were decoded, re-run layout so
     * the intrinsic-size hook (set inside relayout_page) sizes
     * <img> boxes to their real dimensions.  The first layout
     * pass sized them at the 16x16 fallback because the cache
     * was empty; this second pass picks up each image's real
     * dimensions for <img> tags that omitted width=""/height=""
     * (the common case on real-world pages).
     *
     * The second pass is a no-op cache-wise — every image was
     * already fetched and decoded above.  relayout_page() also
     * runs br_attach_images and layout_paint_collect internally,
     * so we don't need a separate paint-collect call below. */
    if (p->img_cache_count > 0) {
        relayout_page(p, viewport);
        BR_TIMING("re-layout w/ intrinsic sizes", t_stage);
    } else {
        /* Re-collect paint commands so the IMAGE-vs-placeholder
         * choice picks up the freshly-attached pixels.  (Skipped
         * when there were no images at all — pb is still valid
         * from the first pass.) */
        if (p->pb_built) { layout_paint_buf_destroy(&p->pb); p->pb_built = 0; }
        layout_paint_collect(&p->ldoc, &p->pb);
        p->pb_built = 1;
    }
    return p;
}

/* Re-layout an already-loaded page at a new viewport width.
 * Used on GUI_EVENT_RESIZE.  Returns 0 on success, -1 on OOM.
 *
 * Always brackets the layout call with the intrinsic-image-size
 * hook so resized windows pick up real <img> dimensions just like
 * the initial load does.  Safe to call when the cache is empty —
 * the hook just returns -1 for every lookup, falling through to
 * the 16x16 placeholder default. */
static int relayout_page(struct loaded_page *p, int viewport)
{
    if (!p || !p->dom) return -1;
    if (p->pb_built)  { layout_paint_buf_destroy(&p->pb);  p->pb_built  = 0; }
    if (p->doc_built) { layout_doc_destroy(&p->ldoc);      p->doc_built = 0; }
    layout_set_img_size_lookup(br_layout_img_size_cb, p);
    int rc = layout_build_and_run(&p->ldoc, dom_root(p->dom),
                                   p->author_css, p->author_len, viewport);
    layout_set_img_size_lookup(0, 0);
    if (rc < 0) return -1;
    p->doc_built = 1;
    /* The image cache is independent of layout; re-attach the
     * already-decoded BGRA pointers to the new box tree. */
    br_attach_images(p);
    layout_paint_collect(&p->ldoc, &p->pb);
    p->pb_built = 1;
    return 0;
}

/* ----------------------------------------------------------------
 *   Chapter 94: parser/layout thread.
 *
 * The relayout_page() above is the heaviest synchronous work the
 * GUI loop ever does — for a page like Hacker News (~1000 DOM
 * nodes, dozens of CSS rules in the cascade) it can take tens
 * of milliseconds, during which gui_poll_event() is not called
 * and the window appears frozen.
 *
 * Chapter 94 moves that work onto a second thread pinned to CPU 1.
 * The GUI thread (CPU 0) keeps polling events at full speed; the
 * parser thread runs layout into LOCAL ldoc / pb structs and,
 * when done, atomically swaps them in for the page's current
 * ldoc / pb under a single mutex.  Until the swap, the GUI keeps
 * blitting the OLD paint buffer — the page is one resize stale
 * but the window is fully interactive.
 *
 * The thread is spawned via thread_spawn_files() (chapter 93)
 * with CLONE_FILES set.  Today the parser thread doesn't actually
 * touch the GUI thread's fds (layout is purely in-memory work),
 * but: (a) future incremental work like "fetch a referenced
 * <link rel=stylesheet>" needs the parent's TCP sockets, and
 * (b) shared fds means an exit() / close() on either side is
 * coherent.  So we always clone with CLONE_FILES.
 *
 * Coalescing: while the parser is mid-layout, the GUI may
 * receive several more resize events.  We don't queue them —
 * we overwrite req_viewport with the latest value, bump req_seq,
 * and let the parser pick up the freshest request when it
 * finishes its current pass.  The parser's loop checks
 * req_seq != last_seq right after publishing, so a backlog
 * of size ≥ 1 always re-runs once more before sleeping.
 *
 * The parser thread NEVER touches the page's dom / author_css
 * concurrently with anything that might free them.  The GUI
 * thread's contract: parser_wait_idle() before any free_page()
 * or load_page() call.  Inside that window the parser owns the
 * dom + css read-only, but neither is mutated. */

struct parser_state {
    /* Protects the request payload (req_*) and the response
     * payload (new_*).  The seq counters are read lockless via
     * atomic_load. */
    mutex_t lock;

    /* Coordination counters.  GUI bumps req_seq on every new
     * request; parser bumps done_seq right after publishing.
     * GUI absorbs only when done_seq != consumed_seq. */
    volatile uint32_t req_seq;
    volatile uint32_t done_seq;
    uint32_t consumed_seq;     /* GUI-thread private */

    /* Shutdown flag (atomic).  parser_shutdown() sets this to 1
     * and bumps req_seq + futex_wake to unstick the parser. */
    volatile uint32_t shutdown;

    /* Request payload (read by parser under lock).  The dom and
     * css pointers refer into the page that's currently active
     * on the GUI side; the GUI thread guarantees they outlive
     * the parser pass via the parser_wait_idle() contract.
     *
     * req_page is needed so the parser can install the intrinsic-
     * image-size hook (chapter 98b) keyed on the page's image
     * cache.  Without this, resize redraws would revert every
     * <img> with no width/height attribute to the layout's 16x16
     * placeholder. */
    struct dom         *req_dom;
    char               *req_css;
    int                 req_css_len;
    int                 req_viewport;
    struct loaded_page *req_page;

    /* Response payload (written by parser, read by GUI).  When
     * new_built == 1 there is an unconsumed result; GUI absorbs
     * it by moving the structs into the page and setting
     * new_built = 0. */
    struct layout_doc       new_ldoc;
    struct layout_paint_buf new_pb;
    int new_built;
    int new_viewport;          /* viewport this result is for */

    /* Parser thread id (set by parser_spawn). */
    int tid;

    /* Stats — for the --bench-resize mode and chapter 94 doc.
     * Parser increments work_done_count after each completed
     * pass; GUI increments gui_iters_during_work each time it
     * spins through the event loop while a request is pending. */
    volatile uint32_t work_done_count;
    volatile uint32_t gui_iters_during_work;
};

/* The parser thread's main loop.  Snapshots a request, runs
 * layout_build_and_run + layout_paint_collect on local structs,
 * then publishes them under the mutex.  Loops as long as
 * req_seq advances; sleeps via futex_wait when caught up.
 *
 * MUST end via exit(), not return — the kernel's clone trampoline
 * zeroes x30, so a plain `return` lands at PC=0 and faults. */
static void parser_thread_main(void *arg)
{
    struct parser_state *ps = (struct parser_state *)arg;
    uint32_t last_seq = 0;

    for (;;) {
        /* Wait for new work (or shutdown).  Re-check the predicate
         * after every wake — futex_wait can return spuriously
         * (kernel side returns -EAGAIN if the word has changed
         * between our cmpxchg and the kernel's read).  Without
         * the loop we'd miss the actual wake. */
        while (atomic_load32_u(&ps->req_seq) == last_seq &&
               !atomic_load32_u(&ps->shutdown)) {
            (void)futex_wait((volatile int *)&ps->req_seq,
                             (int)last_seq);
        }
        if (atomic_load32_u(&ps->shutdown))
            exit(0);

        /* Snapshot under lock.  Lock window is tiny — just a
         * few field reads — so the GUI thread doesn't block
         * on us when it wants to absorb a previous result or
         * post a coalescing request. */
        mutex_lock(&ps->lock);
        uint32_t snap_seq = atomic_load32_u(&ps->req_seq);
        struct dom *dom            = ps->req_dom;
        char *css                  = ps->req_css;
        int   css_len              = ps->req_css_len;
        int   viewport             = ps->req_viewport;
        struct loaded_page *page   = ps->req_page;
        mutex_unlock(&ps->lock);
        last_seq = snap_seq;

        /* Do the work into LOCAL structs.  No shared state is
         * touched here, so the GUI thread can render the OLD
         * paint buffer concurrently with no synchronisation.
         *
         * Chapter 98b: bracket the layout call with the intrinsic-
         * image-size hook so <img> tags without explicit width /
         * height pick up the page's already-decoded image
         * dimensions on resize, exactly like load_page's second
         * pass does.  The hook is a process-global function
         * pointer, but only ONE layout pass is ever in flight at
         * a time (the GUI thread calls parser_wait_idle() before
         * any path that itself runs layout), so set/clear here
         * is race-free. */
        struct layout_doc       local_ldoc;
        struct layout_paint_buf local_pb;
        int built = 0;
        if (dom) {
            layout_set_img_size_lookup(br_layout_img_size_cb, page);
            int rc = layout_build_and_run(&local_ldoc, dom_root(dom),
                                           css, css_len, viewport);
            layout_set_img_size_lookup(0, 0);
            if (rc >= 0) {
                /* Chapter 98b: wire each <img> box's bgra pointer
                 * BEFORE collecting paint so the published paint
                 * buffer already has IMAGE commands.  Without this
                 * the GUI thread would have to re-attach and
                 * re-collect paint after every absorb, doubling
                 * the wall-clock cost of every resize. */
                if (page)
                    br_attach_images_root(page, local_ldoc.root_box);
                layout_paint_collect(&local_ldoc, &local_pb);
                built = 1;
            }
        }

        /* Publish.  If a previous result was never consumed by
         * GUI (rare — only if the GUI thread is severely
         * starved), free it before overwriting so we don't
         * leak. */
        mutex_lock(&ps->lock);
        if (ps->new_built) {
            layout_paint_buf_destroy(&ps->new_pb);
            layout_doc_destroy(&ps->new_ldoc);
            ps->new_built = 0;
        }
        if (built) {
            ps->new_ldoc      = local_ldoc;
            ps->new_pb        = local_pb;
            ps->new_viewport  = viewport;
            ps->new_built     = 1;
        }
        atomic_store32_u(&ps->done_seq, snap_seq);
        (void)atomic_add_return32_u(&ps->work_done_count, 1);
        mutex_unlock(&ps->lock);

        /* Wake GUI in case it's actually waiting (parser_wait_idle).
         * GUI's normal poll-each-frame absorption doesn't need this. */
        (void)futex_wake((volatile int *)&ps->done_seq, 1);
    }
}

static void parser_init(struct parser_state *ps)
{
    mutex_init(&ps->lock);
    atomic_store32_u(&ps->req_seq, 0);
    atomic_store32_u(&ps->done_seq, 0);
    ps->consumed_seq = 0;
    atomic_store32_u(&ps->shutdown, 0);
    ps->req_dom = 0; ps->req_css = 0; ps->req_css_len = 0;
    ps->req_viewport = 0; ps->req_page = 0;
    ps->new_built = 0; ps->new_viewport = 0;
    ps->tid = -1;
    atomic_store32_u(&ps->work_done_count, 0);
    atomic_store32_u(&ps->gui_iters_during_work, 0);
}

/* Spawn the parser thread on `cpu_id` (typically 1) with
 * CLONE_FILES so it shares the GUI thread's fd table.  Returns
 * 0 on success, -1 on failure. */
static int parser_spawn(struct parser_state *ps, int cpu_id)
{
    int tid = thread_spawn_files(parser_thread_main, (void *)ps, cpu_id);
    if (tid < 0) {
        printf("[browser] parser thread spawn failed: %d\n", tid);
        return -1;
    }
    ps->tid = tid;
    printf("[browser] parser thread spawned tid=%d cpu=%d\n", tid, cpu_id);
    return 0;
}

/* Post a relayout request.  The GUI thread keeps using its
 * current ldoc/pb until the parser publishes a result and
 * parser_absorb_completion swaps it in.  This call is
 * non-blocking: returns immediately. */
static void parser_request_relayout(struct parser_state *ps,
                                     struct loaded_page *p,
                                     int viewport)
{
    if (!ps || !p || !p->dom) return;
    mutex_lock(&ps->lock);
    ps->req_dom      = p->dom;
    ps->req_css      = p->author_css;
    ps->req_css_len  = p->author_len;
    ps->req_viewport = viewport;
    ps->req_page     = p;
    uint32_t new_seq = atomic_add_return32_u(&ps->req_seq, 1);
    mutex_unlock(&ps->lock);
    /* Wake the parser if it was sleeping.  Pass the OLD value
     * (new_seq - 1) as the futex word — that's what the parser
     * was waiting on. */
    (void)futex_wake((volatile int *)&ps->req_seq, 1);
    (void)new_seq;   /* unused; future: log seq for diagnostics */
}

/* Check for a pending result and, if present, swap it into
 * the page.  Returns 1 if a swap happened (caller should mark
 * the frame dirty and recompute scroll), 0 otherwise.  Always
 * non-blocking — never sleeps. */
static int parser_absorb_completion(struct parser_state *ps,
                                     struct loaded_page *p)
{
    if (!ps || !p) return 0;
    /* Lockless quick check: if done_seq == consumed_seq there
     * is nothing to absorb.  Avoids a lock in the common case
     * (GUI polls every frame). */
    uint32_t done = atomic_load32_u(&ps->done_seq);
    if (done == ps->consumed_seq) return 0;

    mutex_lock(&ps->lock);
    if (!ps->new_built) {
        /* Race: parser bumped done_seq but a coalescing
         * overwrite happened in between.  Sync our consumed
         * counter and let the next pass deliver the fresh
         * result. */
        ps->consumed_seq = done;
        mutex_unlock(&ps->lock);
        return 0;
    }
    struct layout_doc       new_ldoc = ps->new_ldoc;
    struct layout_paint_buf new_pb   = ps->new_pb;
    ps->new_built = 0;
    ps->consumed_seq = done;
    mutex_unlock(&ps->lock);

    /* Free the page's previous layout outputs and install the
     * new ones.  The dom + author_css are unchanged — they're
     * shared between the old and new ldoc.  The box-tree
     * pointers in new_ldoc point into the same dom, so the
     * dom must NOT have been freed since the request was
     * posted (parser_wait_idle enforces that).
     *
     * The parser already wired image bgra pointers (chapter 98b)
     * before collecting paint, so the new paint buffer already
     * encodes IMAGE commands — no re-attach needed here. */
    if (p->pb_built)  { layout_paint_buf_destroy(&p->pb);  p->pb_built  = 0; }
    if (p->doc_built) { layout_doc_destroy(&p->ldoc);      p->doc_built = 0; }
    p->ldoc = new_ldoc;
    p->pb   = new_pb;
    p->doc_built = 1;
    p->pb_built  = 1;
    return 1;
}

/* Block until the parser thread has finished any in-flight
 * work AND any pending request has been completed.  Used by
 * the GUI thread before any operation that frees the current
 * page (free_page / load_page / navigation), so the parser
 * never holds a dom / css pointer through the free. */
static void parser_wait_idle(struct parser_state *ps,
                              struct loaded_page *p)
{
    if (!ps) return;
    for (;;) {
        uint32_t req  = atomic_load32_u(&ps->req_seq);
        uint32_t done = atomic_load32_u(&ps->done_seq);
        if (req == done) {
            /* Parser is caught up; absorb any pending result so
             * the GUI doesn't see "phantom completion" on the
             * next frame after the page changes underneath. */
            (void)parser_absorb_completion(ps, p);
            return;
        }
        /* Sleep until the parser publishes a result.  futex_wait
         * returns -EAGAIN if done_seq has already changed; we
         * loop and re-check req == done. */
        (void)futex_wait((volatile int *)&ps->done_seq, (int)done);
        /* Drain any newly published result before re-checking,
         * so the next iteration sees the cleared state. */
        (void)parser_absorb_completion(ps, p);
    }
}

/* Cleanly stop the parser thread.  Sets the shutdown flag,
 * bumps req_seq + futex_wake to unstick it, joins.  Safe to
 * call even if the parser was never spawned (tid < 0). */
static void parser_shutdown(struct parser_state *ps)
{
    if (!ps) return;
    if (ps->tid < 0) return;
    atomic_store32_u(&ps->shutdown, 1);
    /* Bump req_seq so the parser's "while seq == last_seq"
     * predicate breaks even if it was already past the
     * shutdown check between iterations. */
    (void)atomic_add_return32_u(&ps->req_seq, 1);
    (void)futex_wake((volatile int *)&ps->req_seq, 1);
    (void)thread_join(ps->tid);
    ps->tid = -1;
    /* Free any unconsumed result so we don't leak on exit. */
    if (ps->new_built) {
        layout_paint_buf_destroy(&ps->new_pb);
        layout_doc_destroy(&ps->new_ldoc);
        ps->new_built = 0;
    }
}

/* Canonicalize a user-typed or relative URL into an absolute URL
 * we can fetch.  Rules:
 *
 *   1. /mnt/... or other absolute filesystem path: pass through.
 *   2. http://...   : pass through verbatim.
 *   3. https://...  : strip scheme, prepend BR_DEFAULT_PROXY so
 *      the M58 proxy upgrades to TLS for us.
 *   4. //host/path  : protocol-relative, treat as scheme:host/path.
 *   5. /host/path   : root-relative against current page (used for
 *      proxy-rewritten links like "/news.ycombinator.com/item?id=").
 *   6. bare host or host/path: prepend the proxy prefix.
 *
 * `current` is the URL of the currently-loaded page (used for case
 * 5 — we need its scheme://host to resolve root-relative refs).
 * May be NULL on the very first navigation.
 *
 * Returns malloc'd absolute URL, NULL on OOM. */
static char *canonicalize_url(const char *input, const char *current)
{
    if (!input || !*input) return 0;

    /* Strip leading whitespace. */
    while (*input == ' ' || *input == '\t') input++;

    /* (1) absolute filesystem path. */
    if (input[0] == '/' &&
        (br_starts(input, "/mnt/") || br_starts(input, "/bin/") ||
         br_starts(input, "/dev/")))
        return br_strdup(input);

    /* (2) http:// passthrough. */
    if (br_starts(input, "http://"))
        return br_strdup(input);

    /* (3) https:// -> proxy. */
    if (br_starts(input, "https://")) {
        const char *rest = input + 8;
        size_t pn = br_strlen(g_proxy_prefix);
        size_t rn = br_strlen(rest);
        char *out = (char *)malloc(pn + rn + 1);
        if (!out) return 0;
        for (size_t i = 0; i < pn; i++) out[i] = g_proxy_prefix[i];
        for (size_t i = 0; i < rn; i++) out[pn + i] = rest[i];
        out[pn + rn] = '\0';
        return out;
    }

    /* (4) //host/path */
    if (input[0] == '/' && input[1] == '/') {
        const char *rest = input + 2;
        size_t pn = br_strlen(g_proxy_prefix);
        size_t rn = br_strlen(rest);
        char *out = (char *)malloc(pn + rn + 1);
        if (!out) return 0;
        for (size_t i = 0; i < pn; i++) out[i] = g_proxy_prefix[i];
        for (size_t i = 0; i < rn; i++) out[pn + i] = rest[i];
        out[pn + rn] = '\0';
        return out;
    }

    /* (5) /path against current page's scheme://host */
    if (input[0] == '/' && current && br_starts(current, "http")) {
        /* Find scheme://host's end (first '/' after "://"). */
        const char *p = current;
        if (br_starts(p, "http://"))  p += 7;
        else if (br_starts(p, "https://")) p += 8;
        while (*p && *p != '/') p++;
        size_t base_len = (size_t)(p - current);
        size_t tail_len = br_strlen(input);
        char *out = (char *)malloc(base_len + tail_len + 1);
        if (!out) return 0;
        for (size_t i = 0; i < base_len; i++) out[i] = current[i];
        for (size_t i = 0; i < tail_len; i++) out[base_len + i] = input[i];
        out[base_len + tail_len] = '\0';
        return out;
    }

    /* (6) bare host or host/path -> proxy + input.  Prefix already
     * ends in '/' so we just concatenate. */
    {
        size_t pn = br_strlen(g_proxy_prefix);
        size_t in = br_strlen(input);
        char *out = (char *)malloc(pn + in + 1);
        if (!out) return 0;
        for (size_t i = 0; i < pn; i++) out[i] = g_proxy_prefix[i];
        for (size_t i = 0; i < in; i++) out[pn + i] = input[i];
        out[pn + in] = '\0';
        return out;
    }
}

/* Walk the box tree iteratively (no recursion — keeps user-thread
 * stack usage flat at 16 KiB) and return the deepest box whose
 * (x,y,w,h) rect contains (px,py).  Returns NULL if no box covers
 * the point.  Containing means px in [x, x+w) and py in [y, y+h). */
static struct layout_box *box_at(struct layout_box *root, int px, int py)
{
    struct layout_box *hit = 0;
    struct layout_box *cur = root;
    while (cur) {
        int contains = (px >= cur->x && px < cur->x + cur->w &&
                        py >= cur->y && py < cur->y + cur->h);
        if (contains) hit = cur;
        if (contains && cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && !cur->next_sibling) cur = cur->parent;
            if (cur) cur = cur->next_sibling;
        }
    }
    return hit;
}

/* Hit-test for clickable links.  Returns the href string of the
 * deepest enclosing `<a href="...">` element, or NULL if none.
 * The pointer aliases into the DOM and is valid until the page is
 * freed, so callers should copy it before navigating. */
static const char *link_href_at(struct layout_box *root, int px, int py)
{
    struct layout_box *box = box_at(root, px, py);
    if (!box) return 0;
    for (const struct dom_node *n = box->dom; n; n = n->parent) {
        if (n->type == DOM_NODE_ELEMENT && n->tag &&
            n->tag[0] == 'a' && n->tag[1] == '\0') {
            const char *h = dom_node_attr(n, "href");
            if (h && h[0]) return h;
        }
    }
    return 0;
}

/* Browser-wide event-loop state.  Lives on run_gui's stack. */
struct browser_state {
    /* History: each entry is a malloc'd absolute URL.  hist_idx
     * is the index of the currently displayed page; -1 means
     * empty. */
    char *history[BR_HISTORY_CAP];
    int   hist_count;
    int   hist_idx;

    /* URL bar editing. */
    char  url_buf[BR_URL_BUF_CAP];
    int   url_len;
    int   url_cursor;
    int   url_focus;

    /* Currently displayed page. */
    struct loaded_page *page;

    /* Window geometry. */
    int   win_id;
    int   win_w, win_h;
    int   viewport_w;     /* layout viewport (== win_w on resize) */
    int   scroll_x, scroll_y;
    int   max_scroll_x, max_scroll_y;

    /* UI state. */
    int   hover_btn;      /* 0=back 1=fwd 2=reload, -1=none */
    int   dirty;

    /* Chapter 94: parser/layout thread state.  Pointer (not
     * inline) so the struct stays small and the parser can be
     * stopped + restarted without resizing the page state. */
    struct parser_state *parser;
};

static void br_url_set(struct browser_state *s, const char *url)
{
    int n = 0;
    if (url) while (url[n] && n < BR_URL_BUF_CAP - 1) {
        s->url_buf[n] = url[n]; n++;
    }
    s->url_buf[n] = '\0';
    s->url_len    = n;
    s->url_cursor = n;
}

/* Push a URL onto the history stack.  Truncates the forward tail
 * (any URLs after hist_idx) before pushing so a back-then-navigate
 * loses the old forward branch — same model as a normal browser. */
static void hist_push(struct browser_state *s, const char *url)
{
    /* Drop tail. */
    for (int i = s->hist_idx + 1; i < s->hist_count; i++) {
        if (s->history[i]) { free(s->history[i]); s->history[i] = 0; }
    }
    s->hist_count = s->hist_idx + 1;
    /* If at cap, drop oldest. */
    if (s->hist_count >= BR_HISTORY_CAP) {
        if (s->history[0]) free(s->history[0]);
        for (int i = 1; i < s->hist_count; i++) s->history[i - 1] = s->history[i];
        s->hist_count--;
        s->hist_idx--;
    }
    s->history[s->hist_count] = br_strdup(url);
    s->hist_count++;
    s->hist_idx = s->hist_count - 1;
}

/* Recompute scroll caps + clamp current scroll position to the
 * new page's dimensions.  Called after every load + resize. */
static void br_recompute_scroll(struct browser_state *s)
{
    int content_h = s->win_h - BR_GUI_STATUS_H;
    if (content_h < 1) content_h = 1;
    int doc_w = s->page && s->page->doc_built ? s->page->ldoc.doc_width_px  : 0;
    int doc_h = s->page && s->page->doc_built ? s->page->ldoc.doc_height_px : 0;
    s->max_scroll_x = doc_w - s->win_w;
    s->max_scroll_y = doc_h - content_h;
    if (s->max_scroll_x < 0) s->max_scroll_x = 0;
    if (s->max_scroll_y < 0) s->max_scroll_y = 0;
    if (s->scroll_x > s->max_scroll_x) s->scroll_x = s->max_scroll_x;
    if (s->scroll_y > s->max_scroll_y) s->scroll_y = s->max_scroll_y;
    if (s->scroll_x < 0) s->scroll_x = 0;
    if (s->scroll_y < 0) s->scroll_y = 0;
}

/* Navigate to `url` (will be canonicalized).  When push != 0 a
 * new history entry is created; pass 0 for back/forward. */
static void navigate_to(struct browser_state *s, const char *url, int push)
{
    char *abs = canonicalize_url(url,
                                  s->page ? s->page->url : (const char *)0);
    if (!abs) return;
    printf("[browser] navigate -> %s\n", abs);
    struct loaded_page *next = load_page(abs, s->viewport_w);
    if (!next) {
        /* Last-ditch fallback: keep the old page; user can retry. */
        free(abs);
        return;
    }
    /* Chapter 94: drain any in-flight relayout before swapping
     * the page pointer.  The parser may still be holding
     * s->page->dom / author_css read-only. */
    if (s->parser) parser_wait_idle(s->parser, s->page);
    if (s->page) free_page(s->page);
    s->page     = next;
    s->scroll_x = 0;
    s->scroll_y = 0;
    if (push) hist_push(s, abs);
    free(abs);

    br_url_set(s, s->page->url);
    s->url_focus = 0;
    br_recompute_scroll(s);
    s->dirty = 1;
}

static void navigate_history(struct browser_state *s, int delta)
{
    int new_idx = s->hist_idx + delta;
    if (new_idx < 0 || new_idx >= s->hist_count) return;
    const char *url = s->history[new_idx];
    if (!url) return;
    /* Reuse load_page; do NOT push (we're just moving within the
     * existing history). */
    struct loaded_page *next = load_page(url, s->viewport_w);
    if (!next) return;
    /* Chapter 94: drain in-flight relayout before swapping page. */
    if (s->parser) parser_wait_idle(s->parser, s->page);
    if (s->page) free_page(s->page);
    s->page     = next;
    s->scroll_x = 0;
    s->scroll_y = 0;
    s->hist_idx = new_idx;
    br_url_set(s, s->page->url);
    s->url_focus = 0;
    br_recompute_scroll(s);
    s->dirty = 1;
}

/* Hit-test the toolbar.  Returns 0=back, 1=fwd, 2=reload, 3=URL
 * field, -1=outside any control. */
static int toolbar_hit(int x, int y)
{
    if (y < 0 || y >= BR_GUI_STATUS_H) return -1;
    if (y < BR_TB_BTN_Y || y >= BR_TB_BTN_Y + BR_TB_BTN_H) {
        /* Bar background, not on any control. */
        return -1;
    }
    if (x >= BR_TB_BACK_X   && x < BR_TB_BACK_X   + BR_TB_BTN_W) return 0;
    if (x >= BR_TB_FWD_X    && x < BR_TB_FWD_X    + BR_TB_BTN_W) return 1;
    if (x >= BR_TB_RELOAD_X && x < BR_TB_RELOAD_X + BR_TB_BTN_W) return 2;
    if (x >= BR_TB_URL_X) return 3;
    return -1;
}

static int run_gui(const char *initial_url, int initial_viewport)
{
    /* Stack-allocated; explicit field init avoids the freestanding
     * `= {0}` -> implicit memset trap. */
    struct browser_state s;
    for (int i = 0; i < BR_HISTORY_CAP; i++) s.history[i] = 0;
    s.hist_count = 0; s.hist_idx = -1;
    s.url_buf[0] = '\0'; s.url_len = 0; s.url_cursor = 0; s.url_focus = 0;
    s.page = 0;
    s.win_id = -1;
    s.win_w = initial_viewport; s.win_h = BR_GUI_DEFAULT_H;
    s.viewport_w = initial_viewport;
    s.scroll_x = 0; s.scroll_y = 0;
    s.max_scroll_x = 0; s.max_scroll_y = 0;
    /* UI state. */
    s.hover_btn = -1;
    s.dirty = 1;
    s.parser = 0;

    /* Read BROWSER_PROXY override before the first load. */
    {
        char tmp[160];
        long got = getenv("BROWSER_PROXY", tmp, sizeof(tmp));
        if (got > 0) {
            int n = 0;
            while (tmp[n] && n < (int)sizeof(g_proxy_prefix) - 1) {
                g_proxy_prefix[n] = tmp[n]; n++;
            }
            /* Ensure trailing slash so concat works. */
            if (n > 0 && g_proxy_prefix[n - 1] != '/') {
                if (n < (int)sizeof(g_proxy_prefix) - 1)
                    g_proxy_prefix[n++] = '/';
            }
            g_proxy_prefix[n] = '\0';
            printf("[browser] proxy: %s\n", g_proxy_prefix);
        }
    }

    /* Initial load. */
    {
        char *abs = canonicalize_url(initial_url, 0);
        if (!abs) {
            printf("browser: oom (initial url)\n");
            return -1;
        }
        s.page = load_page(abs, initial_viewport);
        if (!s.page) {
            printf("browser: failed to open initial page %s\n", abs);
            free(abs);
            return -1;
        }
        hist_push(&s, abs);
        br_url_set(&s, s.page->url);
        free(abs);
    }

    s.win_w = initial_viewport;
    if (s.win_w < 320) s.win_w = 320;
    s.win_h = s.page->ldoc.doc_height_px + BR_GUI_STATUS_H;
    if (s.win_h > BR_GUI_DEFAULT_H) s.win_h = BR_GUI_DEFAULT_H;
    if (s.win_h < 240) s.win_h = 240;

    /* Title bar: stable "browser" so the WM shows a short label. */
    s.win_id = gui_create_window_ex((uint32_t)s.win_w, (uint32_t)s.win_h,
                                     "browser",
                                     GUI_WIN_FLAG_RESIZABLE,
                                     GUI_WIN_POS_AUTO, GUI_WIN_POS_AUTO);
    if (s.win_id < 0) {
        printf("browser: gui_create_window_ex(%d,%d) failed (%d)\n",
               s.win_w, s.win_h, s.win_id);
        free_page(s.page);
        return -1;
    }
    br_recompute_scroll(&s);

    printf("[browser] gui window=%d size=%dx%d content_h=%d doc=%dx%d\n",
           s.win_id, s.win_w, s.win_h, s.win_h - BR_GUI_STATUS_H,
           s.page->ldoc.doc_width_px, s.page->ldoc.doc_height_px);

    /* Chapter 94: spawn the parser/layout thread on CPU 1 with
     * CLONE_FILES.  We do this AFTER the initial load so the
     * first frame is fully synchronous (no race between the
     * initial paint and the parser thread coming up).  */
    {
        struct parser_state *ps =
            (struct parser_state *)malloc(sizeof(*ps));
        if (!ps) {
            printf("browser: oom (parser_state)\n");
            gui_destroy_window(s.win_id);
            free_page(s.page);
            return -1;
        }
        parser_init(ps);
        s.parser = ps;
        if (parser_spawn(ps, /*cpu_id=*/1) < 0) {
            /* Non-fatal: fall back to synchronous relayout in
             * the resize handler.  s.parser stays NULL. */
            free(ps);
            s.parser = 0;
            printf("[browser] parser thread unavailable; resize "
                   "will block the GUI loop\n");
        }
    }

    int first_frame = 1;
    for (;;) {
        /* Chapter 94: drain any completed parser work before
         * deciding whether to redraw.  Non-blocking; sets
         * s.dirty if a swap occurred. */
        if (s.parser && parser_absorb_completion(s.parser, s.page)) {
            br_recompute_scroll(&s);
            s.dirty = 1;
        }
        if (s.dirty) {
            unsigned long _t_frame = g_timing ? uptime_ms() : 0;
            render_gui_frame(s.win_id, s.win_w, s.win_h,
                              &s.page->ldoc, &s.page->pb,
                              s.scroll_x, s.scroll_y);
            render_toolbar(s.win_id, s.win_w,
                            s.url_buf, s.url_len,
                            s.url_focus ? s.url_cursor : -1,
                            s.hist_idx > 0,
                            s.hist_idx >= 0 && s.hist_idx < s.hist_count - 1,
                            s.hover_btn,
                            s.scroll_x, s.scroll_y,
                            s.page->ldoc.doc_width_px,
                            s.page->ldoc.doc_height_px,
                            s.win_w, s.win_h - BR_GUI_STATUS_H);
            gui_flush(s.win_id);
            if (g_timing) {
                printf("[timing] %-22s %lu ms (%d cmds)\n",
                       first_frame ? "first frame" : "redraw",
                       uptime_ms() - _t_frame, s.page->pb.n);
            }
            first_frame = 0;
            s.dirty = 0;
        }

        struct gui_event ev;
        if (!gui_poll_event(&ev)) {
            /* Chapter 94 stats: count the GUI loop iterations
             * that ran while parser work was pending.  This is
             * the number we expect to be > 0 in --bench-resize
             * mode — proof that the GUI loop kept running while
             * the parser was busy. */
            if (s.parser &&
                atomic_load32_u(&s.parser->req_seq) !=
                atomic_load32_u(&s.parser->done_seq)) {
                (void)atomic_add_return32_u(
                    &s.parser->gui_iters_during_work, 1);
            }
            yield(); continue;
        }

        switch (ev.type) {
        case GUI_EVENT_CLOSE:
            /* Chapter 94: drain the parser before tearing down
             * the page or its DOM.  parser_shutdown() also
             * absorbs any in-flight result so we don't leak. */
            if (s.parser) {
                parser_wait_idle(s.parser, s.page);
                parser_shutdown(s.parser);
                free(s.parser);
                s.parser = 0;
            }
            gui_destroy_window(s.win_id);
            free_page(s.page);
            for (int i = 0; i < s.hist_count; i++)
                if (s.history[i]) free(s.history[i]);
            return 0;

        case GUI_EVENT_RESIZE: {
            int new_w = (int)ev.arg0;
            int new_h = (int)ev.arg1;
            if (new_w < 64) new_w = 64;
            if (new_h < 64) new_h = 64;
            /* Update window geometry IMMEDIATELY (cheap; the
             * stale paint buffer will be cropped to the new
             * window in render_gui_frame).  The actual relayout
             * happens on the parser thread; until it publishes
             * a result, render keeps using the OLD ldoc/pb
             * laid out for the OLD viewport. */
            s.win_w = new_w; s.win_h = new_h;
            s.viewport_w = new_w;
            br_recompute_scroll(&s);
            s.dirty = 1;
            if (s.parser) {
                /* Async relayout — never blocks the GUI loop. */
                parser_request_relayout(s.parser, s.page, new_w);
            } else {
                /* Synchronous fallback (parser spawn failed). */
                if (relayout_page(s.page, new_w) < 0)
                    printf("browser: relayout at %d failed\n", new_w);
                br_recompute_scroll(&s);
            }
            break;
        }

        case GUI_EVENT_MOUSE_MOVE: {
            int x = (int)ev.arg0;
            int y = (int)ev.arg1;
            int hit = toolbar_hit(x, y);
            int new_hover = (hit >= 0 && hit < 3) ? hit : -1;
            if (new_hover != s.hover_btn) {
                s.hover_btn = new_hover;
                s.dirty = 1;
            }
            break;
        }

        case GUI_EVENT_MOUSE_DOWN: {
            int x = (int)ev.arg0;
            int y = (int)ev.arg1;
            if (!(ev.arg2 & GUI_BTN_LEFT)) break;

            int tb = toolbar_hit(x, y);
            if (tb == 0) {
                /* Back. */
                if (s.hist_idx > 0) navigate_history(&s, -1);
                break;
            }
            if (tb == 1) {
                /* Forward. */
                if (s.hist_idx >= 0 && s.hist_idx < s.hist_count - 1)
                    navigate_history(&s, +1);
                break;
            }
            if (tb == 2) {
                /* Reload: refetch current URL without touching history. */
                if (s.page) {
                    char *u = br_strdup(s.page->url);
                    if (u) {
                        struct loaded_page *next = load_page(u, s.viewport_w);
                        if (next) {
                            /* Chapter 94: drain parser before
                             * freeing the old page's dom. */
                            if (s.parser)
                                parser_wait_idle(s.parser, s.page);
                            free_page(s.page);
                            s.page     = next;
                            s.scroll_x = 0; s.scroll_y = 0;
                            br_url_set(&s, s.page->url);
                            br_recompute_scroll(&s);
                            s.dirty = 1;
                        }
                        free(u);
                    }
                }
                break;
            }
            if (tb == 3) {
                /* Click in URL field: focus + put caret at end.
                 * Real browsers select-all here; we do the simpler
                 * thing of placing the caret at the end. */
                s.url_focus = 1;
                s.url_cursor = s.url_len;
                s.dirty = 1;
                break;
            }

            /* Click in page content area. */
            if (y >= BR_GUI_STATUS_H && s.page && s.page->doc_built) {
                /* If URL bar was focused, defocus first. */
                if (s.url_focus) {
                    s.url_focus = 0;
                    s.dirty = 1;
                }
                int doc_x = x + s.scroll_x;
                int doc_y = (y - BR_GUI_STATUS_H) + s.scroll_y;
                const char *href = link_href_at(s.page->ldoc.root_box,
                                                 doc_x, doc_y);
                if (href) {
                    /* Copy the href before navigate_to() frees the
                     * source page. */
                    char *href_copy = br_strdup(href);
                    if (href_copy) {
                        navigate_to(&s, href_copy, /*push=*/1);
                        free(href_copy);
                    }
                }
            }
            break;
        }

        case GUI_EVENT_KEY: {
            uint32_t k = ev.arg0;

            if (s.url_focus) {
                /* URL bar editing. */
                if (k == GUI_KEY_LEFT)  {
                    if (s.url_cursor > 0) { s.url_cursor--; s.dirty = 1; }
                    break;
                }
                if (k == GUI_KEY_RIGHT) {
                    if (s.url_cursor < s.url_len) { s.url_cursor++; s.dirty = 1; }
                    break;
                }
                if (k == GUI_KEY_HOME)  { s.url_cursor = 0;          s.dirty = 1; break; }
                if (k == GUI_KEY_END)   { s.url_cursor = s.url_len;  s.dirty = 1; break; }
                char ch = (char)(k & 0xFF);
                if (k == 0x1B) {
                    /* ESC: defocus, restore URL bar to current page url. */
                    s.url_focus = 0;
                    if (s.page) br_url_set(&s, s.page->url);
                    s.dirty = 1;
                    break;
                }
                if (ch == '\n' || ch == '\r') {
                    /* Enter: navigate to whatever is in the bar.
                     * Snapshot first so navigate_to (which writes
                     * to url_buf) doesn't trample its own input. */
                    char snap[BR_URL_BUF_CAP];
                    int sn = 0;
                    while (sn < s.url_len) { snap[sn] = s.url_buf[sn]; sn++; }
                    snap[sn] = '\0';
                    if (sn > 0) navigate_to(&s, snap, /*push=*/1);
                    break;
                }
                if (ch == 0x08 || ch == 0x7F) {
                    /* Backspace. */
                    if (s.url_cursor > 0) {
                        for (int i = s.url_cursor - 1; i < s.url_len - 1; i++)
                            s.url_buf[i] = s.url_buf[i + 1];
                        s.url_len--;
                        s.url_cursor--;
                        s.url_buf[s.url_len] = '\0';
                        s.dirty = 1;
                    }
                    break;
                }
                /* Plain printable ASCII insertion. */
                if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F &&
                    s.url_len < BR_URL_BUF_CAP - 1) {
                    for (int i = s.url_len; i > s.url_cursor; i--)
                        s.url_buf[i] = s.url_buf[i - 1];
                    s.url_buf[s.url_cursor] = ch;
                    s.url_len++;
                    s.url_cursor++;
                    s.url_buf[s.url_len] = '\0';
                    s.dirty = 1;
                }
                break;
            }

            /* Document mode: existing scroll + nav shortcuts. */
            int prev_x = s.scroll_x;
            int prev_y = s.scroll_y;
            int content_h = s.win_h - BR_GUI_STATUS_H;
            switch (k) {
            case GUI_KEY_UP:    s.scroll_y -= 16;            break;
            case GUI_KEY_DOWN:  s.scroll_y += 16;            break;
            case GUI_KEY_LEFT:  s.scroll_x -= 16;            break;
            case GUI_KEY_RIGHT: s.scroll_x += 16;            break;
            case GUI_KEY_HOME:  s.scroll_y = 0; s.scroll_x = 0; break;
            case GUI_KEY_END:   s.scroll_y = s.max_scroll_y;   break;
            default: {
                char c = (char)(k & 0xFF);
                if (c == ' ')      s.scroll_y += content_h - 16;
                else if (c == 'b') s.scroll_y -= content_h - 16;
                else if (c == 'g') { s.scroll_y = 0; s.scroll_x = 0; }
                else if (c == 'G') s.scroll_y = s.max_scroll_y;
                else if (c == 'l') {
                    /* 'l' = focus URL bar (mnemonic: "location"). */
                    s.url_focus = 1;
                    s.url_cursor = s.url_len;
                    s.dirty = 1;
                }
                else if (c == 0x1B || c == 'q') {
                    /* Chapter 94: same drain-parser dance as
                     * GUI_EVENT_CLOSE. */
                    if (s.parser) {
                        parser_wait_idle(s.parser, s.page);
                        parser_shutdown(s.parser);
                        free(s.parser);
                        s.parser = 0;
                    }
                    gui_destroy_window(s.win_id);
                    free_page(s.page);
                    for (int i = 0; i < s.hist_count; i++)
                        if (s.history[i]) free(s.history[i]);
                    return 0;
                }
                break;
            }
            }
            if (s.scroll_y < 0)              s.scroll_y = 0;
            if (s.scroll_y > s.max_scroll_y) s.scroll_y = s.max_scroll_y;
            if (s.scroll_x < 0)              s.scroll_x = 0;
            if (s.scroll_x > s.max_scroll_x) s.scroll_x = s.max_scroll_x;
            if (s.scroll_x != prev_x || s.scroll_y != prev_y)
                s.dirty = 1;
            break;
        }
        default:
            break;
        }
    }
}

/* ----------------------------------------------------------------
 * Top level.
 * ---------------------------------------------------------------- */

static void usage(void)
{
    printf("usage: browser [--paint|--ansi|--gui] [--timing] <url-or-path> [viewport]\n");
    printf("  http://host[:port]/path     fetch over HTTP\n");
    printf("  /mnt/file.html              read from local FS\n");
    printf("  --paint                     dump paint stream (debug)\n");
    printf("  --ansi                      24-bit colour + ESC underline\n");
    printf("  --gui                       open a window; arrow keys / space scroll\n");
    printf("  --timing                    print per-stage uptime_ms() deltas\n");
    printf("  --bench-resize <new_w>      headless chapter-94 parser-thread\n");
    printf("                              benchmark: load page, dispatch a\n");
    printf("                              relayout to <new_w> on the parser\n");
    printf("                              thread, count GUI-loop iterations\n");
    printf("                              that ran while parser was busy.\n");
    printf("  viewport                    layout width in px (default 800)\n");
}

/* Chapter 94: headless parser-thread benchmark.
 *
 * Loads the page synchronously, spawns the parser thread, posts a
 * relayout request to a different viewport, and spins polling for
 * completion.  Each iteration of the spin loop counts as a "GUI
 * iteration that happened while the parser was busy."  When the
 * parser publishes a result, we absorb it and print:
 *
 *   BENCH parse_ms=N gui_iters=N work_done=N
 *   BENCH old_w=N new_w=N old_doc=NxN new_doc=NxN
 *
 * The test (scripts/test_browser_parser_thread.py) asserts
 * gui_iters > 0, which is the chapter-94 invariant in machine-
 * checkable form: the GUI loop kept running while the parser
 * worked. */
static int run_bench_resize(const char *url, int initial_viewport,
                              int new_viewport)
{
    printf("[bench] load %s @ viewport=%d\n", url, initial_viewport);
    char *abs = canonicalize_url(url, 0);
    if (!abs) { printf("bench: oom (url)\n"); return 1; }

    struct loaded_page *p = load_page(abs, initial_viewport);
    free(abs);
    if (!p) { printf("bench: load_page failed\n"); return 1; }
    printf("[bench] loaded ok doc=%dx%d (cmds=%d)\n",
           p->ldoc.doc_width_px, p->ldoc.doc_height_px, p->pb.n);

    struct parser_state *ps =
        (struct parser_state *)malloc(sizeof(*ps));
    if (!ps) { printf("bench: oom (ps)\n"); free_page(p); return 1; }
    parser_init(ps);
    if (parser_spawn(ps, /*cpu_id=*/1) < 0) {
        printf("bench: parser_spawn failed\n");
        free(ps); free_page(p); return 1;
    }

    int old_w = p->ldoc.doc_width_px;
    int old_h = p->ldoc.doc_height_px;

    unsigned long t0 = uptime_ms();
    parser_request_relayout(ps, p, new_viewport);

    /* Spin polling for completion.  Each "no result yet" iteration
     * is one GUI loop tick — exactly the same shape as the real
     * event loop, just without rendering or events. */
    uint32_t gui_iters = 0;
    for (;;) {
        if (parser_absorb_completion(ps, p)) break;
        gui_iters++;
        yield();
    }
    unsigned long t1 = uptime_ms();

    int new_w = p->ldoc.doc_width_px;
    int new_h = p->ldoc.doc_height_px;
    uint32_t work_done = atomic_load32_u(&ps->work_done_count);

    printf("BENCH parse_ms=%lu gui_iters=%u work_done=%u\n",
           t1 - t0, gui_iters, work_done);
    printf("BENCH old_w=%d new_w=%d old_doc=%dx%d new_doc=%dx%d\n",
           initial_viewport, new_viewport,
           old_w, old_h, new_w, new_h);

    parser_shutdown(ps);
    free(ps);
    free_page(p);
    printf("[bench] OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    int mode_paint = 0;
    int mode_ansi  = 0;
    int mode_gui   = 0;
    int viewport   = 800;
    int bench_new_w = 0;       /* nonzero => --bench-resize mode */
    const char *src = 0;

    /* Parse flags. */
    int i = 1;
    while (i < argc && argv[i] && argv[i][0] == '-') {
        if (br_streq(argv[i], "--paint")) { mode_paint = 1; i++; continue; }
        if (br_streq(argv[i], "--ansi"))  { mode_ansi  = 1; i++; continue; }
        if (br_streq(argv[i], "--gui"))   { mode_gui   = 1; i++; continue; }
        if (br_streq(argv[i], "--timing")){ g_timing   = 1; i++; continue; }
        if (br_streq(argv[i], "--bench-resize")) {
            i++;
            if (i >= argc) {
                printf("browser: --bench-resize requires a width\n");
                return 1;
            }
            bench_new_w = br_atoi(argv[i]);
            if (bench_new_w < 64) bench_new_w = 64;
            i++; continue;
        }
        if (br_streq(argv[i], "-h") || br_streq(argv[i], "--help")) {
            usage(); return 0;
        }
        printf("browser: unknown flag '%s'\n", argv[i]);
        usage();
        return 1;
    }
    {
        char envbuf[8];
        if (getenv("BROWSER_TIMING", envbuf, sizeof(envbuf)) > 0
            && envbuf[0] && envbuf[0] != '0')
            g_timing = 1;
    }
    if (i >= argc) {
        /* In GUI mode, default to the test fixture so the launcher
         * can spawn /bin/browser --gui with no args. */
        if (mode_gui) src = "/mnt/test_layout.html";
        else { usage(); return 1; }
    } else {
        src = argv[i++];
    }
    if (i < argc) viewport = br_atoi(argv[i]);
    if (viewport < 64) viewport = 64;
    if (viewport > 4096) viewport = 4096;

    printf("[browser] src=%s viewport=%d mode=%s\n",
           src, viewport,
           mode_paint ? "paint" :
           mode_gui   ? "gui"   :
           (mode_ansi ? "ansi" : "plain"));

    /* GUI mode owns its own pipeline so it can re-load on every
     * navigation (back/forward, link click, address bar Enter). */
    if (mode_gui)
        return run_gui(src, viewport);

    /* Chapter 94 headless benchmark. */
    if (bench_new_w)
        return run_bench_resize(src, viewport, bench_new_w);

    unsigned long t_total = uptime_ms();
    unsigned long t_stage = t_total;

    /* --- fetch --- */
    size_t html_len = 0;
    char  *origin   = 0;
    char  *html_buf = fetch(src, &html_len, &origin);
    if (!html_buf) {
        if (origin) free(origin);
        return 1;
    }
    BR_TIMING("fetch", t_stage);
    if (g_timing)
        printf("[timing] html size: %lu bytes\n", (unsigned long)html_len);

    /* --- tokenise + DOM --- */
    struct html_tokenizer tz;
    html_tok_init(&tz, html_buf, html_len);

    struct html_token *scratch =
        (struct html_token *)malloc(sizeof(*scratch));
    if (!scratch) {
        printf("browser: oom (token scratch)\n");
        free(html_buf);
        if (origin) free(origin);
        return 1;
    }

    struct dom dom;
    if (dom_init(&dom) < 0) {
        printf("browser: oom (dom_init)\n");
        free(scratch); free(html_buf);
        if (origin) free(origin);
        return 1;
    }
    int drc = dom_build(&dom, &tz, scratch);
    if (drc < 0)
        printf("[browser] dom_build returned %d (continuing)\n", drc);
    BR_TIMING("tokenise + DOM", t_stage);

    /* --- author CSS --- */
    int author_len = 0;
    char *author_css = layout_collect_inline_styles(dom_root(&dom), &author_len);

    /* External <link rel="stylesheet"> sheets (HTTP only).  Fetch
     * each one and append to the inline-<style> blob so the cascade
     * sees them in source order (inline first, then external — same
     * as a browser would, since inline <style> in <head> typically
     * precedes <link> here, but this fixture's order doesn't matter
     * for cascade tie-break specificity).  origin_url is NULL for
     * file:// loads, in which case external CSS is silently skipped. */
    size_t link_css_len = 0;
    char *link_css = fetch_external_stylesheets(dom_root(&dom),
                                                  origin, &link_css_len);
    if (link_css && link_css_len > 0) {
        size_t need = (size_t)author_len + link_css_len + 2;
        char *combined = (char *)malloc(need);
        if (combined) {
            size_t off = 0;
            if (author_css && author_len > 0) {
                for (int i = 0; i < author_len; i++) combined[off++] = author_css[i];
                combined[off++] = '\n';
            }
            for (size_t i = 0; i < link_css_len; i++) combined[off++] = link_css[i];
            combined[off] = '\0';
            if (author_css) free(author_css);
            author_css = combined;
            author_len = (int)off;
        }
        free(link_css);
    }
    printf("[browser] author CSS: %d bytes total\n", author_len);
    BR_TIMING("collect/fetch CSS", t_stage);

    /* --- layout --- */
    struct layout_doc ldoc;
    int rc = layout_build_and_run(&ldoc, dom_root(&dom),
                                   author_css, author_len, viewport);
    if (rc < 0) {
        printf("browser: layout failed\n");
        if (author_css) free(author_css);
        dom_destroy(&dom);
        free(scratch); free(html_buf);
        if (origin) free(origin);
        return 1;
    }
    BR_TIMING("layout", t_stage);

    /* --- paint --- */
    struct layout_paint_buf pb;
    layout_paint_collect(&ldoc, &pb);
    BR_TIMING("paint collect", t_stage);
    if (g_timing)
        printf("[timing] paint cmds: %d, doc=%dx%d\n",
               pb.n, ldoc.doc_width_px, ldoc.doc_height_px);

    if (mode_paint) {
        dump_paints(&ldoc, &pb);
    } else {
        int cols = (ldoc.doc_width_px  + BR_CELL_W - 1) / BR_CELL_W;
        int rows = (ldoc.doc_height_px + BR_CELL_H - 1) / BR_CELL_H;
        if (cols < 8)  cols = 8;
        if (rows < 1)  rows = 1;
        if (rows > 4096) rows = 4096;

        struct br_grid g;
        if (br_grid_init(&g, cols, rows, mode_ansi) < 0) {
            printf("browser: grid alloc failed (cols=%d rows=%d)\n", cols, rows);
        } else {
            printf("[browser] cols=%d rows=%d paints=%d\n",
                   cols, rows, pb.n);
            render_paints(&g, &pb);
            if (mode_ansi) render_grid_ansi(&g);
            else           render_grid_plain(&g);
            br_grid_destroy(&g);
        }
    }

    layout_paint_buf_destroy(&pb);
    layout_doc_destroy(&ldoc);

    if (author_css) free(author_css);
    dom_destroy(&dom);
    free(scratch);
    free(html_buf);
    if (origin) free(origin);
    return 0;
}
