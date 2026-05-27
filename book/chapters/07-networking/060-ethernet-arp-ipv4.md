# Chapter 60 — Ethernet, ARP, and IPv4

[Chapter 59](059-virtio-net.md) ended with a driver that could put
bytes on the wire and pull bytes off it. The proof was a
hand-rolled 42-byte ARP request inside `kernel/core/main.c`: we
built every byte by hand and the kernel had no idea what an
"Ethernet header" or an "ARP packet" was — they were just
positions in a buffer.

That's fine for one frame in a self-test. It is not fine for
anything else. This chapter promotes the kernel from
"transmits a fixed string of bytes once at boot" to
"understands what's on the wire". By the end:

- We have an Ethernet RX dispatcher that classifies inbound
  frames by EtherType and routes them to the right handler.
- We have a real ARP cache (8 entries), an ARP request
  builder, and an ARP responder — i.e. the kernel can also
  *answer* ARP queries from the host.
- We have an IPv4 header builder with the correct
  1's-complement Internet checksum (RFC 1071), an IPv4 RX
  validator, and a policy hook upper layers will install in
  the next chapter.
- Static IPv4 configuration: `10.0.2.15/24`, gateway
  `10.0.2.2`. DHCP is [chapter 61](061-icmp-udp-dhcp.md).

The new code lives in `kernel/core/net.{c,h}`. It is roughly
400 lines.

## Why this layer is its own file

`virtio_net.c` is a *driver*: bytes in, bytes out, no opinions
about what the bytes mean. `net.c` is a *stack*: it knows about
Ethernet, ARP, and IPv4. Keeping them separate matters for two
reasons we will care about later:

1. When [chapter 108](../13-tcp-server/108-tcp-loopback.md) adds a
   loopback pseudo-device, the
   stack does not need to grow a notion of "transport-specific"
   anything.
2. Tests for the protocol layer can be written without booting
   QEMU at all. (We don't write those tests in this milestone,
   but the structure now permits it.)

The handoff between the two is the existing
`virtio_net_set_rx_callback()` from chapter 59. `net_init()`
registers a single dispatcher; everything past that point is
pure protocol code with no virtio knowledge.

## Wire formats, briefly

Three structs live at the top of `net.h`. They are
`__attribute__((packed))` because the network does not respect
our compiler's alignment preferences and the layout has to
match the bytes on the wire exactly.

### Ethernet header (14 bytes)

```c
struct __attribute__((packed)) eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;     /* big-endian on the wire */
};
```

EtherType is the field that selects the upper-layer handler:
`0x0806` for ARP, `0x0800` for IPv4. It's stored big-endian,
which is the recurring theme of this whole layer.

### ARP packet (28 bytes)

```c
struct __attribute__((packed)) arp_pkt {
    uint16_t htype;          /* 1 = Ethernet                */
    uint16_t ptype;          /* 0x0800 = IPv4               */
    uint8_t  hlen;           /* 6                           */
    uint8_t  plen;           /* 4                           */
    uint16_t op;             /* 1 = request, 2 = reply      */
    uint8_t  sha[6];         /* sender hardware address     */
    uint8_t  spa[4];         /* sender protocol  address    */
    uint8_t  tha[6];         /* target hardware address     */
    uint8_t  tpa[4];         /* target protocol  address    */
};
```

A request says "who has *target IP*? Tell *sender IP*". A reply
says "*target IP* is at *sender MAC*" — note that the *reply*
puts the answer in the *sender* fields, not the target fields,
which is one of the things ARP is famous for getting wrong in
new implementations. `net.c` uses the wire field names directly
to keep the surprise minimal.

### IPv4 header (20 bytes, no options)

```c
struct __attribute__((packed)) ipv4_hdr {
    uint8_t  vihl;           /* 0x45  (version 4, IHL 5)      */
    uint8_t  tos;            /* DSCP/ECN — we send 0          */
    uint16_t total_len;      /* whole packet incl header (BE) */
    uint16_t id;             /* identification (BE)           */
    uint16_t frag;           /* flags + fragment offset (BE)  */
    uint8_t  ttl;
    uint8_t  proto;          /* IPV4_PROTO_*                  */
    uint16_t checksum;       /* 1's-complement, BE            */
    uint8_t  src[4];
    uint8_t  dst[4];
};
```

We never emit IP options, never fragment, and don't honour
DSCP/ECN. SLIRP doesn't care, and our upper-layer protocols
in chapter 61 don't either.

## Endianness

The kernel runs little-endian; the wire is big-endian. Two
inline helpers do the conversion:

```c
static inline uint16_t net_be16_to_cpu(uint16_t be) { ... }
static inline uint16_t net_cpu_to_be16(uint16_t v)  { ... }
```

They are the same byte swap; the names exist to read better at
the call site. We do not use `htons` / `ntohs` — those names are
ambiguous in a freestanding kernel that has no `<arpa/inet.h>`
to point at.

## The ARP cache

Eight entries, stored as a flat array, scanned linearly. The
data structure:

```c
struct arp_entry {
    int     valid;
    uint8_t ip [4];
    uint8_t mac[6];
};
static struct arp_entry g_arp[ARP_CACHE_CAP];
```

Lookup is `O(8)`, which is fine. Insert is also `O(8)` and goes
in three layers:

1. If there is already an entry for this IP, refresh it in
   place. (This is how the cache stays correct when a host's
   MAC changes — we always trust the most recent reply.)
2. Otherwise, take the first invalid slot.
3. Otherwise, evict slot 0.

That last step is "LRU minus the bookkeeping". For our scale —
gateway, DNS, and a handful of local peers — it is always the
right answer.

### Cache learning, not just request/reply

`rx_handle_arp()` calls `arp_insert(spa, sha)` for **every**
inbound ARP packet, request or reply, regardless of whether
the packet was addressed to us. This is standard practice
(RFC 826's "Probe" rule, refined in RFC 5227): every ARP
packet that arrives is a free assertion of *somebody*'s
binding, and caching it pre-emptively saves a round trip the
first time we want to talk to them.

### Cache stability across `net_init()`

`net_init()` zeroes the cache. We don't currently re-init at
runtime, but DHCP in the next chapter will, and stale bindings
across an IP change would silently break delivery for a few
seconds.

## ARP transmit and ARP responder

Two paths produce ARP frames:

```c
int net_arp_request(const uint8_t target_ip[4]);
```

builds a 42-byte broadcast frame (`ff:ff:ff:ff:ff:ff` Ethernet
destination, op=1) and hands it to `virtio_net_tx`.

The receive path also responds. When `rx_handle_arp()` sees a
request whose `tpa` is our IP, it builds a unicast reply
(op=2) addressed to the requester's MAC. This is how anyone
on the network — including QEMU's SLIRP gateway running its
own ARP — knows how to reach us.

## Synchronous resolution

Most callers don't want to think about the cache at all. The
helper

```c
int net_arp_resolve(const uint8_t ip[4], uint8_t out_mac[6],
                    uint64_t spin_iters);
```

does cache → request → spin-poll → cache, returning 1 if it
got an answer in time. It is the function `net_ipv4_send()`
calls before each transmit. The `spin_iters` budget gives
the caller a way to fail fast — a TCP retry path will choose
a small budget; a startup self-test will pick a generous one.

We spin instead of sleep because the boot-time ARP self-test
runs *before IRQs are enabled*. With IRQs masked, the timer
ISR doesn't fire, so any sleep based on `uptime_ms()` would
hang forever. The same trick was used by the virtio-net
self-test in chapter 59.

## The IPv4 checksum

The IPv4 header checksum is the canonical "1's-complement sum
of 16-bit big-endian words" defined in RFC 1071 — the same
algorithm we will reuse for ICMP, UDP, and TCP in the next
chapter.

The implementation is short enough to read in full:

```c
uint16_t net_ipv4_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len >= 2) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    uint16_t cksum = (uint16_t)~sum;
    return (uint16_t)((cksum >> 8) | (cksum << 8));   /* return BE */
}
```

Three things are worth pointing out:

1. We parse the bytes as big-endian directly (`p[0] << 8 | p[1]`)
   instead of dereferencing as `uint16_t`. This avoids both an
   alignment trap (the buffer may not be 2-aligned) and an
   endianness conversion call.
2. The "fold carries" loop runs at most twice in practice, but
   we write it as `while` because the math demands it.
3. The function returns the value already byte-swapped so the
   caller can drop it straight into the BE `checksum` field
   without thinking.

To verify, on RX we copy the header, zero the `checksum`
field in the copy, recompute, and compare to the original.
A successful transmission satisfies the algebraic identity
"sum of header (with checksum left in place) == 0xFFFF".

## TX path: `net_ipv4_send`

Sending an IPv4 packet is the composition of everything above:

1. Decide the next hop: if `dst_ip` is on our local subnet,
   the next hop *is* `dst_ip`; otherwise it's the gateway.
2. Resolve the next hop's MAC via `net_arp_resolve()`. If
   that fails, return `-1` and let the caller decide what to
   do.
3. Build an Ethernet header.
4. Build an IPv4 header into the frame (with checksum).
5. Append the payload.
6. Hand the whole thing to `virtio_net_tx()`.

The IPv4 build is split out as `net_ipv4_build()` because
chapter 61's UDP and ICMP code will want to compose IPv4
headers without doing the full ARP-resolve dance for every
single packet — broadcast UDP for DHCP being the obvious
example.

## RX dispatch

`net_init()` registers `rx_dispatch` as the virtio-net RX
callback. From then on, every frame the driver hands up runs
through:

```c
static void rx_dispatch(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HDR_LEN) return;
    uint16_t et = ((uint16_t)frame[12] << 8) | frame[13];
    if      (et == ETHERTYPE_ARP)  rx_handle_arp(frame, len);
    else if (et == ETHERTYPE_IPV4) rx_handle_ipv4(frame, len);
    /* Silently drop everything else (IPv6, LLDP, 802.1q, ...). */
}
```

`rx_handle_ipv4()` does three things and then defers:

1. Sanity-check the version and IHL (we only accept v4, no
   options).
2. Verify the Internet checksum (drop on mismatch).
3. If the destination is us (or the broadcast `255.255.255.255`),
   call the registered upper-layer handler.

The upper-layer hook is `net_set_ipv4_rx_callback()`. Chapter
62 will install one that demultiplexes by `proto` into ICMP,
UDP, and TCP.

## The new self-test

The hand-rolled 42-byte ARP frame that lived in `main.c` is
gone. In its place, a much shorter test that
exercises the actual stack:

```c
static void net_self_test(void)
{
    if (!virtio_net_present()) return;
    if (net_init(SLIRP_GUEST_IP, SLIRP_GW_IP, SLIRP_NETMASK) < 0)
        return;
    serial_puts("[net] self-test: ARP resolve gateway\n");
    uint8_t mac[NET_MAC_LEN];
    if (!net_arp_resolve(SLIRP_GW_IP, mac, 50000000ULL)) {
        serial_puts("[net] self-test: ARP timeout\n");
        return;
    }
    /* print mac, declare success */
}
```

Two lines — `net_init()` then `net_arp_resolve()` — exercise
every path this chapter introduced: the dispatcher fires (or
the cache stays empty); the request builder TXes; the response
handler runs; the cache learns; the lookup hits.

Run it with `make` then `python3 scripts/test_net_arp.py` and
all five PASS lines should appear:

```
PASS: net stack initialised with static IP config
PASS: ARP self-test invoked
PASS: ARP resolved gateway -> 52:55:0a:00:02:02
PASS: ARP cache learned the gateway entry
PASS: shell prompt reached after net init
```

The gateway MAC is not random. QEMU's SLIRP backend
synthesises a MAC for each pseudo-host as `52:55:` followed
by the host's IPv4 address in hex — so `10.0.2.2` becomes
`52:55:0a:00:02:02`. (Knowing this turns out to matter when
debugging: if you see a frame destined for `52:55:` you know
it's going to a SLIRP-internal host, not a real peer.)

## What's missing on purpose

- **ICMP**, so we can't ping yet. That's [chapter 61](061-icmp-udp-dhcp.md).
- **UDP and DHCP**, so the static IP is not negotiated. Same
  chapter.
- **IP options**, **fragmentation**, and **multicast**. Not
  needed for HTTP-over-TCP or DHCP, the only protocols this
  book ever runs.
- **Per-route MTUs**. Hard-coded MTU is the SLIRP default, 1500.
- **A "cache age" field** on ARP entries. We refresh on every
  receive, which is correct but means a host that goes silent
  forever still occupies a cache slot until evicted by a new
  insert. With 8 slots and four-or-so peers, this is fine.

## Where this fits in the milestone trail

After this chapter the kernel can:

- Identify itself on a local IPv4 network at a static address.
- Resolve any neighbour's MAC by ARP.
- Build syntactically valid IPv4 packets to anywhere reachable.
- Receive, validate, and dispatch any IPv4 packet addressed to
  it.

What it still *can't* do is have a conversation with anything.
For that we need at least one transport protocol, which is
[chapter 61](061-icmp-udp-dhcp.md).
