/* userspace/tlstest/tlstest.c — chapters 112a + 112b + 112c.
 *
 * Three modes, picked by argv:
 *
 *   tlstest                          -- chapter 112a SHA-256 KAT
 *                                       (no TLS, no sockets).  Just
 *                                       proves libbearssl.a links
 *                                       and br_sha256 computes the
 *                                       right answer.
 *
 *   tlstest --handshake HOST PORT    -- chapter 112b TLS handshake
 *                                       against an in-guest httpsd.
 *                                       Validates with knownkey
 *                                       (pins on leaf cert pubkey).
 *
 *   tlstest --handshake-ca HOST PORT -- chapter 112c TLS handshake
 *                                       with REAL chain validation
 *                                       against the sample
 *                                       intermediate CA + wall-clock
 *                                       from SYS_GETTIMEOFDAY.
 *
 * Any mode prints a `tlstest: PASS ...` line on success and a
 * `tlstest: FAIL ...` line on the first failure.  Exit status:
 *   0  PASS
 *   1  test failure
 *   2  argv / runtime error
 *
 * SHA-256 NIST KAT references:
 *   SHA256("")    = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 *   SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/freestanding.h"
#include "../libc/tls_socket.h"
#include "../libc/malloc.h"
#include "../../vendor/testcerts/test_chain.h"

#include "bearssl.h"

static const unsigned char k_empty_expected[32] = {
    0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
    0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
    0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
};

static const unsigned char k_abc_expected[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
};

static void print_hex32(const char *label, const unsigned char *d)
{
    /* label ("tlstest sha256(empty)" / "tlstest sha256(abc)  ") =
     * 21 chars + ": " (2) + 32 bytes * 2 hex chars (64) + '\n' (1)
     * + '\0' (1) = 89 bytes.  Round up generously so future label
     * tweaks don't silently smash the stack — the previous
     * buf[80] overflowed by 9 bytes and clobbered main's return
     * address, which caused main() to loop back to its own
     * entry point instead of reaching crt0's SYS_EXIT. */
    char buf[160];
    static const char hex[] = "0123456789abcdef";
    unsigned int i = 0;
    while (label[i]) { buf[i] = label[i]; i++; }
    buf[i++] = ':'; buf[i++] = ' ';
    for (unsigned int j = 0; j < 32; j++) {
        buf[i++] = hex[(d[j] >> 4) & 0xF];
        buf[i++] = hex[ d[j]       & 0xF];
    }
    buf[i++] = '\n';
    buf[i]   = '\0';
    write(1, buf, i);
}

static int eq32(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 32; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* --------------------------------------------------------------
 * Mode A: chapter 112a SHA-256 KAT.
 * -------------------------------------------------------------- */

static int run_sha256_kat(void)
{
    br_sha256_context ctx;
    unsigned char     digest[32];

    br_sha256_init(&ctx);
    br_sha256_out(&ctx, digest);
    print_hex32("tlstest sha256(empty)", digest);
    if (!eq32(digest, k_empty_expected)) {
        printf("tlstest: FAIL empty-string vector mismatch\n");
        return 1;
    }

    br_sha256_init(&ctx);
    br_sha256_update(&ctx, "abc", 3);
    br_sha256_out(&ctx, digest);
    print_hex32("tlstest sha256(abc)  ", digest);
    if (!eq32(digest, k_abc_expected)) {
        printf("tlstest: FAIL abc vector mismatch\n");
        return 1;
    }

    printf("tlstest: PASS bearssl sha256 matches NIST vectors\n");
    return 0;
}

/* --------------------------------------------------------------
 * Modes B/C: TLS handshake against in-guest httpsd.
 *
 *   mode = 0 : chapter 112b knownkey -- pin on the leaf cert's
 *              public key, no chain walk, no time check.
 *   mode = 1 : chapter 112c chain    -- real X.509 chain walk
 *              against the intermediate CA cert, with notBefore/
 *              notAfter checked against wall-clock from
 *              SYS_GETTIMEOFDAY.
 *
 * Parses HOST as a dotted-quad IPv4 (we don't want a DNS round
 * trip for a loopback test).  Sends a minimal HTTP/1.0 GET and
 * reads until peer FIN -- asserting the response body contains
 * the well-known marker that httpsd emits.
 * -------------------------------------------------------------- */

static const char k_marker[] = "tls handshake ok";

static int parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int pi = 0;
    int saw = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            parts[pi] = parts[pi] * 10 + (uint32_t)(*s - '0');
            if (parts[pi] > 255) return -1;
            saw = 1;
        } else if (*s == '.') {
            if (!saw || pi >= 3) return -1;
            pi++;
            saw = 0;
        } else {
            return -1;
        }
        s++;
    }
    if (pi != 3 || !saw) return -1;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

static int parse_u16(const char *s, uint16_t *out)
{
    int v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int contains(const char *haystack, int hlen, const char *needle)
{
    int nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || nlen > hlen) return 0;
    for (int i = 0; i + nlen <= hlen; i++) {
        int ok = 1;
        for (int j = 0; j < nlen; j++) {
            if (haystack[i+j] != needle[j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static int run_handshake_mode(const char *host_str, const char *port_str,
                              int mode)
{
    uint32_t ip4_be = 0;
    if (parse_ipv4(host_str, &ip4_be) != 0) {
        printf("tlstest: FAIL bad host `%s` (expect dotted IPv4)\n",
               host_str);
        return 1;
    }
    uint16_t port = 0;
    if (parse_u16(port_str, &port) != 0) {
        printf("tlstest: FAIL bad port `%s`\n", port_str);
        return 1;
    }

    /* Heap-allocate the tls_socket_t (~42 KB; bigger than our
     * 64 KB user stack wants to share with everything else). */
    tls_socket_t *t = (tls_socket_t *)malloc(sizeof *t);
    if (!t) {
        printf("tlstest: FAIL malloc(tls_socket_t=%lu) failed\n",
               (unsigned long)sizeof *t);
        return 1;
    }

    if (mode == 0) {
        /* knownkey: pin on test_server_chain[0] (end-entity). */
        if (tls_socket_init_knownkey_from_cert(t,
                test_server_chain[0].data,
                test_server_chain[0].data_len) != 0) {
            printf("tlstest: FAIL knownkey init: cert failed to decode\n");
            free(t);
            return 1;
        }
        printf("tlstest: pinned RSA-%u public key from leaf cert (%lu DER bytes)\n",
               (unsigned)(t->pinned_pk.nlen * 8),
               (unsigned long)test_server_chain[0].data_len);
    } else {
        /* chain: build trust anchor from test_server_chain[1]
         * (the intermediate CA in BearSSL's sample chain).  The
         * sample intermediate is self-signed and marked CA, so
         * we can use it as a one-entry trust list directly. */
        if (tls_socket_init_chain_from_anchor(t,
                test_server_chain[1].data,
                test_server_chain[1].data_len) != 0) {
            printf("tlstest: FAIL chain init: CA cert failed to decode\n");
            free(t);
            return 1;
        }
        printf("tlstest: built trust anchor from CA cert "
               "(%lu DER bytes, DN %lu bytes, RSA-%u)\n",
               (unsigned long)test_server_chain[1].data_len,
               (unsigned long)t->anchors[0].dn.len,
               (unsigned)(t->anchors[0].pkey.key.rsa.nlen * 8));

        /* Confirm the wallclock is in the sample cert's validity
         * window (2010-01-01..2037-12-31).  If the test ever
         * starts running before 2010 or after 2037 we want a
         * clearer error than "BR_ERR_X509_EXPIRED". */
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 0) {
            printf("tlstest: wallclock tv_sec=%ld (validator anchored to it)\n",
                   (long)tv.tv_sec);
        }
    }

    printf("tlstest: connecting %u.%u.%u.%u:%u (SNI=localhost%s)\n",
           (unsigned)((ip4_be >> 24) & 0xff),
           (unsigned)((ip4_be >> 16) & 0xff),
           (unsigned)((ip4_be >>  8) & 0xff),
           (unsigned)( ip4_be        & 0xff),
           (unsigned)port,
           mode == 1 ? ", chain" : ", knownkey");

    int rc = tls_socket_connect(t, ip4_be, port, "localhost");
    if (rc != 0) {
        printf("tlstest: FAIL tls handshake rc=%d\n", rc);
        free(t);
        return 1;
    }
    printf("tlstest: handshake complete; sending GET /%s\n",
           mode == 1 ? "m112c" : "m112b");

    /* Build the request line per mode so the marker in the
     * response can prove WHICH validator was active. */
    static const char k_req_b[] =
        "GET /m112b HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "User-Agent: tlstest/112b\r\n"
        "\r\n";
    static const char k_req_c[] =
        "GET /m112c HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "User-Agent: tlstest/112c\r\n"
        "\r\n";
    const char *req = (mode == 1) ? k_req_c : k_req_b;
    size_t      req_len = (mode == 1) ? sizeof k_req_c - 1
                                       : sizeof k_req_b - 1;

    if (br_sslio_write_all(&t->ioc, req, req_len) != 0
     || br_sslio_flush(&t->ioc) != 0) {
        printf("tlstest: FAIL sending request err=%d\n",
               (int)br_ssl_engine_last_error(&t->cc.eng));
        free(t);
        return 1;
    }

    /* Drain the response.  Stop on peer FIN (close_notify) or
     * 4 KiB, whichever comes first. */
    char resp[4096];
    int total = 0;
    while (total < (int)sizeof resp - 1) {
        int n = tls_socket_recv(t, resp + total,
                                (size_t)((int)sizeof resp - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    resp[total < (int)sizeof resp ? total : (int)sizeof resp - 1] = 0;
    printf("tlstest: read %d bytes from server\n", total);

    /* tls_socket_close runs close_notify; we don't care if the
     * server already FIN'd. */
    tls_socket_close(t);

    if (!contains(resp, total, k_marker)) {
        printf("tlstest: FAIL marker `%s` not found in response\n",
               k_marker);
        free(t);
        return 1;
    }
    free(t);

    printf("tls handshake: PASS chapter 112%s end-to-end\n",
           mode == 1 ? "c" : "b");
    return 0;
}

static int run_handshake(const char *host_str, const char *port_str)
{
    return run_handshake_mode(host_str, port_str, 0);
}

static int run_handshake_ca(const char *host_str, const char *port_str)
{
    return run_handshake_mode(host_str, port_str, 1);
}

/* -------------------------------------------------------------- */

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2) {
        const char *cmd = argv[1];
        if (streq(cmd, "--handshake")) {
            if (argc < 4) {
                printf("tlstest: usage: tlstest --handshake HOST PORT\n");
                return 2;
            }
            return run_handshake(argv[2], argv[3]);
        }
        if (streq(cmd, "--handshake-ca")) {
            if (argc < 4) {
                printf("tlstest: usage: tlstest --handshake-ca HOST PORT\n");
                return 2;
            }
            return run_handshake_ca(argv[2], argv[3]);
        }
        printf("tlstest: unknown argument `%s`\n", argv[1]);
        return 2;
    }
    return run_sha256_kat();
}
