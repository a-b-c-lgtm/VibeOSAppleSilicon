# Chapter 105 -- Passive open: LISTEN, SYN_RECEIVED, the backlog

For 8 milestones the kernel has been a TCP **client**. It can
`socket_connect()` to any remote host, send a request, drain the
reply, and clean up. The state machine starts at `CLOSED`,
walks `SYN_SENT -> ESTABLISHED -> FIN_WAIT_1 -> FIN_WAIT_2 ->
CLOSED`, and that's the only path it has ever traversed.

This chapter teaches the same state machine the **other half**
of TCP: how to be the side that doesn't initiate. After this
chapter, a kernel-internal caller can do

```c
int lid = tcp_listen(8088);            /* bind a port    */
while (poll) {
    int cid = tcp_accept(lid);         /* harvest a conn */
    if (cid >= 0) serve(cid);
    net_poll();
}
```

and an external client can reach us through SLIRP's hostfwd.
The new states are `TCP_LISTEN` and `TCP_SYN_RECEIVED`; the new
data structure is a small per-listener accept queue. There are
no new syscalls in this chapter -- the userspace `accept()`
syscall is chapter 106, and a real userspace HTTP server is
chapter 107. Chapter 105 is the kernel-internal foundation
those two will sit on.

## Prerequisites

- [Chapter 62 -- TCP and sockets](../07-networking/062-tcp-and-sockets.md):
  the existing `tcp_connect` / `tcp_handle` / `tcp_poll`
  pipeline, the conn-table layout, the segment TX path.
- [Chapter 63 -- socket syscalls and httpget](../07-networking/063-socket-syscalls-and-httpget.md):
  how the cid-as-fd indirection works (relevant context for
  why we don't yet expose `tcp_listen` to userspace).

## What an active open already does

The active path is short:

```
   CLOSED  --connect()-->  SYN_SENT
   SYN_SENT  --recv SYN+ACK-->  ESTABLISHED   (send ACK)
```

`tcp_connect` allocates a slot in `g_conns[]`, picks a random
ISN from the cycle counter, sends a SYN with our MSS option,
and returns. The state machine sits in `SYN_SENT` until
`tcp_handle` sees the peer's SYN+ACK; it then snapshots the
peer's ISN into `rcv_irs`/`rcv_nxt`, sends the bare ACK that
completes the handshake, and flips state to `ESTABLISHED`.

The passive path is the **mirror image** of that:

```
   CLOSED  --listen()-->  LISTEN              (no segment sent)
   LISTEN  --recv SYN-->  SYN_RECEIVED        (send SYN+ACK)
   SYN_RECEIVED  --recv ACK-->  ESTABLISHED   (push onto accept queue)
```

Same three steps, same number of segments, same sequence-number
bookkeeping. The thing that changes is **who fires the first
segment** and **which slot in the conn table the segment lands
in** -- and that second question is the entire reason this
chapter is a chapter rather than a 40-line patch.

## The dispatch problem

`tcp_handle` looks up the right conn by exact 4-tuple match:

```c
int cid = find_conn_for_pkt(ip->src, dst_port, src_port);
```

For an active open this works because the connect call ran
first; the slot already has `local_port` / `remote_ip` /
`remote_port` filled in, and the inbound SYN+ACK matches it.

For a passive open there **is no slot yet** when the SYN
arrives. The kernel has only a `TCP_LISTEN` slot that knows
its `local_port` but knows nothing about the eventual peer.
The 4-tuple match will return `-1`, and the existing code
drops the segment.

We need a second lookup, gated on the first one failing:

```c
int cid = find_conn_for_pkt(ip->src, dst_port, src_port);
if (cid < 0) {
    /* Maybe an opening SYN to a listener. */
    int lid = find_listener_for_port(dst_port);
    if (lid < 0) return;                      /* nobody home */
    if ((flags & TCP_FLAG_SYN) == 0) return;  /* not a SYN   */
    if (flags & TCP_FLAG_ACK)        return;  /* not opening */

    /* Allocate a child slot in TCP_SYN_RECEIVED, fill the
     * 4-tuple from the packet, send SYN+ACK. */
    ...
}
```

That's the new fast path. Everything downstream of it is
existing code that doesn't care whether the slot got its
4-tuple from `tcp_connect` or from the inbound SYN.

A subtle question: does the existing 4-tuple match ever
accidentally **match a listener**? A listener has
`remote_port == 0` and `remote_ip == 0.0.0.0`. An inbound
TCP segment always has `src_port > 0` (TCP forbids port 0)
and `src_ip > 0`. So `find_conn_for_pkt` naturally skips
listeners without any explicit `state != TCP_LISTEN` guard --
the port mismatch alone filters them out. This is the kind
of accidental correctness you only notice when you go
looking for explicit guards and don't find any.

## struct tcp_conn additions

A listener carries two things a normal conn doesn't:

```c
#define TCP_ACCEPT_QCAP 8

struct tcp_conn {
    ...                                   /* unchanged */
    int     accept_q[TCP_ACCEPT_QCAP];    /* cids ready to harvest */
    uint8_t accept_q_n;                   /* count in accept_q */
    int     parent_listen_cid;            /* -1 if not a listener's child */
};
```

`accept_q` is only meaningful when `state == TCP_LISTEN`. It's
a FIFO of cids that have already completed the three-way
handshake and are sitting in `TCP_ESTABLISHED`, waiting for
`tcp_accept` to harvest them. We pick a small fixed cap (8)
rather than allocating dynamically: the whole conn table is
only `TCP_CONN_CAP = 16` slots, so there's never going to be
more than 8 fully-handshaken-but-unaccepted children anyway.

`parent_listen_cid` is the **child's** view of the relationship.
When a child in `TCP_SYN_RECEIVED` finally gets its third-
handshake ACK, the promotion code uses this pointer to find
the right listener and push the child's cid onto that
listener's queue. After `tcp_accept` pops a child, we reset
its `parent_listen_cid` to `-1` -- the caller owns it now,
and the listener should stop counting it against the backlog
cap.

**The -1 sentinel matters.** Cid 0 is a valid cid in our
table, and `t_memset` zeroes a freshly-allocated slot. If we
defaulted to 0, then `count_listener_children(0)` would
count every conn that had never explicitly set its parent --
including the listener itself, every unrelated client conn,
the works. We fix this by explicitly initialising
`parent_listen_cid = -1` in `alloc_conn`:

```c
static int alloc_conn(void)
{
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        if (!g_conns[i].valid) {
            t_memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].valid = 1;
            g_conns[i].parent_listen_cid = -1;
            return i;
        }
    }
    return -1;
}
```

This is one of those "use a real sentinel value" lessons that
keeps coming up in C kernel work: 0 is almost never the right
"nothing here" marker when 0 is also a valid value of the type.

## tcp_listen

The implementation is tiny because most of what a listener
**doesn't** do is what makes it cheap:

```c
int tcp_listen(uint16_t local_port)
{
    if (local_port == 0) return -2;
    for (int i = 0; i < TCP_CONN_CAP; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->valid) continue;
        if (c->state == TCP_LISTEN && c->local_port == local_port)
            return -2;
    }
    int cid = alloc_conn();
    if (cid < 0) return -1;
    struct tcp_conn *c = &g_conns[cid];
    c->local_port = local_port;
    c->state      = TCP_LISTEN;
    return cid;
}
```

No segment is sent (a listener is purely receive-side, and
the first segment on the wire is the peer's SYN). We refuse
duplicate listeners on the same port because the dispatch
loop would only ever pick the first one anyway -- failing
loudly is friendlier than silently shadowing.

We don't refuse a port that's currently in use by an
**outbound** connection, only by another listener. That's
because TCP allows a server's listening socket to coexist
with established connections to the same local port (the
4-tuple makes them distinguishable). Most other OSes
implement this too, and it's the reason port 80 can serve
millions of concurrent clients from one listening socket.

## tcp_accept

Equally short:

```c
int tcp_accept(int listen_cid)
{
    struct tcp_conn *l = get_conn(listen_cid);
    if (!l || l->state != TCP_LISTEN) return -1;
    if (l->accept_q_n == 0) return -2;       /* EAGAIN */

    int cid = l->accept_q[0];
    for (int i = 1; i < l->accept_q_n; i++) {
        l->accept_q[i - 1] = l->accept_q[i];
    }
    l->accept_q_n--;

    struct tcp_conn *c = get_conn(cid);
    if (c) c->parent_listen_cid = -1;
    return cid;
}
```

Non-blocking: it returns `-2` instantly when the queue is
empty. The boot self-test polls it in a loop alongside
`net_poll`:

```c
for (uint64_t i = 0; i < 2000000000ULL; i++) {
    if ((i & 0xfffu) == 0) (void)net_poll();
    int child = tcp_accept(lid);
    if (child >= 0) { accepted = child; break; }
}
```

This is the same idiom as `tcp_connect`'s spin: pump the NIC,
check the state, repeat. A real userspace caller will pair
`tcp_accept` with `yield()` (chapter 106) so it doesn't burn
CPU; for the in-kernel boot test, busy-polling is fine.

The post-pop `parent_listen_cid = -1` is critical. Once a
child is handed to the caller, the listener should stop
counting it against `TCP_ACCEPT_QCAP`; otherwise after 8
accepted-and-served connections the listener would think
its backlog was permanently full and start rejecting all
new SYNs.

## The SYN-at-LISTEN handler

This is the only really new RX code. It sits inside
`tcp_handle`, in the `cid < 0` branch:

```c
if (cid < 0) {
    if (flags & TCP_FLAG_RST) return;
    int lid = find_listener_for_port(dst_port);
    if (lid < 0) return;
    if ((flags & TCP_FLAG_SYN) == 0)  return;
    if (flags & TCP_FLAG_ACK)         return;

    /* Backlog gate. */
    if (count_listener_children(lid) >= TCP_ACCEPT_QCAP) return;

    int child = alloc_conn();
    if (child < 0) return;
    struct tcp_conn *cc = &g_conns[child];
    t_memcpy(cc->remote_ip, ip->src, NET_IPV4_LEN);
    cc->local_port        = dst_port;
    cc->remote_port       = src_port;
    cc->snd_iss           = fresh_isn();
    cc->snd_una           = cc->snd_iss;
    cc->snd_nxt           = cc->snd_iss;
    cc->rcv_irs           = seg_seq;
    cc->rcv_nxt           = seg_seq + 1;     /* peer's SYN consumes 1 */
    cc->snd_wnd           = seg_wnd ? seg_wnd : TCP_BUF_SIZE;
    cc->state             = TCP_SYN_RECEIVED;
    cc->parent_listen_cid = lid;
    cc->last_tx_poll      = g_poll_counter;

    if (tcp_tx(cc, TCP_FLAG_SYN | TCP_FLAG_ACK,
               cc->snd_iss, cc->rcv_nxt,
               NULL, 0, /*with_mss*/1) < 0) {
        release_conn(child);
        return;
    }
    cc->snd_nxt = cc->snd_iss + 1;           /* our SYN consumes 1 */
    return;
}
```

Three filtering steps before we commit any state:

1. **RST is silent**. A stray RST aimed at a port we happen
   to be listening on is not our problem; drop it.
2. **No listener, no service**. If the host scanned every
   port looking for one, only the ones with a `TCP_LISTEN`
   slot get a SYN+ACK back. Everything else is dropped.
   (A real TCP stack would send a RST to "closed" ports;
   ours stays silent. The practical effect is that closed
   ports look filtered rather than closed -- nmap will
   eventually time out instead of getting an immediate
   "closed" verdict.)
3. **Only opening SYNs**. A SYN with ACK set is a half-
   formed reply (or a TCP simultaneous-open attempt), and
   we don't handle those.

After the filters comes the **backlog gate** -- the only
piece of SYN-flood protection this implementation has. We
count how many child conns currently point at this listener
(both `TCP_SYN_RECEIVED` half-opens and any already-promoted
children sitting in the accept queue), and refuse to allocate
a new slot if that count is already at `TCP_ACCEPT_QCAP = 8`.

This is **not** a real defense -- a serious SYN flood would
exhaust the 8-slot backlog in microseconds and then every
subsequent legit connection would be dropped. Real stacks
use **SYN cookies** (RFC 4987): encode the connection state
into the SYN+ACK's sequence number and don't allocate any
state at all until the third handshake message arrives. We
don't implement SYN cookies because (a) the kernel is
single-tenant and not internet-facing in any way that
matters, and (b) "drop new SYNs when the backlog is full"
is sufficient for the test harness and for the chapter
105 HTTP server.

## The SYN_RECEIVED-to-ESTABLISHED promotion

The peer's third-handshake ACK lands at a slot that
already exists (because we created it on the inbound SYN),
so `find_conn_for_pkt` matches it via the normal 4-tuple
path. The conn is in `TCP_SYN_RECEIVED` when the ACK
arrives; we need to:

1. Verify the ACK number matches our SYN+ACK's expected
   ack (`snd_iss + 1`).
2. Slide `snd_una` forward to retire our SYN.
3. Flip to `TCP_ESTABLISHED`.
4. Push the cid onto the listener's accept queue.

Step 4 is what makes the promotion special. If the listener
has been closed (or its queue is full), the brand-new
ESTABLISHED conn has nowhere to go -- we have to abandon
it cleanly:

```c
if (c->state == TCP_SYN_RECEIVED) {
    if (seg_ack != c->snd_iss + 1) return;
    c->snd_una = seg_ack;
    c->snd_wnd = seg_wnd ? seg_wnd : TCP_BUF_SIZE;
    c->state   = TCP_ESTABLISHED;

    struct tcp_conn *l = get_conn(c->parent_listen_cid);
    if (l && l->state == TCP_LISTEN &&
        l->accept_q_n < TCP_ACCEPT_QCAP) {
        l->accept_q[l->accept_q_n++] = cid;
    } else {
        /* Listener gone or queue full -- abandon the child. */
        tcp_tx(c, TCP_FLAG_RST | TCP_FLAG_ACK,
               c->snd_nxt, c->rcv_nxt, NULL, 0, 0);
        release_conn(cid);
    }
    return;
}
```

The early `return` is important. We can't fall through to
the generic `apply_ack` path, because in `SYN_RECEIVED`
our `snd_nxt - snd_una == 1` (the SYN we sent) but
`tx_len == 0` (we never buffered any data). `apply_ack`
would compute `data_acked = 1` and then do
`c->tx_len -= 1`, underflowing `tx_len` from 0 to
`0xFFFFFFFF`. The next `tcp_send` would think it has
4 GB of buffered data to push and segfault on the read.

This is a slight asymmetry with the active path -- the
existing `TCP_SYN_SENT` handler also does `return` after
the promotion, for the exact same reason. The promotion
itself is the only thing the segment carries; data
piggybacks aren't supported in either direction.

## SYN+ACK retransmission

A SYN_RECEIVED child sits in limbo until the peer's ACK
arrives. If our SYN+ACK was lost, the peer is now waiting
for **us** to retransmit (their own SYN-retransmit timer
won't fire because they already saw our SYN+ACK ack their
SYN, so they think the handshake is half-done). We have to
retransmit, on the same timer the existing `tcp_poll`
uses for unacked data:

```c
if (c->state == TCP_SYN_RECEIVED &&
    (g_poll_counter - c->last_tx_poll) > TCP_RTX_THRESH) {
    tcp_tx(c, TCP_FLAG_SYN | TCP_FLAG_ACK,
           c->snd_iss, c->rcv_nxt, NULL, 0, /*with_mss*/1);
    c->last_tx_poll = g_poll_counter;
}
```

This is symmetric with the existing `SYN_SENT` retransmit
that the active path uses. Without it, a single dropped
SYN+ACK would orphan the SYN_RECEIVED slot until... well,
forever, since we don't have any other timeout.

## Closing a listener

`tcp_close` had to learn one new state. Closing the
`TCP_LISTEN` slot itself can't go through the
`ESTABLISHED -> FIN_WAIT_1 -> ...` shutdown sequence because
there's no peer to send a FIN to. So we release the slot
immediately:

```c
if (c->state == TCP_LISTEN) {
    for (int i = 0; i < c->accept_q_n; i++) {
        int child = c->accept_q[i];
        if (child >= 0 && g_conns[child].valid) {
            tcp_tx(&g_conns[child], TCP_FLAG_RST | TCP_FLAG_ACK,
                   g_conns[child].snd_nxt,
                   g_conns[child].rcv_nxt, NULL, 0, 0);
            release_conn(child);
        }
    }
    release_conn(cid);
    return 0;
}
```

The slightly tricky bit is the children. We have to
distinguish three kinds:

- **Already accepted** children have `parent_listen_cid ==
  -1` -- they don't appear in `accept_q` and they're owned
  by the caller. Untouched by `tcp_close(listener)`.
- **Queued ESTABLISHED** children sit in `accept_q[]` but
  haven't been harvested. We RST them, because the
  application clearly doesn't want them (it's closing
  the listener) and leaving them queued would leak slots.
- **In-flight SYN_RECEIVED** children have
  `parent_listen_cid` pointing at the now-released
  listener slot. We orphan them: when their handshake
  completes, the promotion code's `get_conn(parent)` will
  return null, and the abandonment branch RSTs them
  automatically. No special-casing needed.

## Testing through SLIRP

The boot self-test (`kernel/core/main.c` phase 7) calls
`tcp_listen(8088)` and then polls `tcp_accept` for ~30s of
wall time. The host-side harness
(`scripts/test_passive_open.py`) launches QEMU with one
extra flag:

```
-netdev user,id=n0,hostfwd=tcp::18088-:8088
```

SLIRP (QEMU's user-mode networking) understands `hostfwd`
as "open this TCP port on the host and forward connections
to that TCP port on the guest." So `host:18088 ->
guest:8088`. The harness:

1. Waits for the serial line `TCP listen on port 8088`.
2. Opens a TCP socket to `127.0.0.1:18088`.
3. Sends a 16-byte payload.
4. Half-closes (SHUT_WR), drains, full-closes.
5. Verifies the kernel logged `TCP accepted cid=...`,
   `TCP accept payload bytes=10` (16 in hex), and
   `TCP passive close complete`.

On the wire, SLIRP appears to the guest as if a client at
**10.0.2.2** (the SLIRP gateway IP) connected to
**10.0.2.15:8088** (our DHCP-assigned IP). The kernel's
`find_listener_for_port(8088)` matches our listener, the
SYN gets a SYN+ACK, the third ACK lands, the promotion
fires, `tcp_accept` returns the child cid, and we drain
all 16 bytes plus the peer FIN. SLIRP makes this look
identical to a real external client connecting, which is
exactly what we want from a test setup.

## Lesson: the budget mismatch

The first test run failed with:

```
PASS: kernel created listener on port 8088
PASS: host -> guest TCP handshake via SLIRP hostfwd
PASS: payload sent, half-close + drain + full-close issued
FAIL: kernel never reported a successful accept
```

All three "PASS" lines were genuine -- but the kernel had
*already given up* on `tcp_accept` by the time the host's
SYN actually made it to the guest. The accept loop's
budget was

```c
for (uint64_t i = 0; i < 50000000ULL; i++) ...
```

which mirrors the active-side `tcp_connect` self-test's
budget. That budget is fine for the active path, because
the host's HTTP server is **already running** when the
kernel boots and dials it -- so the SYN+ACK reply comes
back near-instantly. The passive path has the opposite
timing: the kernel listens, **then** the host has to
observe the serial line, **then** dial, **then** SLIRP
synthesises the guest-side SYN. That whole sequence took
longer than 50M iterations of the busy-poll loop, so the
kernel timed out and moved on to the shell prompt while
the SYN was still in flight.

Fix: bump the passive-side budget by ~40x:

```c
for (uint64_t i = 0; i < 2000000000ULL; i++) ...
```

This is roughly 30 seconds of wall time -- generous for the
harness to set up, dial, and let SLIRP relay the SYN.

The general lesson:

**Active-side and passive-side timeouts have fundamentally
different requirements**, even when the code structure
looks identical. An active open is "us reaching out to
something that already exists." A passive open is "us
waiting for something to find us." The first one's
acceptable budget is "round-trip time to the peer"
(milliseconds); the second's is "however long it takes
for someone to *think about* connecting" (seconds at
least, often forever in production). Don't size a
listener's accept budget from the same constant as a
connector's connect budget; they answer different
questions.

The same lesson holds for userspace once chapter 106
exposes `accept()` as a syscall: the syscall has to
either block (yielding the thread) or report `EAGAIN`
immediately. There's no good middle-ground spin
budget for an `accept` call, because the answer to
"how long should I wait?" is always "until something
happens, however long that takes."

## What did NOT need to change

A pleasant surprise: most of `tcp.c` was untouched.

- `tcp_tx` already takes arbitrary flags, so it serves
  SYN+ACK as happily as SYN or ACK or FIN.
- `tcp_send` / `tcp_recv` / `tcp_close` work without
  modification on an accepted child, because once it's
  in `TCP_ESTABLISHED` it's indistinguishable from a
  conn produced by `tcp_connect`.
- `apply_ack`, the RX data path, the FIN handling, the
  retransmit-on-stale-data path -- all unchanged.

This is what TCP's design buys you: the state machine is
genuinely symmetric. Adding a third entry point that
arrives at `ESTABLISHED` via a different route requires
nothing of the code that runs *after* `ESTABLISHED` -- it
just sees a fully-set-up conn and starts moving bytes.

## Files changed

- `kernel/core/tcp.h` -- two new enum values
  (`TCP_LISTEN`, `TCP_SYN_RECEIVED`); two new public
  prototypes (`tcp_listen`, `tcp_accept`); doc block
  rewritten to cover the passive side.
- `kernel/core/tcp.c` -- `TCP_ACCEPT_QCAP` constant;
  three new fields on `struct tcp_conn` (`accept_q`,
  `accept_q_n`, `parent_listen_cid`); `alloc_conn` sets
  the sentinel; new helpers `find_listener_for_port` and
  `count_listener_children`; `tcp_listen` and
  `tcp_accept` implementations; new LISTEN branch in
  `tcp_close`; new SYN-at-LISTEN dispatch in
  `tcp_handle`; new SYN_RECEIVED-to-ESTABLISHED branch in
  `tcp_handle`; new SYN+ACK retransmit in `tcp_poll`.
- `kernel/core/main.c` -- phase-7 boot self-test that
  calls `tcp_listen(8088)` and busy-polls `tcp_accept`
  for 2 billion iterations, drains payload, closes both
  ends.
- `scripts/test_passive_open.py` -- new test harness;
  launches QEMU with `hostfwd=tcp::18088-:8088`, dials
  the listener from the host side, asserts every step
  of the handshake-drain-close sequence.

## What this unlocks

- **Chapter 106** -- a userspace `accept()` syscall that
  wraps `tcp_accept` and integrates with the fd table.
- **Chapter 107** -- a tiny `/bin/httpd` that listens
  on port 80 and serves files out of the on-disk
  filesystem.
- **Chapter 108** -- end-to-end: a userspace HTTP server
  in the guest, the host's browser in `userspace/browser`
  fetching its own kernel's files.

## What's deferred

- **SYN cookies** -- we drop new SYNs when the backlog
  is full instead. Fine for a single-user kernel.
- **TIME_WAIT** -- we collapse it to immediate CLOSED,
  the same simplification we made on the active path.
  A real server-side stack needs TIME_WAIT to avoid
  delivering stale segments to a newly-reused 4-tuple,
  but since we never reuse 4-tuples (every new conn
  gets a fresh slot), this doesn't bite us yet.
- **`SO_REUSEADDR`** -- we always refuse a duplicate
  listener on a port. Real servers want to be able to
  restart and re-bind without waiting for outstanding
  TIME_WAITs.
- **Multiple-listener semantics** -- one listener per
  port, full stop.
- **A real `accept()` syscall** -- chapter 106.
- **Listener-side `select`/`poll`** -- harvest is busy-
  poll. The eventual chapter-105 server will yield()
  between polls but won't have proper readiness
  notifications.
