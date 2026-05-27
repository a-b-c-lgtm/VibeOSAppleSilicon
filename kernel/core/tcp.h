/*
 * kernel/core/tcp.h -- TCPv4 (RFC 793, with the
 * usual modern simplifications).  Chapter 105 added passive-
 * open: tcp_listen() / tcp_accept() and the LISTEN /
 * SYN_RECEIVED states.
 *
 * Client side: `tcp_connect()` to a remote endpoint, `tcp_send` /
 * `tcp_recv` arbitrary bytes, `tcp_close` cleanly.
 *
 * Server side (chapter 105): `tcp_listen(port)` returns a
 * "listener" cid in state TCP_LISTEN.  When a SYN arrives at
 * that port the kernel allocates a child slot in TCP_SYN_RECEIVED,
 * sends SYN+ACK, and -- once the peer's final ACK lands --
 * promotes the child to TCP_ESTABLISHED and pushes its cid onto
 * the listener's accept queue.  `tcp_accept(listen_cid)` pops the
 * head of that queue.
 *
 * Connection table is fixed-size (TCP_CONN_CAP entries).  Both
 * listeners and connected (or half-open) conns share the same
 * pool, so a listener counts as 1 slot, every pending SYN_RECEIVED
 * counts as 1 slot, and every accepted ESTABLISHED conn counts as
 * 1 slot.  At TCP_CONN_CAP = 16 this is generous for a single-user
 * kernel.
 *
 * What's missing on purpose:
 *   - SACK, window scaling, timestamp options
 *   - Nagle (we always push), delayed ACK (we always ACK)
 *   - PMTU discovery (we use a single 1460-byte MSS)
 *   - SYN cookies (we drop new SYNs when the accept backlog is
 *     full -- see chapter 105's discussion)
 *   - Sophisticated RTO; we retransmit on a coarse poll budget.
 */
#ifndef KERNEL_CORE_TCP_H
#define KERNEL_CORE_TCP_H

#include <stdint.h>
#include "net.h"

/* ---- wire format ---------------------------------------------- */

struct __attribute__((packed)) tcp_hdr {
    uint16_t src_port;       /* BE */
    uint16_t dst_port;       /* BE */
    uint32_t seq;            /* BE */
    uint32_t ack;            /* BE */
    uint8_t  data_off;       /* upper 4 bits = header words; lower 4 reserved */
    uint8_t  flags;          /* CWR ECE URG ACK PSH RST SYN FIN (high to low) */
    uint16_t window;         /* BE */
    uint16_t checksum;       /* BE; pseudo-header + tcp_hdr + data */
    uint16_t urgent;         /* BE; we never set this */
    /* options follow if data_off > 5 */
};
#define TCP_HDR_MIN_LEN     20u

#define TCP_FLAG_FIN  0x01u
#define TCP_FLAG_SYN  0x02u
#define TCP_FLAG_RST  0x04u
#define TCP_FLAG_PSH  0x08u
#define TCP_FLAG_ACK  0x10u
#define TCP_FLAG_URG  0x20u

/* TCP option codes we use. */
#define TCP_OPT_END    0u
#define TCP_OPT_NOP    1u
#define TCP_OPT_MSS    2u    /* len=4, value=u16 BE */

/* ---- public API ---------------------------------------------- */

enum tcp_state {
    TCP_CLOSED       = 0,
    TCP_LISTEN       = 1,   /* chapter 105: passive open, waiting for SYN */
    TCP_SYN_SENT     = 2,
    TCP_SYN_RECEIVED = 3,   /* chapter 105: SYN+ACK sent, waiting for ACK */
    TCP_ESTABLISHED  = 4,
    TCP_FIN_WAIT_1   = 5,
    TCP_FIN_WAIT_2   = 6,
    TCP_CLOSE_WAIT   = 7,
    TCP_LAST_ACK     = 8,
    TCP_TIME_WAIT    = 9,
};

#define TCP_INVALID_CID  (-1)

/* Open a connection to `dst_ip:dst_port`.  Returns a connection
 * ID >= 0, or -1 if the connection table is full.  This is the
 * "active open" half of the three-way handshake; the SYN is sent
 * before this returns, but the connection is in TCP_SYN_SENT
 * state and the caller must poll `tcp_state(cid)` until it
 * reaches TCP_ESTABLISHED before sending data. */
int tcp_connect(const uint8_t dst_ip[NET_IPV4_LEN], uint16_t dst_port);

/* Chapter 105 -- passive open.
 *
 * Allocate a listener slot on `local_port` and return its cid.
 * The slot transitions to TCP_LISTEN immediately and starts
 * accepting inbound SYNs from any peer.  Returns:
 *   >= 0  the new listener cid
 *   -1    the connection table is full
 *   -2    another conn (listener or established) is already
 *         using local_port
 *
 * Listeners count as one slot in the TCP_CONN_CAP pool.  Every
 * pending half-open (TCP_SYN_RECEIVED) also counts as one slot,
 * as does every accepted ESTABLISHED conn after `tcp_accept`
 * returns it. */
int tcp_listen(uint16_t local_port);

/* Chapter 105 -- accept the next fully-established inbound conn.
 *
 * `listen_cid` must be a cid returned by `tcp_listen`.  Returns:
 *   >= 0  the cid of a fresh TCP_ESTABLISHED conn, popped from
 *         the listener's accept queue.  The caller owns it like
 *         any other conn: use tcp_recv / tcp_send / tcp_close.
 *   -1    `listen_cid` isn't a valid listener
 *   -2    accept queue is empty (poll and try again, or yield)
 *
 * Non-blocking: returns -2 immediately when the queue is empty.
 * Pair with `net_poll()` to drive the state machine forward. */
int tcp_accept(int listen_cid);

/* Append `len` bytes to the connection's send buffer.  Returns
 * the number of bytes queued (may be < `len` if the TX buffer
 * is full).  Calls into the segmenter on its way out so the
 * caller doesn't have to think about segments. */
int tcp_send(int cid, const void *data, uint32_t len);

/* Pull up to `cap` bytes out of the connection's RX buffer.
 * Returns the number of bytes copied.  Returns 0 if the buffer
 * is empty AND the peer has closed (EOF); returns 0 if simply
 * empty (caller should poll again).  Returns -1 if the
 * connection is invalid or has been reset. */
int tcp_recv(int cid, void *buf, uint32_t cap);

/* Initiate close (sends FIN once the TX buffer drains).  After
 * this, `tcp_send` returns -1 but `tcp_recv` continues to drain
 * any data the peer is still sending.  Returns 0 on success,
 * -1 on bad cid. */
int tcp_close(int cid);

/* Returns the connection state, or TCP_CLOSED for an invalid /
 * released cid. */
int tcp_state(int cid);

/* Convenience: 1 if the peer has fully closed (FIN received) AND
 * the local RX buffer is empty.  The standard "connection EOF"
 * test for a HTTP-style request/response. */
int tcp_eof(int cid);

/* Chapter 106 -- read out the peer's 4-tuple coordinates from a
 * connected conn.  Used by SYS_SOCKET_ACCEPT to surface "who
 * connected to me" to userspace.  Either output pointer may be
 * NULL.  `peer_ip_out` receives a packed big-endian IPv4 (same
 * format SYS_SOCKET_CONNECT accepts).  Returns 0 on success,
 * -1 on a bad / non-established cid. */
int tcp_peer(int cid, uint32_t *peer_ip_be_out, uint16_t *peer_port_out);

/* Drain RX queue + run retransmission timers.  Called from
 * `net_poll` (which itself is called from the boot loop and
 * from any spin-poll sites). */
void tcp_poll(void);

/* RX hook called by net.c when an inbound IPv4/TCP packet
 * arrives.  Not for general use. */
void tcp_handle(const struct ipv4_hdr *ip,
                const uint8_t *payload, uint32_t plen);

#endif /* KERNEL_CORE_TCP_H */
