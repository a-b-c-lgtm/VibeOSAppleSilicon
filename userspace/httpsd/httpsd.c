/* userspace/httpsd/httpsd.c -- chapter 125 in-guest TLS test server.
 *
 * Smallest-possible HTTPS server: listens on a port (default
 * 8443), accepts one TLS connection at a time, presents the
 * sample RSA-2048 cert chain for CN=localhost
 * (vendor/testcerts/test_chain.c), reads a single HTTP/1.0
 * request line, and emits a fixed 200 OK response.
 *
 * Used by:
 *   - scripts/test_tls_handshake.py (boots kernel, expects
 *     tlstest --handshake to PASS)
 *   - any future browser test that wants HTTPS over loopback
 *     without going outside the guest
 *
 * What httpsd intentionally does NOT do (yet):
 *   - keep-alive (HTTP/1.0 + Connection: close, like httpd)
 *   - multiple concurrent connections (single-threaded accept loop)
 *   - request body parsing (GET only)
 *   - URL routing (any path returns the same body)
 *   - session resumption (every handshake is full)
 *   - chunked encoding (Content-Length up front)
 *
 * Compared to httpd (chapters 107 / 106a) this binary is ~150
 * lines of glue plus libbearssl.a and our tls_socket.c-style
 * server-half adapter.  The TLS handshake itself is BearSSL's;
 * we just have to feed it bytes via the four-buffer
 * sendrec/recvrec/sendapp/recvapp state machine.
 *
 * Usage:
 *   httpsd                -- listen on port 8443, present the
 *                            chapter-112b RSA-2048 sample chain
 *   httpsd <port>         -- same, on a specific port
 *   httpsd --ec <port>    -- chapter 128: listen on <port> and
 *                            present the EC / ECDSA sample chain
 *                            instead.  Used by the chapter-112e
 *                            multi-anchor test to prove the
 *                            browser's trust store can carry
 *                            (and the X.509 minimal validator
 *                            can honour) more than one CA at a
 *                            time, mixing key types.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/freestanding.h"
#include "../libc/malloc.h"
#include "../../vendor/testcerts/test_chain.h"

#include "bearssl.h"

#define HTTPSD_DEFAULT_PORT  8443
#define HTTPSD_VERSION       "osdev-httpsd/1.0"

/* The response body we serve for any GET.  The marker
 * "tls handshake ok" lets the test script verify end-to-end
 * encryption worked -- you can't see that string in the cleartext
 * TLS records, so receiving it after a TLS decode is proof. */
static const char k_body[] =
    "tls handshake ok\n"
    "served by " HTTPSD_VERSION "\n";

/* ----------------------------------------------------------------
 * Small string helpers (no libc).
 * ---------------------------------------------------------------- */

static size_t s_len(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* ----------------------------------------------------------------
 * argv → port parser (mirrors httpd.c).
 * ---------------------------------------------------------------- */

static int parse_port(const char *s, int *out)
{
    if (!s || !*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    *out = v;
    return 0;
}

/* ----------------------------------------------------------------
 * br_sslio transport adapters (same shape as tls_socket.c's, but
 * we inline them here so httpsd doesn't depend on the client lib
 * surface area).
 * ---------------------------------------------------------------- */

static int srv_low_read(void *ctx, unsigned char *data, size_t len)
{
    int fd = *(int *)ctx;
    if (len > 16384) len = 16384;
    long n = read(fd, data, len);
    if (n <= 0) return -1;
    return (int)n;
}

static int srv_low_write(void *ctx, const unsigned char *data, size_t len)
{
    int fd = *(int *)ctx;
    if (len > 16384) len = 16384;
    long n = write(fd, data, len);
    if (n <= 0) return -1;
    return (int)n;
}

/* ----------------------------------------------------------------
 * Per-connection handler.  Runs the server-side handshake, then
 * reads bytes until it sees the end-of-headers marker, then
 * writes the canned response, then runs close_notify.
 * ---------------------------------------------------------------- */

/* State for one connection.  Lives on the stack of handle_one;
 * sizeof(...) is ~36 KB (mostly the BearSSL bidi I/O buffer plus
 * the server context).  Userspace default stack is 64 KB so this
 * just fits; chapter 127 will move to heap when we add real
 * concurrency. */
typedef struct {
    int                     fd;
    br_ssl_server_context   sc;
    br_sslio_context        ioc;
    unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];
} conn_t;

static int srv_inject_entropy(br_ssl_engine_context *eng)
{
    unsigned char seed[64];
    long got = getrandom(seed, sizeof seed, 0u);
    if (got != (long)sizeof seed) return -1;
    br_ssl_engine_inject_entropy(eng, seed, sizeof seed);
    return 0;
}

/* Chapter 128: which sample chain to present.  Set by main() from
 * the --ec CLI flag; defaulting to RSA preserves the chapter-112b
 * behaviour ("httpsd 8443" → RSA-2048, no flag required). */
static int g_use_ec = 0;

static void handle_one(int cfd)
{
    /* Heap-allocate so we don't overflow the stack.  malloc()
     * here is per-connection; we free at the end of the function. */
    conn_t *c = (conn_t *)malloc(sizeof *c);
    if (!c) {
        printf("httpsd: malloc(%lu) failed\n",
               (unsigned long)sizeof *c);
        close(cfd);
        return;
    }
    c->fd = cfd;

    /* Initialise the server engine with the (chain, key) pair
     * from test_chain*.c.  RSA mode uses init_full_rsa (every
     * RSA-key-exchange suite BearSSL knows); EC mode uses
     * init_full_ec which advertises ECDHE_ECDSA_* suites that
     * sign with our P-256 leaf.  The client will pick the
     * highest-priority intersection from the ServerHello.  */
    if (g_use_ec) {
        br_ssl_server_init_full_ec(&c->sc,
                                   test_server_chain_ec,
                                   test_server_chain_ec_len,
                                   BR_KEYTYPE_EC,
                                   test_server_key_ec);
    } else {
        br_ssl_server_init_full_rsa(&c->sc,
                                    test_server_chain,
                                    test_server_chain_len,
                                    test_server_key);
    }

    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);

    if (srv_inject_entropy(&c->sc.eng) != 0) {
        printf("httpsd: getrandom failed\n");
        free(c); close(cfd); return;
    }
    if (!br_ssl_server_reset(&c->sc)) {
        printf("httpsd: server_reset failed err=%d\n",
               (int)br_ssl_engine_last_error(&c->sc.eng));
        free(c); close(cfd); return;
    }

    br_sslio_init(&c->ioc, &c->sc.eng,
                  srv_low_read,  &c->fd,
                  srv_low_write, &c->fd);

    /* Read up to 1 KiB of the request (or until \r\n\r\n).  We
     * don't actually parse it -- any GET gets the same response
     * -- but we have to drain it so the client's send buffer
     * doesn't back-pressure-deadlock against our response. */
    char req[1024];
    int total = 0;
    int saw_end = 0;
    while (total < (int)sizeof req - 1 && !saw_end) {
        int n = br_sslio_read(&c->ioc, req + total,
                              sizeof req - 1 - total);
        if (n <= 0) break;
        total += n;
        req[total] = 0;
        /* Look for end-of-headers in what we've read so far. */
        for (int i = 3; i < total; i++) {
            if (req[i-3]=='\r' && req[i-2]=='\n'
             && req[i-1]=='\r' && req[i  ]=='\n') {
                saw_end = 1;
                break;
            }
        }
    }

    /* Build response head + body.  Single write so the engine
     * wraps it into one record if it fits (it does -- our body
     * is ~50 bytes). */
    char head[256];
    int hl = snprintf(head, sizeof head,
        "HTTP/1.0 200 OK\r\n"
        "Server: " HTTPSD_VERSION "\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        (unsigned)(sizeof k_body - 1));
    if (hl < 0) hl = 0;

    int rc1 = br_sslio_write_all(&c->ioc, head, (size_t)hl);
    int rc2 = br_sslio_write_all(&c->ioc, k_body, sizeof k_body - 1);
    int rc3 = br_sslio_flush(&c->ioc);
    if (rc1 != 0 || rc2 != 0 || rc3 != 0) {
        printf("httpsd: write/flush failed err=%d\n",
               (int)br_ssl_engine_last_error(&c->sc.eng));
        free(c); close(cfd); return;
    }

    /* Try to do a clean close_notify exchange.  If the client
     * has already gone, this fails -- we don't care; the TCP
     * close will signal end-of-stream regardless. */
    (void)br_sslio_close(&c->ioc);
    close(cfd);
    free(c);
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int port = HTTPSD_DEFAULT_PORT;

    /* Chapter 128: accept an optional "--ec" leading flag that
     * flips the server onto the ECDSA / P-256 sample chain.  The
     * remaining positional arg, if any, is still the port number.
     * Keeping the parser this small (instead of pulling in a
     * full getopt) is deliberate -- httpsd is a regression-only
     * binary, not a user-facing service. */
    int argi = 1;
    if (argi < argc && argv[argi]) {
        const char *a = argv[argi];
        if (a[0] == '-' && a[1] == '-' && a[2] == 'e' && a[3] == 'c'
                                       && a[4] == 0) {
            g_use_ec = 1;
            argi++;
        }
    }
    if (argi < argc && parse_port(argv[argi], &port) != 0) {
        printf("httpsd: bad port `%s`\n", argv[argi]);
        return 2;
    }

    int lfd = socket_listen((uint16_t)port, 4);
    if (lfd < 0) {
        printf("httpsd: socket_listen(%d) failed: %s\n",
               port, strerror(errno));
        return 1;
    }
    printf("httpsd: " HTTPSD_VERSION " listening on port %d\n", port);
    printf("httpsd: serving %u-byte body, sample CN=localhost cert (%s)\n",
           (unsigned)(sizeof k_body - 1),
           g_use_ec ? "ECDSA P-256" : "RSA-2048");
    (void)s_len;  /* future use */

    for (;;) {
        uint32_t peer_ip = 0;
        uint16_t peer_port = 0;
        int cfd = socket_accept(lfd, &peer_ip, &peer_port);
        if (cfd < 0) {
            printf("httpsd: accept failed: %s\n", strerror(errno));
            return 1;
        }
        printf("httpsd: tls connection from %u.%u.%u.%u:%u\n",
               (unsigned)((peer_ip >> 24) & 0xff),
               (unsigned)((peer_ip >> 16) & 0xff),
               (unsigned)((peer_ip >>  8) & 0xff),
               (unsigned)( peer_ip        & 0xff),
               (unsigned)peer_port);
        handle_one(cfd);
    }
}
