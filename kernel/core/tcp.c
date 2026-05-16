/*
 * kernel/core/tcp.c — milestone-55 TCPv4 (client side).
 *
 * Design notes:
 *
 *   - Fixed connection table (TCP_CONN_CAP = 4).  Each entry
 *     owns a 4 KiB TX ring and a 4 KiB RX ring.  4 conns × 8 KiB
 *     = 32 KiB total, which is fine for kernel BSS.
 *
 *   - Local port is chosen from an ephemeral pool starting at
 *     49152 and incrementing modulo 16384.  We don't validate
 *     against a "ports already in use" set because the
 *     remote-tuple uniqueness check in tcp_handle is enough
 *     (TCP allows two connections to share a local port if the
 *     remote 4-tuples differ, but we never do that on purpose).
 *
 *   - State machine:
 *
 *         CLOSED -- connect --> SYN_SENT
 *         SYN_SENT -- recv SYN+ACK --> ESTABLISHED   (send ACK)
 *         ESTABLISHED -- close --> FIN_WAIT_1        (send FIN)
 *         FIN_WAIT_1 -- recv ACK of FIN --> FIN_WAIT_2
 *         FIN_WAIT_2 -- recv FIN --> TIME_WAIT       (send ACK, then CLOSED)
 *         ESTABLISHED -- recv FIN --> CLOSE_WAIT     (send ACK)
 *         CLOSE_WAIT -- close --> LAST_ACK           (send FIN)
 *         LAST_ACK -- recv ACK of FIN --> CLOSED
 *
 *     We collapse TIME_WAIT to immediate CLOSED because we
 *     don't reuse 4-tuples within a window.
 *
 *   - Retransmission: every tcp_poll() (called from net_poll
 *     and from the boot self-test spin loop) we look for an
 *     unacked-segment > N polls old and resend.  Crude, but
 *     SLIRP rarely loses anything so this almost never fires.
 *
 *   - Window: we always advertise the free space in our RX
 *     buffer.  If the buffer fills, the peer stalls — we don't
 *     drop bytes.
 *
 *   - We always ACK immediately (no delayed-ACK), and we always
 *     PSH on send (no Nagle).
 */

#include "tcp.h"
#include "net.h"
#include "udp.h"     /* for the pseudo-header style only */
#include "serial.h"
#include "../device/virtio_net.h"

/* ---- micro mem* (avoids implicit memset/memcpy in freestanding) ---- */
static void *t_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t       *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}
static void t_memset(void *d, int v, uint32_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)v;
}
static int t_memeq(const void *a, const void *b, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) { if (*p++ != *q++) return 0; }
    return 1;
}

/* ----------------------------------------------------------------
 * Connection table
 * ---------------------------------------------------------------- */

#define TCP_CONN_CAP   16
/* Receive window: also doubles as our SO_RCVBUF.  Larger windows
 * let a fast peer send more bytes per round trip before stalling
 * on our zero-window ACK.  At 4 KB the M63 browser was getting
 * ~1 KB/s on HN's 38 KB index because each `<= 4 KB` chunk had
 * to wait for the peer's persist timer (~200 ms first probe,
 * doubling) between window-update opportunities.  32 KB cuts a
 * 38 KB transfer to two windows of bytes-in-flight, which the
 * peer can stream back-to-back.  The TCP header's window field
 * is 16 bits so 65535 is the absolute ceiling without window
 * scaling (RFC 7323), which we don't implement. */
#define TCP_BUF_SIZE   32768u
#define TCP_MSS        1460u   /* MSS we advertise in our SYN */

#define TCP_RTX_THRESH 200000ULL  /* poll iterations before retransmit */
#define TCP_TX_MAX     1400u      /* max payload bytes per outbound segment
                                     (under MSS by 60 bytes for safety) */

struct tcp_conn {
    uint8_t  valid;
    uint8_t  state;            /* enum tcp_state */
    uint8_t  remote_ip[NET_IPV4_LEN];
    uint16_t local_port;
    uint16_t remote_port;

    /* Sequence space. */
    uint32_t snd_iss;          /* our chosen initial sequence number */
    uint32_t snd_una;          /* oldest unacknowledged byte */
    uint32_t snd_nxt;          /* next byte to send */
    uint32_t snd_wnd;          /* peer's advertised receive window */
    uint32_t rcv_irs;          /* peer's initial sequence number */
    uint32_t rcv_nxt;          /* next byte we expect from peer */

    /* TX ring: bytes from `snd_una` (logically) up to
     * `snd_una + tx_len`.  When data is acked we shift the ring. */
    uint8_t  tx_buf[TCP_BUF_SIZE];
    uint32_t tx_len;           /* bytes currently buffered */

    /* RX ring: bytes the user hasn't pulled yet.  Bytes arrive
     * via tcp_handle and leave via tcp_recv. */
    uint8_t  rx_buf[TCP_BUF_SIZE];
    uint32_t rx_len;           /* bytes available */

    /* FIN bookkeeping. */
    uint8_t  fin_sent;         /* we've queued a FIN at snd_nxt+tx_len */
    uint8_t  fin_acked;        /* peer has ACKed our FIN */
    uint8_t  peer_fin;         /* peer sent us a FIN (rcv_nxt advanced past it) */
    uint8_t  reset;            /* peer sent RST or we hit a fatal error */
    uint8_t  user_closed;      /* sys_close has been called on the user fd —
                                * once state reaches TCP_CLOSED the slot is
                                * safe to recycle.  Without this flag we'd
                                * pin the slot forever after a normal active
                                * close (FIN_WAIT_1 -> ... -> CLOSED transitions
                                * happen asynchronously inside tcp_handle, but
                                * release_conn was only ever called from the
                                * synchronous close-while-already-CLOSED path). */

    /* Retransmission. */
    uint64_t last_tx_poll;     /* tcp_poll counter at last data TX */
};

static struct tcp_conn g_conns[TCP_CONN_CAP];
static uint64_t       g_poll_counter;
static uint16_t       g_next_eph_port = 49152u;

/* ----------------------------------------------------------------
 * Utility: timer counter for ISN selection
 * ---------------------------------------------------------------- */

static uint32_t fresh_isn(void)
{
    uint64_t t;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(t));
    return (uint32_t)(t & 0xFFFFFFFFu);
}

/* ----------------------------------------------------------------
 * Checksum (pseudo-header + tcp_hdr + data), exactly like UDP.
 * ---------------------------------------------------------------- */

static uint16_t tcp_compute_checksum(const uint8_t src_ip[NET_IPV4_LEN],
                                     const uint8_t dst_ip[NET_IPV4_LEN],
                                     const uint8_t *seg, uint32_t seg_len)
{
    uint8_t buf[12 + 1500];
    if (seg_len > sizeof(buf) - 12) return 0;
    t_memcpy(buf + 0, src_ip, NET_IPV4_LEN);
    t_memcpy(buf + 4, dst_ip, NET_IPV4_LEN);
    buf[8]  = 0;
    buf[9]  = IPV4_PROTO_TCP;
    buf[10] = (uint8_t)(seg_len >> 8);
    buf[11] = (uint8_t)(seg_len & 0xFF);
    t_memcpy(buf + 12, seg, seg_len);
    return net_ipv4_checksum(buf, 12 + seg_len);
}

/* ----------------------------------------------------------------
 * Conn table helpers
 * ---------------------------------------------------------------- */

static int alloc_conn(void)
{
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        if (!g_conns[i].valid) {
            t_memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].valid = 1;
            return i;
        }
    }
    return -1;
}

static void release_conn(int cid)
{
    if (cid < 0 || cid >= TCP_CONN_CAP) return;
    t_memset(&g_conns[cid], 0, sizeof(g_conns[cid]));
}

static struct tcp_conn *get_conn(int cid)
{
    if (cid < 0 || cid >= TCP_CONN_CAP) return (struct tcp_conn *)0;
    if (!g_conns[cid].valid)            return (struct tcp_conn *)0;
    return &g_conns[cid];
}

static int find_conn_for_pkt(const uint8_t remote_ip[NET_IPV4_LEN],
                             uint16_t local_port, uint16_t remote_port)
{
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid) continue;
        if (c->local_port  != local_port)  continue;
        if (c->remote_port != remote_port) continue;
        if (!t_memeq(c->remote_ip, remote_ip, NET_IPV4_LEN)) continue;
        return i;
    }
    return -1;
}

/* ----------------------------------------------------------------
 * TX a single segment.  Builds a TCP header (with up to 4 bytes
 * of MSS option for SYNs), patches the checksum, and hands it
 * to net_ipv4_send.
 * ---------------------------------------------------------------- */

static int tcp_tx(struct tcp_conn *c, uint8_t flags,
                  uint32_t seq, uint32_t ack,
                  const uint8_t *data, uint32_t data_len,
                  int with_mss_option)
{
    uint8_t pkt[TCP_HDR_MIN_LEN + 4 + 1460];
    uint32_t hdr_len = TCP_HDR_MIN_LEN;
    if (with_mss_option) hdr_len += 4;
    if (data_len > 1460) data_len = 1460;
    if (hdr_len + data_len > sizeof(pkt)) return -1;

    struct tcp_hdr *h = (struct tcp_hdr *)pkt;
    h->src_port = net_cpu_to_be16(c->local_port);
    h->dst_port = net_cpu_to_be16(c->remote_port);
    h->seq      = (uint32_t)((seq >> 24) & 0xFF);  /* placeholder; fill below */
    /* Write seq/ack as big-endian 32-bit values byte-by-byte so we
     * don't depend on a host bswap32 helper. */
    {
        uint8_t *p = (uint8_t *)&h->seq;
        p[0] = (uint8_t)(seq >> 24);
        p[1] = (uint8_t)(seq >> 16);
        p[2] = (uint8_t)(seq >>  8);
        p[3] = (uint8_t)(seq      );
        p = (uint8_t *)&h->ack;
        p[0] = (uint8_t)(ack >> 24);
        p[1] = (uint8_t)(ack >> 16);
        p[2] = (uint8_t)(ack >>  8);
        p[3] = (uint8_t)(ack      );
    }
    h->data_off = (uint8_t)((hdr_len / 4) << 4);
    h->flags    = flags;

    /* Advertise the free space in our RX ring as the receive
     * window.  If a FIN has been delivered we still advertise
     * what's left — the peer needs that to know whether they
     * can send more before we close. */
    uint32_t free_rx = TCP_BUF_SIZE - c->rx_len;
    if (free_rx > 0xFFFFu) free_rx = 0xFFFFu;
    h->window = net_cpu_to_be16((uint16_t)free_rx);

    h->checksum = 0;
    h->urgent   = 0;

    if (with_mss_option) {
        uint8_t *opt = pkt + TCP_HDR_MIN_LEN;
        opt[0] = TCP_OPT_MSS;
        opt[1] = 4;
        opt[2] = (uint8_t)(TCP_MSS >> 8);
        opt[3] = (uint8_t)(TCP_MSS & 0xFF);
    }

    if (data_len) t_memcpy(pkt + hdr_len, data, data_len);

    /* Compute checksum. */
    uint8_t our_ip[NET_IPV4_LEN];
    net_get_config((uint8_t *)0, our_ip, (uint8_t *)0, (uint8_t *)0);
    uint16_t cks = tcp_compute_checksum(our_ip, c->remote_ip,
                                        pkt, hdr_len + data_len);
    h->checksum = cks;

    return net_ipv4_send(c->remote_ip, IPV4_PROTO_TCP,
                         pkt, hdr_len + data_len);
}

/* Send a pure ACK (no data, no SYN/FIN). */
static void tcp_send_ack(struct tcp_conn *c)
{
    tcp_tx(c, TCP_FLAG_ACK, c->snd_nxt, c->rcv_nxt, (const uint8_t *)0, 0, 0);
}

/* Push as much TX-buffer data + any pending FIN as fits in the
 * peer's window, in MSS-sized segments. */
static void tcp_drain_tx(struct tcp_conn *c)
{
    /* How many bytes have we already put on the wire but not yet
     * acked?  That's snd_nxt - snd_una. */
    uint32_t in_flight = c->snd_nxt - c->snd_una;
    uint32_t wnd       = c->snd_wnd;
    if (wnd == 0) wnd = 1;       /* probe with 1 byte if zero window */

    while (in_flight < wnd) {
        uint32_t buf_off = c->snd_nxt - c->snd_una;
        if (buf_off >= c->tx_len) break;       /* nothing new to send */
        uint32_t avail   = c->tx_len - buf_off;
        uint32_t can     = wnd - in_flight;
        uint32_t seg     = avail < can ? avail : can;
        if (seg > TCP_TX_MAX) seg = TCP_TX_MAX;
        uint8_t flags = TCP_FLAG_ACK | TCP_FLAG_PSH;
        if (tcp_tx(c, flags, c->snd_nxt, c->rcv_nxt,
                   c->tx_buf + buf_off, seg, 0) < 0) break;
        c->snd_nxt    += seg;
        in_flight     += seg;
        c->last_tx_poll = g_poll_counter;
    }

    /* Send FIN once all queued bytes have been put on the wire. */
    if (!c->fin_sent &&
        (c->state == TCP_FIN_WAIT_1 || c->state == TCP_LAST_ACK)) {
        if (c->snd_nxt - c->snd_una >= c->tx_len) {
            tcp_tx(c, TCP_FLAG_ACK | TCP_FLAG_FIN,
                   c->snd_nxt, c->rcv_nxt, (const uint8_t *)0, 0, 0);
            c->snd_nxt   += 1;     /* FIN consumes 1 sequence number */
            c->fin_sent   = 1;
            c->last_tx_poll = g_poll_counter;
        }
    }
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int tcp_connect(const uint8_t dst_ip[NET_IPV4_LEN], uint16_t dst_port)
{
    int cid = alloc_conn();
    if (cid < 0) return -1;
    struct tcp_conn *c = &g_conns[cid];

    t_memcpy(c->remote_ip, dst_ip, NET_IPV4_LEN);
    c->remote_port = dst_port;
    c->local_port  = g_next_eph_port++;
    if (g_next_eph_port == 0) g_next_eph_port = 49152u;

    c->snd_iss     = fresh_isn();
    c->snd_una     = c->snd_iss;
    c->snd_nxt     = c->snd_iss;       /* SYN consumes 1 below */
    c->snd_wnd     = TCP_BUF_SIZE;
    c->state       = TCP_SYN_SENT;
    c->last_tx_poll = g_poll_counter;

    /* Send SYN with MSS option. */
    if (tcp_tx(c, TCP_FLAG_SYN, c->snd_iss, 0,
               (const uint8_t *)0, 0, /*with_mss_option*/1) < 0) {
        release_conn(cid);
        return -1;
    }
    c->snd_nxt = c->snd_iss + 1;       /* SYN consumes 1 sequence number */
    return cid;
}

int tcp_send(int cid, const void *data, uint32_t len)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c) return -1;
    if (c->reset) return -1;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return -1;

    /* Append to TX buffer (truncating if it would overflow). */
    uint32_t free = TCP_BUF_SIZE - c->tx_len;
    uint32_t put  = len < free ? len : free;
    if (put) t_memcpy(c->tx_buf + c->tx_len, data, put);
    c->tx_len += put;

    tcp_drain_tx(c);
    return (int)put;
}

int tcp_recv(int cid, void *buf, uint32_t cap)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c) return -1;
    if (c->reset) return -1;

    if (c->rx_len == 0) {
        /* No data buffered.  Distinguish "more might come" from EOF. */
        if (c->peer_fin) return 0;       /* EOF: no more bytes ever */
        return 0;                        /* spurious; caller polls */
    }

    uint32_t take = c->rx_len < cap ? c->rx_len : cap;
    uint32_t free_before = TCP_BUF_SIZE - c->rx_len;
    t_memcpy(buf, c->rx_buf, take);
    /* Shift the ring left by `take`. */
    if (c->rx_len > take) {
        t_memcpy(c->rx_buf, c->rx_buf + take, c->rx_len - take);
    }
    c->rx_len -= take;

    /* Window-update ACK.  We last advertised `free_before` bytes;
     * after this read we have `free_before + take` available.  If
     * the peer was paused (window was small relative to MSS) and
     * we just opened a meaningful slot, send a pure ACK so they
     * resume immediately rather than waiting for their persist
     * timer.  Threshold: opened up at least 1 MSS of new room AND
     * either the new window is now >= MSS, or we previously had
     * essentially nothing to advertise.
     *
     * Without this, a userspace fetch like the M63 browser pulls
     * a 4-KB-window full, drains it locally, and then sits idle
     * waiting for the peer to probe again — which on Linux/macOS
     * starts at ~200 ms and doubles per probe.  For a 38 KB HN
     * index that turned a sub-second fetch into 45 s.  Sending
     * the ACK eagerly costs one 40-byte packet per call but
     * uncaps end-to-end throughput. */
    if (c->state == TCP_ESTABLISHED && take >= TCP_MSS) {
        tcp_send_ack(c);
    } else if (c->state == TCP_ESTABLISHED &&
               free_before < TCP_MSS &&
               (TCP_BUF_SIZE - c->rx_len) >= TCP_MSS) {
        /* We were below 1 MSS of free room (peer likely stalled);
         * we now have at least 1 MSS free.  Tell them right away. */
        tcp_send_ack(c);
    }
    return (int)take;
}

int tcp_close(int cid)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c) return -1;
    /* Mark that the user has released the fd.  Any subsequent
     * transition to TCP_CLOSED (driven asynchronously by tcp_handle
     * when our FIN is acked, or by tcp_poll if we time out) will
     * now reap the slot — see the sweep at the bottom of tcp_poll. */
    c->user_closed = 1;

    if (c->reset) { release_conn(cid); return 0; }

    if (c->state == TCP_ESTABLISHED) {
        c->state = TCP_FIN_WAIT_1;
        tcp_drain_tx(c);
    } else if (c->state == TCP_CLOSE_WAIT) {
        c->state = TCP_LAST_ACK;
        tcp_drain_tx(c);
    } else if (c->state == TCP_CLOSED) {
        release_conn(cid);
    }
    /* Other states are mid-shutdown; let the state machine finish. */
    return 0;
}

int tcp_state(int cid)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c) return TCP_CLOSED;
    return c->state;
}

int tcp_eof(int cid)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c) return 1;
    return (c->peer_fin && c->rx_len == 0) ? 1 : 0;
}

/* ----------------------------------------------------------------
 * RX
 * ---------------------------------------------------------------- */

static void apply_ack(struct tcp_conn *c, uint32_t ack)
{
    /* Ignore ACKs outside (snd_una, snd_nxt]. */
    uint32_t in_flight = c->snd_nxt - c->snd_una;
    uint32_t acked     = ack        - c->snd_una;
    if (acked == 0 || acked > in_flight) return;

    /* Was our FIN included in this ack? */
    uint32_t data_in_flight = in_flight;
    if (c->fin_sent) data_in_flight -= 1;   /* the FIN is the last byte */
    uint32_t data_acked = acked < data_in_flight ? acked : data_in_flight;

    /* Slide the TX buffer left by `data_acked` bytes. */
    if (data_acked) {
        if (c->tx_len > data_acked) {
            t_memcpy(c->tx_buf, c->tx_buf + data_acked,
                     c->tx_len - data_acked);
        }
        c->tx_len -= data_acked;
    }
    c->snd_una = ack;

    /* Did this ack cover our FIN? */
    if (c->fin_sent && c->snd_una == c->snd_nxt) c->fin_acked = 1;
}

void tcp_handle(const struct ipv4_hdr *ip,
                const uint8_t *payload, uint32_t plen)
{
    if (plen < TCP_HDR_MIN_LEN) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)payload;
    uint32_t hdr_len = (uint32_t)(h->data_off >> 4) * 4u;
    if (hdr_len < TCP_HDR_MIN_LEN || hdr_len > plen) return;

    /* Validate checksum (mandatory for TCP, unlike UDP).  Use a
     * mutable copy so we can zero the checksum field. */
    {
        uint8_t buf[1500];
        if (plen > sizeof(buf)) return;
        t_memcpy(buf, payload, plen);
        struct tcp_hdr *tmp = (struct tcp_hdr *)buf;
        uint16_t sent = tmp->checksum;
        tmp->checksum = 0;
        uint16_t want = tcp_compute_checksum(ip->src, ip->dst, buf, plen);
        if (sent != want) return;
    }

    uint16_t src_port = net_be16_to_cpu(h->src_port);
    uint16_t dst_port = net_be16_to_cpu(h->dst_port);
    int cid = find_conn_for_pkt(ip->src, dst_port, src_port);
    if (cid < 0) {
        /* Unmatched: in a full impl we'd send a RST; we just drop. */
        return;
    }
    struct tcp_conn *c = &g_conns[cid];

    /* Decode seq/ack/window/flags. */
    const uint8_t *p = (const uint8_t *)&h->seq;
    uint32_t seg_seq = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    p = (const uint8_t *)&h->ack;
    uint32_t seg_ack = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    uint16_t seg_wnd = net_be16_to_cpu(h->window);
    uint8_t  flags   = h->flags;

    /* RST: blow the connection away. */
    if (flags & TCP_FLAG_RST) {
        c->reset = 1;
        c->state = TCP_CLOSED;
        return;
    }

    /* SYN_SENT: expect SYN+ACK. */
    if (c->state == TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) !=
            (TCP_FLAG_SYN | TCP_FLAG_ACK)) return;
        if (seg_ack != c->snd_iss + 1) return;
        c->rcv_irs = seg_seq;
        c->rcv_nxt = seg_seq + 1;     /* peer's SYN consumes 1 */
        c->snd_una = seg_ack;
        c->snd_wnd = seg_wnd ? seg_wnd : TCP_BUF_SIZE;
        c->state   = TCP_ESTABLISHED;
        tcp_send_ack(c);
        return;
    }

    /* For all later states we expect ACK. */
    if (!(flags & TCP_FLAG_ACK)) return;

    /* Update peer window + apply incoming ack. */
    c->snd_wnd = seg_wnd ? seg_wnd : 1;
    apply_ack(c, seg_ack);

    /* Handle data payload (only if seg_seq matches rcv_nxt — no
     * out-of-order buffering in this milestone). */
    uint32_t data_len = plen - hdr_len;
    if (data_len && seg_seq == c->rcv_nxt) {
        uint32_t free_rx = TCP_BUF_SIZE - c->rx_len;
        uint32_t take    = data_len < free_rx ? data_len : free_rx;
        if (take) {
            t_memcpy(c->rx_buf + c->rx_len, payload + hdr_len, take);
            c->rx_len  += take;
            c->rcv_nxt += take;
        }
    }

    /* Handle FIN. */
    if (flags & TCP_FLAG_FIN) {
        /* FIN consumes one sequence number; only honour if it's
         * positioned at rcv_nxt (after any in-order data we just
         * accepted above). */
        uint32_t fin_seq = seg_seq + data_len;
        if (fin_seq == c->rcv_nxt) {
            c->rcv_nxt += 1;
            c->peer_fin = 1;
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
            } else if (c->state == TCP_FIN_WAIT_2) {
                c->state = TCP_TIME_WAIT;   /* collapsed to CLOSED below */
            } else if (c->state == TCP_FIN_WAIT_1) {
                /* Simultaneous close.  If our FIN was acked in the
                 * same packet, jump to TIME_WAIT; otherwise CLOSING. */
                if (c->fin_acked) c->state = TCP_TIME_WAIT;
                else              c->state = TCP_LAST_ACK;
            }
        }
    }

    /* Send ACK if data or FIN consumed sequence space. */
    if (data_len || (flags & TCP_FLAG_FIN)) {
        tcp_send_ack(c);
    }

    /* State transitions driven by acks. */
    if (c->state == TCP_FIN_WAIT_1 && c->fin_acked) {
        c->state = TCP_FIN_WAIT_2;
    }
    if (c->state == TCP_LAST_ACK && c->fin_acked) {
        c->state = TCP_CLOSED;
    }
    if (c->state == TCP_TIME_WAIT) {
        /* Skip TIME_WAIT entirely.  We don't reuse 4-tuples. */
        c->state = TCP_CLOSED;
    }

    /* Drain any newly-sendable data. */
    tcp_drain_tx(c);
}

void tcp_poll(void)
{
    g_poll_counter++;
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid) continue;

        /* Reap conns the user has closed AND that have finished
         * their TCP shutdown.  Without this, a normal active close
         * (ESTABLISHED -> FIN_WAIT_1 -> FIN_WAIT_2 -> TIME_WAIT ->
         * CLOSED) would pin the slot forever, and after
         * TCP_CONN_CAP back-to-back fetches socket_connect would
         * fail with -EMFILE — exactly the symptom hit by the
         * browser when navigating between many pages. */
        if (c->user_closed && c->state == TCP_CLOSED) {
            release_conn(i);
            continue;
        }
        if (c->state == TCP_CLOSED) continue;

        /* Retransmit if anything's been unacked too long. */
        if (c->snd_nxt != c->snd_una &&
            (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
            /* Walk back snd_nxt to snd_una and re-drain. */
            c->snd_nxt = c->snd_una;
            tcp_drain_tx(c);
            c->last_tx_poll = g_poll_counter;
        }

        /* Retransmit SYN if SYN_SENT has been silent too long. */
        if (c->state == TCP_SYN_SENT &&
            (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
            tcp_tx(c, TCP_FLAG_SYN, c->snd_iss, 0,
                   (const uint8_t *)0, 0, /*with_mss_option*/1);
            c->last_tx_poll = g_poll_counter;
        }
    }
}

/* See note in udp.c: keep g_conns in BSS without an implicit
 * libc memset call. */
__attribute__((used))
static void tcp_static_init(void)
{
    t_memset(g_conns, 0, sizeof(g_conns));
}
