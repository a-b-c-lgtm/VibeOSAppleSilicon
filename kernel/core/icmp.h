/*
 * kernel/core/icmp.h \u2014 ICMPv4 (RFC 792).
 *
 * The kernel only consumes one ICMP message type for now: echo
 * request (type 8).  Inbound echo requests are answered
 * automatically with an echo reply (type 0); no API is required
 * to enable this \u2014 just having the file compiled in is enough.
 *
 * The handler is invoked from the IPv4 RX dispatcher in
 * `kernel/core/net.c` once per inbound IPv4 packet whose proto
 * field is IPV4_PROTO_ICMP and whose destination is us.
 */
#ifndef KERNEL_CORE_ICMP_H
#define KERNEL_CORE_ICMP_H

#include <stdint.h>
#include "net.h"

#define ICMP_TYPE_ECHO_REPLY    0u
#define ICMP_TYPE_ECHO_REQUEST  8u

/* Minimum ICMP header (echo header is the same shape: type, code,
 * checksum, id, sequence, then 0..N bytes of opaque "data" the
 * sender expects echoed back verbatim). */
struct __attribute__((packed)) icmp_echo_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};
#define ICMP_ECHO_HDR_LEN  8u

void icmp_handle(const struct ipv4_hdr *ip,
                 const uint8_t *payload, uint32_t plen);

/* ----------------------------------------------------------------
 * Outbound echo (ping client).  Used at boot to prove the full
 * stack works end-to-end against the SLIRP gateway, and may
 * eventually back a userspace `ping` once we have an
 * ICMP-shaped syscall.
 * ---------------------------------------------------------------- */

/* Build and transmit one ICMP echo request to `dst_ip`.  `id`
 * and `seq` go straight into the corresponding header fields
 * (host order in, BE on the wire).  No payload other than the
 * 8-byte echo header.  Returns 0 on success, -1 on failure. */
int  icmp_send_echo(const uint8_t dst_ip[NET_IPV4_LEN],
                    uint16_t id, uint16_t seq);

/* Install a callback fired once per inbound echo REPLY (type 0).
 * Echo REQUESTS are still answered automatically regardless of
 * whether a callback is installed.  Pass NULL to clear. */
typedef void (*icmp_echo_reply_cb)(const uint8_t src_ip[NET_IPV4_LEN],
                                   uint16_t id, uint16_t seq);
void icmp_set_echo_reply_callback(icmp_echo_reply_cb cb);

#endif /* KERNEL_CORE_ICMP_H */
