# Chapter 106 -- accept() and a server-socket API

Chapter 105 taught the kernel how to **be** a TCP server:
allocate a `TCP_LISTEN` slot, accept inbound SYNs, walk a child
through `SYN_RECEIVED` to `ESTABLISHED`, and surface the new
cid through `tcp_accept`. All of that lived behind a phase-7
boot self-test. No userspace program could touch it.

This chapter exposes the server side. After this chapter, a
35-line C program can do:

```c
int lfd = socket_listen(7777, 4);          /* bind a port      */
for (;;) {
    uint32_t ip; uint16_t port;
    int cfd = socket_accept(lfd, &ip, &port);   /* harvest one */
    serve(cfd);                                  /* read/write  */
    close(cfd);
}
```

The accompanying demo is `/bin/echod`, which echoes whatever a
client sends. It compiles to about 7 KiB stripped, exercises
all the new code, and proves end-to-end that the kernel-side
accept queue, the new fd kind, the two new syscalls, the libc
wrappers, and the existing socket read/write paths all line
up. Chapter 107 will replace the echo loop with an HTTP/1.0
parser to give us `/bin/httpd`; chapter 108 will close the loop
by having the in-tree browser fetch from the in-tree server.

## Prerequisites

- [Chapter 105 -- Passive open](105-passive-open-listen.md):
  the kernel-internal `tcp_listen` / `tcp_accept` pair, the
  per-listener accept queue, the new `TCP_LISTEN` and
  `TCP_SYN_RECEIVED` states.
- [Chapter 63 -- socket syscalls and httpget](../07-networking/063-socket-syscalls-and-httpget.md):
  how the cid-as-fd indirection works for `FD_SOCKET`; the
  read/write paths that demux onto `tcp_send` / `tcp_recv`.

## What "expose to userspace" means

It would have been tempting to add one syscall:

```c
int sys_socket_listen_accept(uint16_t port);   /* blocks */
```

...and call it done. That's actually how a hobby OS could ship a
single-port-only echo daemon. But it papers over the only thing
that makes server sockets interesting: **the listening socket is
a long-lived resource**. The peer connections come and go on top
of it. Conflating "bind a port" with "wait for a peer" forces
the program to re-bind on every iteration -- which fails the
second time around because the original port is still in
`TCP_LISTEN`.

So we follow POSIX's split:

```c
int socket_listen(uint16_t port, int backlog);
   /* one-time bind: returns a listening fd */

int socket_accept(int listen_fd,
                  uint32_t *peer_ip_out,
                  uint16_t *peer_port_out);
   /* per-peer harvest: returns a normal connected fd */
```

`socket_listen` happens once at startup. `socket_accept` runs
in a loop. The fd returned by `accept` is a regular `FD_SOCKET`
that you `read`, `write`, and `close` just like the result of
`socket_connect`. The listening fd lives until the daemon
exits, at which point `close()` releases the port.

## A new fd kind: `FD_SOCKET_LISTEN`

The first design choice is "can a listening fd be `read()` or
`write()`?" POSIX says no -- both return `EINVAL`. We follow
suit, and the way we enforce it is by introducing a second
socket-flavoured fd kind:

```c
enum fd_kind {
    ...
    FD_SOCKET,         /* connected TCP */
    FD_SOCKET_LISTEN,  /* ch104: listening TCP */
    ...
};
```

Both kinds use the same `socket_cid` field in the fd entry
(the listening fd's cid points at the `TCP_LISTEN` slot, the
connected fd's at an `ESTABLISHED` slot), so the close path
needs almost no change:

```c
else if ((e->kind == FD_SOCKET ||
          e->kind == FD_SOCKET_LISTEN) && e->socket_cid >= 0)
    tcp_close(e->socket_cid);
```

`tcp_close` already handles both flavours: chapter 105 taught it
that closing a listener RSTs every queued ESTABLISHED child and
releases the slot, while closing a connected conn sends a FIN.
By routing both fd kinds through the same `tcp_close` call we
inherit that behaviour for free.

The read and write paths get a one-line rejection right after
the existing `FD_SOCKET` branch:

```c
if (e->kind == FD_SOCKET_LISTEN) return -EINVAL_VFS;
```

The reason this guard is necessary, rather than letting the
existing "unknown fd kind" branch handle it, is that without it
the read path falls through to the **ramfs** branch -- which
would happily try to deref `g_ramfs[e->ramfs_index]` for a
socket fd whose `ramfs_index` is `-1`. The crash would be
mysterious. Six lines of explicit `EINVAL` are cheap insurance.

A third site needs the same treatment: `thread_inherit_fds`.
Sockets aren't inherited across `fork` in our system (the
single-owner refcount model would race), and listening sockets
inherit the same restriction:

```c
if (src->kind == FD_SOCKET)        continue;
if (src->kind == FD_SOCKET_LISTEN) continue;   /* ch104 */
```

That's it for the kernel-side fd plumbing. Three sites
acknowledge the new kind, one fd-allocator function is added,
and `tcp_close` keeps its existing two-arm dispatch.

## The new syscalls

```c
SYS_SOCKET_LISTEN  = 64,    /* (port, backlog)        -> fd */
SYS_SOCKET_ACCEPT  = 65,    /* (lfd, *ip_out, *p_out) -> fd */
```

### `sys_socket_listen(port, backlog)`

```c
static long sys_socket_listen(long port, long backlog)
{
    (void)backlog;          /* honoured implicitly */
    if (port <= 0 || port > 65535) return -EINVAL_VFS;
    int cid = tcp_listen((uint16_t)port);
    if (cid == -2) return -EADDRINUSE;
    if (cid <  0)  return -EMFILE;
    int fd = vfs_alloc_listen_fd(cid);
    if (fd < 0) { tcp_close(cid); return fd; }
    return fd;
}
```

Three things deserve commentary.

**`backlog` is ignored.** The kernel's accept queue size is
`TCP_ACCEPT_QCAP = 8`, baked in at compile time. Real systems
let userspace pick a per-listener value, but until we have any
program that wants more than 8 we don't pay the storage cost.
The syscall still **accepts** the argument so the call site is
forward-compatible -- when we eventually honour it, no caller
has to change.

**Three distinct errors, two of them new.** `tcp_listen`
returns `-2` for "another listener is already on this port"
and `-1` for "the conn table is full". We translate them into
`EADDRINUSE` (which POSIX uses for exactly this situation)
and `EMFILE` (the fd-table-full error, repurposed for "TCP
table full" since the user can't distinguish them anyway).
Both are surfaced through the `errno` numbers defined in
`vfs.h`, so a userspace `printf("%d\n", -ret)` can be checked
against the canonical Linux values.

**The cleanup on `vfs_alloc_listen_fd` failure** is important.
If the kernel runs out of fd-table slots after we've already
created the `TCP_LISTEN` slot, we'd otherwise leak the slot
forever (it's in `TCP_LISTEN` with no fd pointing at it, so
no one can close it). Calling `tcp_close(cid)` on the failure
path releases it. This is the same defensive pattern that
`vfs_alloc_socket_fd`'s callers already use.

### `sys_socket_accept(lfd, *ip_out, *port_out)`

```c
for (;;) {
    (void)net_poll();
    child = tcp_accept(e->socket_cid);
    if (child >= 0) break;
    if (child == -1) return -EBADF;   /* listener went away */
    yield();
}
```

This is the only blocking syscall in the new pair. Our kernel
doesn't have proper `wait_queue` primitives bolted to TCP yet
(future work: futex-style sleep on conn-state
changes), so accept spins. The spin alternates `net_poll()`
(to actually pump packets through the NIC) with `yield()` (so
other threads keep running). The `tcp_accept` return codes
distinguish "queue empty -- try again" (`-2`) from "listener
no longer exists" (`-1`); the second one is fatal and gets
turned into `EBADF`.

Why poll instead of block? Three reasons, in order of how much
they matter:

1. **TCP has no callback hook into the scheduler.** Adding one
   means changing `tcp_handle` to wake a list of waiters every
   time an accept queue gains an entry. That's a real piece of
   plumbing -- chapter 107 territory or later.
2. **`yield()` is cooperative anyway.** Even a "proper" block
   would only escape the busy loop by parking the thread on a
   condvar. The cost difference is "we burn a few thousand
   wasted CPU cycles per accept" versus "we burn zero". Under
   HVF on macOS we don't care.
3. **It makes the code path identical to other blocking
   syscalls in our kernel.** `sys_socket_connect` polls the
   same way. `read()` on a pipe polls the same way. Adding a
   second pattern just for accept would be premature.

After the spin completes, we surface the peer's address:

```c
uint32_t peer_ip_be = 0;
uint16_t peer_port  = 0;
(void)tcp_peer(child, &peer_ip_be, &peer_port);
if (peer_ip_uptr)
    if (copy_to_user(peer_ip_uptr, &peer_ip_be, 4) < 0) {
        tcp_close(child); return -EFAULT;
    }
if (peer_port_uptr)
    if (copy_to_user(peer_port_uptr, &peer_port, 2) < 0) {
        tcp_close(child); return -EFAULT;
    }
int fd = vfs_alloc_socket_fd(child);
if (fd < 0) { tcp_close(child); return fd; }
return fd;
```

`tcp_peer` is a new accessor we added to `tcp.c` -- nothing
exotic, it just packs the 4-byte `c->remote_ip[]` into a
network-byte-order `uint32_t` so the format matches what
`socket_connect` consumes. Either output pointer can be `NULL`
("don't care").

The order matters: we surface the peer **before** we hand back
the fd. If either `copy_to_user` faults (caller passed a bad
pointer), we have to undo the accept -- otherwise we'd leak a
fully-connected slot the user can't free. The fd allocator
has the same property: if `vfs_alloc_socket_fd` fails (table
full), we `tcp_close(child)`.

## The libc wrappers

The libc side is two thin inline functions in
`userspace/libc/syscall.h`:

```c
static inline int socket_listen(uint16_t port, int backlog)
{
    return (int)_svc2(SYS_SOCKET_LISTEN, (long)port, (long)backlog);
}

static inline int socket_accept(int listen_fd,
                                uint32_t *peer_ip_be,
                                uint16_t *peer_port)
{
    return (int)_svc3(SYS_SOCKET_ACCEPT,
                      (long)listen_fd,
                      (long)peer_ip_be,
                      (long)peer_port);
}
```

Nothing surprising. The interesting part is what's **not**
here:

- No `bind()`. Port is baked into `listen`. POSIX splits them
  because of the wildcard-address dance for SO_REUSEADDR and
  IPV6_V6ONLY; we don't have any of that.
- No `setsockopt()`. Same reason.
- No `accept4()` with non-blocking flag. We don't have
  `O_NONBLOCK` for any fd kind, never mind sockets.
- No `recvfrom` / `sendto`. We're TCP-only at the syscall
  layer; UDP exists in the kernel but is wired only into the
  DHCP/DNS internal paths.

Each of those is a real future chapter, not a "TODO we'll do
when we have time".

## /bin/echod: 150 lines of demo

The full daemon fits on one screen of useful code:

```c
int lfd = socket_listen(port, 4);
if (lfd < 0) {
    printf("echod: listen failed: %d\n", lfd);
    return 1;
}
printf("echod: listening on port %d\n", (int)port);

for (;;) {
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int cfd = socket_accept(lfd, &peer_ip, &peer_port);
    if (cfd < 0) { printf("echod: accept failed\n"); break; }

    printf("echod: accepted from %d.%d.%d.%d:%d\n", /* ... */);
    long n = echo_one(cfd);
    close(cfd);
    printf("echod: echoed %ld byte(s)\n", n);

    if (once) break;
}
close(lfd);
```

`echo_one` is the per-peer loop, an unsurprising
`read`-then-`write` pump:

```c
static long echo_one(int cfd)
{
    char buf[512];
    long total = 0;
    for (;;) {
        long n = read(cfd, buf, sizeof(buf));
        if (n <  0) return total;
        if (n == 0) return total;        /* peer FIN */
        long off = 0;
        while (off < n) {
            long w = write(cfd, buf + off, (size_t)(n - off));
            if (w < 0) return total;
            off += w;
        }
        total += n;
    }
}
```

Two design decisions in echod that are worth flagging:

**Why port 7777 by default?** The kernel's phase-7 boot
self-test (chapter 105) uses port 8088 to verify passive open
during boot. If echod also defaulted to 8088, the listen would
fail with `EADDRINUSE` for the entire ~30 second window of the
boot test, which is exactly the wrong UX. 7777 is unallocated
in our environment and easy to type.

**Why `--once`?** The chapter-104 test harness needs a way to
make echod exit so QEMU can be torn down cleanly. We don't have
signals from the host yet, and we don't have Ctrl-C in the
serial line either (the kernel's tty is too primitive). The
`--once` flag accepts exactly one peer and then exits, which is
the minimal contract the test harness can rely on. Real daemons
in chapter 107+ will have proper exit conditions.

The whole thing is about 150 lines, most of which is decimal
parsing for the port argument and the four-decimal IP pretty
printer. The accept loop itself is **eleven lines of C**.

## The peer-address out-pointer pattern

The accept syscall surfaces the peer's IP and port via in/out
pointers, not via the return value:

```c
int cfd = socket_accept(lfd, &peer_ip, &peer_port);
```

This is POSIX. Why?

- The return value is reserved for the new fd (or `-errno`).
  TCP gives you a 32-bit IP plus a 16-bit port plus a 16-bit
  fd -- 64 bits total. You can't pack all three into one
  positive 32-bit int and still distinguish from `-errno`.
- An "address" in POSIX is a polymorphic blob (`struct
  sockaddr`). The accept syscall doesn't need to know whether
  it's IPv4 or IPv6 or AF_UNIX -- it just copies whatever the
  protocol layer hands it into the buffer the caller pointed
  at. The pattern degrades cleanly when we eventually add
  IPv6.
- Either pointer can be NULL, which is the standard way to
  say "don't care". Saves the caller a dummy local variable.

Our concrete shape diverges from POSIX in one way: we pass two
typed pointers (`uint32_t *ip`, `uint16_t *port`) rather than
one opaque `struct sockaddr *` plus `socklen_t *`. The reason
is that we don't have `struct sockaddr_in` in our libc and
adding it would mean pulling in `<netinet/in.h>` and friends.
Two typed pointers is the smallest design that gives the same
information.

## Test harness: scripts/test_echod.py

The harness mirrors `scripts/test_passive_open.py` from the
previous chapter. The skeleton is:

1. Boot QEMU with `hostfwd=tcp::17777-:7777` so the host can
   reach the guest's listener.
2. Wait for the shell prompt. (We need a long timeout here:
   the phase-7 boot self-test still runs and busy-polls
   `tcp_accept(8088)` for ~30 seconds before timing out
   gracefully. Budget 120 seconds for the prompt.)
3. Send `echod 7777 --once\n` to the serial line.
4. Wait for `echod: listening on port 7777` to confirm the
   daemon is up.
5. Dial `127.0.0.1:17777` from the host.
6. Send a 44-byte payload (`"the quick brown fox..."`),
   half-close, drain the echo back.
7. Assert the echoed bytes match exactly.
8. Wait for `echod: accepted from`, `echod: closed peer`,
   `echod: done` in the guest's serial output.

All seven assertions pass. The end-to-end path is:

- host's `socket(SOCK_STREAM)` to SLIRP hostfwd
- SLIRP to guest's virtio-net device
- net_rx -> tcp_handle -> SYN+ACK to listener
- listener spawns child in `SYN_RECEIVED`
- peer's ACK promotes child to `ESTABLISHED`
- listener's accept queue gains an entry
- echod's `socket_accept` returns the new fd
- echod's `read()` -> `tcp_recv` -> payload bytes
- echod's `write()` -> `tcp_send` -> echoed bytes
- back through virtio-net to SLIRP to host's `recv()`
- echod's `close()` -> FIN, peer's FIN, mutual teardown

That every link survives is what the green test bar means.

## Lesson: the read/write fall-through trap

The single most subtle bug in this chapter would have been not
adding the explicit `if (kind == FD_SOCKET_LISTEN) return
-EINVAL` guard to `vfs_read` and `sys_write`. Without it, the
existing code falls through to the **ramfs** read path:

```c
struct ramfs_entry *r = &g_ramfs[e->ramfs_index];
if (e->cursor >= r->size) return 0;
...
```

For a listening socket, `e->ramfs_index` is `-1` (we never set
it; `vfs_alloc_listen_fd` only touches `kind` and `socket_cid`).
The deref would hit an arbitrary kernel address and either
return garbage or fault.

The general rule is: **whenever you add a new fd kind, audit
every site that dispatches on `enum fd_kind` and decide what
the new kind should do there.** The kinds audited for
chapter 106 were:

- `vfs.c::fd_table_unref` (close path) -- needs the new kind.
- `vfs.c::vfs_close` (close path) -- needs the new kind.
- `vfs.c::vfs_read` -- explicit `EINVAL`.
- `syscall.c::sys_write` -- explicit `EINVAL`.
- `thread.c::thread_inherit_fds` -- skip on fork.

That's it. Five sites. The kernel doesn't have a `stat()` or
`fstat()` yet, otherwise that'd be a sixth.

## Lesson: surface peer addr before allocating the fd

The accept syscall does this:

```
1. tcp_accept(lid)        -> get child cid
2. tcp_peer(child, ...)   -> read addr
3. copy_to_user(ip, ...)  -> can fault
4. copy_to_user(port, ...)-> can fault
5. vfs_alloc_socket_fd    -> can fail
6. return fd
```

Steps 3, 4, 5 can all fail. If they fail **after** we've
allocated the fd, the fd is in `child`'s slot of the fd table
and userspace can `close()` it -- fine. But the more interesting
case is that step 5 fails (the fd table is full): we've already
got `child` in `ESTABLISHED`, no fd points at it, and we'd leak
the cid forever.

The fix is the explicit `tcp_close(child)` on every failure
path between step 1 and step 6. The order in the code is
"surface peer -> alloc fd" not "alloc fd -> surface peer"
because reversing those would mean a `copy_to_user` failure
leaks the fd as well as the cid -- you'd have to call **both**
`tcp_close(child)` and `vfs_release_fd(fd)` to unwind.

## Files changed

Kernel:

- `kernel/core/vfs.h` -- new `FD_SOCKET_LISTEN` enum value,
  `EADDRINUSE` errno, `vfs_alloc_listen_fd` prototype.
- `kernel/core/vfs.c` -- `vfs_alloc_listen_fd` helper; close
  paths handle the new kind; `vfs_read` rejects with `EINVAL`.
- `kernel/core/syscall.h` -- `SYS_SOCKET_LISTEN = 64`,
  `SYS_SOCKET_ACCEPT = 65`.
- `kernel/core/syscall.c` -- `sys_socket_listen`,
  `sys_socket_accept`, dispatcher cases; `sys_write` rejects
  the new kind.
- `kernel/core/thread.c` -- `thread_inherit_fds` skips the
  new kind.
- `kernel/core/tcp.h`, `kernel/core/tcp.c` -- new `tcp_peer`
  accessor.

Userspace:

- `userspace/libc/syscall.h` -- new enum values, two new
  inline wrappers.
- `userspace/echod/echod.c` -- the demo daemon.

Build and test:

- `Makefile` -- ECHOD_OBJS / ECHOD_ELF / ECHOD_STRIPPED, link
  rule, strip rule, OSFS bundling, mkosfs invocation.
- `scripts/test_echod.py` -- the new test.
- `book/INDEX.md` -- new milestone row.

## What's deferred

These are the obvious next things, none of which we need for
the immediate goal of "let /bin/httpd exist":

- **Non-blocking accept** (`-EAGAIN`). The kernel returns
  `-EAGAIN` from `tcp_accept` internally, but our syscall
  always spins until success. Adding `O_NONBLOCK` is a
  cross-cutting change to every blocking syscall, not just
  accept.
- **`select()` / `poll()`**. Right now a daemon can only
  serve one peer at a time. The natural fix is select-style
  multiplexing, which itself wants real wait_queues in the
  kernel.
- **`setsockopt(SO_REUSEADDR)`**. Useful in development (lets
  you restart a server immediately after killing it), but our
  `tcp_close` releases the port the moment the listener fd
  drops, so we don't have the TIME_WAIT-blocks-rebind problem
  that motivates SO_REUSEADDR on Linux.
- **Per-listener backlog**. The `backlog` argument to
  `socket_listen` is parsed and ignored; the cap is the
  global `TCP_ACCEPT_QCAP = 8`.
- **IPv6**. Our entire stack is IPv4.
- **fork-inherited sockets**. Listening fds, like connected
  fds, are explicitly **not** inherited across fork.
  Real Unix lets you do this, but it requires a refcounted
  socket-table entry shared between processes -- chapter 120+
  territory in our roadmap.

## What this unlocks

- **`/bin/httpd`** (chapter 107). Drop in an HTTP/1.0 parser
  in place of `echo_one` and you've got a static-file server.
- **End-to-end loop testing** (chapter 108). Once `/bin/httpd`
  exists, the in-tree browser can fetch from the in-tree
  server. The whole networking stack -- from the SLIRP packet
  to the rendered DOM -- exercises in one boot.
