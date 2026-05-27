# Chapter 107 -- /bin/httpd: a static-file HTTP server

Chapter 106 gave us `socket_listen` and `socket_accept`. Chapter
105 spends them. We build `/bin/httpd`, a single-threaded
static-file HTTP/1.0 server that lives entirely in userspace,
serves arbitrary VFS paths, and fits in **about 400 lines of C**
(half of which are comments).

After this chapter you can do this from the guest's shell:

```sh
/$ httpd 8080 &
[httpd] listening on port 8080 (once=0)
/$
```

…and from the host, provided QEMU was booted with a SLIRP
hostfwd that maps a host port onto the guest's 8080:

```sh
# In one terminal -- boots with hostfwd tcp::18080-:8080 baked in
$ make run-graphical

# In another terminal -- the host now has a listener on :18080
$ curl http://127.0.0.1:18080/mnt/hello.txt
hello, world from /mnt!
```

Without the hostfwd, SLIRP silently drops inbound connections
to the guest -- the host sees an immediate connection refused
because nothing on the host is bound to that port. `make run`
(serial only, headless) intentionally stays minimal and does
NOT add the forward. The daily-driver `make run-graphical`
target bakes in `hostfwd=tcp::18080-:8080` so this Just Works.
Override `HTTPD_HOST_PORT` / `HTTPD_GUEST_PORT` on the make
command line if those clash.

The path you GET is interpreted as a VFS path in the guest --
`/mnt/foo.html` opens kernel path `/mnt/foo.html`. There is no
separate "doc root" or rewriting layer. URL = path.

This is the first program in the book where **the kernel is
talking to itself over the network and the user can see it
happen from a real browser**. Chapter 108 will finish that loop
by having our in-tree browser fetch from this in-tree server.

## Prerequisites

- [Chapter 106 -- accept and server sockets](106-accept-and-server-sockets.md):
  the syscalls and libc wrappers `socket_listen` /
  `socket_accept`; the FD_SOCKET_LISTEN fd kind.
- [Chapter 63 -- socket syscalls and httpget](../07-networking/063-socket-syscalls-and-httpget.md):
  the client-side counterpart. Our server has the same shape,
  just with the read/write directions swapped.
- [Chapter 20 -- the OSFS mount at /mnt](../05-devices/020-osfs-and-mount.md):
  where the file content actually comes from.

## How small can an HTTP server be?

The accept loop itself is **fourteen lines**:

```c
int lfd = socket_listen(port, 4);
if (lfd < 0) { printf("listen failed\n"); return 1; }
printf("httpd: listening on port %d\n", (int)port);

for (;;) {
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int cfd = socket_accept(lfd, &peer_ip, &peer_port);
    if (cfd < 0) { printf("accept failed\n"); break; }
    handle_one(cfd, peer_ip, peer_port);
    close(cfd);
    if (once) break;
}
close(lfd);
```

Every line is doing exactly one thing: binding the port,
harvesting peers, dispatching, tearing down. The interesting
code is all inside `handle_one` -- read a request line, parse
it, serve the response -- and that's the part we'll dwell on.

## Why HTTP/1.0 + Connection: close

This is the most important design decision in the chapter.
A more ambitious server would speak HTTP/1.1 with keep-alive,
chunked transfer encoding, and pipelining. We deliberately ship
**none** of that. Here's why each one is deferred.

### Keep-alive needs a request loop, not just a connection loop

A keep-alive connection serves N requests before closing. The
server has to:

1. read one request,
2. send the response,
3. **rewind** -- read another request on the same fd,
4. send another response,
5. ...until the client closes or some timeout fires.

That changes the entire structure: `handle_one(cfd)` becomes
`while (read_one_request(cfd)) serve(...)`. It also means the
server has to know exactly where each response ends -- you
can't rely on close-delimited framing any more, because the
connection isn't closing. That's where Content-Length and
chunked encoding come in.

By picking **HTTP/1.0 + Connection: close**, framing is
implicit: the body is "every byte until I close the socket."
The client (which already understands this dialect via
`libc/http.h`) drains until peer FIN. We never have to know
the file size before sending the headers.

### Content-Length requires either fstat() or buffering

To send `Content-Length: N` you need to know N before you've
written the first byte of the body. Our kernel has no
`fstat()` syscall yet (chapter 120+ territory). The
alternatives are:

- **Buffer the whole file in userspace memory**, then send
  the length. Caps the max servable size at whatever the
  user-heap can hold (today, ~64 MiB).
- **Walk `listdir_at` over the parent directory** to find
  the file's `size_out`. Adds a directory scan to every
  request. Doesn't generalise to `/proc` or `/data` (where
  sizes are dynamic).
- **Stream chunked.** Requires HTTP/1.1 framing.
- **Don't send Content-Length, rely on close.** That's
  HTTP/1.0 + Connection: close. RFC 1945 section 5.3
  explicitly endorses it for response bodies.

Option four is the simplest and the most correct given what
we have. We pick it.

### Concurrency: one peer at a time

Our daemon serves one connection, finishes, then loops to
accept the next. A real server uses one of:

- **One thread per connection** (Apache prefork).
- **One process per connection** (CGI-era hostservers).
- **An event loop** (nginx, Node).

We don't have multi-threaded userspace cleanly enough yet
(chapter 92 added futexes but not a thread-pool primitive),
fork-with-shared-sockets is explicitly **not** supported (see
chapter 106), and our kernel doesn't have `epoll` or `kqueue`.
So we serialise. For a workload that's "two browser tabs
hitting localhost" this is fine; for production it'd be a
disaster. Chapter 107 is the simplest correct version, not
the fastest.

## URL = path: no doc root

Every static-file server has to answer "given the URL the
client typed, which file on disk should I open?" The usual
answer is **doc root**: a configured prefix that's prepended
to the URL. So `DOCROOT=/srv/www` plus a request for `/index.html`
opens `/srv/www/index.html`.

We don't do that. The URL **is** the kernel path. `GET /mnt/foo.html`
opens `/mnt/foo.html`. `GET /data/notes/today.md` opens
`/data/notes/today.md`. There is no rewriting.

The reasons are:

- **The VFS already namespaces everything.** `/mnt` is the
  read-only baked image, `/data` is the writable mount,
  `/proc` is procfs, etc. The URL inheriting that structure
  is the most natural mapping.
- **No configuration.** The server has zero command-line
  knobs except the port (and `--once`). Anyone reading the
  code sees the whole protocol.
- **It composes with chapter 101's procfs.** A future hack
  could `curl http://localhost:8080/proc/cpuinfo` from the
  host. We get that for free.

The tradeoff is path-traversal risk: if you just `open(target)`
on whatever the client sent, `GET /../../etc/passwd` escapes
out of any intended directory. We handle this with explicit
component-by-component validation rather than `realpath()`
canonicalisation:

```c
static int path_is_safe(const char *p)
{
    if (p[0] != '/') return -1;
    /* For each '/'-delimited segment: */
    /*   - empty segment ("//"), reject (except trailing)  */
    /*   - segment == "..", reject                         */
    /*   - non-[A-Za-z0-9._-] character, reject            */
    ...
}
```

The character allowlist is strictly safer than what Apache
defaults to. Specifically: no `%`-decoding, no `+`, no `~`,
no Unicode. Our test corpus is ASCII; this rule doesn't
restrict any legitimate request we'd want to serve.

## The request parser

The HTTP/1.0 request line is the only thing we need to parse:

```
METHOD SP TARGET SP HTTP/VERSION CRLF
Header: value CRLF
Header: value CRLF
CRLF
```

The headers go into the void -- we don't condition any
behaviour on a header, so reading them is purely about
draining bytes until we hit the blank line that ends the
request block. The reason we **must** drain past the blank
line is that some clients won't accept the response until
they've finished sending the request. (Curl on macOS does
this if you give it a body; even though our GET has no body,
it's good hygiene.)

The parser fits on one screen:

```c
static int read_request(int cfd, char *target, size_t tcap,
                        char *method_out)
{
    char buf[HTTPD_REQ_CAP];   /* 2 KiB */
    size_t off = 0;
    int saw_blank_line = 0;

    /* Pump bytes until we see "\r\n\r\n" or fill the buffer. */
    while (off < sizeof(buf) - 1) {
        long n = read(cfd, buf + off, sizeof(buf) - 1 - off);
        if (n <= 0) return -400;
        off += (size_t)n;
        for (size_t i = 3; i < off; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n' &&
                buf[i-1] == '\r' && buf[i  ] == '\n') {
                saw_blank_line = 1; break;
            }
        }
        if (saw_blank_line) break;
    }
    if (!saw_blank_line) return -400;
    /* ...parse method/target/version from buf[0..]...     */
}
```

Two things worth flagging:

**The scan-from-zero on every iteration.** It's not optimal
(O(N^2) on a worst-case 2 KiB request), but the constant is
tiny and the request fits in one packet 99% of the time. The
loop runs at most once for normal traffic; the scan-from-zero
is a one-time cost. Optimising this is on the same shelf as
"writing a real lexer for HTTP" -- worth it when we have a
production workload, premature now.

**The HTTP/0.9 problem.** Clients before HTTP/1.0 sent just
`GET /path\r\n` with no version and no headers. We refuse those
with `-400`. RFC 1945 says we *should* accept them; we choose
not to because the asymmetry would mean writing two response
paths (one with HTTP/0.9 = body-only, one with headers).
Nothing in our test corpus or the real world still sends 0.9.

## The response

```
HTTP/1.0 200 OK\r\n
Server: osdev/1.0\r\n
Connection: close\r\n
Content-Type: text/html; charset=utf-8\r\n
\r\n
<body bytes streamed in 1 KiB slices>
```

That's it. No `Date:`, no `Last-Modified:`, no `ETag:`, no
`Cache-Control:`. Each of those is a real future addition,
not a TODO -- and not one of them is needed for the
chapter-106 end-to-end loop test.

Status codes we emit:

- **200 OK** -- file served successfully.
- **400 Bad Request** -- malformed request line, or path
  failed `path_is_safe`.
- **404 Not Found** -- `open()` returned `-ENOENT`.
- **405 Method Not Allowed** -- method other than GET / HEAD.
- **500 Internal Server Error** -- `open()` failed with
  something other than `ENOENT` (out of memory, etc).

The 405 list is interesting: we accept GET and HEAD but
nothing else. POST / PUT / DELETE all 405. HEAD is **almost
free** to support -- the server still has to `open()` the
file (otherwise it'd lie about 200 on a missing file), but it
skips the body-streaming loop:

```c
send_status_line(cfd, 200, "OK");
send_common_headers(cfd, content_type_for(path));
if (method == 'H') { close(fd); return 0; }  /* HEAD short-circuit */
/* ...body streaming loop... */
```

## Content-Type sniffing

Extension-based table, first match wins:

```c
if (s_endswith_ci(path, ".html")) return "text/html; charset=utf-8";
if (s_endswith_ci(path, ".css" )) return "text/css; charset=utf-8";
if (s_endswith_ci(path, ".txt" )) return "text/plain; charset=utf-8";
if (s_endswith_ci(path, ".png" )) return "image/png";
if (s_endswith_ci(path, ".jpg" )) return "image/jpeg";
if (s_endswith_ci(path, ".svg" )) return "image/svg+xml";
if (s_endswith_ci(path, ".json")) return "application/json";
/* ...and a few more... */
return "application/octet-stream";
```

The unknown-extension default is `application/octet-stream`,
which Apache also picks. Browsers will offer a download for
that type rather than try to render it.

**Why not real magic-byte sniffing?** Because we have neither
a `libmagic` nor a need. The chapter-106 end-to-end loop
fetches our own files; we know what's in them. A real CDN
absolutely should do content-sniffing (otherwise misconfigured
sites bleed XSS); we are not a CDN.

## /bin/httpd in 14 lines

The whole accept-and-serve loop, dropping comments:

```c
int lfd = socket_listen(port, 4);
printf("httpd: listening on port %d\n", port);
for (;;) {
    uint32_t ip; uint16_t pp;
    int cfd = socket_accept(lfd, &ip, &pp);
    if (cfd < 0) break;
    handle_one(cfd, ip, pp);
    close(cfd);
    if (once) break;
}
close(lfd);
printf("httpd: done\n");
```

Compare that to the **two** new syscalls it relies on, both
new in chapter 106:

- `socket_listen(port, backlog)` -> listening fd
- `socket_accept(lfd, *ip, *port)` -> connected fd

Everything else (read, write, open, close, printf) was already
in our libc by chapter 55. The new code budget is the request
parser, the response writer, and the content-type table.

## The test: `scripts/test_httpd.py`

```python
1.  Boot QEMU with hostfwd tcp::18080-:8080.
2.  Wait for the shell prompt.
3.  Send "httpd 8080 --once\n" to the serial line.
4.  Wait for "httpd: listening on port 8080".
5.  Dial 127.0.0.1:18080 from the host.
6.  Send "GET /mnt/hello.txt HTTP/1.0\r\nHost:..\r\n\r\n".
7.  Read until peer FIN.
8.  Assert status == 200.
9.  Assert Content-Type contains "text/plain".
10. Assert Connection: close header present.
11. Assert body equals on-disk /assets/osfs/hello.txt byte-for-byte.
12. Wait for "[httpd] ... GET /mnt/hello.txt -> 200" log line.
13. Wait for "httpd: done" (clean exit after --once).
```

All 8 assertions pass (we collapse 8-13 into the visible PASS
list; the count is 8 PASS lines printed on stdout). The end-to-
end pipe is:

```
   host socket()        SLIRP hostfwd       guest virtio-net
   curl-like GET   --------------------->   net_rx -> tcp_handle
                                                 |
                                                 v
                                            accept queue gains entry
                                                 |
                                                 v
                                            socket_accept syscall
                                                 |
                                                 v
                                            httpd's read_request
                                                 |
                                                 v
                                            open("/mnt/hello.txt")
                                                 |
                                                 v
                                            httpd's write loop
                                                 |
                                                 v
                                            virtio-net TX
                                                 |
                                                 v
   host recv()  <--------------------- SLIRP <--/
```

Every box in that chain shipped in an earlier chapter
(virtio-net = ch52, tcp_handle = ch55, OSFS open = ch12). The
last new piece -- `socket_accept` -- arrived in chapter 106.
Chapter 107 is just the glue program that lights them up
in sequence.

## Lesson: HTTP/1.0 + close is a complete protocol

It's easy to feel that we're "cheating" by not implementing
keep-alive, chunked encoding, or Content-Length. We're not.
**HTTP/1.0 with `Connection: close` and no Content-Length is a
complete, RFC-compliant framing** -- the body extends to TCP
FIN, the client knows it, the spec endorses it, and every
HTTP client in the world handles it correctly.

The win for our codebase is that the server has **no
per-request state machine** beyond "read request, write
response, close." A keep-alive server would have to:

- track how many requests have used this connection,
- decide when to close,
- handle a peer that pipelines while we're still writing,
- timeout idle connections.

That's four state machines we just don't have to write. The
cost is one TCP handshake per request, which over loopback
(SLIRP, not even a real NIC) is microseconds. The HTTP/1.0
+ close dialect is the right tool for **this workload right
now**, full stop.

## Lesson: path safety as an allowlist

The first thing every "small HTTP server" tutorial gets wrong
is path traversal. The standard wrong pattern is:

```c
if (strstr(target, "..")) return 400;
```

…which is **not safe**: it allows `/foo./bar` (legitimate) but
also rejects nothing about `%2e%2e/` (encoded `..`), `....//`
(double-dot bypass), `/foo/./../bar` (a `..` segment that
isn't lexically `..`), or absolute paths that escape via
symlinks.

Our rule is the opposite: **enumerate exactly what's allowed
per segment, reject everything else**. The allowlist is
`[A-Za-z0-9._-]`. No `/`, `\`, `%`, `~`, `?`, NUL, space, or
any control byte gets through. The `..` reject is on the
already-decoded segment, not on the substring -- so even if a
future hack added URL decoding, the check would still fire on
the right unit.

This is the same pattern that nginx's `internal_redirect` uses
under the hood. It's the only path-safety check that survives
weird inputs.

## Lesson: open() is the dispatch

The biggest thing this chapter has going for it is that
`/mnt`, `/data`, `/proc`, etc. are **already** VFS paths. The
server doesn't have to know which mount type it's reading
from -- it calls `open()` and the VFS dispatches into OSFS,
tmpfs, procfs, or whatever else.

A future addition: `httpd` will serve content from `/proc`
without a single line of code change. `curl
http://localhost:8080/proc/uptime` will Just Work the moment
procfs is reachable from the OSFS root.

This kind of compositional power is the payoff for designing
the VFS as a tree of mounts (chapter 8). Each new mount type
becomes a new "thing httpd can serve" for free.

## Files changed

Userspace:

- `userspace/httpd/httpd.c` -- the whole daemon, ~400 lines.

Build and test:

- `Makefile` -- HTTPD_OBJS / HTTPD_ELF / HTTPD_STRIPPED, link
  rule, strip rule, OSFS bundling, mkosfs invocation. Also
  bakes `hostfwd=tcp::18080-:8080` into the `run-graphical`
  target so the host can curl into the guest's httpd without
  a separate convenience target.
- `scripts/test_httpd.py` -- end-to-end test.
- `book/INDEX.md` -- updated.

No kernel changes. All of httpd's needs were already covered
by chapter 106's syscall surface plus the existing
`open` / `read` / `write` / `close`.

## What's deferred

- **Keep-alive** (HTTP/1.1 + Connection: keep-alive). Real
  performance win when you're serving many small files from
  one origin, but our workload is fetching one HTML page per
  navigation -- the TCP handshake cost is irrelevant.
- **Content-Length**. Needs `fstat()` (chapter 120+) or
  buffering. Doesn't change client-visible behaviour for our
  test suite.
- **Range requests** (HTTP/1.1 `Range:` header). Useful for
  video streaming and resumable downloads; we have neither.
- **POST / PUT / DELETE**. Adds a body parser and writable
  output handlers. Out of scope; our `/data` mount is the
  only writable spot and we don't expose it that way.
- **Multi-connection serving**. Single-threaded is enough.
- **TLS**. Real TLS needs either BearSSL or a host-side
  proxy. Chapter 123 will pick one.
- **Logging beyond the per-request line**. No log file, no
  access-log format. The serial line is the log.
- **Virtual hosts / Host: routing**. We have one origin.

## What this unlocks

- **Chapter 108 -- end-to-end loop**. With httpd serving
  files out of `/mnt` and the in-tree browser able to dial
  the guest's IP, we can run the entire network + browser
  stack against itself in one boot. The browser fetches an
  HTML page from `http://10.0.2.15:8080/mnt/test.html`, the
  kernel handles both sides of the TCP connection, the
  rendered DOM lights up the GPU.
- **Future devmode dashboards**. Anything we want to expose
  to the host (kernel-internal stats, perf counters, the
  process table) can be served as a `/proc/...` text file
  through this server, with zero new code.
