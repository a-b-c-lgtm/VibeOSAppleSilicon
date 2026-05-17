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

/* Chapter 103: per-listener accept queue depth.  A listener can
 * have up to this many fully-handshaked-but-not-yet-accepted
 * children queued.  Once the queue is full the next SYN_RECEIVED
 * -> ESTABLISHED promotion is dropped (the child is RSTed and
 * its slot recycled).
 *
 * Conservative cap: the whole pool is TCP_CONN_CAP = 16 slots
 * shared between listeners, half-opens, and accepted conns, so
 * picking 8 leaves headroom for a couple of listeners plus the
 * outbound client conns the rest of the kernel uses (DNS,
 * httpget, etc). */
#define TCP_ACCEPT_QCAP 8

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
    uint8_t  user_closed;      /* sys_close has been called on the user fd --
                                * once state reaches TCP_CLOSED the slot is
                                * safe to recycle.  Without this flag we'd
                                * pin the slot forever after a normal active
                                * close (FIN_WAIT_1 -> ... -> CLOSED transitions
                                * happen asynchronously inside tcp_handle, but
                                * release_conn was only ever called from the
                                * synchronous close-while-already-CLOSED path). */

    /* Retransmission. */
    uint64_t last_tx_poll;     /* tcp_poll counter at last data TX */

    /* Chapter 103 -- passive open.
     *
     * For a listener (state == TCP_LISTEN): `accept_q[0..accept_q_n)`
     * holds cids of fully-ESTABLISHED children waiting to be
     * harvested by tcp_accept().  remote_ip / remote_port are
     * zero so find_conn_for_pkt naturally never matches us.
     *
     * For a child in TCP_SYN_RECEIVED (and later TCP_ESTABLISHED
     * until accept harvests it): `parent_listen_cid` points back
     * to the listener whose queue we should join when the third
     * handshake message lands.  -1 means "not a listener's child"
     * (the value alloc_conn() sets for fresh slots).
     *
     * We default to -1 (rather than 0, which is a valid cid)
     * specifically so a child whose parent_listen_cid was never
     * set doesn't accidentally point at slot 0. */
    int      accept_q[TCP_ACCEPT_QCAP];
    uint8_t  accept_q_n;
    int      parent_listen_cid;

    /* Chapter 106b diag: per-conn counters to chase the 8 MiB
     * amplification on a 38 KiB response.  Both reset by
     * alloc_conn's t_memset. */
    uint32_t dbg_rx_total;      /* bytes accepted into rx_buf this conn */
    uint32_t dbg_rx_next_log;   /* next 1 MiB threshold to print at */
    uint32_t dbg_rx_rejected;   /* data segments rejected by seq mismatch */
    /* Chapter 106b: how many times this conn's RTO-rewind fired.
     * If non-zero on a successful fetch it is a strong signal
     * that TCP_RTX_THRESH (poll-iteration counter, not wall ms)
     * tripped under the in-desktop cooperative-yield load.
     * Reset by alloc_conn's t_memset. */
    uint32_t dbg_rtx_fired;
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
            /* Chapter 103: default to "not a listener's child".
             * Cid 0 is a valid cid, so a literal 0 here would
             * misroute brand-new active-open conns to the slot-0
             * listener (if any) when we count children. */
            g_conns[i].parent_listen_cid = -1;
            /* Chapter 106b diag: first 1 MiB threshold. */
            g_conns[i].dbg_rx_next_log   = 1024u * 1024u;
            return i;
        }
    }
    return -1;
}

static void release_conn(int cid)
{
    if (cid < 0 || cid >= TCP_CONN_CAP) return;
    /* Chapter 106b diag: print the 4-tuple of the conn being
     * released, plus a small set of state counters.  Useful
     * for catching slot reuse and "who hung up first" bugs. */
    struct tcp_conn *c = &g_conns[cid];
    if (c->valid) {
        serial_puts("[tcp] release cid=");
        serial_puthex((uint64_t)cid);
        serial_puts(" state=");
        serial_puthex((uint64_t)c->state);
        serial_puts(" lport=");
        serial_puthex((uint64_t)c->local_port);
        serial_puts(" rport=");
        serial_puthex((uint64_t)c->remote_port);
        serial_puts(" rip=");
        serial_puthex(((uint64_t)c->remote_ip[0]<<24) |
                      ((uint64_t)c->remote_ip[1]<<16) |
                      ((uint64_t)c->remote_ip[2]<<8)  |
                       (uint64_t)c->remote_ip[3]);
        serial_puts(" rx_total=");
        serial_puthex((uint64_t)c->dbg_rx_total);
        serial_puts(" rtx=");
        serial_puthex((uint64_t)c->dbg_rtx_fired);
        serial_puts("\n");
    }
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

/* Chapter 103: scan for a listener slot bound to `local_port`.
 * Used by tcp_handle when an inbound packet has no exact 4-tuple
 * match -- maybe it's the first SYN of a new connection to one
 * of our listeners.  Returns the listener's cid, or -1. */
static int find_listener_for_port(uint16_t local_port)
{
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid) continue;
        if (c->state != TCP_LISTEN) continue;
        if (c->local_port != local_port) continue;
        return i;
    }
    return -1;
}

/* Chapter 103: count children (TCP_SYN_RECEIVED + queued
 * accepted) attached to a given listener.  Used to enforce the
 * backlog cap on incoming SYNs without having to walk the queue
 * itself (the queue only holds promoted ESTABLISHED children;
 * SYN_RECEIVED slots are accounted for here too). */
static int count_listener_children(int listen_cid)
{
    if (listen_cid < 0) return 0;
    int n = 0;
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid) continue;
        if (c->parent_listen_cid != listen_cid) continue;
        n++;
    }
    return n;
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

    /* Compute checksum.
     *
     * Source-address selection is loopback-aware: when the peer
     * is one of our own addresses, net_choose_src returns the
     * peer's address (so both halves of the conversation observe
     * a symmetric 4-tuple).  See chapter 106 for the full story.
     * The same `our_ip` MUST be used for the pseudo-header here
     * and for the IPv4 header in net_ipv4_send_from below -- a
     * mismatch would make the receiver drop the segment as
     * corrupt. */
    uint8_t our_ip[NET_IPV4_LEN];
    net_choose_src(c->remote_ip, our_ip);
    uint16_t cks = tcp_compute_checksum(our_ip, c->remote_ip,
                                        pkt, hdr_len + data_len);
    h->checksum = cks;

    return net_ipv4_send_from(our_ip, c->remote_ip, IPV4_PROTO_TCP,
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

    /* Chapter 106b diag: log every active open with its 4-tuple
     * so the [tcp] rx_total lines can be cross-referenced to
     * "who connected to whom".  Helps disambiguate which slot
     * is the browser-to-httpd path vs httpd-to-host. */
    serial_puts("[tcp] connect cid=");
    serial_puthex((uint64_t)cid);
    serial_puts(" lport=");
    serial_puthex((uint64_t)c->local_port);
    serial_puts(" -> ");
    serial_puthex(((uint64_t)dst_ip[0]<<24) | ((uint64_t)dst_ip[1]<<16) |
                  ((uint64_t)dst_ip[2]<<8)  |  (uint64_t)dst_ip[3]);
    serial_puts(":");
    serial_puthex((uint64_t)dst_port);
    serial_puts("\n");

    /* Send SYN with MSS option. */
    if (tcp_tx(c, TCP_FLAG_SYN, c->snd_iss, 0,
               (const uint8_t *)0, 0, /*with_mss_option*/1) < 0) {
        release_conn(cid);
        return -1;
    }
    c->snd_nxt = c->snd_iss + 1;       /* SYN consumes 1 sequence number */
    return cid;
}

/* ----------------------------------------------------------------
 * Chapter 103 -- passive open
 * ---------------------------------------------------------------- */

int tcp_listen(uint16_t local_port)
{
    /* Refuse port 0 (illegal in TCP) and refuse duplicates --
     * we don't support two listeners on the same port and the
     * dispatch in tcp_handle would only ever pick one of them
     * anyway (whichever sits first in the table). */
    if (local_port == 0) return -2;
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid) continue;
        if (c->state == TCP_LISTEN && c->local_port == local_port)
            return -2;
    }

    int cid = alloc_conn();
    if (cid < 0) return -1;
    struct tcp_conn *c = &g_conns[cid];

    /* remote_ip / remote_port stay zeroed.  find_conn_for_pkt
     * filters us out naturally (real inbound packets have
     * remote_port > 0).  alloc_conn() already set
     * parent_listen_cid = -1 and accept_q_n = 0. */
    c->local_port = local_port;
    c->state      = TCP_LISTEN;
    return cid;
}

int tcp_accept(int listen_cid)
{
    struct tcp_conn *l = get_conn(listen_cid);
    if (!l || l->state != TCP_LISTEN) return -1;
    if (l->accept_q_n == 0) return -2;     /* nothing ready -- EAGAIN */

    /* Pop the head of the FIFO queue. */
    int cid = l->accept_q[0];
    for (int i = 1; i < l->accept_q_n; i++) {
        l->accept_q[i - 1] = l->accept_q[i];
    }
    l->accept_q_n--;

    /* The child is now "owned" by the caller -- detach it from
     * the listener so closing the listener later doesn't
     * accidentally touch it (and so count_listener_children
     * doesn't keep counting it against the backlog cap). */
    struct tcp_conn *c = get_conn(cid);
    if (c) c->parent_listen_cid = -1;
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
     * uncaps end-to-end throughput.
     *
     * Chapter 106b — we also need to fire in TCP_FIN_WAIT_1 and
     * TCP_FIN_WAIT_2.  serve_forward's splice issues
     * socket_shutdown(up) RIGHT after sending the request body
     * (so upstream knows we're done and can start streaming the
     * response).  That puts the upstream connection into
     * FIN_WAIT_1 / FIN_WAIT_2 for the ENTIRE response stream.
     * Without this gate widening, the window-zero probe ping-
     * pong above kicks in on every shutdown-then-read pattern,
     * producing a ~10 s plateau on any response > rx_buf
     * (32 KiB) — which is most real-world HTML pages.  The
     * receive path in those states is identical to ESTABLISHED;
     * we just happen to have already promised "we're done
     * sending."  Peer is still sending normally. */
    int receive_active = (c->state == TCP_ESTABLISHED ||
                          c->state == TCP_FIN_WAIT_1  ||
                          c->state == TCP_FIN_WAIT_2);
    if (receive_active && take >= TCP_MSS) {
        tcp_send_ack(c);
    } else if (receive_active &&
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

    /* Chapter 103: closing a listener releases it immediately.
     * Any children already promoted to TCP_ESTABLISHED and
     * waiting in the accept queue have to be flushed first --
     * leaking them would leak conn-table slots.  Half-open
     * children (TCP_SYN_RECEIVED still in flight) are simply
     * orphaned; their parent_listen_cid is now stale, and when
     * their handshake completes the SYN_RECEIVED -> ESTABLISHED
     * promotion will find no live listener and drop them.
     *
     * Children that were already accepted (parent_listen_cid
     * reset to -1 by tcp_accept) are untouched -- they belong
     * to the caller now. */
    if (c->state == TCP_LISTEN) {
        for (int i = 0; i < c->accept_q_n; i++) {
            int child = c->accept_q[i];
            if (child >= 0 && child < TCP_CONN_CAP &&
                g_conns[child].valid) {
                /* Best-effort RST so the peer notices.  Send
                 * BEFORE clearing the slot. */
                tcp_tx(&g_conns[child], TCP_FLAG_RST | TCP_FLAG_ACK,
                       g_conns[child].snd_nxt,
                       g_conns[child].rcv_nxt,
                       (const uint8_t *)0, 0, 0);
                release_conn(child);
            }
        }
        release_conn(cid);
        return 0;
    }

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

int tcp_peer(int cid, uint32_t *peer_ip_be_out, uint16_t *peer_port_out)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c) return -1;
    /* Listeners and freshly-allocated slots don't have a peer
     * yet; only conns that have seen the remote 4-tuple (i.e.
     * everything from SYN_RECEIVED onwards on the passive side,
     * and SYN_SENT onwards on the active side) carry a valid
     * remote_ip / remote_port. */
    if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) return -1;
    if (peer_ip_be_out) {
        /* Pack the 4 octets in network byte order to match the
         * BE-uint32 format SYS_SOCKET_CONNECT consumes. */
        *peer_ip_be_out = ((uint32_t)c->remote_ip[0] << 24) |
                          ((uint32_t)c->remote_ip[1] << 16) |
                          ((uint32_t)c->remote_ip[2] <<  8) |
                          ((uint32_t)c->remote_ip[3]      );
    }
    if (peer_port_out) *peer_port_out = c->remote_port;
    return 0;
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

    /* Was our FIN included in this ack?
     *
     * The FIN is in-flight iff in_flight > tx_len -- i.e. we've
     * advanced snd_nxt one seq past the end of buffered data.
     * Just testing `c->fin_sent` is WRONG: an RTO rewind can
     * have walked snd_nxt back to snd_una and re-sent data only
     * (the FIN gate below is gated on !fin_sent, so the FIN is
     * NOT in fact re-emitted by the retransmit).  In that state
     * fin_sent=1 but the FIN no longer occupies any seq number,
     * and subtracting 1 leaves one trailing data byte stranded
     * in tx_buf permanently -- which drain_tx then ships at a
     * fresh seq number every poll, amplifying a 38 KiB response
     * into megabytes on the peer.  See chapter 106b. */
    uint32_t data_in_flight = in_flight;
    if (c->fin_sent && in_flight > c->tx_len) data_in_flight -= 1;
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

    /* Decode seq/ack/window/flags up front -- we need them both
     * for the LISTEN fast-path and for the existing 4-tuple
     * path. */
    const uint8_t *p = (const uint8_t *)&h->seq;
    uint32_t seg_seq = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    p = (const uint8_t *)&h->ack;
    uint32_t seg_ack = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    uint16_t seg_wnd = net_be16_to_cpu(h->window);
    uint8_t  flags   = h->flags;

    int cid = find_conn_for_pkt(ip->src, dst_port, src_port);
    if (cid < 0) {
        /* Chapter 103: no exact 4-tuple match -- maybe this is
         * the opening SYN of a brand-new connection to one of
         * our listeners.  Look for a TCP_LISTEN slot bound to
         * dst_port; if found and the packet is a pure SYN
         * (SYN set, ACK clear, RST clear), allocate a child
         * conn in TCP_SYN_RECEIVED and reply with SYN+ACK. */
        if (flags & TCP_FLAG_RST) return;
        int lid = find_listener_for_port(dst_port);
        if (lid < 0) return;
        if ((flags & TCP_FLAG_SYN) == 0)  return;
        if (flags & TCP_FLAG_ACK)         return;

        /* Backlog gate: cap the number of half-open + queued
         * children per listener.  Once full, drop the new SYN
         * silently -- the peer will retry, and by then we may
         * have room. */
        if (count_listener_children(lid) >= TCP_ACCEPT_QCAP) return;

        int child = alloc_conn();
        if (child < 0) return;        /* pool exhausted */
        struct tcp_conn *cc = &g_conns[child];
        t_memcpy(cc->remote_ip, ip->src, NET_IPV4_LEN);
        cc->local_port        = dst_port;
        cc->remote_port       = src_port;
        cc->snd_iss           = fresh_isn();
        cc->snd_una           = cc->snd_iss;
        cc->snd_nxt           = cc->snd_iss;    /* SYN consumes 1 below */
        cc->rcv_irs           = seg_seq;
        cc->rcv_nxt           = seg_seq + 1;    /* peer's SYN consumes 1 */
        cc->snd_wnd           = seg_wnd ? seg_wnd : TCP_BUF_SIZE;
        cc->state             = TCP_SYN_RECEIVED;
        cc->parent_listen_cid = lid;
        cc->last_tx_poll      = g_poll_counter;

        if (tcp_tx(cc, TCP_FLAG_SYN | TCP_FLAG_ACK,
                   cc->snd_iss, cc->rcv_nxt,
                   (const uint8_t *)0, 0, /*with_mss*/1) < 0) {
            release_conn(child);
            return;
        }
        cc->snd_nxt = cc->snd_iss + 1;          /* our SYN consumes 1 */
        return;
    }
    struct tcp_conn *c = &g_conns[cid];

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

    /* Chapter 103 -- SYN_RECEIVED: the peer's final handshake
     * ACK has arrived.  Promote the child to ESTABLISHED and
     * push it onto its parent listener's accept queue.
     *
     * Why handle this before the generic apply_ack: in
     * SYN_RECEIVED our snd_nxt - snd_una == 1 (the SYN we
     * sent), but tx_buf is empty.  Letting apply_ack run would
     * underflow tx_len when it tries to slide data we never
     * buffered.  Easier to special-case the promotion here. */
    if (c->state == TCP_SYN_RECEIVED) {
        if (seg_ack != c->snd_iss + 1) return;   /* not our ACK */
        c->snd_una = seg_ack;
        c->snd_wnd = seg_wnd ? seg_wnd : TCP_BUF_SIZE;
        c->state   = TCP_ESTABLISHED;

        struct tcp_conn *l = get_conn(c->parent_listen_cid);
        if (l && l->state == TCP_LISTEN &&
            l->accept_q_n < TCP_ACCEPT_QCAP) {
            l->accept_q[l->accept_q_n++] = cid;
        } else {
            /* Listener gone (closed) or queue full -- abandon
             * the brand-new conn.  Send RST so the peer doesn't
             * think it has an open connection. */
            tcp_tx(c, TCP_FLAG_RST | TCP_FLAG_ACK,
                   c->snd_nxt, c->rcv_nxt,
                   (const uint8_t *)0, 0, 0);
            release_conn(cid);
        }
        return;
    }

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
            /* Chapter 106b diag: log a serial line each time
             * a conn's accepted-data total crosses a 1 MiB
             * threshold.  If the browser claims to read 8 MiB
             * on a fetch where httpd wrote 38 KiB, this line
             * should fire ~8 times — and the cid in the log
             * tells us WHICH side is accepting the duplicates. */
            c->dbg_rx_total += take;
            if (c->dbg_rx_total >= c->dbg_rx_next_log) {
                serial_puts("[tcp] cid=");
                serial_puthex((uint64_t)(c - g_conns));
                serial_puts(" state=");
                serial_puthex((uint64_t)c->state);
                serial_puts(" rx_total=");
                serial_puthex((uint64_t)c->dbg_rx_total);
                serial_puts(" rcv_nxt=");
                serial_puthex((uint64_t)c->rcv_nxt);
                serial_puts(" lport=");
                serial_puthex((uint64_t)c->local_port);
                serial_puts(" rport=");
                serial_puthex((uint64_t)c->remote_port);
                serial_puts(" remote_ip=");
                serial_puthex(((uint64_t)c->remote_ip[0]<<24) |
                              ((uint64_t)c->remote_ip[1]<<16) |
                              ((uint64_t)c->remote_ip[2]<<8)  |
                               (uint64_t)c->remote_ip[3]);
                serial_puts(" data_len=");
                serial_puthex((uint64_t)data_len);
                serial_puts("\n");
                /* After first hit, log every 1 MiB. */
                if (c->dbg_rx_next_log < 1024u * 1024u) {
                    c->dbg_rx_next_log = 1024u * 1024u;
                } else {
                    c->dbg_rx_next_log += 1024u * 1024u;
                }
            }
        }
    } else if (data_len) {
        /* Diag: data segment didn't match rcv_nxt -- either a
         * duplicate retransmit (seg_seq < rcv_nxt) or out-of-
         * order future data (seg_seq > rcv_nxt).  Count and log
         * every 256th occurrence so a feedback loop is visible. */
        c->dbg_rx_rejected++;
        if ((c->dbg_rx_rejected & 0xFF) == 0) {
            serial_puts("[tcp] reject cid=");
            serial_puthex((uint64_t)(c - g_conns));
            serial_puts(" n=");
            serial_puthex((uint64_t)c->dbg_rx_rejected);
            serial_puts(" data_len=");
            serial_puthex((uint64_t)data_len);
            serial_puts(" seg_seq=");
            serial_puthex((uint64_t)seg_seq);
            serial_puts(" rcv_nxt=");
            serial_puthex((uint64_t)c->rcv_nxt);
            serial_puts("\n");
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
         * their TCP shutdown AND whose receive buffer has been
         * fully drained.  Without this, a normal active close
         * (ESTABLISHED -> FIN_WAIT_1 -> FIN_WAIT_2 -> TIME_WAIT ->
         * CLOSED) would pin the slot forever, and after
         * TCP_CONN_CAP back-to-back fetches socket_connect would
         * fail with -EMFILE -- exactly the symptom hit by the
         * browser when navigating between many pages.
         *
         * The `rx_len == 0` guard was added in chapter 106:
         * loopback delivers the peer's full echo plus FIN in a
         * single net_poll drain cycle, which means we hit
         * CLOSED before user code has had a chance to drain
         * the rx_buf via read().  Reaping eagerly would free the
         * cid and the next tcp_recv would return -1, surfacing
         * to userspace as -EIO instead of "EOF after these N
         * bytes."  Wait for the user to consume the buffered
         * data first; once they call read() and get 0 (peer FIN +
         * empty buffer), rx_len stays 0 and this branch fires
         * on the very next tcp_poll. */
        if (c->user_closed && c->state == TCP_CLOSED && c->rx_len == 0) {
            release_conn(i);
            continue;
        }
        if (c->state == TCP_CLOSED) continue;

        /* Retransmit if anything's been unacked too long. */
        if (c->snd_nxt != c->snd_una &&
            (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
            /* Walk back snd_nxt to snd_una and re-drain.
             *
             * Chapter 106b: if the FIN was in-flight (peer never
             * ACKed it), the rewind drops it -- tcp_drain_tx's
             * FIN gate is `!fin_sent`, so it WON'T re-emit.
             * Clear fin_sent so the gate re-fires after the data
             * loop drains tx_buf, ensuring the FIN is back on
             * the wire.  Without this the conn stalls in
             * FIN_WAIT_1 forever (FIN can't be acked if it was
             * never re-sent), and apply_ack's `-1 for FIN`
             * accounting starts under-counting data and strands
             * a trailing byte in tx_buf. */
            int fin_was_in_flight = c->fin_sent &&
                                    (c->snd_nxt - c->snd_una > c->tx_len);
            c->snd_nxt = c->snd_una;
            if (fin_was_in_flight) c->fin_sent = 0;
            tcp_drain_tx(c);
            c->last_tx_poll = g_poll_counter;
            c->dbg_rtx_fired++;
        }

        /* Retransmit SYN if SYN_SENT has been silent too long. */
        if (c->state == TCP_SYN_SENT &&
            (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
            tcp_tx(c, TCP_FLAG_SYN, c->snd_iss, 0,
                   (const uint8_t *)0, 0, /*with_mss_option*/1);
            c->last_tx_poll = g_poll_counter;
            c->dbg_rtx_fired++;
        }

        /* Chapter 103: retransmit SYN+ACK if SYN_RECEIVED is
         * idle too long.  The peer may have lost our SYN+ACK
         * and is waiting on its own SYN-retransmit timer, but
         * sending it again can't hurt -- if the peer already
         * promoted, our duplicate ack just rides through their
         * established path harmlessly. */
        if (c->state == TCP_SYN_RECEIVED &&
            (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
            tcp_tx(c, TCP_FLAG_SYN | TCP_FLAG_ACK,
                   c->snd_iss, c->rcv_nxt,
                   (const uint8_t *)0, 0, /*with_mss_option*/1);
            c->last_tx_poll = g_poll_counter;
            c->dbg_rtx_fired++;
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
