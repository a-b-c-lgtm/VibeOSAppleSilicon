/* userspace/libc/tls_socket.c -- in-guest TLS client (112b + 112c).
 *
 * Wraps BearSSL's client SSL engine around our chapter-104 TCP
 * sockets and our chapter-112 SYS_GETRANDOM-backed CSPRNG.  The
 * three pieces interact through three callback surfaces:
 *
 *   1. Entropy injection.  Vendor BearSSL was compiled freestanding
 *      (BR_USE_URANDOM=0, BR_USE_GETENTROPY=0, BR_USE_WIN32_RAND=0),
 *      so the engine has NO automatic OS-entropy source.  Without
 *      a manual br_ssl_engine_inject_entropy() before reset, the
 *      engine refuses to generate the ClientHello.Random and
 *      br_ssl_client_reset() returns 0.
 *
 *   2. Transport.  br_sslio_* takes two callbacks
 *      (low_read / low_write); we pass thin wrappers around the
 *      read() / write() syscalls.  EOF maps to -1 (BearSSL treats
 *      early EOF as a hard error -- the close_notify is the only
 *      legitimate end-of-stream).
 *
 *   3. X.509.  Two shapes:
 *
 *        knownkey (112b) -- init_full with an empty anchor list,
 *          then override the X.509 vtable with a br_x509_knownkey
 *          context pinned to the leaf cert's public key.
 *
 *        chain (112c)    -- init_full with a one-entry anchor list
 *          built at runtime from a DER CA cert (DN bytes captured
 *          via append_dn callback, RSA pubkey via decoder_get_pkey),
 *          plus br_x509_minimal_set_time from SYS_GETTIMEOFDAY.
 *          The minimal validator stays in place; no override.
 *
 * Both shapes use the same tls_socket_t footprint -- whichever
 * validator isn't active is just dead state in the struct.  The
 * mode is selected by tls_socket_init_*(); tls_socket_connect()
 * branches on t->chain_mode.
 */

#include "tls_socket.h"

#include "syscall.h"
#include "printf.h"

#include "bearssl.h"

#include <stddef.h>
#include <stdint.h>

/* Local copies to avoid pulling in <string.h>: the BearSSL header
 * shim declares mem* as extern symbols (cstring.c defines them),
 * but we want this TU to compile cleanly even if it's used
 * standalone before cstring.o gets linked. */
static void tls_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static void tls_memzero(void *dst, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = 0;
}

/* --------------------------------------------------------------
 * br_sslio_* transport adapters.
 *
 * Map BearSSL's "return 1..len bytes, -1 on error" contract onto
 * our read()/write() syscalls (which return ssize_t: -errno on
 * failure, 0 on EOF, positive on success).  EOF on a TLS record
 * boundary is illegal -- the peer must send close_notify first
 * -- so we surface read()==0 as -1.
 * -------------------------------------------------------------- */

static int tls_low_read(void *ctx, unsigned char *data, size_t len)
{
    int fd = *(int *)ctx;
    /* Cap at 16 KiB so the return value always fits an int and
     * doesn't exceed BearSSL's "no more than 20000" contract. */
    if (len > 16384) len = 16384;
    long n = read(fd, data, len);
    if (n <= 0) return -1;       /* error or peer FIN */
    return (int)n;
}

static int tls_low_write(void *ctx, const unsigned char *data, size_t len)
{
    int fd = *(int *)ctx;
    if (len > 16384) len = 16384;
    long n = write(fd, data, len);
    if (n <= 0) return -1;
    return (int)n;
}

/* --------------------------------------------------------------
 * Entropy: pull 64 bytes from the kernel CSPRNG and inject.
 * 64 bytes = 512 bits, comfortably above BearSSL's documented
 * 80-bit minimum.  getrandom() never partials on success.
 * -------------------------------------------------------------- */

static int tls_inject_entropy(br_ssl_engine_context *eng)
{
    unsigned char seed[64];
    long got = getrandom(seed, sizeof seed, 0u);
    if (got != (long)sizeof seed) {
        return -1;
    }
    br_ssl_engine_inject_entropy(eng, seed, sizeof seed);
    tls_memzero(seed, sizeof seed);
    return 0;
}

/* --------------------------------------------------------------
 * Cert decode: walk the leaf cert via br_x509_decoder_* and copy
 * its RSA public key into the caller's tls_socket_t.
 *
 * BearSSL's decoder returns a pointer to the public key bytes
 * INSIDE the decoder context, which becomes invalid the moment
 * the context is recycled.  We therefore copy the (n, e) bytes
 * into the long-lived tls_socket_t buffers.
 * -------------------------------------------------------------- */

static int tls_pin_pubkey_from_cert(tls_socket_t *t,
                                    const unsigned char *cert, size_t cert_len)
{
    br_x509_decoder_context dec;
    br_x509_decoder_init(&dec, 0, 0);
    br_x509_decoder_push(&dec, cert, cert_len);
    if (br_x509_decoder_last_error(&dec) != 0) {
        return -1;
    }
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dec);
    if (pk == NULL || pk->key_type != BR_KEYTYPE_RSA) {
        return -1;
    }
    size_t nlen = pk->key.rsa.nlen;
    size_t elen = pk->key.rsa.elen;
    if (nlen == 0 || nlen > sizeof t->pinned_pk_n) return -1;
    if (elen == 0 || elen > sizeof t->pinned_pk_e) return -1;
    tls_memcpy(t->pinned_pk_n, pk->key.rsa.n, nlen);
    tls_memcpy(t->pinned_pk_e, pk->key.rsa.e, elen);
    t->pinned_pk.n    = t->pinned_pk_n;
    t->pinned_pk.nlen = nlen;
    t->pinned_pk.e    = t->pinned_pk_e;
    t->pinned_pk.elen = elen;
    return 0;
}

/* --------------------------------------------------------------
 * Chain mode (chapter 112c + multi-anchor 112e): build trust
 * anchor(s) from DER CA cert(s).
 *
 * br_x509_decoder_init takes an `append_dn` callback that fires
 * once or more during parse with successive slices of the
 * Subject DN's raw DER bytes (the SEQUENCE plus the inner
 * RelativeDistinguishedName SET-of structures, EXACTLY as they
 * appeared in the cert).  We accumulate into the long-lived
 * t->anchor_dn[idx] buffer so the trust anchor can reference
 * them after the decoder is destroyed.
 *
 * The mirror pattern (pubkey bytes copied out of the decoder
 * into long-lived storage) is identical to the knownkey path
 * above -- BearSSL's decoder returns pointers into its OWN
 * scratch and gives no ownership.
 *
 * Chapter 112e adds ECDSA support: the decoder reports
 * BR_KEYTYPE_EC and exposes pk->key.ec.{curve, q, qlen}.  We
 * copy q into anchor_pk_q[idx] and stash curve in the trust
 * anchor's pkey.key.ec.
 * -------------------------------------------------------------- */

typedef struct {
    unsigned char *buf;
    size_t         cap;
    size_t         len;
    int            overflow;
} dn_accum_t;

static void tls_dn_append(void *ctx, const void *data, size_t len)
{
    dn_accum_t *a = (dn_accum_t *)ctx;
    if (a->overflow) return;
    if (a->len + len > a->cap) {
        a->overflow = 1;
        return;
    }
    const unsigned char *src = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) a->buf[a->len + i] = src[i];
    a->len += len;
}

/* Build anchor[idx] from a DER CA cert.  Returns 0 on success,
 * -1 on any decode / size failure.  Handles both RSA and ECDSA
 * CAs; falls through on any other key type. */
static int tls_bake_anchor_into(tls_socket_t *t, int idx,
                                const unsigned char *cert, size_t cert_len)
{
    dn_accum_t acc;
    acc.buf      = t->anchor_dn[idx];
    acc.cap      = sizeof t->anchor_dn[idx];
    acc.len      = 0;
    acc.overflow = 0;

    br_x509_decoder_context dec;
    br_x509_decoder_init(&dec, tls_dn_append, &acc);
    br_x509_decoder_push(&dec, cert, cert_len);
    if (br_x509_decoder_last_error(&dec) != 0) return -1;
    if (acc.overflow || acc.len == 0) return -1;

    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dec);
    if (pk == NULL) return -1;

    t->anchors[idx].dn.data         = t->anchor_dn[idx];
    t->anchors[idx].dn.len          = acc.len;
    /* BR_X509_TA_CA: the minimal validator will use this anchor
     * to verify chain signatures (not just direct-trust the EE).
     * Required when trusting an intermediate/root CA cert. */
    t->anchors[idx].flags           = BR_X509_TA_CA;
    t->anchors[idx].pkey.key_type   = pk->key_type;

    if (pk->key_type == BR_KEYTYPE_RSA) {
        size_t nlen = pk->key.rsa.nlen;
        size_t elen = pk->key.rsa.elen;
        if (nlen == 0 || nlen > sizeof t->anchor_pk_n[idx]) return -1;
        if (elen == 0 || elen > sizeof t->anchor_pk_e[idx]) return -1;
        tls_memcpy(t->anchor_pk_n[idx], pk->key.rsa.n, nlen);
        tls_memcpy(t->anchor_pk_e[idx], pk->key.rsa.e, elen);
        t->anchors[idx].pkey.key.rsa.n    = t->anchor_pk_n[idx];
        t->anchors[idx].pkey.key.rsa.nlen = nlen;
        t->anchors[idx].pkey.key.rsa.e    = t->anchor_pk_e[idx];
        t->anchors[idx].pkey.key.rsa.elen = elen;
        return 0;
    }
    if (pk->key_type == BR_KEYTYPE_EC) {
        size_t qlen = pk->key.ec.qlen;
        if (qlen == 0 || qlen > sizeof t->anchor_pk_q[idx]) return -1;
        tls_memcpy(t->anchor_pk_q[idx], pk->key.ec.q, qlen);
        t->anchors[idx].pkey.key.ec.curve = pk->key.ec.curve;
        t->anchors[idx].pkey.key.ec.q     = t->anchor_pk_q[idx];
        t->anchors[idx].pkey.key.ec.qlen  = qlen;
        return 0;
    }
    return -1;                          /* unsupported key type */
}

/* -------------------------------------------------------------- */

int tls_socket_init_knownkey_from_cert(tls_socket_t *t,
                                       const unsigned char *cert_der,
                                       size_t cert_der_len)
{
    if (!t || !cert_der || cert_der_len == 0) return -1;

    /* Zero everything: the BearSSL contexts have lots of pointer
     * fields that are read during init_full and would crash if
     * left as stack garbage. */
    tls_memzero(t, sizeof *t);
    t->fd = -1;
    t->chain_mode = 0;

    if (tls_pin_pubkey_from_cert(t, cert_der, cert_der_len) != 0) {
        return -1;
    }
    /* The knownkey validator only ever inspects the leaf cert; it
     * accepts any cert that bears our pinned public key, with both
     * keyx (RSA key exchange) and sign (RSA-PSS / RSA-SHA*) usages
     * allowed.  Real chain walking comes in 112c. */
    br_x509_knownkey_init_rsa(&t->xkc, &t->pinned_pk,
                              BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN);
    return 0;
}

int tls_socket_init_chain_multi(tls_socket_t *t,
                                const unsigned char *const *anchor_ders,
                                const size_t        *anchor_der_lens,
                                int                  n)
{
    if (!t || !anchor_ders || !anchor_der_lens) return -1;
    if (n <= 0 || n > TLS_MAX_ANCHORS) return -1;

    tls_memzero(t, sizeof *t);
    t->fd = -1;
    t->chain_mode = 1;

    for (int i = 0; i < n; i++) {
        if (!anchor_ders[i] || anchor_der_lens[i] == 0) {
            tls_memzero(t, sizeof *t);
            return -1;
        }
        if (tls_bake_anchor_into(t, i,
                                 anchor_ders[i], anchor_der_lens[i]) != 0) {
            tls_memzero(t, sizeof *t);
            return -1;
        }
    }
    t->anchor_count = n;
    return 0;
}

int tls_socket_init_chain_from_anchor(tls_socket_t *t,
                                      const unsigned char *anchor_der,
                                      size_t anchor_der_len)
{
    const unsigned char *ders[1] = { anchor_der };
    size_t               lens[1] = { anchor_der_len };
    return tls_socket_init_chain_multi(t, ders, lens, 1);
}

/* Read a little-endian u32 from `p`.  Used by the on-disk CA
 * bundle parser.  Off-by-one safety is the caller's job. */
static uint32_t tls_rd_u32_le(const unsigned char *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int tls_socket_init_chain_from_bundle(tls_socket_t *t,
                                      const unsigned char *bundle,
                                      size_t bundle_len)
{
    if (!t || !bundle) return -1;
    /* 4 bytes magic + 4 bytes count = 8 byte minimum header. */
    if (bundle_len < 8) return -1;
    if (bundle[0] != 'C' || bundle[1] != 'A'
     || bundle[2] != 'B' || bundle[3] != '1') return -1;

    uint32_t count = tls_rd_u32_le(bundle + 4);
    if (count == 0 || count > TLS_MAX_ANCHORS) return -1;

    /* Two parallel arrays for tls_socket_init_chain_multi: a
     * vector of byte-pointers and a vector of lengths.  We
     * borrow from the bundle directly -- the underlying bytes
     * have to outlive ONLY the call to _multi, which copies
     * everything it needs into t->anchor_*. */
    const unsigned char *ders[TLS_MAX_ANCHORS];
    size_t               lens[TLS_MAX_ANCHORS];

    size_t off = 8;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 4 > bundle_len) return -1;
        uint32_t der_len = tls_rd_u32_le(bundle + off);
        off += 4;
        if (der_len == 0) return -1;
        if (off + der_len > bundle_len) return -1;
        ders[i] = bundle + off;
        lens[i] = der_len;
        off += der_len;
    }
    /* Trailing bytes after the last anchor are allowed (the
     * bundle generator might one day pad for alignment), but
     * count must match the data we just parsed. */

    return tls_socket_init_chain_multi(t, ders, lens, (int)count);
}

/* --------------------------------------------------------------
 * Wall-clock -> BearSSL (days since 0 AD, seconds within day).
 *
 * BearSSL's minimal validator expresses its "now" as a proleptic
 * Gregorian (days, secs) pair from 0 AD Jan 1.  The Unix epoch
 * (1970-01-01 00:00 UTC) sits at day 719528 in that calendar --
 * see vendor/bearssl/src/x509/x509_minimal.c line 1406 where the
 * engine adds exactly that constant when consuming a Unix time.
 * SYS_GETTIMEOFDAY (chapter 95) returns POSIX wall-clock seconds
 * sourced from the PL031 RTC at boot, so the conversion is
 * literally:
 *
 *     days = unix_secs / 86400 + 719528
 *     secs = unix_secs % 86400
 *
 * If SYS_GETTIMEOFDAY hands back tv_sec == 0 (no PL031 found or
 * the host has wall clock = epoch -- which never happens on a
 * QEMU virt under HVF), we leave the validator's time at zero,
 * which causes notBefore/notAfter to fail with BR_ERR_X509_EXPIRED.
 * That's the conservative default.
 * -------------------------------------------------------------- */

#define TLS_UNIX_EPOCH_DAYS 719528u

static void tls_set_validator_time(br_x509_minimal_context *xc)
{
    struct timeval tv;
    if (gettimeofday(&tv) != 0) return;
    if (tv.tv_sec <= 0) return;
    uint64_t s = (uint64_t)tv.tv_sec;
    uint32_t days = (uint32_t)(s / 86400u) + TLS_UNIX_EPOCH_DAYS;
    uint32_t secs = (uint32_t)(s % 86400u);
    br_x509_minimal_set_time(xc, days, secs);
}

/* -------------------------------------------------------------- */

int tls_socket_connect(tls_socket_t *t,
                       uint32_t ip4_be, uint16_t port,
                       const char *sni)
{
    if (!t) return -1;
    if (t->chain_mode) {
        /* Chain mode: the minimal validator IS the active vtable
         * (no override below).  Pass our anchor list in as the
         * trust set; init_full wires it through to t->xc and
         * leaves the validator pointing at &t->xc.vtable.  At
         * handshake time the minimal validator walks the server's
         * chain and accepts whichever anchor (RSA or EC) signed
         * the last certificate. */
        if (t->anchor_count <= 0) return -1;
        if (t->anchors[0].dn.len == 0) return -1;
        br_ssl_client_init_full(&t->cc, &t->xc,
                                t->anchors, (size_t)t->anchor_count);
        tls_set_validator_time(&t->xc);
    } else {
        /* knownkey mode (112b): init_full unconditionally calls
         * br_x509_minimal_init on its `xc` arg, so we pass a real
         * pointer.  We immediately override the validator vtable
         * with our knownkey one, after which t->xc is dead state. */
        if (t->pinned_pk.nlen == 0) return -1;
        br_ssl_client_init_full(&t->cc, &t->xc, NULL, 0);
        br_ssl_engine_set_x509(&t->cc.eng, &t->xkc.vtable);
    }

    /* Bidi I/O buffer: SSL records can flow in either direction
     * at any time during the handshake, so half-duplex would
     * deadlock when the server's Certificate message is larger
     * than the input slice we drain in a single recvrec. */
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof t->iobuf, 1);

    if (tls_inject_entropy(&t->cc.eng) != 0) {
        return -1;
    }
    if (!br_ssl_client_reset(&t->cc, sni, 0)) {
        return (int)br_ssl_engine_last_error(&t->cc.eng);
    }

    int fd = socket_connect(ip4_be, port);
    if (fd < 0) {
        return fd;
    }
    t->fd = fd;

    /* Wire the br_sslio_ wrapper around the engine.  Pass the
     * address of t->fd (not the fd value) -- the callbacks need a
     * stable pointer they can dereference each call so we can
     * tear the socket down without invalidating the engine. */
    br_sslio_init(&t->ioc, &t->cc.eng,
                  tls_low_read,  &t->fd,
                  tls_low_write, &t->fd);

    /* The actual handshake is driven implicitly by the first
     * sslio_read/write/flush call.  We force it here by flushing
     * an empty buffer, which causes the engine to run the state
     * machine until BR_SSL_SENDAPP|BR_SSL_RECVAPP is reached. */
    if (br_sslio_flush(&t->ioc) != 0) {
        int err = (int)br_ssl_engine_last_error(&t->cc.eng);
        close(t->fd);
        t->fd = -1;
        return err;
    }
    return 0;
}

/* -------------------------------------------------------------- */

int tls_socket_send(tls_socket_t *t, const void *buf, size_t len)
{
    if (!t || t->fd < 0) return -1;
    return br_sslio_write(&t->ioc, buf, len);
}

int tls_socket_recv(tls_socket_t *t, void *buf, size_t len)
{
    if (!t || t->fd < 0) return -1;
    return br_sslio_read(&t->ioc, buf, len);
}

int tls_socket_flush(tls_socket_t *t)
{
    if (!t || t->fd < 0) return -1;
    return br_sslio_flush(&t->ioc);
}

int tls_socket_close(tls_socket_t *t)
{
    if (!t) return -1;
    int rc = 0;
    if (t->fd >= 0) {
        /* br_sslio_close runs the close_notify protocol.  If the
         * peer has already gone away the call returns -1; that's
         * a soft failure (we still close the underlying fd). */
        if (br_sslio_close(&t->ioc) != 0) rc = -1;
        close(t->fd);
        t->fd = -1;
    }
    return rc;
}
