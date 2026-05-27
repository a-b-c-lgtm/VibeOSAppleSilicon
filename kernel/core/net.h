/*
 * kernel/core/net.h — in-kernel network stack
 * (Ethernet + ARP + IPv4).
 *
 * Sits directly on top of `virtio_net` (chapter 59).  Owns:
 *
 *   - the local IPv4 / netmask / gateway configuration (static
 *     initially; DHCP follows),
 *   - a small ARP cache (8 entries, LRU on insert),
 *   - the RX dispatcher that classifies an inbound Ethernet
 *     frame by its EtherType and routes it to the ARP or IPv4
 *     handler,
 *   - small builders for outbound Ethernet, ARP, and IPv4
 *     frames, with the IPv4 1's-complement checksum.
 *
 * What this layer does NOT include:
 *
 *   - ICMP, UDP, DHCP   — separate modules
 *   - TCP + sockets     — separate modules
 *
 * Buffer ownership: the RX path is callback-driven from inside
 * `virtio_net_drain_rx()`.  The frame pointer handed to a
 * handler is valid only for the duration of that handler.  TX
 * builds frames on the caller's stack and hands them straight
 * to `virtio_net_tx`, which copies into the device descriptor
 * buffer before returning.
 */
#ifndef KERNEL_CORE_NET_H
#define KERNEL_CORE_NET_H

#include <stdint.h>

#define NET_MAC_LEN          6
#define NET_IPV4_LEN         4

/* EtherType values, big-endian when on the wire but stored host-
 * order in C structs. */
#define ETHERTYPE_IPV4       0x0800u
#define ETHERTYPE_ARP        0x0806u

/* IPv4 protocol numbers (a few we care about). */
#define IPV4_PROTO_ICMP      1u
#define IPV4_PROTO_TCP       6u
#define IPV4_PROTO_UDP       17u

/* ----------------------------------------------------------------
 * Layout structs.  All on-the-wire integer fields are big-endian;
 * helpers below convert.  We deliberately don't typedef ntohs /
 * htons because the names are ambiguous in a freestanding kernel
 * — `net_be16_to_cpu` says exactly what it does.
 * ---------------------------------------------------------------- */

struct __attribute__((packed)) eth_hdr {
    uint8_t  dst[NET_MAC_LEN];
    uint8_t  src[NET_MAC_LEN];
    uint16_t ethertype;          /* big-endian on the wire */
};
#define ETH_HDR_LEN  14u

struct __attribute__((packed)) arp_pkt {
    uint16_t htype;              /* 1 = Ethernet                */
    uint16_t ptype;              /* 0x0800 = IPv4               */
    uint8_t  hlen;               /* 6                           */
    uint8_t  plen;               /* 4                           */
    uint16_t op;                 /* 1 = request, 2 = reply      */
    uint8_t  sha[NET_MAC_LEN];   /* sender hardware address     */
    uint8_t  spa[NET_IPV4_LEN];  /* sender protocol  address    */
    uint8_t  tha[NET_MAC_LEN];   /* target hardware address     */
    uint8_t  tpa[NET_IPV4_LEN];  /* target protocol  address    */
};
#define ARP_PKT_LEN  28u
#define ARP_OP_REQUEST   1u
#define ARP_OP_REPLY     2u

struct __attribute__((packed)) ipv4_hdr {
    uint8_t  vihl;               /* version (4) << 4 | IHL words  */
    uint8_t  tos;                /* DSCP/ECN — we send 0          */
    uint16_t total_len;          /* whole packet incl header (BE) */
    uint16_t id;                 /* identification (BE)           */
    uint16_t frag;               /* flags + fragment offset (BE)  */
    uint8_t  ttl;
    uint8_t  proto;              /* IPV4_PROTO_*                  */
    uint16_t checksum;           /* 1's-complement, BE            */
    uint8_t  src[NET_IPV4_LEN];
    uint8_t  dst[NET_IPV4_LEN];
};
#define IPV4_HDR_LEN     20u
#define IPV4_DEFAULT_TTL 64u

/* ---- byte order helpers ---- */
static inline uint16_t net_be16_to_cpu(uint16_t be)
{
    return (uint16_t)(((be & 0xFF00u) >> 8) | ((be & 0x00FFu) << 8));
}
static inline uint16_t net_cpu_to_be16(uint16_t v) { return net_be16_to_cpu(v); }

/* ----------------------------------------------------------------
 * Initialisation and configuration
 * ---------------------------------------------------------------- */

/* Two-phase init.
 *
 * `net_attach()` installs our RX dispatcher with virtio-net and
 * reads our MAC.  After this call the stack will dispatch any
 * inbound frame correctly (e.g. a DHCP OFFER addressed to
 * 255.255.255.255 will reach the UDP layer), but it has no
 * IPv4 address of its own yet.
 *
 * `net_set_ipv4_config()` installs (or updates) the local IPv4
 * configuration.  It can be called more than once — at boot
 * we typically attach, run DHCP, then set_ipv4_config() with
 * the lease results.
 *
 * `net_init()` is the one-shot convenience wrapper used by
 * code paths that already know their IP statically: it simply
 * calls attach + set_ipv4_config in sequence.
 *
 * All three return 0 on success and -1 if the NIC is absent. */
int net_attach(void);
int net_set_ipv4_config(const uint8_t local_ip[NET_IPV4_LEN],
                        const uint8_t gateway_ip[NET_IPV4_LEN],
                        const uint8_t netmask[NET_IPV4_LEN]);
int net_init(const uint8_t local_ip[NET_IPV4_LEN],
             const uint8_t gateway_ip[NET_IPV4_LEN],
             const uint8_t netmask[NET_IPV4_LEN]);

/* Read back our MAC / IPv4 config (e.g. for a `ifconfig`-style
 * command in userspace).  Pointers may be NULL to
 * skip individual fields. */
void net_get_config(uint8_t out_mac[NET_MAC_LEN],
                    uint8_t out_ip[NET_IPV4_LEN],
                    uint8_t out_gw[NET_IPV4_LEN],
                    uint8_t out_mask[NET_IPV4_LEN]);

/* DNS server address (typically learned from
 * DHCP option 6).  `net_set_dns` accepts 0.0.0.0 to mean "none";
 * `net_get_dns` returns 1 if a server has been set, 0 otherwise
 * (and zeros `out_ip` in that case). */
void net_set_dns(const uint8_t dns_ip[NET_IPV4_LEN]);
int  net_get_dns(uint8_t out_ip[NET_IPV4_LEN]);

/* Drain any pending RX frames from virtio-net.  Equivalent to
 * `virtio_net_drain_rx()` but routed through our dispatcher.
 * Returns the number of frames processed. */
int  net_poll(void);

/* ----------------------------------------------------------------
 * Loopback (chapter 108)
 * ----------------------------------------------------------------
 *
 * "Local" addresses are addresses that we should deliver to
 * ourselves rather than send out the wire.  Two cases qualify:
 *
 *   - 127.0.0.0/8         (the classical loopback prefix)
 *   - our own DHCP IP     (g_ip, when nonzero) -- traffic to
 *                         ourselves should also stay on the box
 *
 * `net_is_local_ip()` is the predicate used by both the TX
 * short-circuit in `net_ipv4_send_from` and the RX accept gate
 * in `rx_handle_ipv4` to recognise loopback traffic.
 *
 * `net_choose_src()` is the source-address selection helper
 * used by upper layers (TCP, UDP) when picking the src to
 * stamp on an outbound segment.  For a local destination it
 * returns the destination itself (so both sides of the
 * conversation observe a symmetric 4-tuple); for a real
 * destination it returns g_ip.  This is what Linux's source-
 * selection algorithm does for loopback. */
int  net_is_local_ip(const uint8_t ip[NET_IPV4_LEN]);
void net_choose_src (const uint8_t dst_ip[NET_IPV4_LEN],
                     uint8_t out_src[NET_IPV4_LEN]);

/* ----------------------------------------------------------------
 * ARP
 * ---------------------------------------------------------------- */

/* Look up the MAC for `ip` in the cache.  Returns 1 and writes
 * `out_mac` on hit; returns 0 on miss (caller should issue an
 * ARP request via `net_arp_request`). */
int  net_arp_lookup(const uint8_t ip[NET_IPV4_LEN],
                    uint8_t out_mac[NET_MAC_LEN]);

/* Transmit a broadcast ARP "who has" request for `ip`.
 * Returns 0 on success, -1 if the NIC is absent / TX is full. */
int  net_arp_request(const uint8_t ip[NET_IPV4_LEN]);

/* Synchronously block (poll-spinning) until `ip` is in the
 * ARP cache or the deadline expires.  Returns 1 if the entry
 * is in the cache on return, 0 on timeout.  Issues an initial
 * ARP request if the cache is cold. */
int  net_arp_resolve(const uint8_t ip[NET_IPV4_LEN],
                     uint8_t out_mac[NET_MAC_LEN],
                     uint64_t spin_iters);

/* ----------------------------------------------------------------
 * IPv4
 * ---------------------------------------------------------------- */

/* 1's-complement checksum over `len` bytes of `data`, computed
 * as a 16-bit big-endian value (i.e. the value the IPv4 header
 * `checksum` field expects). */
uint16_t net_ipv4_checksum(const void *data, uint32_t len);

/* Build an IPv4 packet: write a header into `out`, then the
 * caller's `payload` of `payload_len` bytes immediately after.
 * Returns the total IPv4 packet length (IPV4_HDR_LEN + payload_len)
 * on success or -1 on overflow.  Sets `id` to a monotonically-
 * increasing value, TTL to IPV4_DEFAULT_TTL, no options, no
 * fragmentation. */
int  net_ipv4_build(uint8_t *out, uint32_t cap,
                    const uint8_t dst_ip[NET_IPV4_LEN],
                    uint8_t proto,
                    const void *payload, uint32_t payload_len);

/* Same as `net_ipv4_build` but lets the caller specify the
 * source IP explicitly.  Used by DHCP, which sends DISCOVER
 * frames with src=0.0.0.0 before it has a lease. */
int  net_ipv4_build_src(uint8_t *out, uint32_t cap,
                        const uint8_t src_ip[NET_IPV4_LEN],
                        const uint8_t dst_ip[NET_IPV4_LEN],
                        uint8_t proto,
                        const void *payload, uint32_t payload_len);

/* Send an IPv4 packet.  Resolves the destination MAC via the
 * ARP cache (gateway MAC for off-subnet destinations, broadcast
 * MAC for 255.255.255.255).  Returns 0 on success or -1 on ARP
 * miss / TX failure. */
int  net_ipv4_send(const uint8_t dst_ip[NET_IPV4_LEN],
                   uint8_t proto,
                   const void *payload, uint32_t payload_len);

/* Same as `net_ipv4_send` but with an explicit source IP.  DHCP
 * uses this with src=0.0.0.0 for DISCOVER. */
int  net_ipv4_send_from(const uint8_t src_ip[NET_IPV4_LEN],
                        const uint8_t dst_ip[NET_IPV4_LEN],
                        uint8_t proto,
                        const void *payload, uint32_t payload_len);

/* ----------------------------------------------------------------
 * IPv4 RX demultiplexing.
 *
 * The dispatcher in net.c routes inbound IPv4 packets directly
 * to the ICMP and UDP modules (kernel/core/{icmp,udp}.{c,h}).
 * No registration API for upstream code — those modules are
 * compiled into the kernel and call out to user-installed
 * callbacks of their own (see udp_set_rx_callback).
 * ---------------------------------------------------------------- */

#endif /* KERNEL_CORE_NET_H */
