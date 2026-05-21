/* userspace/libc/tls_socket.h -- in-guest TLS client (112b/112c/112e).
 *
 * Thin wrapper that glues:
 *
 *   - osdev userspace TCP sockets (socket_connect / read / write
 *     / close from syscall.h, chapter 104)
 *   - BearSSL's client SSL engine (br_ssl_client_init_full,
 *     br_sslio_*, chapter 112a vendoring)
 *   - osdev's kernel CSPRNG (SYS_GETRANDOM, chapter 112)
 *   - osdev's wall-clock (SYS_GETTIMEOFDAY, chapter 95) for the
 *     X.509 minimal validator's notBefore/notAfter checks
 *
 * Three trust modes:
 *
 *   - knownkey (112b): pin on the leaf cert's public key. No
 *     chain walk, no time check, no name check beyond SNI.
 *
 *   - chain (112c): real X.509 chain validation against a single
 *     trust anchor.  The anchor is built at runtime from a DER
 *     CA cert (DN bytes captured via append_dn, pubkey via
 *     get_pkey -- supports both RSA and ECDSA anchors).
 *
 *   - chain-multi (112e): same validator, but pass N anchors at
 *     once so a single tls_socket_t can talk to multiple servers
 *     issued by different CAs.  The anchor list lives inside the
 *     struct (compile-time TLS_MAX_ANCHORS).  Build it from a
 *     framed on-disk CA bundle ("CAB1" magic) with
 *     tls_socket_init_chain_from_bundle, or from an array of DER
 *     blobs with tls_socket_init_chain_multi.
 *
 * Usage (client):
 *
 *     tls_socket_t *t = malloc(sizeof *t);
 *     // pick ONE of:
 *     tls_socket_init_knownkey_from_cert(t, leaf_der, leaf_len);
 *     tls_socket_init_chain_from_anchor(t,  ca_der,   ca_len);
 *     tls_socket_init_chain_from_bundle(t,  bundle_buf, bundle_len);
 *     int rc = tls_socket_connect(t, IP4(127,0,0,1), 8443, "localhost");
 *     if (rc != 0) ... ;
 *     tls_socket_send(t, "GET / HTTP/1.0\r\n\r\n", 18);
 *     ssize_t n = tls_socket_recv(t, buf, sizeof buf);
 *     tls_socket_close(t);
 *
 * The struct is ~60 KB at TLS_MAX_ANCHORS = 8 (mostly
 * BR_SSL_BUFSIZE_BIDI plus the per-anchor DN+RSA-n buffers).
 * HEAP-ALLOCATE, do not put it on the stack.
 */
#ifndef OSDEV_LIBC_TLS_SOCKET_H
#define OSDEV_LIBC_TLS_SOCKET_H

#include <stdint.h>
#include <stddef.h>

#include "bearssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chapter 112e/112f/112g compile-time limits.
 *
 * TLS_MAX_ANCHORS = 256 covers a full Mozilla NSS root list
 * (~150 entries on current macOS / curl.se bundles) with
 * headroom for new roots without a recompile.  The cost is
 * ~1.2 KiB per slot of zero-init scratch in the tls_socket_t
 * struct -- a fully-populated socket is ~310 KiB on the heap.
 * Each in-flight browser fetch allocates one; that's well
 * within the user heap budget.
 *
 * TLS_ANCHOR_DN_MAX = 512 covers any X.500 name seen in
 * practice (typical roots: 80-150 B; long enterprise names can
 * hit ~300).
 *
 * TLS_ANCHOR_RSA_N_MAX = 520 covers RSA up to ~4096-bit moduli
 * with margin (ISRG Root X1 is 512 bytes).
 *
 * TLS_ANCHOR_EC_Q_MAX = 133 covers P-521 uncompressed points
 * (0x04 || X-66 || Y-66); P-256 fits in 65. */
#define TLS_MAX_ANCHORS       256
#define TLS_ANCHOR_DN_MAX     512
#define TLS_ANCHOR_RSA_N_MAX  520
#define TLS_ANCHOR_RSA_E_MAX    8
#define TLS_ANCHOR_EC_Q_MAX   133

/* Magic + version for the on-disk CA bundle that
 * tls_socket_init_chain_from_bundle reads.  Layout:
 *
 *   [4] magic     = "CAB1" (ASCII, no NUL)
 *   [4] count     = u32 LE
 *   for each anchor:
 *       [4] der_len = u32 LE
 *       [der_len] DER bytes (raw X.509 certificate)
 *
 * Built at host time by scripts/mkcabundle.py from PEM or
 * .h-extracted sources.  Lives in OSFS as /mnt/ca.bundle. */
#define TLS_BUNDLE_MAGIC "CAB1"

typedef struct {
    int                       fd;        /* underlying TCP fd, -1 if closed */
    br_ssl_client_context     cc;
    br_x509_minimal_context   xc;        /* the minimal X.509 validator.
                                          * Required by br_ssl_client_init_full
                                          * (it dereferences this even when we
                                          * override the validator).  Active in
                                          * chain mode; dead state in knownkey. */
    br_x509_knownkey_context  xkc;       /* pinned-pubkey validator (knownkey) */

    /* Pinned-pubkey storage (knownkey mode). */
    br_rsa_public_key         pinned_pk;
    unsigned char             pinned_pk_n[520]; /* up to RSA-4096 modulus      */
    unsigned char             pinned_pk_e[8];   /* up to 64-bit exponent       */

    /* Multi-anchor storage (chain mode, 112c + 112e).  The
     * anchor's dn.data, pkey.key.{rsa.n,rsa.e,ec.q} MUST point
     * at long-lived memory inside this struct -- BearSSL never
     * copies.  Each row of the parallel arrays below stores the
     * backing bytes for anchors[i].  anchor_count is the
     * effective length; up to TLS_MAX_ANCHORS. */
    br_x509_trust_anchor      anchors[TLS_MAX_ANCHORS];
    unsigned char             anchor_dn  [TLS_MAX_ANCHORS][TLS_ANCHOR_DN_MAX];
    unsigned char             anchor_pk_n[TLS_MAX_ANCHORS][TLS_ANCHOR_RSA_N_MAX];
    unsigned char             anchor_pk_e[TLS_MAX_ANCHORS][TLS_ANCHOR_RSA_E_MAX];
    unsigned char             anchor_pk_q[TLS_MAX_ANCHORS][TLS_ANCHOR_EC_Q_MAX];
    int                       anchor_count;

    int                       chain_mode;       /* 0 = knownkey, 1 = chain     */

    br_sslio_context          ioc;
    unsigned char             iobuf[BR_SSL_BUFSIZE_BIDI];
} tls_socket_t;

/* Initialise `t` with a pinned RSA public key extracted from a
 * DER X.509 certificate.  The caller is asserting "any server
 * presenting a chain whose leaf has THIS public key is trusted"
 * -- no chain validation, no time check, no name check beyond
 * SNI matching.  Returns 0 on success, -1 if the cert failed to
 * decode (in which case `t` is left in a state where connect()
 * will refuse). */
int tls_socket_init_knownkey_from_cert(tls_socket_t *t,
                                       const unsigned char *cert_der,
                                       size_t cert_der_len);

/* Initialise `t` with a single trust anchor extracted from a
 * DER X.509 CA certificate.  The handshake will:
 *
 *   - walk the server's chain,
 *   - require it to terminate at a cert whose subject DN +
 *     public key match the anchor bytes (after signature
 *     verification of the chain links),
 *   - check the SAN/CN against the SNI host name,
 *   - check notBefore/notAfter against the wall-clock from
 *     SYS_GETTIMEOFDAY.
 *
 * Convenience wrapper around tls_socket_init_chain_multi with
 * n == 1.  Returns 0 on success; -1 if the CA cert failed to
 * decode, has an unsupported key type, or has a DN larger than
 * the inline buffer (TLS_ANCHOR_DN_MAX bytes). */
int tls_socket_init_chain_from_anchor(tls_socket_t *t,
                                      const unsigned char *anchor_der,
                                      size_t anchor_der_len);

/* Initialise `t` with `n` trust anchors extracted from
 * `n` DER X.509 CA certificates.  Each anchor must individually
 * fit in TLS_ANCHOR_DN_MAX / TLS_ANCHOR_RSA_N_MAX /
 * TLS_ANCHOR_EC_Q_MAX; n must be <= TLS_MAX_ANCHORS.
 *
 * Supports both RSA and ECDSA anchors (mixed freely in the
 * same list).  The handshake against any one server picks the
 * anchor that signed (transitively) the leaf cert.
 *
 * Returns 0 on success; -1 on any per-anchor decode failure or
 * limit overrun.  On failure `t` is left zeroed-out so a
 * subsequent connect() refuses. */
int tls_socket_init_chain_multi(tls_socket_t *t,
                                const unsigned char *const *anchor_ders,
                                const size_t        *anchor_der_lens,
                                int                  n);

/* Initialise `t` from a framed CA bundle blob (TLS_BUNDLE_MAGIC
 * layout, see the comment above).  Equivalent to parsing the
 * bundle and calling tls_socket_init_chain_multi with the
 * resulting list -- this helper just saves the caller from
 * writing the same loop.  Returns 0 on success; -1 on a
 * malformed bundle, a per-anchor decode failure, or more than
 * TLS_MAX_ANCHORS entries. */
int tls_socket_init_chain_from_bundle(tls_socket_t *t,
                                      const unsigned char *bundle,
                                      size_t bundle_len);

/* Open a TCP connection to ip4_be:port, inject 64 bytes of
 * entropy from SYS_GETRANDOM, run the TLS handshake.  `sni` is
 * sent in the ClientHello and (for non-knownkey validators)
 * checked against the server cert's SAN/CN; pass "" or NULL to
 * omit SNI.  Returns:
 *
 *   0       handshake completed; ready for send/recv
 *   <0      transport-layer error (negative errno from socket_*)
 *   >0      BearSSL BR_ERR_* code (handshake failed)
 */
int tls_socket_connect(tls_socket_t *t,
                       uint32_t ip4_be, uint16_t port,
                       const char *sni);

/* Send / receive application data after a successful connect.
 * Both return the number of bytes processed (>=1, <20000) or -1
 * on transport or TLS error. */
int tls_socket_send(tls_socket_t *t, const void *buf, size_t len);
int tls_socket_recv(tls_socket_t *t,       void *buf, size_t len);

/* Flush any buffered application data through the SSL engine
 * and out to the wire.  Required after the last send() in a
 * request/response cycle if the request body is small enough
 * to live entirely in the engine's output buffer. */
int tls_socket_flush(tls_socket_t *t);

/* Send TLS close_notify, wait for peer close_notify, close fd.
 * Returns 0 on clean closure, -1 if either side faulted. */
int tls_socket_close(tls_socket_t *t);

#ifdef __cplusplus
}
#endif

#endif
