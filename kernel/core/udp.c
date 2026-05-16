/*
 * kernel/core/udp.c \u2014 milestone-54 UDP/IPv4.
 *
 * Tiny, stateless, polling-driven.  See udp.h for the design
 * notes.  The interesting part is the checksum, which is
 * computed over a pseudo-header that includes IP src, IP dst,
 * the IP `proto` byte, and the UDP length.
 */

#include "udp.h"
#include "net.h"
#include "../device/virtio_net.h"

/* ---- micro mem* ---- */
static void *u_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t       *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}
static void u_memset(void *d, int v, uint32_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)v;
}

/* ----------------------------------------------------------------
 * Port binding table
 * ---------------------------------------------------------------- */

#define UDP_BIND_CAP 8

struct udp_binding {
    int       valid;
    uint16_t  port;       /* host order */
    udp_rx_cb cb;
};
static struct udp_binding g_bind[UDP_BIND_CAP];

int udp_bind(uint16_t port, udp_rx_cb cb)
{
    /* Update existing binding if any. */
    for (int i = 0; i < UDP_BIND_CAP; i++) {
        if (g_bind[i].valid && g_bind[i].port == port) {
            if (cb == (udp_rx_cb)0) g_bind[i].valid = 0;
            else                    g_bind[i].cb    = cb;
            return 0;
        }
    }
    if (cb == (udp_rx_cb)0) return 0;   /* unbinding a free port: no-op */
    for (int i = 0; i < UDP_BIND_CAP; i++) {
        if (!g_bind[i].valid) {
            g_bind[i].valid = 1;
            g_bind[i].port  = port;
            g_bind[i].cb    = cb;
            return 0;
        }
    }
    return -1;
}

static udp_rx_cb find_binding(uint16_t port)
{
    for (int i = 0; i < UDP_BIND_CAP; i++) {
        if (g_bind[i].valid && g_bind[i].port == port) return g_bind[i].cb;
    }
    return (udp_rx_cb)0;
}

/* ----------------------------------------------------------------
 * Checksum.
 *
 * UDP-over-IPv4 checksum is the 1's-complement sum of:
 *   src_ip (4) + dst_ip (4) + 0x00 + proto (1) + udp_length (2)
 *   + udp header (8) + payload (N)
 * with payload zero-padded to even length when computing.
 *
 * net_ipv4_checksum() already does the "sum 16-bit BE words +
 * fold + invert" math.  We assemble the pseudo-header into a
 * stack buffer first so we can hand it the whole blob in one
 * call.
 * ---------------------------------------------------------------- */

static uint16_t udp_compute_checksum(const uint8_t src_ip[NET_IPV4_LEN],
                                     const uint8_t dst_ip[NET_IPV4_LEN],
                                     const uint8_t *udp_pkt,
                                     uint32_t udp_len)
{
    /* 12-byte pseudo-header. */
    uint8_t buf[12 + 1500];
    if (udp_len > sizeof(buf) - 12) return 0;
    u_memcpy(buf + 0, src_ip, NET_IPV4_LEN);
    u_memcpy(buf + 4, dst_ip, NET_IPV4_LEN);
    buf[8]  = 0;
    buf[9]  = IPV4_PROTO_UDP;
    buf[10] = (uint8_t)(udp_len >> 8);
    buf[11] = (uint8_t)(udp_len & 0xFF);
    u_memcpy(buf + 12, udp_pkt, udp_len);
    return net_ipv4_checksum(buf, 12 + udp_len);
}

/* ----------------------------------------------------------------
 * TX
 * ---------------------------------------------------------------- */

int udp_send(const uint8_t *src_ip,
             const uint8_t  dst_ip[NET_IPV4_LEN],
             uint16_t src_port, uint16_t dst_port,
             const void *payload, uint32_t payload_len)
{
    if (UDP_HDR_LEN + payload_len > 1500 - IPV4_HDR_LEN) return -1;

    uint8_t our_ip[NET_IPV4_LEN];
    if (src_ip == (const uint8_t *)0) {
        net_get_config((uint8_t *)0, our_ip,
                       (uint8_t *)0, (uint8_t *)0);
        src_ip = our_ip;
    }

    /* Assemble the UDP packet (header + payload) in a stack
     * buffer.  Then compute the checksum (which itself needs
     * the pseudo-header) and patch it back in. */
    uint8_t pkt[UDP_HDR_LEN + 1500];
    struct udp_hdr *h = (struct udp_hdr *)pkt;
    uint32_t udp_len = UDP_HDR_LEN + payload_len;
    h->src_port = net_cpu_to_be16(src_port);
    h->dst_port = net_cpu_to_be16(dst_port);
    h->length   = net_cpu_to_be16((uint16_t)udp_len);
    h->checksum = 0;
    if (payload_len) u_memcpy(pkt + UDP_HDR_LEN, payload, payload_len);

    uint16_t cks = udp_compute_checksum(src_ip, dst_ip, pkt, udp_len);
    /* RFC 768: a transmitted all-zeros checksum means "no
     * checksum"; if the real checksum happens to compute to
     * zero we transmit 0xFFFF instead, which has the same
     * mathematical value under 1's complement. */
    if (cks == 0) cks = 0xFFFFu;
    h->checksum = cks;

    return net_ipv4_send_from(src_ip, dst_ip, IPV4_PROTO_UDP,
                              pkt, udp_len);
}

/* ----------------------------------------------------------------
 * RX dispatch
 * ---------------------------------------------------------------- */

void udp_handle(const struct ipv4_hdr *ip,
                const uint8_t *payload, uint32_t plen)
{
    if (plen < UDP_HDR_LEN) return;
    const struct udp_hdr *h = (const struct udp_hdr *)payload;
    uint32_t udp_len = net_be16_to_cpu(h->length);
    if (udp_len < UDP_HDR_LEN || udp_len > plen) return;

    /* Optional checksum.  0 means "no checksum" \u2014 don't validate.
     * Otherwise verify against the pseudo-header.  We need a
     * mutable copy of the packet to zero the checksum field. */
    if (h->checksum != 0) {
        uint8_t buf[1500];
        if (udp_len > sizeof(buf)) return;
        u_memcpy(buf, payload, udp_len);
        struct udp_hdr *tmp = (struct udp_hdr *)buf;
        uint16_t sent = tmp->checksum;
        tmp->checksum = 0;
        uint16_t want = udp_compute_checksum(ip->src, ip->dst,
                                             buf, udp_len);
        /* As above, an "all zeros" computed checksum is sent as
         * 0xFFFF on the wire; treat both the same. */
        if (want == 0) want = 0xFFFFu;
        if (sent != want) return;
        /* fall through using `payload` (the read-only original) */
        (void)tmp;
    }

    uint16_t dst_port = net_be16_to_cpu(h->dst_port);
    udp_rx_cb cb = find_binding(dst_port);
    if (!cb) return;

    const uint8_t *body = payload + UDP_HDR_LEN;
    uint32_t       blen = udp_len   - UDP_HDR_LEN;
    cb(ip->src, net_be16_to_cpu(h->src_port), body, blen);
}

/* Force g_bind to be definitely-zeroed at boot under aggressive
 * BSS layout choices.  This also keeps the compiler from
 * "optimising" g_bind into something that needs runtime memset. */
__attribute__((used))
static void udp_static_init(void)
{
    u_memset(g_bind, 0, sizeof(g_bind));
}
