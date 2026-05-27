/*
 * kernel/core/dns.h — DNS resolver (RFC 1035, A records).
 *
 * Synchronous, blocking-via-spin-yield resolver. One outstanding
 * query at a time; the API is "give me a name, get an IPv4 address
 * back". No caching, no parallelism, no AAAA, no CNAME chasing
 * beyond what the recursive resolver already inlined into the
 * answer section.
 *
 * The resolver talks UDP/53 to the DNS server learned via DHCP
 * option 6 (typically SLIRP's 10.0.2.3 in our QEMU setup).
 */
#ifndef KERNEL_CORE_DNS_H
#define KERNEL_CORE_DNS_H

#include <stdint.h>

/* Resolve `name` to an IPv4 address.  Writes 4 bytes into
 * `out_ip` and returns 0 on success.
 *
 * Returns -1 on any failure (no DNS server configured, name too
 * long, query timed out, server returned RCODE != 0, no A record
 * in the answer section).  Caller can retry; we do not
 * automatically.
 *
 * `name` is a NUL-terminated ASCII hostname like "example.com".
 * Maximum 253 bytes (RFC 1035 § 2.3.4).  Bare IPv4 dotted-quad
 * strings ("10.0.2.2") are NOT accepted by this function — that
 * parsing happens in userspace, in the libc wrapper. */
int dns_resolve(const char *name, uint8_t out_ip[4]);

#endif /* KERNEL_CORE_DNS_H */
