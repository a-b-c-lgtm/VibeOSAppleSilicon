/*
 * kernel/core/udp.h \u2014 milestone-54 UDP/IPv4 (RFC 768).
 *
 * Stateless: TX is a single function call, RX is a single
 * port-keyed callback.  Eight RX bindings simultaneously, one
 * per (port, callback) pair \u2014 enough for DHCP (port 68) plus
 * a couple of user-installable services (DNS in M55, a tiny
 * TFTP-style protocol if we ever want one).
 *
 * No socket layer, no buffering, no in-flight retransmits.
 * Those things live in the BSD-shaped socket layer that
 * milestone 55 adds for TCP \u2014 they are deliberately NOT a
 * UDP concern.
 */
#ifndef KERNEL_CORE_UDP_H
#define KERNEL_CORE_UDP_H

#include <stdint.h>
#include "net.h"

struct __attribute__((packed)) udp_hdr {
    uint16_t src_port;     /* big-endian */
    uint16_t dst_port;     /* big-endian */
    uint16_t length;       /* big-endian; includes header        */
    uint16_t checksum;     /* big-endian; 0 = "not used" in IPv4 */
};
#define UDP_HDR_LEN 8u

/* RX callback signature.  Invoked once per inbound UDP datagram
 * whose destination port matches a registered binding.  All
 * pointers are valid only for the duration of the call. */
typedef void (*udp_rx_cb)(const uint8_t src_ip[NET_IPV4_LEN],
                          uint16_t src_port,
                          const uint8_t *payload,
                          uint32_t payload_len);

/* Bind / unbind a port to a callback.  Calling `udp_bind` with a
 * port already bound replaces the existing handler.  Calling
 * with `cb == NULL` releases the binding.  Returns 0 on success,
 * -1 if the table is full. */
int  udp_bind(uint16_t port, udp_rx_cb cb);

/* Send one UDP datagram.  `src_ip` defaults to our configured
 * local IP if NULL is passed (DHCP needs to specify 0.0.0.0
 * explicitly, which a NULL-coalesced default would obscure).
 *
 * Computes the proper UDP checksum over the IPv4 pseudo-header
 * + UDP header + payload.  Returns 0 on success, -1 on TX
 * failure / payload too large. */
int  udp_send(const uint8_t *src_ip,
              const uint8_t  dst_ip[NET_IPV4_LEN],
              uint16_t src_port, uint16_t dst_port,
              const void *payload, uint32_t payload_len);

/* Called by net.c's RX dispatcher; not for general use. */
void udp_handle(const struct ipv4_hdr *ip,
                const uint8_t *payload, uint32_t plen);

#endif /* KERNEL_CORE_UDP_H */
