# Chapter 106 — TCP loopback (lo0 and 127.0.0.0/8)

> **Milestone in this chapter:** 95 — add an in-kernel loopback
> interface so two in-guest TCP processes can talk without
> tromboning through the host.
> **Code referenced:**
> - [kernel/core/net.c](../../../kernel/core/net.c) (`lo0` and
>   the 127.0.0.0/8 short-circuit)
> - [kernel/core/tcp.c](../../../kernel/core/tcp.c)
>
> **At the end of this chapter** you will have an in-guest
> TCP path between any two processes via 127.0.0.1, with no
> dependency on QEMU's SLIRP hostfwd. Prerequisite: chapter
> 39 (TCP) and the chapter-37 net stack.

Before this chapter, two processes in our OS could only talk
TCP to each other by tromboning through the host: process A
dials `10.0.2.2:18080`, SLIRP's hostfwd rule bounces that
back to `10.0.2.15:8080`, process B accepts. Two virtio-net
round trips, one host-side detour, and a hard dependency on
having run `make run-graphical` (which bakes the hostfwd).
Without the hostfwd, **there was no path at all from one
in-guest TCP process to another** -- not even via 127.0.0.1,
because our TCP stack shipped every segment out the wire.

This chapter adds the missing path: a short-circuit in the
TX path that recognises segments addressed to ourselves and
delivers them directly to our own RX queue, without touching
virtio-net. The result is real `127.0.0.1`, real
`10.0.2.15`-to-`10.0.2.15`, real loopback -- the same
primitive every production kernel has from day one.

## Why this comes before the end-to-end loop

The end-to-end browser-talks-to-our-own-httpd test is the
celebration of part XIII. It can't be the celebration if it
secretly depends on SLIRP hostfwd. So loopback ships first,
in this chapter, and chapter 106c gets to demo the clean
version. Three chapters later we'll have a browser in the
guest pulling pages off an httpd in the same guest with
nothing on the host side except a TLS bridge -- and the
moment we don't have to think about the host at all is the
moment our OS feels real.

## The short-circuit

`net_ipv4_send_from` used to be unconditional: build the
ethernet+IPv4 frame, ARP-resolve the next hop, hand the
frame to `virtio_net_tx`. Chapter 106 inserts one branch
right after the frame is built:

```c
if (net_is_local_ip(dst_ip)) {
    eth_hdr_build(frame, g_mac, ETHERTYPE_IPV4);
    int ip_len = net_ipv4_build_src(frame + ETH_HDR_LEN,
                                    sizeof(frame) - ETH_HDR_LEN,
                                    src_ip, dst_ip, proto,
                                    payload, payload_len);
    if (ip_len < 0) return -1;
    return loopback_enqueue(frame,
                            ETH_HDR_LEN + (uint32_t)ip_len);
}
```

That's almost the whole chapter. The rest is the supporting
cast: who decides what counts as "local," who picks the
source IP, where the queue gets drained, and why the queue
exists in the first place instead of a tail call into
`rx_dispatch`.

## What counts as local

`net_is_local_ip(ip)` is intentionally narrow:

```c
int net_is_local_ip(const uint8_t ip[NET_IPV4_LEN])
{
    if (ip[0] == 127) return 1;          /* 127.0.0.0/8 */
    if (g_ip[0] == 0 && g_ip[1] == 0 &&
        g_ip[2] == 0 && g_ip[3] == 0) return 0;
    return n_memeq(ip, g_ip, NET_IPV4_LEN);
}
```

Two cases:

1. Anything in `127.0.0.0/8` -- the classic loopback prefix.
2. Our own DHCP-assigned address. A process that asks for
   `10.0.2.15` (our address) should reach itself, not its
   neighbour.

The `g_ip == 0.0.0.0` guard matters. DHCP DISCOVER goes out
before we have a lease, with `src = 0.0.0.0` and
`dst = 255.255.255.255`. Without the guard, every DISCOVER
would short-circuit to our own RX queue and we'd never get
a lease. With the guard, we only treat `g_ip` as local once
DHCP has assigned us a non-zero address.

## Who picks the source

This is the part that needs care. TCP's 4-tuple is `(src_ip, src_port,
dst_ip, dst_port)`. When you dial `127.0.0.1:9999`, your
client conn records:

```
remote_ip   = 127.0.0.1
remote_port = 9999
local_ip    = ???           /* what do we put here? */
local_port  = 49152         /* ephemeral */
```

What goes in `local_ip`? The temptation is "well, `g_ip`,
that's our address." But then the SYN you send has
`src = 10.0.2.15, dst = 127.0.0.1`. When the kernel
short-circuits and your own RX path processes the SYN, it
sees `src = 10.0.2.15`. It looks up the listener on
`local_port = 9999` and finds it -- the listener doesn't
care about the source. The listener spawns a child conn
with:

```
remote_ip   = 10.0.2.15      /* from packet src */
remote_port = 49152          /* from packet src_port */
local_port  = 9999
```

The child sends SYN+ACK with `src = 10.0.2.15` (`g_ip`)
and `dst = 10.0.2.15` (the remote_ip we recorded). Now the
packet has nothing to do with `127.0.0.1` anymore -- it's
`10.0.2.15 -> 10.0.2.15`. When the client side processes it,
it tries to match the 4-tuple and finds:

- packet has `src = 10.0.2.15, src_port = 9999, dst_port = 49152`
- client conn has `remote_ip = 127.0.0.1, remote_port = 9999, local_port = 49152`

`remote_ip` mismatch. No match. The SYN+ACK is silently
dropped. The handshake stalls.

The fix is **source-symmetry**: both halves of a loopback
conn must observe `src = dst = 127.0.0.1`. The rule is
implemented by `net_choose_src`, which the upper layers
(TCP, eventually UDP) call before computing any pseudo-
header checksum:

```c
void net_choose_src(const uint8_t dst_ip[NET_IPV4_LEN],
                    uint8_t out_src[NET_IPV4_LEN])
{
    if (net_is_local_ip(dst_ip)) {
        n_memcpy(out_src, dst_ip, NET_IPV4_LEN);
    } else {
        n_memcpy(out_src, g_ip, NET_IPV4_LEN);
    }
}
```

The TX side of TCP now reads:

```c
uint8_t our_ip[NET_IPV4_LEN];
net_choose_src(c->remote_ip, our_ip);
uint16_t cks = tcp_compute_checksum(our_ip, c->remote_ip,
                                    pkt, hdr_len + data_len);
h->checksum = cks;
return net_ipv4_send_from(our_ip, c->remote_ip, IPV4_PROTO_TCP,
                          pkt, hdr_len + data_len);
```

The same `our_ip` feeds the pseudo-header checksum AND the
IP header in `net_ipv4_send_from`. If those two disagreed,
the RX side would compute the checksum from the (wire) IP
header, get a different number than the TX side stamped in,
and drop the segment as corrupt.

**Why isn't this a silent rewrite inside `net.c`?** Because
the upper layer has already computed a checksum by the time
`net_ipv4_send_from` runs. If `net.c` swapped `src_ip` after
the fact, the checksum would be wrong. The src-selection
decision belongs to whoever owns the checksum, and that's
TCP.

Linux's `inet_select_addr` does the same thing for the same
reason. It's nice to know your toy kernel and the canonical
kernel agree on where a particular line of code belongs.

## Why a queue, not recursion

The shortest possible loopback implementation is two lines:

```c
if (net_is_local_ip(dst_ip)) {
    rx_dispatch(frame, len);   /* just hand it back to ourselves */
    return 0;
}
```

This is also a one-way trip to a stack overflow. Here's why.

Our kernel stack is 16 KiB
(see [`linker/kernel.ld`](../../../linker/kernel.ld) and
the `.stack` section). `tcp_tx` carries a 1500-byte segment
buffer on its stack frame; `rx_dispatch -> rx_handle_ipv4
-> tcp_handle` brings a 1500-byte mutable copy for checksum
validation; `tcp_handle -> tcp_send_ack -> tcp_tx` is
another frame full of locals.

Trace one TCP handshake under the recursive scheme:

```
tcp_connect              -> tcp_tx(SYN)
  net_ipv4_send_from     -> rx_dispatch
    tcp_handle           -> tcp_tx(SYN+ACK)     /* server side */
      net_ipv4_send_from -> rx_dispatch
        tcp_handle       -> tcp_tx(ACK)         /* client side */
          net_ipv4_send_from -> rx_dispatch
            tcp_handle   -> tcp_tx(ACK)         /* server side */
              ...
```

We're four stack frames deep and we haven't even sent data
yet. Each frame is at least 2 KiB. A 16 KiB stack tolerates
maybe 7-8 frames before it eats into adjacent BSS and starts
corrupting kernel data. A full request/response cycle
(handshake + data + FIN exchange + closing acks) is well
over 20 segments. Recursion is not an option.

The fix is what every real kernel does: bounded queue, drain
later. `net_ipv4_send_from` enqueues; the queue gets drained
by the next call into `net_poll`:

```c
#define LOOPBACK_QUEUE_CAP 16
#define LOOPBACK_DRAIN_CAP 256
static uint8_t  g_lo_buf [LOOPBACK_QUEUE_CAP][ETH_HDR_LEN + 1500];
static uint32_t g_lo_len [LOOPBACK_QUEUE_CAP];
static int      g_lo_head, g_lo_tail;

static int loopback_drain(void)
{
    int n = 0;
    while (n < LOOPBACK_DRAIN_CAP && g_lo_head != g_lo_tail) {
        uint32_t       len   = g_lo_len[g_lo_head];
        const uint8_t *frame = g_lo_buf[g_lo_head];
        g_lo_head = (g_lo_head + 1) % LOOPBACK_QUEUE_CAP;
        rx_dispatch(frame, len);
        n++;
    }
    return n;
}
```

`net_poll` calls `loopback_drain` between the virtio-net RX
drain and `tcp_poll`. So every time TCP wakes up to check
for retransmits, it also processes whatever loopback
segments piled up since the last tick.

`LOOPBACK_QUEUE_CAP = 16` is enough for a TCP handshake plus
a few segments of data in flight. The drain cap of 256 is a
safety net against a runaway producer; in practice the
handshake completes in 4-6 drain iterations.

The queue lives in `.bss` (`16 * 1514 ~= 24 KiB`), which is
why kernel `.bss` grew from `0x1721f8` to `0x1780e8` when
chapter 106 landed.

## Drain ordering inside one tick

`loopback_drain`'s `while` re-checks `g_lo_head != g_lo_tail`
each iteration. That's important because `rx_dispatch` can
trigger `tcp_send_ack -> tcp_tx -> loopback_enqueue`, which
appends new entries while we're iterating. The loop picks
them up in the same drain call.

That means one `net_poll` tick can complete the entire
three-way handshake: pop SYN, dispatch to server, server
queues SYN+ACK; pop SYN+ACK, dispatch to client, client
queues ACK; pop ACK, dispatch to server, child promoted
to ESTABLISHED, added to accept queue. The client's
`sys_socket_connect` busy-loop sees `ESTABLISHED` and
returns the fd, all without ever yielding to the server
process.

This is faster than real loopback (Linux's `lo` goes
through a softirq) but it's perfectly fine for us because
we don't have softirqs.

## ARP and L2: skipped

The loopback path bypasses ARP entirely. It does build a
synthetic ethernet header (because `rx_dispatch` parses one
off the front of the frame), but uses our own MAC for both
src and dst. The L2 layer never sees these frames; they go
straight from `net_ipv4_send_from` into our RX queue.

We don't poison the ARP cache with `g_mac -> g_ip` either.
Real Linux does add a `lo` route, but Linux's `lo` is a
full netdev with its own queue and tx_ops; our path is
narrower (we have one address family to worry about, IPv4).

## The reaper trap

Implementing the short-circuit was straightforward.
Implementing the test was where the lesson lived.

The first looptest fork did `write(15 bytes) + shutdown +
read until EOF`. The first `read` came back as `-EIO`
instead of returning the echoed bytes. The diagnostic
showed:

```
[tcp_handle data]   cid=2 data accepted (15 bytes -> server)
[tcp_recv drain]    cid=2 took 15 bytes (server read)
[tcp_handle data]   cid=1 echo accepted (15 bytes -> client)
[tcp_poll reap]     cid=2 rx_len=0
[tcp_recv drain]    cid=1 took 15 bytes (client first read -- OK!)
[tcp_poll reap]     cid=1 rx_len=0
[vfs_read sock]     tcp_recv -1 cid=1 state=CLOSED eof=1 -> -EIO
[loopcli] read err -5
```

The first `read` HAD returned 15 bytes; the user code
looped back and called `read` again expecting `0` (EOF),
got `-EIO` instead, and the test failed.

Two bugs surfaced in one shot:

1. **`tcp_poll`'s reaper was too eager.** The original
   reap check was
   `if (c->user_closed && c->state == CLOSED) release_conn`.
   Loopback delivers FIN-and-final-data in the SAME drain
   tick, so the conn hits `CLOSED` while still holding the
   final data in `rx_buf`. Eager reap freed the slot before
   user code could read it. **Fix:** also require
   `c->rx_len == 0`. The slot stays alive until the user
   has consumed the bytes.

2. **`vfs_read` mapped "conn vanished" to `EIO`.** Even
   after the rx_len guard, the client's SECOND read still
   raced the reaper -- the first read drained the buffer,
   the next `net_poll` tick reaped the conn, and the user's
   poll-loop saw the conn gone. `vfs_read` was returning
   `-EIO` because `tcp_recv` returned `-1`. **Fix:** check
   `tcp_eof(cid)` BEFORE the `n < 0 -> -EIO` branch.
   `tcp_eof` returns 1 when the conn is gone, which is the
   right answer to "is there any more data?" The new order:

   ```c
   for (;;) {
       (void)net_poll();
       int n = tcp_recv(e->socket_cid, buf, len);
       if (n > 0) return n;
       if (tcp_eof(e->socket_cid)) return 0;
       if (n < 0) return -EIO;
       yield();
   }
   ```

   Reordering `tcp_eof` before the `n < 0` branch turns
   "conn was reaped after delivering its data" into
   `read() == 0` (the standard POSIX end-of-stream signal).
   The `EIO` branch now only fires for live conns that were
   reset (RST in flight), which is the right semantic.

The lesson: **on the wire, FIN takes a round trip to
deliver. On loopback, FIN arrives in the same tick as the
final byte.** The TCP state machine doesn't care -- it
moves through the same transitions either way -- but
anything that races state transitions against user reads
(reapers, eofs, retransmit timers) needs to handle the
collapsed-timeline case. We hit it twice in this chapter
and we'll probably hit it again in 106a when httpd starts
serving its own dashboard over the same loopback.

## The demo: `/bin/looptest`

The test program forks. Parent listens on a port and runs
an echo server; child connects to `127.0.0.1` on the same
port and sends a known phrase. Source:
[userspace/looptest/looptest.c](../../../userspace/looptest/looptest.c).

```
$ looptest 9999
[looptest] listening on port 9999
[loopcli] connecting to 127.0.0.1:9999
[loopcli] connected (fd=3)
[loopsrv] accepted from 127.0.0.1:49153 (cfd=4)
[loopsrv] echoed 15 bytes; closing
[loopcli] GOT: loopback-hello
[looptest] child exit=0 srv_rc=0
[looptest] done
```

Note `peer reported as 127.0.0.1` in the server log -- this
is the proof that 4-tuple symmetry works. If we had let
`g_ip` leak into the source address, the accepted peer would
have read as `10.0.2.15` and `find_conn_for_pkt` would never
have matched the client's conn on the way back.

The harness, [`scripts/test_tcp_loopback.py`](../../../scripts/test_tcp_loopback.py),
boots the kernel with a vanilla `-netdev user,id=n0` (no
hostfwd!), drops to the shell prompt, runs `looptest 9999`,
and asserts each line in turn. The whole test takes no
host-side network setup -- which is exactly the property
chapter 106 was meant to deliver.

## Files changed

- [`kernel/core/net.h`](../../../kernel/core/net.h) --
  added `net_is_local_ip` and `net_choose_src` to the
  public API.
- [`kernel/core/net.c`](../../../kernel/core/net.c) --
  loopback queue, drain in `net_poll`, short-circuit in
  `net_ipv4_send_from`, "for us" check now also matches
  127/8.
- [`kernel/core/tcp.c`](../../../kernel/core/tcp.c) --
  `tcp_tx` now calls `net_choose_src` before computing
  the pseudo-header checksum, and uses
  `net_ipv4_send_from` (not `net_ipv4_send`) so the chosen
  source actually lands in the IP header. The reaper in
  `tcp_poll` now requires `rx_len == 0`.
- [`kernel/core/vfs.c`](../../../kernel/core/vfs.c) --
  socket `read()` checks `tcp_eof` before `n < 0`, so
  "conn was reaped after FIN" surfaces as `0` (EOF), not
  `EIO`.
- [`userspace/looptest/looptest.c`](../../../userspace/looptest/looptest.c)
  -- the fork-and-echo demo.
- [`scripts/test_tcp_loopback.py`](../../../scripts/test_tcp_loopback.py)
  -- hermetic regression test, no host-network deps.
- [`Makefile`](../../../Makefile) -- wire up `LOOPTEST_*`
  objects, include in OSFS image.

## What this unlocks

- **Chapter 106a** -- httpd serving over a TLS bridge.
  We can run an in-guest httpd that listens on
  `127.0.0.1:8080` AND have a host-side TLS terminator
  speak to it without any port-forwarding rules.
- **Chapter 106b** -- the browser's `BROWSER_PROXY` env
  var (or equivalent) finally has a stable in-guest
  address to point at: `127.0.0.1:8080`.
- **Chapter 106c** -- the hermetic browser-to-httpd test.
  The grand demo of part XIII: two of our own programs,
  one our HTTP client and one our HTTP server, having a
  full conversation over our own TCP stack, with the
  packets never leaving our own kernel.

## Prerequisites

- [Chapter 53](../07-networking/53-eth-arp-ipv4.md) --
  where `net_ipv4_send_from` was first introduced.
- [Chapter 55](../07-networking/55-tcp-client.md) --
  `tcp_tx`'s checksum machinery.
- [Chapter 103](103-tcp-passive-open.md) -- the listener
  + child-conn pattern that loopback exercises.
- [Chapter 104](104-accept-and-server-sockets.md) -- the
  `socket_accept` syscall the looptest parent uses.
- [Chapter 105](105-bin-httpd.md) -- the predecessor
  whose hostfwd dependency motivated this chapter.
