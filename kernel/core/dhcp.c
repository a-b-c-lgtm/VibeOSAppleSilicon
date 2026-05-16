/*
 * kernel/core/dhcp.c \u2014 milestone-54 DHCPv4 client.
 *
 * The minimum implementation that can hold a conversation with
 * a DHCP server: build DISCOVER, parse OFFER, build REQUEST,
 * parse ACK, install the lease, return.  No renewals, no
 * rebind, no DECLINE.  When the lease eventually expires we'll
 * just lose connectivity \u2014 fine for QEMU work where reboots
 * are minutes apart.
 *
 * Wire format is BOOTP (RFC 951) extended by DHCP (RFC 2131).
 * The fixed part is 240 bytes; options follow, terminated by
 * the END byte (255).
 */

#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "../device/virtio_net.h"
#include "serial.h"

#define DHCP_OP_BOOTREQUEST   1u
#define DHCP_OP_BOOTREPLY     2u

#define DHCP_MAGIC_COOKIE     0x63825363u

/* DHCP option codes we care about. */
#define DHCP_OPT_PAD             0u
#define DHCP_OPT_SUBNET_MASK     1u
#define DHCP_OPT_ROUTER          3u
#define DHCP_OPT_DNS             6u
#define DHCP_OPT_REQUESTED_IP   50u
#define DHCP_OPT_LEASE_TIME     51u
#define DHCP_OPT_MSG_TYPE       53u
#define DHCP_OPT_SERVER_ID      54u
#define DHCP_OPT_PARAM_LIST     55u
#define DHCP_OPT_END           255u

/* DHCP message types (option 53 values). */
#define DHCP_MSG_DISCOVER     1u
#define DHCP_MSG_OFFER        2u
#define DHCP_MSG_REQUEST      3u
#define DHCP_MSG_ACK          5u

#define BOOTP_FIXED_LEN     240u

#define DHCP_CLIENT_PORT     68u
#define DHCP_SERVER_PORT     67u

/* ---- micro mem* ---- */
static void d_memset(void *p, int v, uint32_t n)
{
    uint8_t *q = (uint8_t *)p;
    while (n--) *q++ = (uint8_t)v;
}
static void d_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
}

/* ----------------------------------------------------------------
 * State machine
 * ---------------------------------------------------------------- */

enum {
    DHCP_STATE_INIT     = 0,
    DHCP_STATE_OFFERED  = 1,
    DHCP_STATE_BOUND    = 2,
    DHCP_STATE_FAILED   = 3,
};

static volatile int    g_state;
static uint32_t        g_xid;        /* host order */
static uint8_t         g_offered_ip [NET_IPV4_LEN];
static uint8_t         g_offered_gw [NET_IPV4_LEN];
static uint8_t         g_offered_msk[NET_IPV4_LEN];
static uint8_t         g_offered_dns[NET_IPV4_LEN];
static uint8_t         g_server_id  [NET_IPV4_LEN];
static volatile int    g_have_subnet;
static volatile int    g_have_router;
static volatile int    g_have_dns;

/* ----------------------------------------------------------------
 * BOOTP packet builder
 * ---------------------------------------------------------------- */

/* Returns the total packet length (fixed + options). */
static uint32_t build_bootp(uint8_t *buf, uint32_t cap,
                            uint8_t  msg_type,
                            const uint8_t *requested_ip,    /* 4 or NULL */
                            const uint8_t *server_id)       /* 4 or NULL */
{
    if (cap < BOOTP_FIXED_LEN + 64) return 0;
    d_memset(buf, 0, BOOTP_FIXED_LEN);

    buf[0] = DHCP_OP_BOOTREQUEST;
    buf[1] = 1;                            /* htype = Ethernet */
    buf[2] = 6;                            /* hlen  = 6        */
    buf[3] = 0;                            /* hops             */

    /* xid */
    buf[4] = (uint8_t)(g_xid >> 24);
    buf[5] = (uint8_t)(g_xid >> 16);
    buf[6] = (uint8_t)(g_xid >>  8);
    buf[7] = (uint8_t)(g_xid >>  0);

    /* secs = 0, flags = 0x8000 (BROADCAST bit \u2014 ask the server
     * to broadcast its reply, since we have no IP yet to receive
     * a unicast reply targeted at us). */
    buf[10] = 0x80;
    buf[11] = 0x00;

    /* ciaddr/yiaddr/siaddr/giaddr stay zero. */

    /* chaddr = our MAC, padded with zeros to 16 bytes. */
    uint8_t mac[NET_MAC_LEN];
    net_get_config(mac, (uint8_t *)0, (uint8_t *)0, (uint8_t *)0);
    d_memcpy(buf + 28, mac, NET_MAC_LEN);

    /* Magic cookie at offset 236. */
    buf[236] = (uint8_t)(DHCP_MAGIC_COOKIE >> 24);
    buf[237] = (uint8_t)(DHCP_MAGIC_COOKIE >> 16);
    buf[238] = (uint8_t)(DHCP_MAGIC_COOKIE >>  8);
    buf[239] = (uint8_t)(DHCP_MAGIC_COOKIE >>  0);

    /* Options begin at offset 240. */
    uint32_t off = BOOTP_FIXED_LEN;

    /* 53: message type (always present) */
    buf[off++] = DHCP_OPT_MSG_TYPE;
    buf[off++] = 1;
    buf[off++] = msg_type;

    if (requested_ip) {
        buf[off++] = DHCP_OPT_REQUESTED_IP;
        buf[off++] = 4;
        d_memcpy(buf + off, requested_ip, NET_IPV4_LEN);
        off += NET_IPV4_LEN;
    }
    if (server_id) {
        buf[off++] = DHCP_OPT_SERVER_ID;
        buf[off++] = 4;
        d_memcpy(buf + off, server_id, NET_IPV4_LEN);
        off += NET_IPV4_LEN;
    }

    /* 55: parameter request list */
    buf[off++] = DHCP_OPT_PARAM_LIST;
    buf[off++] = 4;
    buf[off++] = DHCP_OPT_SUBNET_MASK;
    buf[off++] = DHCP_OPT_ROUTER;
    buf[off++] = DHCP_OPT_DNS;
    buf[off++] = DHCP_OPT_LEASE_TIME;

    /* End marker. */
    buf[off++] = DHCP_OPT_END;

    return off;
}

/* ----------------------------------------------------------------
 * BOOTP RX parser  (called from udp_bind callback)
 * ---------------------------------------------------------------- */

static void parse_options(const uint8_t *opt, uint32_t len,
                          uint8_t *out_msg_type)
{
    *out_msg_type = 0;
    g_have_subnet = 0;
    g_have_router = 0;
    g_have_dns    = 0;
    d_memset(g_offered_gw,  0, NET_IPV4_LEN);
    d_memset(g_offered_msk, 0, NET_IPV4_LEN);
    d_memset(g_offered_dns, 0, NET_IPV4_LEN);
    d_memset(g_server_id,   0, NET_IPV4_LEN);

    uint32_t i = 0;
    while (i < len) {
        uint8_t code = opt[i++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (i >= len) break;
        uint8_t olen = opt[i++];
        if (i + olen > len) break;
        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (olen == 1) *out_msg_type = opt[i];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (olen == 4) {
                d_memcpy(g_offered_msk, opt + i, NET_IPV4_LEN);
                g_have_subnet = 1;
            }
            break;
        case DHCP_OPT_ROUTER:
            /* Some servers list multiple routers; take the first. */
            if (olen >= 4) {
                d_memcpy(g_offered_gw, opt + i, NET_IPV4_LEN);
                g_have_router = 1;
            }
            break;
        case DHCP_OPT_SERVER_ID:
            if (olen == 4) d_memcpy(g_server_id, opt + i, NET_IPV4_LEN);
            break;
        case DHCP_OPT_DNS:
            /* Servers may list multiple DNS servers; take the first. */
            if (olen >= 4) {
                d_memcpy(g_offered_dns, opt + i, NET_IPV4_LEN);
                g_have_dns = 1;
            }
            break;
        default: /* lease time, etc. — ignored */
            break;
        }
        i += olen;
    }
}

static void dhcp_rx(const uint8_t src_ip[NET_IPV4_LEN],
                    uint16_t src_port,
                    const uint8_t *payload, uint32_t plen)
{
    (void)src_ip; (void)src_port;
    if (plen < BOOTP_FIXED_LEN + 4) return;
    if (payload[0] != DHCP_OP_BOOTREPLY) return;

    /* Match xid. */
    uint32_t xid = ((uint32_t)payload[4] << 24) |
                   ((uint32_t)payload[5] << 16) |
                   ((uint32_t)payload[6] <<  8) |
                   ((uint32_t)payload[7] <<  0);
    if (xid != g_xid) return;

    /* Magic cookie. */
    uint32_t cookie = ((uint32_t)payload[236] << 24) |
                      ((uint32_t)payload[237] << 16) |
                      ((uint32_t)payload[238] <<  8) |
                      ((uint32_t)payload[239] <<  0);
    if (cookie != DHCP_MAGIC_COOKIE) return;

    /* yiaddr at offset 16. */
    d_memcpy(g_offered_ip, payload + 16, NET_IPV4_LEN);

    /* Options start at offset 240. */
    uint8_t msg_type;
    parse_options(payload + 240, plen - 240, &msg_type);

    if (msg_type == DHCP_MSG_OFFER) {
        if (g_state == DHCP_STATE_INIT) g_state = DHCP_STATE_OFFERED;
    } else if (msg_type == DHCP_MSG_ACK) {
        if (g_state == DHCP_STATE_OFFERED) g_state = DHCP_STATE_BOUND;
    }
}

/* ----------------------------------------------------------------
 * Synchronous acquire
 * ---------------------------------------------------------------- */

/* Cheap "random enough" xid: cycle counter on AArch64.  We just
 * need it different from anything the host-side server might
 * have cached for this MAC. */
static uint32_t fresh_xid(void)
{
    uint64_t t;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(t));
    /* Mix in our MAC's last byte so simultaneous boots don't
     * collide if we ever run two instances. */
    uint8_t mac[NET_MAC_LEN];
    net_get_config(mac, (uint8_t *)0, (uint8_t *)0, (uint8_t *)0);
    return ((uint32_t)(t & 0xFFFFFFFFu)) ^ ((uint32_t)mac[5] << 24);
}

static const uint8_t IP_ZERO    [NET_IPV4_LEN] = { 0,   0,   0,   0   };
static const uint8_t IP_BCAST   [NET_IPV4_LEN] = { 255, 255, 255, 255 };

int dhcp_acquire(uint64_t spin_iters)
{
    if (!virtio_net_present()) return -1;

    g_xid          = fresh_xid();
    g_state        = DHCP_STATE_INIT;
    g_have_subnet  = 0;
    g_have_router  = 0;

    /* Bind 68 to receive OFFER/ACK. */
    if (udp_bind(DHCP_CLIENT_PORT, dhcp_rx) < 0) return -1;

    /* Send DISCOVER. */
    uint8_t pkt[BOOTP_FIXED_LEN + 64];
    uint32_t plen = build_bootp(pkt, sizeof(pkt),
                                DHCP_MSG_DISCOVER, (const uint8_t *)0,
                                (const uint8_t *)0);
    if (!plen ||
        udp_send(IP_ZERO, IP_BCAST,
                 DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 pkt, plen) < 0) {
        udp_bind(DHCP_CLIENT_PORT, (udp_rx_cb)0);
        return -1;
    }
    serial_puts("[dhcp] DISCOVER sent\n");

    /* Spin until OFFER arrives or budget runs out. */
    uint64_t halfway = spin_iters / 2;
    for (uint64_t i = 0; i < halfway && g_state == DHCP_STATE_INIT; i++) {
        if ((i & 0xfffu) == 0) (void)net_poll();
        __asm__ volatile("" ::: "memory");
    }
    if (g_state != DHCP_STATE_OFFERED) {
        serial_puts("[dhcp] no OFFER\n");
        udp_bind(DHCP_CLIENT_PORT, (udp_rx_cb)0);
        return -1;
    }
    serial_puts("[dhcp] OFFER received\n");

    /* Send REQUEST for the offered IP. */
    plen = build_bootp(pkt, sizeof(pkt),
                       DHCP_MSG_REQUEST, g_offered_ip, g_server_id);
    if (!plen ||
        udp_send(IP_ZERO, IP_BCAST,
                 DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 pkt, plen) < 0) {
        udp_bind(DHCP_CLIENT_PORT, (udp_rx_cb)0);
        return -1;
    }
    serial_puts("[dhcp] REQUEST sent\n");

    /* Spin until ACK arrives or budget runs out. */
    for (uint64_t i = 0; i < halfway && g_state == DHCP_STATE_OFFERED; i++) {
        if ((i & 0xfffu) == 0) (void)net_poll();
        __asm__ volatile("" ::: "memory");
    }
    udp_bind(DHCP_CLIENT_PORT, (udp_rx_cb)0);

    if (g_state != DHCP_STATE_BOUND) {
        serial_puts("[dhcp] no ACK\n");
        return -1;
    }

    /* Apply the lease.  If the server didn't include a subnet
     * mask or router (rare, but possible) make conservative
     * assumptions: /24 and "gateway is .1 of our subnet". */
    if (!g_have_subnet) {
        g_offered_msk[0] = 255; g_offered_msk[1] = 255;
        g_offered_msk[2] = 255; g_offered_msk[3] = 0;
    }
    if (!g_have_router) {
        d_memcpy(g_offered_gw, g_offered_ip, NET_IPV4_LEN);
        g_offered_gw[3] = 1;
    }

    serial_puts("[dhcp] lease acquired\n");
    if (net_set_ipv4_config(g_offered_ip, g_offered_gw, g_offered_msk) < 0)
        return -1;
    if (g_have_dns) net_set_dns(g_offered_dns);
    return 0;
}
