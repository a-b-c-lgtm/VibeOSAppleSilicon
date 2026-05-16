# Chapter 64 — Socket syscalls and a userspace `httpget`

[Chapter 63](63-tcp-and-sockets.md) ended with a TCP client
that worked beautifully... from inside the kernel. The boot
self-test could open `10.0.2.2:8888`, fetch a real HTTP/1.0
response, and close cleanly. But every byte of that traffic
was driven by C functions called from `main.c`. There was no
way for a *user* program to do the same thing.

This chapter fixes that. By the end:

- A new fd kind (`FD_SOCKET`) lets the existing `read()`,
  `write()`, and `close()` syscalls operate on a TCP
  connection identically to how they operate on a file or a
  pipe.
- Three small new syscalls — `SYS_SOCKET_CONNECT`,
  `SYS_SOCKET_STATE`, `SYS_SOCKET_SHUTDOWN` — open
  connections, query their state, and signal a half-close.
- A 130-line userspace tool, `/bin/httpget`, walks the
  whole stack: it parses a dotted-quad IP, calls
  `socket_connect`, writes a `GET / HTTP/1.0` request,
  drains the response, and prints it to stdout.
- A new harness, `scripts/test_httpget.py`, spins up a
  Python `http.server` on the host, boots the kernel, types
  `httpget 10.0.2.2 8888 /m56` at the shell prompt, and
  asserts the server's payload appears on the guest console.

The total kernel-side delta is under 100 lines. Most of the
work is design: deciding *what* the user-facing surface
should look like, and which existing kernel code we can avoid
duplicating.

## The "everything is an fd" trick (again)

The cleanest way to expose TCP to userspace is to make a
connection look like any other readable/writable handle.
Linux does this. Plan 9 took it the furthest. We adopt the
same idea, scaled down to fit our fd table:

- The *creation* operation is special — it has to specify a
  destination and synchronously wait for the handshake. So
  there is one new syscall, `SYS_SOCKET_CONNECT`, that
  returns a fresh fd.
- The *streaming* operations are not special — once the
  connection is up, sending bytes is sending bytes. So
  `read()`/`write()` work unchanged; the fd simply dispatches
  to TCP instead of the OSFS or a pipe.
- The *teardown* operation is also not special — `close()`
  on a socket fd already does the right thing if the fd
  knows how to free its underlying object.

The net result is one new fd kind plus three new syscalls,
not the dozen-or-more BSD socket API would suggest.

## Step 1 — `FD_SOCKET`

`enum fd_kind` gets a new entry, and `struct fd_entry` gets
one new field:

```c
enum fd_kind {
    FD_CONSOLE = 0,
    FD_FILE,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_TMPFS_RW,
    FD_SOCKET,      /* TCP socket; tcp_cid in `socket_cid` */
};

struct fd_entry {
    /* ... existing fields ... */
    int  socket_cid;   /* tcp_connect()'s opaque id, or -1 */
};
```

`vfs_init_fdtable` initialises the new field to `-1` so an
fd that's never been a socket doesn't accidentally look like
one. Three other call-sites need a one-line addition each:

- `vfs_close` and `vfs_close_all` learn that for `FD_SOCKET`
  fds they must call `tcp_close(socket_cid)` before clearing
  the slot.
- `vfs_read` learns to dispatch to TCP when the fd kind is
  `FD_SOCKET`.

There is also one new helper:

```c
int vfs_alloc_socket_fd(int cid)
{
    struct thread *t = thread_current();
    for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &t->fds[fd];
        if (!e->in_use) {
            e->in_use     = 1;
            e->kind       = FD_SOCKET;
            e->socket_cid = cid;
            /* ... clear other fields ... */
            return fd;
        }
    }
    return -EMFILE;
}
```

It's the moral equivalent of `dup` for sockets: given an
existing TCP cid, reserve a fresh fd slot in the calling
thread's table that will route reads and writes to that cid.

## Step 2 — `vfs_read` for sockets

The read path is the most interesting because TCP is
*asynchronous* with the calling thread. When userspace calls
`read(fd, buf, n)`, the bytes the user wants might already
be in the receive ring (return immediately), or might not
have arrived yet (block until they do), or may never arrive
because the peer FIN'd already (return 0).

```c
if (e->kind == FD_SOCKET) {
    if (e->socket_cid < 0) return -EBADF;
    for (;;) {
        (void)net_poll();
        int n = tcp_recv(e->socket_cid, buf, len);
        if (n > 0) return n;
        if (n < 0) return -EIO;
        if (tcp_eof(e->socket_cid)) return 0;
        yield();
    }
}
```

Three subtle points:

1. **`net_poll()` is mandatory.** `yield()` reschedules the
   caller but does *not* pump the NIC. If we omit the
   `net_poll` call, segments can sit in the virtio receive
   ring forever and the read spins until the kernel's own
   periodic poll happens to fire. The first version of this
   code lacked the call and `httpget` returned `-EIO` from
   `socket_connect` every time — the SYN+ACK was sitting in
   the ring, but the spin loop never read it.

2. **Spin-yield, not block.** TCP has no scheduler-blocking
   primitives wired up; there is no equivalent of
   `pipe_wait()` for sockets yet. The polling loop is fine
   for now because the caller is the only thread that
   matters, and `yield()` lets the timer-driven preemption
   keep the rest of the system alive.

3. **`tcp_eof` is sticky.** Once the peer has FIN'd and the
   receive buffer is drained, `tcp_eof()` returns true
   permanently. Returning `0` matches POSIX `read()` EOF
   semantics and is what `cat`-style loops expect.

## Step 3 — `sys_write` for sockets

The write side is simpler because there's no asynchrony to
the *caller*: when `tcp_send` returns `n`, those `n` bytes
have been queued for transmission.

```c
if (e->in_use && e->kind == FD_SOCKET) {
    if (e->socket_cid < 0) return -EBADF;
    char chunk[512];
    long total = 0;
    while (total < len) {
        size_t n = (size_t)(len - total);
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (copy_from_user(chunk, ptr + total, n) < 0)
            return -EFAULT;
        long w = tcp_send(e->socket_cid, chunk, n);
        if (w < 0) return total > 0 ? total : -EIO;
        if (w == 0) { (void)net_poll(); yield(); continue; }
        total += w;
    }
    return total;
}
```

The `if (w == 0)` branch is the throttle: when the TX ring
is full, `tcp_send` returns 0 and we pump+yield until the
segmenter has drained some bytes onto the wire. Same
`net_poll()` rationale as the read side — without it the
ring would never drain.

## Step 4 — the three new syscalls

```
SYS_SOCKET_CONNECT  = 60   (uint32_t ip4_be, uint16_t port) -> fd
SYS_SOCKET_STATE    = 61   (int fd)                        -> int
SYS_SOCKET_SHUTDOWN = 62   (int fd)                        -> 0/-errno
```

The chosen numbers (60+) leave the 50s open for more GUI
syscalls without renumbering.

### `sys_socket_connect`

This is the only one that does real work:

```c
static long sys_socket_connect(long ip4_be32, long port)
{
    if (port <= 0 || port > 65535) return -EINVAL_VFS;
    uint8_t ip[4] = {
        (uint8_t)(ip4_be32 >> 24), (uint8_t)(ip4_be32 >> 16),
        (uint8_t)(ip4_be32 >>  8), (uint8_t) ip4_be32,
    };
    int cid = tcp_connect(ip, (uint16_t)port);
    if (cid < 0) return -EMFILE;

    for (uint64_t i = 0; i < 200000000ULL; i++) {
        (void)net_poll();
        int s = tcp_state(cid);
        if (s == TCP_ESTABLISHED) {
            int fd = vfs_alloc_socket_fd(cid);
            if (fd < 0) { tcp_close(cid); return fd; }
            return fd;
        }
        if (s == TCP_CLOSED) {  /* RST */
            tcp_close(cid);
            return -EIO;
        }
        if ((i & 0x3FFu) == 0) yield();
    }
    tcp_close(cid);
    return -EIO;
}
```

A few design notes worth highlighting:

**IP-as-uint32_t.** The natural ABI for an IPv4 address in a
single syscall argument is the four octets packed into a
32-bit big-endian integer. `10.0.2.2` becomes `0x0A000202`.
This avoids plumbing a `uint8_t ip[4]` pointer through the
syscall surface — userspace doesn't have to copy it,
`copy_from_user` doesn't have to validate it, and the kernel
unpacks it with three shifts. When we add DNS, the
"resolver" function will have a `getaddrinfo`-shaped
signature and *its* output will fit the same uint32 ABI.

**Spin-bounded with a coarse timeout.** The 2×10⁸ iterations
work out to roughly the same wall-clock budget as the
kernel's M55 self-test (~5 seconds on this machine).
Crossing that bound returns `-EIO`; userspace can retry or
give up. This is ugly compared to a real `connect()` timeout
parameter, but it's a precondition for not hanging the whole
system if SLIRP refuses or the host firewall drops the SYN.

**`TCP_CLOSED` after `tcp_connect` means RST.** The state
machine moves to `CLOSED` only after the handshake fails
(remote sent RST, or our retransmits ran out). We map that
to `-EIO`, which is what POSIX uses for "connection
refused"-shaped errors when the API doesn't have a richer
errno set.

### `sys_socket_state` and `sys_socket_shutdown`

These are one-line wrappers around `tcp_state` and
`tcp_close`. Worth noting: `sys_socket_shutdown` does *not*
free the fd — only `close()` does. This matters because
HTTP/1.0 callers want the pattern

```c
write(fd, request, n);
shutdown(fd);              /* peer sees EOF, sends response */
while ((r = read(fd, buf, sizeof buf)) > 0) ...;
close(fd);
```

If `shutdown` released the fd, the subsequent `read` calls
would all return `-EBADF`. Splitting close into two phases
(half-close = FIN, full-close = release) is the classic
fix.

## Step 5 — userspace libc wrappers

`userspace/libc/syscall.h` grows three small inlines:

```c
#define IP4(a,b,c,d) ((uint32_t)(((a)<<24)|((b)<<16)|((c)<<8)|(d)))

static inline int socket_connect(uint32_t ip4_be, uint16_t port)
    { return (int)_svc2(SYS_SOCKET_CONNECT, ip4_be, port); }
static inline int socket_state(int fd)
    { return (int)_svc1(SYS_SOCKET_STATE, fd); }
static inline int socket_shutdown(int fd)
    { return (int)_svc1(SYS_SOCKET_SHUTDOWN, fd); }
```

The `IP4` macro is the bridge from human-readable
`IP4(10,0,2,2)` to the wire-format uint32 the syscall
wants. `read()`, `write()`, `close()` need no changes —
they already dispatch on fd kind in the kernel.

## Step 6 — `/bin/httpget`

The tool fits in 130 lines. The core loop is small enough
to quote in full:

```c
int fd = socket_connect(ip_be, port);
if (fd < 0) { printf("connect failed (%d)\n", fd); return 2; }

char req[256];
int n = snprintf_like(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\n"
                     "Connection: close\r\n\r\n", path, ip_str);
if (write(fd, req, n) < 0) { close(fd); return 3; }

unsigned long total = 0;
char buf[512];
for (;;) {
    long r = read(fd, buf, sizeof buf);
    if (r < 0) { close(fd); return 4; }
    if (r == 0) break;
    write(1, buf, r);
    total += r;
}
close(fd);
printf("\n[httpget] received %lu bytes\n", total);
```

Two things worth flagging:

- **No `Connection: keep-alive`.** HTTP/1.0 with explicit
  `Connection: close` means the *server's FIN is our EOF*.
  We don't have to parse `Content-Length` or chunked
  transfer encoding — just read until 0. This is the same
  trick `curl --http1.0` uses with non-keepalive servers.

- **Argv-only configuration.** No environment, no config
  file. `httpget <ip> <port> [path]`. With `path` defaulting
  to `/`, the smoke-test invocation is the seven-character
  `httpget 10.0.2.2 8888`.

## Step 7 — the test harness

`scripts/test_httpget.py` follows the established pattern
(`scripts/test_tcp.py`, `scripts/test_notepad.py`):

1. Start a Python `http.server` on `127.0.0.1:8888` whose
   only behaviour is to return a fixed-byte body containing
   the marker string `M56-HTTPGET-OK-PAYLOAD`.
2. Boot the kernel under SLIRP user-mode networking. SLIRP
   forwards `10.0.2.2:8888` to host's `127.0.0.1:8888`.
3. Wait for the kernel's M55 self-test to finish (this also
   serves as a *liveness check* for the whole network
   stack).
4. Wait for the shell prompt.
5. Send `httpget 10.0.2.2 8888 /m56\n` over the serial
   line.
6. Assert the marker string appears on the guest console.
7. Assert the byte-count summary line appears.

The test passes only if a userspace process opened a real
TCP connection, sent a request, and received a body all
through the M56 syscall surface. There is no shortcut.

## What we deliberately did not do

A few things that look conspicuously absent:

- **No `bind()` / `listen()` / `accept()`.** Server side is
  milestone 57. The browser only needs the client side.
- **No `getaddrinfo()` / DNS.** Hostnames need DNS, DNS
  needs UDP, UDP we have — but a resolver is its own
  protocol implementation. We hand-write IPs for now.
- **No real `select()` / `poll()`.** The spin-yield loop in
  `vfs_read` is good enough for one connection at a time;
  the browser's pipeline parallelism will need real
  multiplexing eventually, but not in this milestone.
- **No `SO_*` options.** No reuseaddr, no nodelay (we
  always immediately send), no buffer-size knobs. The
  defaults are the only configuration.
- **No `errno`-style structured error reporting.** We
  collapse everything that isn't `ESTABLISHED` into `-EIO`.
  Real BSD sockets distinguish `ECONNREFUSED`,
  `ETIMEDOUT`, `ENETUNREACH`, etc.; we don't bother
  because there's nothing useful userspace could do
  differently.

Each of these is a fine future milestone; none of them
block "fetch a URL" semantics.

## What this unlocks

With M56 in place, we have, end-to-end:

```
shell                    /bin/sh
   ↓ spawn
userspace TCP client     /bin/httpget
   ↓ socket_connect / read / write / close
kernel socket fd         FD_SOCKET dispatch in vfs_read / sys_write
   ↓ tcp_recv / tcp_send / tcp_close
TCP state machine        kernel/core/tcp.c
   ↓ ipv4_send_from / rx_handle_ipv4
IP layer                 kernel/core/net.c
   ↓ NIC TX / RX rings
virtio-net               kernel/device/virtio_net.c
   ↓
SLIRP user-mode networking
   ↓
host kernel
   ↓
python http.server
```

That is a real network stack. Eight layers, none of them
faked. The next chapter (passive open) will let us be the
*server* end of this same picture; the chapter after that
will start sketching a hand-rolled HTTP parser to lift
"fetch raw bytes" into "parse a response into headers and
body". From there the road to a browser is short — just a
lot of HTML.

## Verification

```
$ python3 scripts/test_httpget.py
PASS: kernel TCP self-test completed
PASS: shell prompt available
PASS: httpget delivered HTTP body to userspace (M56-HTTPGET-OK-PAYLOAD)
PASS: httpget printed byte count summary

MILESTONE 56 (sockets+httpget): ALL TESTS PASSED
```

And the regression sweep over Part VII still passes:

```
test_virtio_net   M52   PASS
test_net_arp      M53   PASS
test_dhcp         M54   PASS
test_ping         M54   PASS
test_tcp          M55   PASS
test_httpget      M56   PASS
```
