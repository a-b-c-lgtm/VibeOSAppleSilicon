/*
 * kernel/core/tcp.h — milestone-55 TCPv4 (RFC 793, with the
 * usual modern simplifications).
 *
 * Client-only for this milestone: we can `connect()` to a
 * remote TCP endpoint, `send()` and `recv()` arbitrary bytes,
 * and `close()` cleanly.  Server-side LISTEN/ACCEPT is
 * milestone 56.
 *
 * Connection table is fixed-size (4 entries), one per active
 * 4-tuple `(g_ip, local_port, remote_ip, remote_port)`.  Each
 * connection owns 4 KiB of TX buffer + 4 KiB of RX buffer.
 *
 * What's missing on purpose:
 *   - LISTEN / ACCEPT (M56)
 *   - SACK, window scaling, timestamp options
 *   - Nagle (we always push), delayed ACK (we always ACK)
 *   - PMTU discovery (we use a single 1460-byte MSS)
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
    TCP_CLOSED      = 0,
    TCP_SYN_SENT    = 1,
    TCP_ESTABLISHED = 2,
    TCP_FIN_WAIT_1  = 3,
    TCP_FIN_WAIT_2  = 4,
    TCP_CLOSE_WAIT  = 5,
    TCP_LAST_ACK    = 6,
    TCP_TIME_WAIT   = 7,
};

#define TCP_INVALID_CID  (-1)

/* Open a connection to `dst_ip:dst_port`.  Returns a connection
 * ID >= 0, or -1 if the connection table is full.  This is the
 * "active open" half of the three-way handshake; the SYN is sent
 * before this returns, but the connection is in TCP_SYN_SENT
 * state and the caller must poll `tcp_state(cid)` until it
 * reaches TCP_ESTABLISHED before sending data. */
int tcp_connect(const uint8_t dst_ip[NET_IPV4_LEN], uint16_t dst_port);

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

/* Drain RX queue + run retransmission timers.  Called from
 * `net_poll` (which itself is called from the boot loop and
 * from any spin-poll sites). */
void tcp_poll(void);

/* RX hook called by net.c when an inbound IPv4/TCP packet
 * arrives.  Not for general use. */
void tcp_handle(const struct ipv4_hdr *ip,
                const uint8_t *payload, uint32_t plen);

#endif /* KERNEL_CORE_TCP_H */
