/*
 * kernel/core/dns.c — DNS resolver (RFC 1035, A records).
 *
 * Wire format (queries and replies share the same 12-byte header):
 *
 *    0                   1                   2                   3
 *    0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5  6  7  8  9 ...
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |              ID (16)             |QR| OP |AA|TC|RD|RA| Z |RCODE|
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |             QDCOUNT              |             ANCOUNT          |
 *   +-----------------------------------+-------------------------------+
 *   |             NSCOUNT              |             ARCOUNT          |
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *
 * After the header come QDCOUNT questions (each: NAME, QTYPE u16, QCLASS u16),
 * then ANCOUNT answer RRs (each: NAME, TYPE u16, CLASS u16, TTL u32,
 * RDLENGTH u16, RDATA[RDLENGTH]).
 *
 * NAME is a sequence of length-prefixed labels terminated by a zero
 * byte, or a 16-bit pointer (top two bits = 11) into the message
 * for name compression.  We only emit uncompressed names in our
 * queries; we have to handle compressed names in answers because
 * basically every recursive resolver uses them.
 *
 * What we do:
 *   - Build a single query for QTYPE=A QCLASS=IN.
 *   - Send via udp_send to (g_dns, 53) with our ephemeral
 *     source port.
 *   - Bind that source port to a callback that copies the
 *     response into a static scratch buffer and sets a flag.
 *   - Spin-yield (with net_poll) until the flag is set or a
 *     coarse timeout elapses, then parse.
 *
 * What we don't do:
 *   - Caching.  Every call hits the wire.  Fine for the
 *     handful of lookups a browser session makes.
 *   - AAAA.  We're IPv4-only.
 *   - Truncation handling.  If TC=1 we just fail; the answer
 *     for an A record is always small enough to fit in 512.
 *   - Retry.  Caller decides.
 */

#include "dns.h"
#include "net.h"
#include "udp.h"
#include "serial.h"
#include "thread.h"

#include <stdint.h>
#include <stddef.h>

/* ── tiny standalone mem* (avoid GCC implicit memset trap) ── */
static void *d_memset(void *dst, int c, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}
static void *d_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((used)) static void dns_static_init(void)
{
    /* Reference d_memset/d_memcpy so -ffunction-sections + LTO
     * never strip the bodies that GCC may emit calls to from
     * the implicit-init pathway. */
    static uint8_t scratch[1];
    d_memset(scratch, 0, 0);
    d_memcpy(scratch, scratch, 0);
}

/* ── header bits ── */
#define DNS_QR_QUERY     0x0000u
#define DNS_QR_RESPONSE  0x8000u
#define DNS_OPCODE_QUERY 0x0000u  /* shifted into bits 11-14 already */
#define DNS_FLAG_RD      0x0100u  /* recursion desired */
#define DNS_FLAG_TC      0x0200u  /* truncated */
#define DNS_RCODE_MASK   0x000Fu

#define DNS_TYPE_A     1u
#define DNS_TYPE_CNAME 5u
#define DNS_CLASS_IN   1u

#define DNS_PORT       53

/* Maximum total query/response we accept. */
#define DNS_MSG_MAX    512

/* Outstanding-query state.  One slot.  Guarded by the udp_bind
 * port + xid: callbacks for stale ports / xids are dropped. */
static volatile int       g_pending;        /* 1 while waiting for reply */
static volatile uint16_t  g_xid;            /* expected transaction id */
static volatile uint16_t  g_my_port;        /* source port we bound */
static volatile uint32_t  g_resp_len;       /* bytes copied into g_resp */
static          uint8_t   g_resp[DNS_MSG_MAX];

static uint16_t dns_be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void put_be16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p)
{ return (uint16_t)((p[0] << 8) | p[1]); }

/* Encode a DNS name into the buffer.  "example.com" becomes
 * "\x07example\x03com\x00".  Returns the number of bytes
 * written, or -1 on overflow / bad input.  No support for
 * trailing dots, escapes, or international names. */
static int encode_qname(uint8_t *buf, uint32_t cap, const char *name)
{
    if (!name || !*name) return -1;
    uint32_t off = 0;
    const char *p = name;
    while (*p) {
        const char *label = p;
        while (*p && *p != '.') p++;
        size_t llen = (size_t)(p - label);
        if (llen == 0 || llen > 63) return -1;
        if (off + 1 + llen > cap) return -1;
        buf[off++] = (uint8_t)llen;
        for (size_t i = 0; i < llen; i++) buf[off++] = (uint8_t)label[i];
        if (*p == '.') p++;
    }
    if (off + 1 > cap) return -1;
    buf[off++] = 0;
    return (int)off;
}

/* Skip a (possibly compressed) DNS name in a message.  Returns
 * the offset just past the name on success, -1 on error.  Does
 * not write the name out — we don't need it for our purposes. */
static int skip_name(const uint8_t *msg, uint32_t mlen, uint32_t off)
{
    int hops = 0;
    while (off < mlen) {
        uint8_t b = msg[off];
        if ((b & 0xC0) == 0xC0) {
            /* Compression pointer is 2 bytes; we never follow it,
             * just jump past it. */
            return (int)(off + 2);
        }
        if (b == 0) return (int)(off + 1);
        /* Plain label: length byte + that many name bytes. */
        if (off + 1 + b > mlen) return -1;
        off += 1 + b;
        if (++hops > 128) return -1;
    }
    return -1;
}

/* udp_bind callback for our ephemeral source port. */
static void dns_rx(const uint8_t *src_ip, uint16_t src_port,
                   const uint8_t *payload, uint32_t plen)
{
    (void)src_ip; (void)src_port;
    if (!g_pending) return;
    if (plen < 12 || plen > DNS_MSG_MAX) return;
    /* Match the transaction id. */
    uint16_t xid = get_be16(payload);
    if (xid != g_xid) return;
    /* Copy the raw payload — we'll parse on the resolver thread. */
    d_memcpy(g_resp, payload, plen);
    g_resp_len = plen;
    g_pending  = 0;
}

static uint16_t pick_xid(void)
{
    /* Cheap pseudo-random based on the cycle counter.  Good
     * enough — collisions only matter across our own outstanding
     * queries (we have at most one) and against stale packets
     * (the udp_bind port narrows the window already). */
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return (uint16_t)(v ^ (v >> 16));
}

static uint16_t pick_port(void)
{
    /* Ephemeral range 49152..65535.  Same trick as ISN selection. */
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return (uint16_t)(49152u + (v % 16384u));
}

/* Locate the answer section and find the first A record.  Returns
 * 0 on success and writes 4 bytes to out_ip; -1 otherwise. */
static int parse_answer(const uint8_t *msg, uint32_t mlen, uint8_t out_ip[4])
{
    if (mlen < 12) return -1;
    uint16_t flags    = get_be16(msg + 2);
    uint16_t qdcount  = get_be16(msg + 4);
    uint16_t ancount  = get_be16(msg + 6);

    if (!(flags & DNS_QR_RESPONSE))   return -1;
    if (flags & DNS_FLAG_TC)          return -1;
    if (flags & DNS_RCODE_MASK)       return -1;
    if (ancount == 0)                 return -1;

    uint32_t off = 12;
    /* Skip questions: each is name + qtype(2) + qclass(2). */
    for (uint16_t i = 0; i < qdcount; i++) {
        int n = skip_name(msg, mlen, off);
        if (n < 0) return -1;
        off = (uint32_t)n;
        if (off + 4 > mlen) return -1;
        off += 4;
    }

    /* Walk answers looking for QTYPE=A. */
    for (uint16_t i = 0; i < ancount; i++) {
        int n = skip_name(msg, mlen, off);
        if (n < 0) return -1;
        off = (uint32_t)n;
        if (off + 10 > mlen) return -1;
        uint16_t type    = get_be16(msg + off);
        uint16_t cls     = get_be16(msg + off + 2);
        /* TTL is 4 bytes at off+4; we ignore it. */
        uint16_t rdlen   = get_be16(msg + off + 8);
        off += 10;
        if (off + rdlen > mlen) return -1;
        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            out_ip[0] = msg[off + 0];
            out_ip[1] = msg[off + 1];
            out_ip[2] = msg[off + 2];
            out_ip[3] = msg[off + 3];
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

/* External entry point. */
int dns_resolve(const char *name, uint8_t out_ip[4])
{
    if (!name || !out_ip) return -1;

    uint8_t dns[4];
    if (!net_get_dns(dns)) {
        serial_puts("[dns] no DNS server configured\n");
        return -1;
    }

    /* Build the query in a stack buffer. */
    uint8_t  msg[DNS_MSG_MAX];
    uint16_t xid = pick_xid();
    if (xid == 0) xid = 1;

    put_be16(msg + 0, xid);
    put_be16(msg + 2, DNS_QR_QUERY | DNS_FLAG_RD);   /* RD=1, RA via the server */
    put_be16(msg + 4, 1);  /* QDCOUNT */
    put_be16(msg + 6, 0);  /* ANCOUNT */
    put_be16(msg + 8, 0);  /* NSCOUNT */
    put_be16(msg +10, 0);  /* ARCOUNT */
    int qn = encode_qname(msg + 12, DNS_MSG_MAX - 12 - 4, name);
    if (qn < 0) {
        serial_puts("[dns] bad name\n");
        return -1;
    }
    uint32_t off = 12 + (uint32_t)qn;
    put_be16(msg + off,     DNS_TYPE_A);
    put_be16(msg + off + 2, DNS_CLASS_IN);
    off += 4;

    /* Bind a source port and arm the pending slot. */
    uint16_t src_port = pick_port();
    if (udp_bind(src_port, dns_rx) < 0) {
        serial_puts("[dns] udp_bind failed\n");
        return -1;
    }
    g_xid     = xid;
    g_my_port = src_port;
    g_resp_len = 0;
    g_pending = 1;
    (void)dns_be16;  /* reserved if we ever need an inline swap helper */

    if (udp_send((const uint8_t *)0, dns, src_port, DNS_PORT,
                 msg, off) < 0) {
        serial_puts("[dns] udp_send failed\n");
        g_pending = 0;
        udp_bind(src_port, (udp_rx_cb)0);
        return -1;
    }

    /* Spin-yield until the callback fires or we time out.  Use
     * cntvct_el0 for a high-resolution wall clock independent of
     * the 100 ms scheduler tick.  cntfrq_el0 gives Hz; on the
     * QEMU virt machine that's 24 MHz under HVF / 62.5 MHz on
     * actual silicon — either way 3 seconds is plenty of
     * cycles to count without overflow. */
    uint64_t freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    uint64_t budget = freq * 3;     /* 3 seconds */
    while (g_pending) {
        (void)net_poll();
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
        if (now - start > budget) break;
        yield();
    }

    udp_bind(src_port, (udp_rx_cb)0);

    if (g_pending) {
        g_pending = 0;
        serial_puts("[dns] timeout\n");
        return -1;
    }

    int rc = parse_answer(g_resp, g_resp_len, out_ip);
    if (rc < 0) {
        serial_puts("[dns] no A record in response\n");
        return -1;
    }
    return 0;
}
