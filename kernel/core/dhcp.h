/*
 * kernel/core/dhcp.h \u2014 milestone-54 DHCPv4 client.
 *
 * One function: `dhcp_acquire()` runs a synchronous DISCOVER /
 * OFFER / REQUEST / ACK exchange against whichever DHCP server
 * is reachable on the local segment (QEMU SLIRP runs one
 * built-in).  On success it has installed the lease into the
 * net stack via `net_set_ipv4_config()` and returns 0.  On
 * timeout it returns -1 and the caller should fall back to a
 * static config.
 */
#ifndef KERNEL_CORE_DHCP_H
#define KERNEL_CORE_DHCP_H

#include <stdint.h>

/* Spin budget is in the same units as net_arp_resolve's: each
 * iteration is one tight loop pass, periodically polling the
 * RX queue.  ~5e7 corresponds to roughly half a second on M2/HVF.
 * Pass a budget large enough to cover DISCOVER \u2192 OFFER \u2192
 * REQUEST \u2192 ACK; ~2e8 is comfortable. */
int dhcp_acquire(uint64_t spin_iters);

#endif /* KERNEL_CORE_DHCP_H */
