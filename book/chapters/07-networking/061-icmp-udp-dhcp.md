# Chapter 61 — ICMP, UDP, and DHCP

[Chapter 60](060-ethernet-arp-ipv4.md) ended with a stack that
could put correctly-formed Ethernet, ARP, and IPv4 frames on
the wire and validate the ones it received — but no transport
protocol on top, so it could not actually have a conversation
with anything. The static configuration `10.0.2.15 / 24,
gateway 10.0.2.2` was a lie we agreed with QEMU; if any of those
numbers had been wrong we would not have noticed until milestone
55's TCP stack tried to connect to a real host.

This chapter promotes the kernel from "speaks IP" to "speaks
something useful over IP". Three new files appear:

```
kernel/core/icmp.{c,h}    ~150 lines
kernel/core/udp.{c,h}     ~200 lines
kernel/core/dhcp.{c,h}    ~350 lines
```

Plus a small refactor of `net.{c,h}` to support init *before* we
have an IP — the bootstrap problem DHCP creates.

By the end:

- We can answer ICMP echo requests automatically (so
  `ping 10.0.2.15` from the host works), and we can issue
  echo requests of our own.
- We have a port-keyed UDP receive demultiplexer with eight
  bindings and a pseudo-header-aware checksum.
- We have a DHCP client that runs at boot, completes a full
  `DISCOVER → OFFER → REQUEST → ACK` exchange against the SLIRP
  gateway's built-in server, and installs the resulting lease
  via `net_set_ipv4_config()`.
- The kernel boots with the line
  `[net] up: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0`
  computed from the lease, not hard-coded.

## Two-phase init

DHCP creates a chicken-and-egg problem the static-IP path didn't
have. Before we can ask for an IP, we need to:

1. Have the virtio-net driver up so we can transmit a DISCOVER
   broadcast.
2. Have the IPv4 RX dispatcher live so the OFFER reply can find
   us.
3. *Not* have an IPv4 address yet — the whole point of the
   exchange is to learn it.

The chapter-61 `net_init(local_ip, gw, mask)` API forced you to
pick all three at once. So the first refactor in this chapter is
to split it:

```c
int net_attach(void);                         /* phase 1 */
int net_set_ipv4_config(const uint8_t ip[4],  /* phase 2 */
                        const uint8_t gw[4],
                        const uint8_t mask[4]);
int net_init(const uint8_t ip[4],             /* convenience */
             const uint8_t gw[4],
             const uint8_t mask[4]);
```

`net_attach()` reads the MAC, zeroes the ARP cache, and
registers the RX dispatcher. After this call inbound frames are
classified and routed correctly — including a UDP datagram on
port 68 that is still addressed to `0.0.0.0` because we haven't
been assigned anything yet (more on that below).

`net_set_ipv4_config()` installs (or re-installs) the local IPv4
config. It can be called more than once; the boot path calls it
either from the DHCP success path or the static-fallback path.

`net_init()` is now just `net_attach() + net_set_ipv4_config()`.
It survives for code that genuinely wants the static path —
right now nothing inside the kernel uses it, but it leaves the
tests in chapter 60 unchanged.

## RX dispatch when you have no IP

The IPv4 receive validator in chapter 60 dropped any frame whose
destination was not us. With `g_ip = 0.0.0.0` for the duration
of the DHCP exchange, that would drop *everything*, including
the OFFER. The fix in `rx_handle_ipv4()` is to accept three
classes of destination:

```c
static int dst_is_for_us(const uint8_t dst[4])
{
    if (memcmp(dst, g_ip,      4) == 0) return 1; /* unicast to us  */
    if (memcmp(dst, g_bcast_ip,4) == 0) return 1; /* 255.255.255.255 */
    static const uint8_t zero[4] = {0,0,0,0};
    if (memcmp(dst, zero,      4) == 0) return 1; /* DHCP-window 0.0.0.0 */
    return 0;
}
```

The third class is the one DHCP needs. Some servers honour the
broadcast flag and reply to `255.255.255.255`; some unicast the
reply to `chaddr` with `dst = 0.0.0.0` because the client has no
IP yet. SLIRP does the broadcast; the dispatcher allows both so
the same code works against any conforming server.

The matching transmit-side helper goes in `net_ipv4_send`:

```c
if (memcmp(dst_ip, g_bcast_ip, 4) == 0) {
    static const uint8_t bcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    /* skip ARP entirely; deliver to broadcast MAC */
}
```

Broadcast IPv4 must never trigger ARP — there is no host that
owns `255.255.255.255`. The dispatcher special-cases it before
looking at the routing table.

Two new TX entrypoints, `net_ipv4_send_from()` and
`net_ipv4_build_src()`, exist purely so DHCP can synthesise
frames with `src = 0.0.0.0`. They share their implementation
with the existing `_send` / `_build` versions; the public
versions just default the source IP to `g_ip`.

## ICMP

`kernel/core/icmp.c` is the simplest of the three. ICMPv4
(RFC 792) reuses the same 1's-complement Internet checksum that
chapter 60's IPv4 code computes — but with one important
difference.

> **The ICMP checksum covers ONLY the ICMP header + data.**
> No pseudo-header, no IPv4 fields. This is *unlike* UDP and
> TCP, both of which include a pseudo-header.

Get this wrong and SLIRP will silently drop your echo requests
for hours. (A trap easy to fall into.)

The wire layout for an echo packet is the same in both
directions:

```c
struct __attribute__((packed)) icmp_echo_hdr {
    uint8_t  type;       /* 8 = request, 0 = reply */
    uint8_t  code;       /* 0                      */
    uint16_t checksum;   /* BE                     */
    uint16_t id;         /* opaque, echoed         */
    uint16_t seq;        /* opaque, echoed         */
};
```

`icmp_handle()` is one branch:

- If `type == ECHO_REQUEST`, copy the packet into a TX buffer,
  patch `type = ECHO_REPLY`, zero the checksum, recompute, and
  hand it back to `net_ipv4_send` with `dst = ip->src`. That's
  it — `id` and `seq` and any trailing payload bytes are
  preserved verbatim, which is what `ping` uses to match
  replies to its outstanding requests.
- If `type == ECHO_REPLY`, fire the user-installed
  `g_reply_cb` (the boot self-test installs one to flip a
  "reply received" flag).

`icmp_send_echo()` is the outbound counterpart used by the boot
self-test:

```c
struct icmp_echo_hdr h = {
    .type = ICMP_TYPE_ECHO_REQUEST,
    .code = 0,
    .id   = net_cpu_to_be16(id),
    .seq  = net_cpu_to_be16(seq),
};
h.checksum = net_ipv4_checksum(&h, ICMP_ECHO_HDR_LEN);
return net_ipv4_send(dst_ip, IPV4_PROTO_ICMP, &h, ICMP_ECHO_HDR_LEN);
```

## UDP

UDP/IPv4 (RFC 768) is almost as simple as ICMP, with the
checksum being the only awkward part.

```c
struct __attribute__((packed)) udp_hdr {
    uint16_t src_port;     /* BE */
    uint16_t dst_port;     /* BE */
    uint16_t length;       /* BE; includes the 8-byte header */
    uint16_t checksum;     /* BE; 0 means "not used" in IPv4 */
};
```

### The UDP checksum

UDP (and TCP) compute their checksum over a *pseudo-header*
prepended to the real packet. The pseudo-header doesn't go on
the wire — it exists purely to bind the checksum to the IP
addresses, so a packet that gets re-routed (or fails our
`dst_is_for_us` check) is rejected when it arrives at the wrong
host.

The IPv4 pseudo-header is 12 bytes:

```
+-------------------------------+
| source IP address (4 bytes)   |
+-------------------------------+
| destination IP address (4)    |
+-------------------------------+
| zero (1) | proto (1) | UDP    |
|                      | length |
|                      | (2 BE) |
+-------------------------------+
```

`udp_send()` builds the pseudo-header on the stack, runs
`net_ipv4_checksum()` over `pseudo + udp_hdr + payload`, and
patches the result into `udp_hdr.checksum`. The checksum
algorithm itself doesn't care that the pseudo-header is
"virtual"; it just sums 16-bit words, which is why we can hand
it three logical regions concatenated in one buffer.

There is a wart in the spec worth knowing about. RFC 768:

> If the computed checksum is zero, it is transmitted as all
> ones (the equivalent in one's complement arithmetic). An all
> zero transmitted checksum value means that the transmitter
> generated no checksum (for debugging or for higher level
> protocols that don't care).

So we explicitly clamp:

```c
uint16_t cs = net_ipv4_checksum(buf, total_len);
if (cs == 0) cs = 0xFFFFu;
udp->checksum = cs;
```

On receive, if the inbound checksum is zero we skip validation
(the sender opted out); otherwise we fold the pseudo-header in
the same way and reject mismatches.

### Port bindings

The receive side is a plain 8-entry table:

```c
struct udp_binding {
    int        valid;
    uint16_t   port;
    udp_rx_cb  cb;
};
static struct udp_binding g_binds[UDP_BIND_CAP];
```

`udp_handle()` linear-scans the table for the destination port
and invokes the matching callback (or drops, if none). Eight is
plenty for our needs — DHCP (port 68), DNS (port 53 in the
client, added later), and a couple of slots free for
experiments.

A defensive `__attribute__((used)) static void udp_static_init()`
sits in `udp.c` purely to prevent GCC from compiling the
zero-init of the binding table into a `memset` call (the
freestanding-memset trap shows up wherever struct-zero-init
meets `-O2` -- explicit field init avoids it).

## DHCP

DHCPv4 (RFC 2131) is the longest-form protocol in the chapter
not because it is conceptually hard but because the wire format
has a lot of fixed-offset accidents from its BOOTP ancestry.

### BOOTP fixed payload

Every DHCP message begins with a 240-byte fixed payload — the
original BOOTP header — and only after it does the variable
"options" section start. The fields we care about:

| Offset | Size | Field   | Meaning                          |
|-------:|-----:|---------|----------------------------------|
|     0  |   1  | op      | 1 = boot request, 2 = boot reply |
|     1  |   1  | htype   | 1 = Ethernet                     |
|     2  |   1  | hlen    | 6                                |
|     3  |   1  | hops    | 0                                |
|     4  |   4  | xid     | transaction ID, BE               |
|     8  |   2  | secs    | 0                                |
|    10  |   2  | flags   | bit 15 = BROADCAST               |
|    12  |   4  | ciaddr  | client IP                        |
|    16  |   4  | yiaddr  | "your" IP — server's offer       |
|    20  |   4  | siaddr  | next server IP                   |
|    24  |   4  | giaddr  | relay IP                         |
|    28  |  16  | chaddr  | client hardware (MAC + 10 zero)  |
|    44  |  64  | sname   | server name                      |
|   108  | 128  | file    | boot file                        |
|   236  |   4  | magic   | 0x63 0x82 0x53 0x63              |
|   240  |  ... | options | TLV list, 0xFF = END             |

Two of those fields warrant elaboration:

**`xid`** is a 32-bit transaction ID the client picks; the
server echoes it in every reply for this exchange. We compute
it as `cntvct_el0 ^ (mac[5] << 24)` — the timer counter is
unique enough across reboots, and XORing in a MAC byte avoids
two clients on the same host colliding.

**The BROADCAST flag (bit 15 of `flags`, big-endian)** asks the
server to send its replies to the broadcast MAC. We always set
it. Without the flag, a strict server will unicast the reply to
`yiaddr` — but `yiaddr` doesn't exist on the wire yet (it's
the address being offered), so the unicast goes to `0.0.0.0`
which the NIC's filter may drop. Setting the flag sidesteps
that whole class of problem at the cost of a single broadcast
frame per exchange.

**The magic cookie at offset 236** (`0x63 0x82 0x53 0x63`) is
the four-byte sentinel that marks "this is DHCP, not bare
BOOTP". The server checks for it; we set it; both replies have
it; we check for it on receive.

### Options

After the magic cookie comes the options TLV stream:

```
[code 1 byte] [length 1 byte] [length bytes of value] [code...] ...  [0xFF END]
```

Two codes have no length: `PAD = 0` (single byte, ignored —
useful for alignment, never emitted by us) and `END = 255`
(single byte, terminates the stream).

The options we care about in this milestone:

| Code | Name           | Used by                          |
|-----:|----------------|----------------------------------|
|    1 | SUBNET_MASK    | OFFER, ACK (parsed)              |
|    3 | ROUTER         | OFFER, ACK (parsed)              |
|    6 | DNS            | parsed (currently logged only)   |
|   50 | REQUESTED_IP   | REQUEST (sent)                   |
|   51 | LEASE_TIME     | parsed (currently ignored)       |
|   53 | MSG_TYPE       | every message — 1=DISCOVER, 2=OFFER, 3=REQUEST, 5=ACK |
|   54 | SERVER_ID      | OFFER, REQUEST, ACK              |
|   55 | PARAM_LIST     | DISCOVER, REQUEST                |
|  255 | END            | terminator                       |

`MSG_TYPE` (53) is the field that makes a DHCP packet a
*specific* DHCP packet. A DISCOVER carries `MSG_TYPE=1`; an
OFFER carries `MSG_TYPE=2`; the dispatcher in `dhcp_rx()`
keys off it.

### State machine

The client is a four-state synchronous loop:

```
                +--------+   send DISCOVER    +---------+
   dhcp_acquire | INIT   | ----------------> | wait    |
   ----------> +--------+                    | OFFER   |
                                              +---------+
                                                  | rx OFFER
                                                  v
                                              +---------+
                                              | OFFERED |
                                              +---------+
                                                  | send REQUEST
                                                  v
                                              +---------+
                                              | wait    |
                                              | ACK     |
                                              +---------+
                                                  | rx ACK
                                                  v
                                              +---------+
                                              | BOUND   |
                                              +---------+
```

There is also a `FAILED` state for any spin-budget timeout. The
state variable is `static volatile int g_state` because the RX
callback `dhcp_rx()` runs from the IRQ-less polling path inside
`net_poll()`, and the spin loop in `dhcp_acquire()` needs to
see the transition without the compiler caching `g_state` in a
register. (We don't have IRQs on this code path yet; if/when we
do, this will need to grow into a proper atomic.)

`dhcp_rx()` is what sits in the `udp_bind(68, dhcp_rx)`
callback slot. It validates the magic cookie, checks the xid
matches the one DISCOVER picked, parses out
`yiaddr / SERVER_ID / SUBNET_MASK / ROUTER / DNS / MSG_TYPE`,
and flips `g_state` → OFFERED or BOUND depending on the
message type.

The spin loop in `dhcp_acquire()` polls `net_poll()` until
either the state advances or the budget runs out. The budget is
halved between the OFFER wait and the ACK wait so that a
flaky-server case still gives both phases a chance.

### Putting the lease on the stack

When we reach `BOUND` we have:

- `g_offered_ip[4]` from `yiaddr`,
- `g_subnet_mask[4]` from option 1, defaulting to `255.255.255.0`,
- `g_gateway_ip[4]` from option 3, defaulting to
  `[g_offered_ip[0..2], 1]` if the server omits it.

Those three get handed to `net_set_ipv4_config()` and the
ARP cache is implicitly invalidated by the state change in
`net.c`. The kernel logs:

```
[net] up: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
```

This is the first time those numbers were not literals in a
header file.

## The new `net_self_test`

The boot self-test in `kernel/core/main.c` is now a four-phase
script:

```c
static void net_self_test(void)
{
    if (!virtio_net_present()) return;

    /* 1: bring up the dispatcher with no IP */
    if (net_attach() < 0) return;

    /* 2: try DHCP, fall back to static if it times out */
    if (dhcp_acquire(200000000ULL) < 0)
        net_set_ipv4_config(SLIRP_GUEST_IP, SLIRP_GW_IP, SLIRP_NETMASK);

    /* 3: ARP-resolve the gateway */
    uint8_t mac[6];
    net_arp_resolve(g_gateway_ip, mac, 50000000ULL);

    /* 4: ping the gateway */
    icmp_set_echo_reply_callback(icmp_test_reply);
    icmp_send_echo(g_gateway_ip, 0xBEEF, 1);
    /* spin-poll for up to ~50e6 iters waiting for the reply */
}
```

Each phase logs a recognisable line:

```
[dhcp] DISCOVER sent
[dhcp] OFFER received
[dhcp] REQUEST sent
[dhcp] lease acquired
[net]  up: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
[net]  self-test: ARP resolve gateway
[net]  self-test: gateway MAC=52:55:0a:00:02:02
[net]  self-test: ARP cache populated (stack OK)
[net]  self-test: ICMP echo gateway
[net]  self-test: ICMP echo reply received (ping OK)
```

Both `scripts/test_dhcp.py` and `scripts/test_ping.py` assert on
those lines.

## What's missing on purpose

- **DHCP renewals.** We use `LEASE_TIME` for nothing yet — the
  kernel never reboots inside a single QEMU run, so the lease
  effectively never expires. A renewal timer can land alongside
  the timer-driven scheduler interactions later.
- **A proper DNS resolver.** We parse option 6 and stash the
  servers, but nothing dials them yet. The next chapter adds a
  short stub-resolver as part of the socket layer.
- **A loopback interface.** ICMP echo to `127.0.0.1` doesn't
  work because there is no loopback NIC. The closest substitute
  is the gateway pseudo-host. We'll add `lo` if/when we want
  multi-NIC routing tests, not before.
- **Source-port randomisation in `udp_send()`.** We use the
  caller's `src_port` verbatim; DHCP picks 68 because the
  protocol pins it, but anything else needs to think about
  collisions itself.

## Where this fits in the milestone trail

After this chapter the kernel can:

- Acquire its IP automatically from any DHCP-speaking router on
  the local segment.
- Answer pings (and issue them).
- Send and receive arbitrary UDP datagrams via a port-keyed
  callback.

What it still cannot do is have a *connection* — request /
response framing, reliable delivery, ordered bytes. That is the
job of TCP, in [chapter 62](062-tcp-and-sockets.md).
