# Chapter 62 — TCP and a kernel-side socket API

[Chapter 61](061-icmp-udp-dhcp.md) ended with a stack that could
acquire its IP automatically (DHCP), answer pings (ICMP), and
exchange port-keyed datagrams (UDP). What it could not do was
have a *connection* — request/response framing, reliable
delivery, ordered bytes. That is what TCP exists to provide,
and it is what this chapter adds.

Concretely:

```
kernel/core/tcp.{c,h}    ~600 lines
```

A small refactor to `net.c` plumbs IPv4 PROTO=6 through to
`tcp_handle()`, and `net_poll()` now also calls `tcp_poll()`
on every spin so retransmission timers and FIN-driven state
transitions advance.

By the end:

- We can `tcp_connect(ip, port)` to a remote endpoint.
- We can `tcp_send()` arbitrary bytes; the segmenter slices
  them at MSS.
- We can `tcp_recv()` arbitrary bytes; the receiver buffers
  in-order data into a 4 KiB ring.
- We can `tcp_close()` cleanly via a four-way FIN exchange.
- The boot self-test opens `10.0.2.2:8888`, sends a real
  HTTP/1.0 GET, drains the response, and closes — all against
  a real `http.server` listener the test harness runs on the
  host.

This is the longest chapter in Part VII, mostly because TCP's
state machine has more states than every other protocol in
this part *combined*.

## Scope: client only this chapter

A full TCP implementation also handles the *passive open* path:
`bind()`, `listen()`, `accept()`. Servers. We deliberately
defer that, because:

1. Everything we need TCP for in the immediate future
   (HTTP-as-client, eventually the browser) is the active-open
   side.
2. The passive side adds another half-dozen states (LISTEN,
   SYN_RECEIVED) and a BACKLOG queue with non-trivial
   ordering requirements. It's worth its own focused milestone.
3. Splitting the work means the chapter you are reading right
   now is finite.

What the client side *does* cover, in full:

```
CLOSED -- connect --> SYN_SENT
SYN_SENT -- recv SYN+ACK --> ESTABLISHED   (send ACK)
ESTABLISHED -- close --> FIN_WAIT_1        (send FIN)
FIN_WAIT_1 -- recv ACK of FIN --> FIN_WAIT_2
FIN_WAIT_2 -- recv FIN --> TIME_WAIT       (send ACK, then CLOSED)
ESTABLISHED -- recv FIN --> CLOSE_WAIT     (send ACK)
CLOSE_WAIT -- close --> LAST_ACK           (send FIN)
LAST_ACK -- recv ACK of FIN --> CLOSED
```

Plus a fast path for `RST` (jump to CLOSED, mark `reset = 1`,
return errors from any subsequent API call).

We collapse `TIME_WAIT` to immediate `CLOSED`. The orthodox
2×MSL wait exists to reject delayed segments from a previous
incarnation of the 4-tuple; since our ephemeral port allocator
walks the whole 49152..65535 range monotonically and never
reuses a tuple within tens of seconds anyway, the wait adds
nothing on this scale.

## Wire format

```c
struct __attribute__((packed)) tcp_hdr {
    uint16_t src_port;     /* BE */
    uint16_t dst_port;     /* BE */
    uint32_t seq;          /* BE */
    uint32_t ack;          /* BE */
    uint8_t  data_off;     /* upper 4 bits = header length in 32-bit words */
    uint8_t  flags;        /* CWR ECE URG ACK PSH RST SYN FIN (high to low) */
    uint16_t window;       /* BE */
    uint16_t checksum;     /* BE */
    uint16_t urgent;       /* BE; we never set this */
    /* options follow if data_off > 5 */
};
```

The minimum header is 20 bytes. The only option we *send* is
MSS (kind 2, len 4) on our SYN, with a value of 1460. The
options we *accept* are anything: the parser skips any field
between the fixed header and the data section as opaque,
which is correct — TCP options never carry semantics that
affect data delivery (timestamps, SACK, window-scaling all
optimise but never change the contract).

`data_off` is the field that tells you where the data starts.
Multiply the upper 4 bits by 4 to get the header length in
bytes. We write `(hdr_len / 4) << 4` on TX; we read
`(data_off >> 4) * 4` on RX.

### Sequence space

The interesting field in TCP is the 32-bit sequence number,
modulo 2³². Every byte the sender ever transmits has a unique
sequence number; the SYN flag and the FIN flag each consume
one too (that's why the handshake's first ACK is `ISN+1`, not
`ISN`). All sequence-space comparisons in `tcp.c` are written
as straight unsigned subtraction:

```c
uint32_t in_flight = c->snd_nxt - c->snd_una;
uint32_t acked     = ack         - c->snd_una;
if (acked == 0 || acked > in_flight) return;
```

This works correctly across wrap because unsigned arithmetic
on `uint32_t` is modulo 2³² by definition: if `snd_una` was
`0xFFFF_FFE0` and `ack` is `0x0000_0010`, then
`ack - snd_una` is `0x30` — exactly the number of bytes the
peer acknowledged across the wrap. The trap is using `<` /
`>` directly between two sequence numbers; that's a bug. The
kernel does not.

### Connection identity

A TCP connection is identified by the 4-tuple:

```
(local_ip, local_port, remote_ip, remote_port)
```

Two connections may share three of those four fields and still
be distinct as long as the fourth differs. The kernel stores
this 4-tuple in `struct tcp_conn` and `find_conn_for_pkt()`
linear-scans the table for an exact match on inbound packets.
Without an exact match, the packet is silently dropped (a full
implementation would emit a `RST`; we don't bother).

`local_ip` is implicit — it's `g_ip` from `net.c`.

`local_port` is allocated from the ephemeral pool starting at
49152, incrementing modulo 16384 (the IANA "dynamic" range).
We don't validate against an in-use set; with only 4
simultaneous connections, collision is negligible.

## The connection table

```c
#define TCP_CONN_CAP   4
#define TCP_BUF_SIZE   4096u

struct tcp_conn {
    uint8_t  valid;
    uint8_t  state;
    uint8_t  remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t snd_iss, snd_una, snd_nxt, snd_wnd;
    uint32_t rcv_irs, rcv_nxt;

    uint8_t  tx_buf[TCP_BUF_SIZE];   /* user pushes, segmenter pops    */
    uint32_t tx_len;
    uint8_t  rx_buf[TCP_BUF_SIZE];   /* RX path pushes, user pops      */
    uint32_t rx_len;

    uint8_t  fin_sent, fin_acked, peer_fin, reset;
    uint64_t last_tx_poll;
};
static struct tcp_conn g_conns[TCP_CONN_CAP];
```

4 connections × (4 KiB TX + 4 KiB RX + bookkeeping) ≈ 33 KiB
of BSS. Comfortably under the kernel image budget.

The TX buffer is *bytes the user has handed us but the segmenter
hasn't yet acknowledged*. Once a byte is acked by the peer, it
slides off the front. The RX buffer is *bytes we've received
in order but the user hasn't pulled yet*. Out-of-order data
goes on the floor — we don't reassemble across gaps in this
milestone.

(A real-world implementation would buffer out-of-order
segments to enable SACK-based recovery from spurious loss.
With SLIRP as our only network we never see loss; the
simplification is fine.)

## The receive window

The window field on every outbound segment advertises *the
free space in our RX buffer right now*:

```c
uint32_t free_rx = TCP_BUF_SIZE - c->rx_len;
if (free_rx > 0xFFFFu) free_rx = 0xFFFFu;
h->window = net_cpu_to_be16((uint16_t)free_rx);
```

The peer must respect this; if it sends more than `window`
bytes past `rcv_nxt`, the spec lets us drop them (we do).
Conversely, if the peer's advertised window in *its* segments
shrinks to zero, we can only send a 1-byte "window probe"
until it opens up.

Without window scaling (which we don't negotiate), the maximum
window is 65535 bytes, which is plenty for our 4 KiB ring.

## The segmenter

`tcp_send()` does nothing more than append into the TX buffer
and call `tcp_drain_tx()`:

```c
int tcp_send(int cid, const void *data, uint32_t len)
{
    struct tcp_conn *c = get_conn(cid);
    if (!c || c->reset) return -1;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return -1;
    uint32_t free = TCP_BUF_SIZE - c->tx_len;
    uint32_t put  = len < free ? len : free;
    if (put) t_memcpy(c->tx_buf + c->tx_len, data, put);
    c->tx_len += put;
    tcp_drain_tx(c);
    return (int)put;
}
```

`tcp_drain_tx()` is where the segmentation actually lives:

```c
while (in_flight < wnd) {
    uint32_t buf_off = c->snd_nxt - c->snd_una;
    if (buf_off >= c->tx_len) break;
    uint32_t avail   = c->tx_len - buf_off;
    uint32_t can     = wnd - in_flight;
    uint32_t seg     = min(min(avail, can), TCP_TX_MAX);
    tcp_tx(c, ACK | PSH, c->snd_nxt, c->rcv_nxt,
           c->tx_buf + buf_off, seg, /*MSS option=*/0);
    c->snd_nxt += seg;
    in_flight  += seg;
}
```

The interesting part is `buf_off = snd_nxt - snd_una`. Before
any data is acked, this is 0 and we send from the start of
the buffer. As ACKs come in and slide bytes off the front,
the buffer shrinks, and `snd_nxt - snd_una` tracks "how many
bytes from the front of the *current* buffer have already
been put on the wire". On retransmission we set
`snd_nxt = snd_una`, which makes `buf_off = 0` and the loop
resends from the beginning of the unacked window.

The PSH flag is set on every data segment because we have no
Nagle. Why no Nagle? Two reasons:

1. We have no concept of "small writes" yet — `tcp_send` is
   called from kernel C code that already knows what it
   wants to send.
2. SLIRP, our only network for the next several milestones,
   doesn't suffer from the silly-window problem Nagle was
   invented to fix.

If we ever attach to a real network, Nagle is one
six-line patch away.

## Acks

The receive path always ACKs immediately. There is no
delayed-ACK timer. This wastes a few bytes per segment in the
common case (one extra ACK per data segment instead of
piggy-backing on our reply data), but it eliminates a class of
"why is this connection sluggish" bugs. The only place we
*don't* send an explicit ACK is when there's no data and no
FIN to acknowledge:

```c
if (data_len || (flags & TCP_FLAG_FIN)) {
    tcp_send_ack(c);
}
```

A pure ACK has no data, no SYN, no FIN — just `flags = ACK`
and the current `snd_nxt` / `rcv_nxt`. `tcp_send_ack` is the
single helper for it.

## The receive path, end to end

`net.c`'s IPv4 dispatcher case:

```c
case IPV4_PROTO_TCP: tcp_handle(h, payload, plen); break;
```

`tcp_handle` does six things in order:

1. **Validate the header length.** `data_off`'s upper 4 bits
   must be ≥ 5 (i.e. ≥ 20 bytes), and the indicated header
   length must fit in the segment.

2. **Validate the checksum.** Same algorithm as UDP: build a
   12-byte pseudo-header in front of the segment, fold with
   `net_ipv4_checksum()`, compare to the segment's
   `checksum` field after zeroing it. *Unlike* UDP, the
   checksum is mandatory — there is no "0 means none" in TCP.
   A bad checksum drops the segment silently.

3. **Find the connection by 4-tuple.** Linear scan; if no
   match, drop. (A real stack would emit a RST. SLIRP doesn't
   send unsolicited segments at us, so we never need to.)

4. **Handle RST.** Set `reset = 1`, transition to CLOSED, and
   return — every subsequent API call on this cid will return
   `-1`.

5. **Drive the state machine.** SYN_SENT expects exactly
   `SYN+ACK` with `ack == iss + 1`; everything else expects
   `ACK` and updates `snd_una` from the ack field, copies any
   in-order data into `rx_buf`, and walks the FIN handling.

6. **Send our ACK if any sequence space was consumed**, then
   `tcp_drain_tx()` to push out anything new the ack opened
   up window for.

The state-machine block is mostly `if/else` cascades that match
the diagram at the top of this chapter exactly. The most
important detail is *FIN ordering*: a peer's FIN consumes the
sequence number positioned after any in-order data in the
same segment. So we accept the data first, advance `rcv_nxt`,
then test `seg_seq + data_len == rcv_nxt` before honouring
the FIN. Get this wrong and the peer's ACK to your final FIN
gets stuck waiting for a sequence number that already passed.

## Retransmission

The retransmission path is deliberately crude:

```c
void tcp_poll(void)
{
    g_poll_counter++;
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid || c->state == TCP_CLOSED) continue;

        if (c->snd_nxt != c->snd_una &&
            (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
            c->snd_nxt = c->snd_una;     /* rewind */
            tcp_drain_tx(c);             /* re-send everything */
            c->last_tx_poll = g_poll_counter;
        }
        /* same again for SYN_SENT to retransmit a lost SYN */
    }
}
```

There is no RTO estimator, no exponential backoff, no
fast-retransmit on three duplicate ACKs. The threshold is a
fixed number of `tcp_poll()` invocations, which itself is
called from `net_poll()` and from the boot spin loop — so the
real-world threshold is "a few hundred milliseconds" at boot
and "until someone calls `tcp_send` or `tcp_recv` again"
afterwards.

This is more than enough for SLIRP, which essentially never
loses anything (it's a host-side userspace NAT). When we
eventually attach to a real network — a separate milestone
sometime after the browser lands — this routine will need to
grow into a proper RTO estimator. The RFC 6298 algorithm is
the obvious target.

## The boot self-test

```c
serial_puts("[net] self-test: TCP connect to 10.0.2.2:8888\n");
int cid = tcp_connect(gw, 8888);
/* spin until ESTABLISHED or CLOSED */
const char req[] = "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
tcp_send(cid, req, sizeof(req) - 1);
/* spin tcp_recv until tcp_eof or budget */
serial_puts("[net] self-test: HTTP response bytes=");
serial_puthex(total);
tcp_close(cid);
```

The fact that QEMU's SLIRP user-mode networking forwards
`10.0.2.2:N` to the host's `127.0.0.1:N` is the magic that
makes this testable. The test harness in
`scripts/test_tcp.py` spins up a tiny `http.server` on
`127.0.0.1:8888` before booting the kernel; the kernel's
boot self-test connects to it; the response (a fixed 64-byte
body, ~220 bytes total with headers) is fully drained
before close.

The self-test fails open: if the harness isn't running, the
SYN times out, we log "TCP SYN timeout (no listener)", call
`tcp_close()`, and the boot proceeds normally to the shell
prompt. This means a normal `make run` of the kernel doesn't
require a host-side server running.

## What's missing on purpose

- **LISTEN / ACCEPT.** Server-side TCP. Coming later.
- **A userspace socket API.** Right now `tcp_*` is callable
  only from the kernel. Wrapping it in `socket()` /
  `connect()` / `read()` / `write()` syscalls is the next
  step toward an HTTP-fetching userspace tool. Same chapter
  bracket as LISTEN.
- **DNS.** We hardcode `10.0.2.2`. Resolving "example.com"
  needs a stub-resolver that talks to UDP port 53 — which
  exists, we just haven't built the resolver yet.
- **Out-of-order reassembly + SACK.** Drop on gap is simpler;
  SLIRP doesn't reorder.
- **PMTU discovery, ECN, timestamps, window scaling.** Fixed
  MSS=1460 + 64-KiB max window covers everything we plan to
  do.
- **RTO estimation.** A fixed retransmission timer in
  poll-iterations is enough today; an RFC 6298 estimator
  comes when we attach to a real network.

## Where this fits in the milestone trail

After this chapter the kernel can hold a TCP conversation
with any host the SLIRP gateway can reach — which, since
SLIRP NATs out through the host's network stack, is "anywhere
the host machine can reach". The combination of DHCP (for
IP), ICMP (for connectivity sanity), UDP (for DNS later), and
TCP (for HTTP) closes the loop on the protocols a browser
needs.

The next milestone (56) wraps these in a syscall surface and
adds the passive-open path so userspace programs — including
a tiny `httpget` and eventually the full browser of Part VIII
— can use the stack directly.

## Postscript: the receive window in practice

The "4 KiB ring" of this chapter survived through the early
browser work
because every consumer of TCP fetched at most a few KB at a
time: the boot self-test pulls 220 bytes, `/bin/httpget` against
`example.com` pulls about 1300 bytes, and even the first
browser chapters rendered only a hand-built test page around
4 KB. The first time a real-world site landed on the screen
(Hacker News' index, 38 KB of HTML) the receive ring stopped
being big enough — not in the abstract, but in a way that
cost two real bugs.

### Bug 1 — throughput collapse on a 4 KiB window

Fetching the HN index took ~45 seconds, against the same
proxy and the same network where `/bin/httpget` of a 1 KB body
completed in tens of milliseconds. The packet trace showed the
shape:

```
... peer sends 4096 bytes ...
... we ACK with window=0  (ring is full, user hasn't drained yet) ...
... peer's persist timer fires ~200 ms later, sends 1-byte probe ...
... user has drained, we ACK with window>0 ...
... peer sends next 4096 bytes ...
```

With a 4 KiB ring and a 38 KB document we paid the persist-timer
round-trip ten times in a row, each successive interval doubling
because RFC 6298 says so. That's where the 45 seconds went —
none of it in the kernel, none of it in layout, all of it in TCP
idle.

Two small changes restored sanity:

1. **`TCP_BUF_SIZE` from 4096 to 32768.** The TCP header's
   `window` field is 16 bits, so 65535 is the absolute ceiling
   without the window-scaling option (RFC 7323) which we
   deliberately don't implement. 32 KB happens to fit a single
   HN-sized response in two windows of bytes-in-flight, which
   the peer can stream back-to-back.

2. **A window-update ACK in `tcp_recv`.** Even with a bigger
   ring, the peer doesn't know the user has drained until our
   next outbound segment carries an updated `window` field.
   `tcp_recv` now sends an empty ACK after copying bytes out
   when either (a) we just freed at least one MSS, or (b) we
   were below 1 MSS of free space and now have at least 1 MSS
   free. Both cases mean the peer was very likely sitting on a
   stalled persist timer, and an unsolicited ACK is the cheapest
   way to wake it.

After the two changes the same fetch ran in ~400 ms — a 100×
speedup with no protocol changes, just unblocking the peer's
flow-control logic faster.

### Bug 2 — a kernel TCP slot leak after active close

The second symptom appeared once the browser could navigate
between pages: after roughly 16 successful fetches
`socket_connect` started returning -24 (`-EMFILE`), even though
userspace was correctly `close()`ing every fd.

The culprit lived in `tcp_close`. It only released the conn slot
if the state was *already* `TCP_CLOSED` at call time:

- `ESTABLISHED → FIN_WAIT_1`, no release
- `CLOSE_WAIT  → LAST_ACK`, no release
- `TCP_CLOSED  → release_conn`

The state machine in `tcp_handle` then *did* eventually walk the
slot through `FIN_WAIT_2 → TIME_WAIT (collapsed) → CLOSED`, but
nobody was watching, so `release_conn` was never called on the
async path. The slot sat at `valid = 1` forever. After 16
active closes (`TCP_CONN_CAP = 16`) the table was full and
`alloc_conn` returned -1.

The fix follows the same rule every real TCP stack uses: a slot
becomes reclaimable when *both* the user has released their fd
*and* the protocol state machine has reached `CLOSED` — either
condition alone is insufficient. We added a `user_closed` flag
to `struct tcp_conn`, set it in `tcp_close`, and made `tcp_poll`
sweep all slots and release any with
`user_closed && state == TCP_CLOSED`. Because `tcp_poll` runs on
every `net_poll`, the sweep happens within microseconds of the
final state transition.

### Lessons

Both bugs were latent for many milestones because the workloads
were too small to surface them: 4 KiB is a perfectly fine receive
window if every response fits in one window, and a slot that's
leaked once doesn't matter if you never open a 17th connection.
The browser was the first consumer that exercised the stack the
way a real network does, and it found exactly the assumptions
that needed loosening.

As a general principle for any fixed-size kernel resource pool
freed via a multi-step async state machine: do not rely on the
synchronous release path catching every case. Sweep from a
periodic poll. The difference between "works on the test fixture"
and "works on the public internet" is very often a missing sweep.

