/*
 * kernel/core/net.c — milestone-53 in-kernel net stack.
 *
 * See net.h for the design rationale.  Three responsibilities,
 * one per section below: ARP cache, RX dispatch, and TX builders.
 *
 * Kept deliberately small (~400 LOC) and policy-light: there's
 * no socket layer here, no DHCP, no TCP, and no IP fragmentation.
 * Those are scheduled for milestones 54/55.
 */

#include "net.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "../device/virtio_net.h"
#include "serial.h"

/* Local memcpy / memset.  The kernel doesn't link a libc; we
 * keep a private pair here to avoid pulling in any GCC-builtin
 * call that might end up referencing an unresolved symbol when
 * `{ 0 }` initialises a struct over a certain size threshold.
 * (See user-memory note `freestanding-c-memset-trap.md`.) */
static void *n_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t       *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}
static void n_memset(void *d, int v, uint32_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)v;
}
static int n_memeq(const void *a, const void *b, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) if (*p++ != *q++) return 0;
    return 1;
}

/* ----------------------------------------------------------------
 * Configuration / global state
 * ---------------------------------------------------------------- */

static int     g_initted = 0;
static uint8_t g_mac[NET_MAC_LEN];
static uint8_t g_ip [NET_IPV4_LEN];
static uint8_t g_gw [NET_IPV4_LEN];
static uint8_t g_msk[NET_IPV4_LEN];
static uint8_t g_dns[NET_IPV4_LEN];   /* M57: DNS server (0 = unset) */

/* Forward decl: print_ipv4 is defined later but needed by
 * net_set_dns (which logs the new server). */
static void print_ipv4(const uint8_t ip[NET_IPV4_LEN]);

static const uint8_t g_bcast_mac[NET_MAC_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t g_bcast_ip [NET_IPV4_LEN] = { 255, 255, 255, 255 };

/* Monotonically-increasing IPv4 ID field. */
static uint16_t g_ip_id = 1;

/* ----------------------------------------------------------------
 * ARP cache
 *
 * Eight entries.  On insert we look for an existing entry for
 * the same IP first (refresh in place), then the first empty
 * slot, then evict slot 0 — a one-line LRU approximation that's
 * fine for the volumes we care about (gateway, DNS, plus a few
 * local peers).
 * ---------------------------------------------------------------- */

#define ARP_CACHE_CAP   8

struct arp_entry {
    int     valid;
    uint8_t ip [NET_IPV4_LEN];
    uint8_t mac[NET_MAC_LEN];
};
static struct arp_entry g_arp[ARP_CACHE_CAP];

static int arp_find(const uint8_t ip[NET_IPV4_LEN])
{
    for (int i = 0; i < ARP_CACHE_CAP; i++) {
        if (!g_arp[i].valid) continue;
        if (n_memeq(g_arp[i].ip, ip, NET_IPV4_LEN)) return i;
    }
    return -1;
}

static void arp_insert(const uint8_t ip[NET_IPV4_LEN],
                       const uint8_t mac[NET_MAC_LEN])
{
    int slot = arp_find(ip);
    if (slot < 0) {
        for (int i = 0; i < ARP_CACHE_CAP; i++) {
            if (!g_arp[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0;     /* evict slot 0 */
    g_arp[slot].valid = 1;
    n_memcpy(g_arp[slot].ip,  ip,  NET_IPV4_LEN);
    n_memcpy(g_arp[slot].mac, mac, NET_MAC_LEN);
}

int net_arp_lookup(const uint8_t ip[NET_IPV4_LEN],
                   uint8_t out_mac[NET_MAC_LEN])
{
    int i = arp_find(ip);
    if (i < 0) return 0;
    n_memcpy(out_mac, g_arp[i].mac, NET_MAC_LEN);
    return 1;
}

/* ----------------------------------------------------------------
 * Frame builders
 * ---------------------------------------------------------------- */

/* Write an Ethernet header at `frame` (must be at least
 * ETH_HDR_LEN bytes).  `dst_mac` may be `g_bcast_mac` for
 * broadcast. */
static void eth_hdr_build(uint8_t *frame,
                          const uint8_t dst_mac[NET_MAC_LEN],
                          uint16_t ethertype_host)
{
    n_memcpy(frame + 0, dst_mac, NET_MAC_LEN);
    n_memcpy(frame + 6, g_mac,   NET_MAC_LEN);
    frame[12] = (uint8_t)(ethertype_host >> 8);
    frame[13] = (uint8_t)(ethertype_host & 0xFF);
}

int net_arp_request(const uint8_t target_ip[NET_IPV4_LEN])
{
    /* Allow ARP request even when our IPv4 config is still
     * zero (g_ip == 0.0.0.0) — used during the DHCP
     * handshake's optional ARP probe.  The frame is well-
     * formed even with sender IP = 0.0.0.0. */
    if (!virtio_net_present()) return -1;

    uint8_t f[ETH_HDR_LEN + ARP_PKT_LEN];
    eth_hdr_build(f, g_bcast_mac, ETHERTYPE_ARP);

    struct arp_pkt *a = (struct arp_pkt *)(f + ETH_HDR_LEN);
    a->htype = net_cpu_to_be16(1);
    a->ptype = net_cpu_to_be16(ETHERTYPE_IPV4);
    a->hlen  = NET_MAC_LEN;
    a->plen  = NET_IPV4_LEN;
    a->op    = net_cpu_to_be16(ARP_OP_REQUEST);
    n_memcpy(a->sha, g_mac,      NET_MAC_LEN);
    n_memcpy(a->spa, g_ip,       NET_IPV4_LEN);
    n_memset(a->tha, 0,          NET_MAC_LEN);
    n_memcpy(a->tpa, target_ip,  NET_IPV4_LEN);

    return virtio_net_tx(f, sizeof(f));
}

int net_arp_resolve(const uint8_t ip[NET_IPV4_LEN],
                    uint8_t out_mac[NET_MAC_LEN],
                    uint64_t spin_iters)
{
    if (net_arp_lookup(ip, out_mac)) return 1;
    if (net_arp_request(ip) < 0)     return 0;
    /* Same approach used by the milestone-52 driver self-test:
     * we don't have a sleep primitive at boot time, so we spin.
     * Drain the RX ring periodically so any inbound ARP reply
     * gets routed through the dispatcher and into the cache. */
    for (uint64_t i = 0; i < spin_iters; i++) {
        if ((i & 0xfffu) == 0) (void)net_poll();
        if (net_arp_lookup(ip, out_mac)) return 1;
        __asm__ volatile("" ::: "memory");
    }
    (void)net_poll();
    return net_arp_lookup(ip, out_mac);
}

/* ----------------------------------------------------------------
 * IPv4 builders
 * ---------------------------------------------------------------- */

uint16_t net_ipv4_checksum(const void *data, uint32_t len)
{
    /* Standard 1's-complement sum of 16-bit big-endian words. */
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len >= 2) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p   += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    uint16_t cksum = (uint16_t)~sum;
    /* Return as big-endian byte order. */
    return (uint16_t)((cksum >> 8) | (cksum << 8));
}

int net_ipv4_build(uint8_t *out, uint32_t cap,
                   const uint8_t dst_ip[NET_IPV4_LEN],
                   uint8_t proto,
                   const void *payload, uint32_t payload_len)
{
    return net_ipv4_build_src(out, cap, g_ip, dst_ip, proto,
                              payload, payload_len);
}

int net_ipv4_build_src(uint8_t *out, uint32_t cap,
                       const uint8_t src_ip[NET_IPV4_LEN],
                       const uint8_t dst_ip[NET_IPV4_LEN],
                       uint8_t proto,
                       const void *payload, uint32_t payload_len)
{
    uint32_t total = IPV4_HDR_LEN + payload_len;
    if (total > cap || total > 0xFFFFu) return -1;

    struct ipv4_hdr *h = (struct ipv4_hdr *)out;
    h->vihl      = (4u << 4) | (IPV4_HDR_LEN / 4);
    h->tos       = 0;
    h->total_len = net_cpu_to_be16((uint16_t)total);
    h->id        = net_cpu_to_be16(g_ip_id++);
    h->frag      = 0;
    h->ttl       = IPV4_DEFAULT_TTL;
    h->proto     = proto;
    h->checksum  = 0;
    n_memcpy(h->src, src_ip,  NET_IPV4_LEN);
    n_memcpy(h->dst, dst_ip,  NET_IPV4_LEN);
    h->checksum  = net_ipv4_checksum(h, IPV4_HDR_LEN);

    if (payload_len)
        n_memcpy(out + IPV4_HDR_LEN, payload, payload_len);
    return (int)total;
}

/* True if `ip` is on our local subnet (i.e. we can deliver it
 * directly via ARP rather than going through the gateway). */
static int ip_on_local_subnet(const uint8_t ip[NET_IPV4_LEN])
{
    for (int i = 0; i < NET_IPV4_LEN; i++) {
        if ((ip[i] & g_msk[i]) != (g_ip[i] & g_msk[i])) return 0;
    }
    return 1;
}

int net_ipv4_send(const uint8_t dst_ip[NET_IPV4_LEN],
                  uint8_t proto,
                  const void *payload, uint32_t payload_len)
{
    return net_ipv4_send_from(g_ip, dst_ip, proto,
                              payload, payload_len);
}

int net_ipv4_send_from(const uint8_t src_ip[NET_IPV4_LEN],
                       const uint8_t dst_ip[NET_IPV4_LEN],
                       uint8_t proto,
                       const void *payload, uint32_t payload_len)
{
    if (!virtio_net_present()) return -1;
    /* Build the IP packet first, in a stack buffer.  ETH_HDR_LEN +
     * IPV4_HDR_LEN + max payload (1500 - 20) = 1514 = our
     * VIRTIO_NET_FRAME_MAX, so a single MTU-sized buffer fits. */
    uint8_t frame[ETH_HDR_LEN + 1500];
    if (IPV4_HDR_LEN + payload_len > 1500) return -1;

    /* Resolve next-hop MAC.
     *
     * Three cases:
     *   - Limited broadcast (255.255.255.255): use the L2
     *     broadcast MAC; no ARP, no gateway.  This is the path
     *     DHCP DISCOVER takes.
     *   - Same subnet: ARP-resolve the destination directly.
     *   - Off subnet: ARP-resolve the gateway.
     *
     * For ARP we use a generous spin budget because the very
     * first packet to a brand-new neighbour will typically miss
     * the cache. */
    uint8_t next_hop_mac[NET_MAC_LEN];
    if (n_memeq(dst_ip, g_bcast_ip, NET_IPV4_LEN)) {
        n_memcpy(next_hop_mac, g_bcast_mac, NET_MAC_LEN);
    } else {
        const uint8_t *next_hop_ip =
            ip_on_local_subnet(dst_ip) ? dst_ip : g_gw;
        if (!net_arp_resolve(next_hop_ip, next_hop_mac, 50000000ULL))
            return -1;
    }

    eth_hdr_build(frame, next_hop_mac, ETHERTYPE_IPV4);
    int ip_len = net_ipv4_build_src(frame + ETH_HDR_LEN,
                                    sizeof(frame) - ETH_HDR_LEN,
                                    src_ip, dst_ip, proto,
                                    payload, payload_len);
    if (ip_len < 0) return -1;

    return virtio_net_tx(frame, ETH_HDR_LEN + (uint32_t)ip_len);
}

/* ----------------------------------------------------------------
 * RX dispatch
 * ---------------------------------------------------------------- */

static void rx_handle_arp(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HDR_LEN + ARP_PKT_LEN) return;
    const struct arp_pkt *a =
        (const struct arp_pkt *)(frame + ETH_HDR_LEN);
    if (net_be16_to_cpu(a->htype) != 1) return;
    if (net_be16_to_cpu(a->ptype) != ETHERTYPE_IPV4) return;
    if (a->hlen != NET_MAC_LEN || a->plen != NET_IPV4_LEN) return;

    /* Always learn from the sender — even on a request, since
     * the sender just told us their (sha, spa) tuple. */
    arp_insert(a->spa, a->sha);

    /* Reply to requests targeting our IP. */
    uint16_t op = net_be16_to_cpu(a->op);
    if (op == ARP_OP_REQUEST &&
        n_memeq(a->tpa, g_ip, NET_IPV4_LEN)) {
        uint8_t f[ETH_HDR_LEN + ARP_PKT_LEN];
        eth_hdr_build(f, a->sha, ETHERTYPE_ARP);
        struct arp_pkt *r = (struct arp_pkt *)(f + ETH_HDR_LEN);
        r->htype = net_cpu_to_be16(1);
        r->ptype = net_cpu_to_be16(ETHERTYPE_IPV4);
        r->hlen  = NET_MAC_LEN;
        r->plen  = NET_IPV4_LEN;
        r->op    = net_cpu_to_be16(ARP_OP_REPLY);
        n_memcpy(r->sha, g_mac,  NET_MAC_LEN);
        n_memcpy(r->spa, g_ip,   NET_IPV4_LEN);
        n_memcpy(r->tha, a->sha, NET_MAC_LEN);
        n_memcpy(r->tpa, a->spa, NET_IPV4_LEN);
        (void)virtio_net_tx(f, sizeof(f));
    }
}

static void rx_handle_ipv4(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HDR_LEN + IPV4_HDR_LEN) return;
    const struct ipv4_hdr *h =
        (const struct ipv4_hdr *)(frame + ETH_HDR_LEN);

    /* Sanity: version == 4, IHL == 5 (no options).  Drop options
     * silently for now — rare on a SLIRP backend, and the upper
     * layers we'll write don't need them. */
    if ((h->vihl >> 4) != 4)         return;
    if ((h->vihl & 0x0Fu) != 5)      return;

    uint16_t total = net_be16_to_cpu(h->total_len);
    if (total > len - ETH_HDR_LEN)   return;
    if (total < IPV4_HDR_LEN)        return;

    /* Verify checksum.  Replace `checksum` field with 0 in a
     * local copy, recompute, and compare to the original. */
    struct ipv4_hdr tmp = *h;
    tmp.checksum = 0;
    if (net_ipv4_checksum(&tmp, IPV4_HDR_LEN) != h->checksum) return;

    /* Accept packets addressed to us OR to the limited broadcast.
     * During the DHCP handshake our IP is still 0.0.0.0; in that
     * window we ALSO accept packets addressed to 0.0.0.0 so the
     * lookup matches an incoming OFFER that some servers unicast
     * to the client's MAC with dst-IP=0.0.0.0.  We don't yet
     * support multicast. */
    int for_us = n_memeq(h->dst, g_ip, NET_IPV4_LEN) ||
                 n_memeq(h->dst, g_bcast_ip, NET_IPV4_LEN);
    if (!for_us) return;

    const uint8_t *payload = frame + ETH_HDR_LEN + IPV4_HDR_LEN;
    uint32_t       plen    = total - IPV4_HDR_LEN;
    switch (h->proto) {
    case IPV4_PROTO_ICMP: icmp_handle(h, payload, plen); break;
    case IPV4_PROTO_UDP:  udp_handle (h, payload, plen); break;
    case IPV4_PROTO_TCP:  tcp_handle (h, payload, plen); break;
    default: /* unknown protocol */                       break;
    }
}

static void rx_dispatch(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HDR_LEN) return;
    uint16_t et = ((uint16_t)frame[12] << 8) | frame[13];
    if      (et == ETHERTYPE_ARP)  rx_handle_arp(frame, len);
    else if (et == ETHERTYPE_IPV4) rx_handle_ipv4(frame, len);
    /* Silently drop everything else (IPv6, LLDP, 802.1q, ...). */
}

/* ----------------------------------------------------------------
 * Public init / poll
 * ---------------------------------------------------------------- */

int net_poll(void)
{
    int n = virtio_net_drain_rx();
    tcp_poll();
    return n;
}

void net_get_config(uint8_t out_mac[NET_MAC_LEN],
                    uint8_t out_ip[NET_IPV4_LEN],
                    uint8_t out_gw[NET_IPV4_LEN],
                    uint8_t out_mask[NET_IPV4_LEN])
{
    if (out_mac)  n_memcpy(out_mac,  g_mac, NET_MAC_LEN);
    if (out_ip)   n_memcpy(out_ip,   g_ip,  NET_IPV4_LEN);
    if (out_gw)   n_memcpy(out_gw,   g_gw,  NET_IPV4_LEN);
    if (out_mask) n_memcpy(out_mask, g_msk, NET_IPV4_LEN);
}

void net_set_dns(const uint8_t dns_ip[NET_IPV4_LEN])
{
    n_memcpy(g_dns, dns_ip, NET_IPV4_LEN);
    serial_puts("[net] dns=");
    print_ipv4(g_dns);
    serial_puts("\n");
}

int net_get_dns(uint8_t out_ip[NET_IPV4_LEN])
{
    int set = (g_dns[0] | g_dns[1] | g_dns[2] | g_dns[3]) != 0;
    if (out_ip) {
        if (set) n_memcpy(out_ip, g_dns, NET_IPV4_LEN);
        else     n_memset(out_ip, 0, NET_IPV4_LEN);
    }
    return set;
}

int net_attach(void)
{
    if (!virtio_net_present()) return -1;
    virtio_net_get_mac(g_mac);
    /* Zero the IPv4 config; caller installs it later via
     * net_set_ipv4_config (or via the convenience net_init). */
    n_memset(g_ip,  0, NET_IPV4_LEN);
    n_memset(g_gw,  0, NET_IPV4_LEN);
    n_memset(g_msk, 0, NET_IPV4_LEN);
    n_memset(g_dns, 0, NET_IPV4_LEN);
    for (int i = 0; i < ARP_CACHE_CAP; i++) g_arp[i].valid = 0;

    virtio_net_set_rx_callback(rx_dispatch);
    g_initted = 1;
    return 0;
}

static void print_ipv4(const uint8_t ip[NET_IPV4_LEN])
{
    for (int i = 0; i < NET_IPV4_LEN; i++) {
        uint8_t v = ip[i];
        char buf[4]; int n = 0;
        if (v == 0) buf[n++] = '0';
        else while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
        while (n--) serial_putc(buf[n]);
        if (i < 3) serial_putc('.');
    }
}

int net_set_ipv4_config(const uint8_t local_ip[NET_IPV4_LEN],
                        const uint8_t gateway_ip[NET_IPV4_LEN],
                        const uint8_t netmask[NET_IPV4_LEN])
{
    if (!g_initted) return -1;
    n_memcpy(g_ip,  local_ip,   NET_IPV4_LEN);
    n_memcpy(g_gw,  gateway_ip, NET_IPV4_LEN);
    n_memcpy(g_msk, netmask,    NET_IPV4_LEN);
    /* Drop any stale ARP entries learned under the old IP. */
    for (int i = 0; i < ARP_CACHE_CAP; i++) g_arp[i].valid = 0;

    serial_puts("[net] up: ip=");
    print_ipv4(g_ip);
    serial_puts(" gw=");
    print_ipv4(g_gw);
    serial_puts(" mask=");
    print_ipv4(g_msk);
    serial_puts("\n");
    return 0;
}

int net_init(const uint8_t local_ip[NET_IPV4_LEN],
             const uint8_t gateway_ip[NET_IPV4_LEN],
             const uint8_t netmask[NET_IPV4_LEN])
{
    if (net_attach() < 0) return -1;
    return net_set_ipv4_config(local_ip, gateway_ip, netmask);
}
