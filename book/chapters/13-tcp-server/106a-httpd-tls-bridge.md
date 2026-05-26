# Chapter 106a — httpd as a forwarding proxy (TLS bridge)

> **Milestone in this chapter:** 96 — httpd as a forwarding proxy.
> **Code referenced:**
> - [userspace/httpd/httpd.c](../../../userspace/httpd/httpd.c)
>   (the new proxy dispatch)
> - [scripts/https_proxy.py](../../../scripts/https_proxy.py)
>   (the host-side TLS terminator)
>
> **At the end of this chapter** `/bin/httpd` has two jobs: it serves
> static files out of `/mnt`, `/data`, and `/proc` as it always has,
> *and* it forwards any request whose path is not a local VFS prefix
> to a configurable upstream proxy, splicing the response straight
> back to the client. The middle stays dumb — httpd neither parses
> the response nor knows TLS exists.

Chapter 105 gave `/bin/httpd` exactly one job: serve files out of
the VFS. This chapter teaches it a second job: when a request comes
in for a path that *isn't* a local VFS prefix, forward the request
to a configurable upstream proxy (default: the host's
[`scripts/https_proxy.py`](../../../scripts/https_proxy.py)) and
splice the response back to the client. The middle stays dumb —
httpd neither parses the response nor knows TLS exists.

The motivation is downstream: chapter 106b wants the browser's
`BROWSER_PROXY` to point at the in-guest httpd instead of the host
proxy. For that to work, **httpd has to act as a proxy itself for
the cases where it isn't acting as a static-file server.** This
chapter adds that dispatch and ships about 90 new lines of C.

Everything here is userspace. The kernel doesn't change at all —
the chapter-93 file-descriptor model already lets one process hold
an inbound and an outbound TCP fd at the same time, and
`socket_connect`, `socket_listen`, `socket_accept`, and
`socket_shutdown` are all in place from Part VII.

## Prerequisites

- [Chapter 105 -- /bin/httpd](105-bin-httpd.md) -- the
  static-file server we're extending.
- [Chapter 106 -- TCP loopback](106-tcp-loopback.md) -- so
  the eventual chapter-106b setup (browser dials
  `127.0.0.1:8080`, httpd dials `10.0.2.2:8080`) doesn't
  share a netdev between the two TCP conns.
- [Chapter 58 -- URL and HTTP parser](../07-networking/66-url-and-http-parser.md)
  -- describes the host proxy
  [`scripts/https_proxy.py`](../../../scripts/https_proxy.py)
  which was originally written to let the chapter-58
  `httpget` reach HTTPS sites without a TLS stack.
- [Chapter 33 -- env vars and PATH](../05-devices/33-env-vars-and-path.md)
  -- `HTTPD_UPSTREAM` reads through `getenv`.

## The dispatch

The chapter-105 `handle_one` was three steps: read request,
validate path, serve. Chapter 106a inserts ONE branch in
the middle:

```c
if (is_local_path(target)) {
    if (path_is_safe(target) < 0) { send_error(cfd, 400, ...); return; }
    long n = serve_get(cfd, target, method);          /* ch 105 */
    log_request(peer_ip, peer_port, "local",   ..., n);
} else {
    long n = serve_forward(cfd, raw, raw_len);        /* ch 106a */
    log_request(peer_ip, peer_port, "forward", ..., n);
}
```

`is_local_path` is the obvious table:

```c
static int is_local_path(const char *target)
{
    return s_starts_with(target, "/mnt/")  ||
           s_starts_with(target, "/data/") ||
           s_starts_with(target, "/proc/");
}
```

These are exactly the three VFS prefixes the kernel mounts
today. Add a new mount type (a future `/var/`, say) and you
extend this table; you don't touch `serve_get`, because the
VFS already dispatches inside `open()`.

`path_is_safe` is now scoped to the local branch only.
That's the single subtle reasoning step in the chapter:
**the forward path never calls `open()`**. Path traversal
protects against escaping a doc root; we have no doc root
on the forward path. The whole `target` becomes part of an
opaque request bytestream forwarded to a trusted upstream
we deployed ourselves. The upstream gets to decide what it
allows.

## `serve_forward`: splice in 45 lines

```c
static long serve_forward(int cfd, const char *req_buf, size_t req_len)
{
    int up = socket_connect(g_upstream_ip, g_upstream_port);
    if (up < 0) {
        send_error(cfd, 502, "Bad Gateway");
        return -502;
    }
    if (write_all(up, req_buf, req_len) < 0) {
        close(up);
        send_error(cfd, 502, "Bad Gateway");
        return -502;
    }
    (void)socket_shutdown(up);

    char chunk[HTTPD_SEND_CHUNK];
    long total = 0;
    for (;;) {
        long n = read(up, chunk, sizeof(chunk));
        if (n <= 0) break;
        if (write_all(cfd, chunk, (size_t)n) < 0) break;
        total += n;
    }
    close(up);
    return total;
}
```

Four moving parts:

1. **`socket_connect(g_upstream_ip, g_upstream_port)`** --
   the same syscall `httpget` and `browser` use to dial
   out. Returns either a fresh fd or a negative errno. On
   failure we emit `502 Bad Gateway` to the client, which
   is the only HTTP response `serve_forward` ever generates
   on its own. Everything else passes through as bytes.

2. **Verbatim request replay.** We send `req_buf[0..req_len)`
   exactly as we received it from the client: same method,
   same headers, same casing, same `Host:` value. The
   upstream proxy gets to decide what to do with each
   header. We don't rewrite, we don't add `Via:`, we don't
   strip `User-Agent`. Every rewrite is a future
   regression risk.

3. **`socket_shutdown(up)`** sends FIN on the request half
   so the upstream knows the request is complete. Without
   this, an HTTP/1.1 upstream might wait for a body that
   isn't coming, hold the conn open expecting pipelining,
   and stall serve_forward's drain loop. With it, the
   upstream sees end-of-request and starts streaming the
   response immediately.

4. **The splice loop.** Read from `up`, write to `cfd`,
   until `up` reads 0 (FIN). The chapter-106 `vfs_read`
   fix matters here: read on a socket that has been fully
   closed and drained returns `0`, not `-EIO`, so the
   loop terminates cleanly. (Linux's `splice(2)` syscall
   would let us do this without ever copying through user
   memory; we don't have that primitive yet, and a 1 KiB
   bounce buffer is fast enough.)

## Why the upstream is configurable

`HTTPD_UPSTREAM=host:port` overrides the default
(`10.0.2.2:8080`). Three reasons to support it:

- **The default is fragile.** `10.0.2.2` is SLIRP's
  gateway-of-the-guest magic IP. The day we move from QEMU
  user-mode networking to a real bridge (or a different
  emulator), that IP becomes meaningless.
- **Testing.** The chapter's regression script
  ([`scripts/test_httpd_forward.py`](../../../scripts/test_httpd_forward.py))
  spins up a tiny Python `http.server` on a fixed host
  port and points the guest's `HTTPD_UPSTREAM` at it.
  Hermetic: no real internet, no `https_proxy.py` running.
- **Loopback dev mode.** A future "browser self-test"
  could set `HTTPD_UPSTREAM=127.0.0.1:9090` to chain
  httpd into a second in-guest service. The chapter-106
  loopback short-circuit makes that work.

The parser accepts both IPv4 dotted-quad (the fast path)
and hostnames (resolved via the chapter-57 `resolve`
syscall):

```c
static int parse_upstream(const char *spec,
                          uint32_t *out_ip, uint16_t *out_port)
{
    /* Find LAST ':' so future [::1]:port input fails cleanly. */
    int colon = -1;
    for (int i = 0; spec[i]; i++) if (spec[i] == ':') colon = i;
    /* host: copy out spec[0..colon] (or whole string if no colon). */
    /* IP: parse_dotted first; resolve() if that fails. */
    /* port: optional -- if no colon, keep the caller's default. */
}
```

The "no colon" branch is deliberate: `HTTPD_UPSTREAM=10.0.2.5`
keeps port 8080. This matches how `curl --proxy` and ssh
config files behave.

## Be a dumb pipe

This is the design lesson of the chapter. `serve_forward`
goes out of its way to know as little as possible. It
doesn't:

- parse the HTTP response status line,
- inspect any header,
- compute or trust `Content-Length`,
- decode `Transfer-Encoding: chunked`,
- reframe the body in any way,
- log per-request metrics beyond byte count,
- cache anything,
- handle any retry on upstream failure.

Each of those is a real future addition, and each one
breaks the "splice" property in a different way. Once we
parse the response we have to handle malformed responses.
Once we cache, we have to handle cache invalidation. Once
we retry, we have to handle idempotency. The splice loop's
power is exactly that it sidesteps all of those questions.

Why is this a common architecture?

- **SSH proxy jump** (`ssh -J`) -- the jump host runs a
  TCP forwarder, doesn't parse SSH.
- **nginx `proxy_pass`** -- defaults to "splice this
  upstream response back to the client" mode; rewriting is
  opt-in.
- **Envoy / Istio sidecars** -- terminate TLS, splice the
  decrypted bytes onward.
- **macOS `transparent` HTTP proxy** -- splice + rewrite
  destination IP.
- **Docker's HTTP-over-Unix-socket forwarders** -- splice
  client bytes into a `unix://` socket.

All of them are splice loops. All of them treat the
protocol as opaque on the wire and let the endpoints
negotiate framing. Our `serve_forward` is the simplest
honest version of that pattern.

## Three FINs, one chain

The framing works end to end because every link uses
`Connection: close`:

```
client  -- FIN -->  httpd  -- FIN -->  upstream
client  <-- FIN --  httpd  <-- FIN --  upstream
```

The client sends its request, half-closes (or sends an
HTTP/1.0 request which is implicitly half-closing). httpd
replays the request, then `socket_shutdown(up)` sends FIN
upstream. The upstream serves its response, then closes
its end. httpd's `read(up)` returns 0 -- that's the loop
exit -- and `handle_one` returns. `main`'s loop then
calls `close(cfd)`, which sends FIN to the client. The
client's `read()` returns 0 (the same chapter-106 fix that
made loopback work) and the conversation is over.

No single layer knows the body length. No layer needs to.

## Why this comes after loopback

`serve_forward` makes a fresh outbound TCP connection from
inside httpd's per-request handler. With chapter 106's
loopback in place, the inbound and outbound 4-tuples live
in completely separate netdevs (lo0 vs virtio-net) when
the upstream happens to be local. Without loopback, both
would share virtio-net's single TX queue -- and any stall
on either half blocks the other.

For the **current** default (`HTTPD_UPSTREAM=10.0.2.2:8080`,
which crosses SLIRP NAT to the host), the issue doesn't
arise because the outbound traffic goes to a real
non-loopback destination. But chapter 106b will repoint
the browser to `BROWSER_PROXY=http://127.0.0.1:8080/`,
and at that point the proxy's outbound conn to
`10.0.2.2:8080` and the inbound conn from `127.0.0.1`
share neither queue nor 4-tuple namespace. That separation
is exactly what chapter 106 bought us.

## The 4 KiB request cap

Chapter 105 sized `HTTPD_REQ_CAP` at 2 KiB, on the
reasoning that our test-harness GETs are short. Chapter
106a bumps it to 4 KiB because the forward path replays
whatever the client sent us, and real-world HTTP clients
are chattier than our test harness. A `curl` with the
default headers sends ~300 bytes; a Chrome request with
all its cookies and `Sec-*` headers can hit 1.5 KiB; a
Firefox request with a large `Cookie:` header can easily
push past 2 KiB. The 4 KiB cap is generous without being
wasteful (the buffer lives on the per-connection stack
frame, and we serve one connection at a time).

The cap is enforced silently: if the request doesn't end
before the buffer fills, `read_request` returns `-400`
and the client gets `Bad Request`. That's the same
behaviour chapter 105 had; we just chose a different
boundary.

## The test: `scripts/test_httpd_forward.py`

The regression has two phases, ONE QEMU boot, TWO httpd
runs:

```
Phase A -- forward path (chapter 106a)
  1.  Spawn Python http.server on 127.0.0.1:18082 returning
      "M96-FORWARD-OK-PAYLOAD|path=<requested-path>\n".
  2.  Boot QEMU with hostfwd tcp::18081-:8081 + outbound NAT.
  3.  Wait for shell prompt.
  4.  Send "export HTTPD_UPSTREAM=10.0.2.2:18082\n".
  5.  Send "httpd 8081 --once\n".
  6.  Wait for "httpd: listening on port 8081".
  7.  Assert "forward upstream 10.0.2.2:18082" line printed.
  8.  curl-style GET http://127.0.0.1:18081/upstream/news.yc...
  9.  Assert 200 OK + body == "M96-...|path=/upstream/...".
 10.  Assert serial log contains "forward GET <path> -> 200".
 11.  Wait for "httpd: done".

Phase B -- local path regression (chapter 105)
 12.  Send "httpd 8081 --once\n" (same env).
 13.  GET http://127.0.0.1:18081/mnt/hello.txt.
 14.  Assert 200 OK + body == on-disk /assets/osfs/hello.txt.
 15.  Assert serial log contains "local GET /mnt/... -> 200".
 16.  Wait for "httpd: done".
```

12 PASS lines total. The body-equality check in step 9 is
the strongest assertion: it proves the request bytes
arrived at upstream verbatim (including the exact path),
AND it proves the response bytes arrived at the test
client verbatim. Either rewriting or truncation would
make this check fail.

The Phase B regression matters because the chapter's only
code change was inserting an `if/else` in `handle_one`.
That's exactly the kind of change that's easy to break
silently -- e.g. forgetting to call `path_is_safe` in the
local branch, or accidentally falling through to the
forward branch for a `/mnt/...` path. Phase B catches
both.

## A note on security

The chapter ships **no** authentication or access control
on the forward path. Anyone who can reach the guest's
port 8081 can make httpd connect to whatever
`HTTPD_UPSTREAM` is. This is fine because:

- The guest's network is QEMU SLIRP, NAT'd to the host's
  loopback only.
- The host's port 18081 is bound to `127.0.0.1` by SLIRP's
  default `hostfwd` semantics (no external interface).
- The upstream is something the developer chose.

The day we connect this to a real network -- bridged
networking, a TAP device, or running on hardware -- the
analysis changes. A real production-shaped fix would be:

- Bind only on `127.0.0.1` in the guest.
- Require an auth token in a header (the proxy splices
  the rest of the conn, the auth check fails fast).
- Whitelist allowed upstream destinations.

None of those are chapter-106a work. They're chapter-15X
work when we revisit the security story.

## Applied to / what gets exercised in tests

Per the project's "apps must use the OS features" discipline:

- **Modified app:** [`userspace/httpd/httpd.c`](../../../userspace/httpd/httpd.c)
  gains the chapter-106a forward path. ~90 new lines, all
  in one translation unit, no new files.
- **No new app:** the new capability is wired into the
  existing daemon. A standalone proxy binary would be
  pure ceremony.
- **New test:** [`scripts/test_httpd_forward.py`](../../../scripts/test_httpd_forward.py)
  exercises both the new forward path AND the chapter-105
  local-path regression in one QEMU boot.
- **Existing tests unchanged:** [`scripts/test_httpd.py`](../../../scripts/test_httpd.py)
  still passes (local-path behaviour is identical from
  the client's perspective; the only visible change in
  the serial log is the per-request line now says
  `local GET ...` instead of `GET ...`). The sweep
  re-verified this.
- **New env var:** `HTTPD_UPSTREAM=host:port`. Demo: from
  the shell, `export HTTPD_UPSTREAM=10.0.2.5:9090 && httpd 8080`
  and watch the listen line print the new upstream address.

## Files changed

- [`userspace/httpd/httpd.c`](../../../userspace/httpd/httpd.c)
  -- 4 new helpers (`s_starts_with`, `s_parse_dotted`,
  `parse_upstream`, `load_upstream_from_env`), 2 new
  globals (`g_upstream_ip`, `g_upstream_port`), 1 new
  request-handler (`serve_forward`), `read_request`
  reshaped to expose raw bytes, `handle_one` rewritten as
  a dispatcher. `HTTPD_REQ_CAP` bumped 2 KiB -> 4 KiB.
- [`scripts/test_httpd_forward.py`](../../../scripts/test_httpd_forward.py)
  -- new regression. Picked up automatically by
  [`scripts/sweep.sh`](../../../scripts/sweep.sh)'s
  `test_*.py` glob.
- [`book/INDEX.md`](../../INDEX.md) -- milestone-96 row
  flipped from Planned -> Done.

No kernel changes. Build system unchanged.

## What this unlocks

- **Chapter 106b** -- the browser's `BR_DEFAULT_PROXY`
  flips from `http://10.0.2.2:8080/` to
  `http://127.0.0.1:8080/`. The "TLS lives on the host"
  knowledge vanishes from the browser; it dials a stable
  in-guest address that just works.
- **Chapter 106c** -- the end-to-end celebration. Two of
  our own programs (browser + httpd) talking over
  loopback, with httpd transparently bridging to the
  host's HTTPS proxy for outbound TLS.
- **Future devmode dashboards** -- any tool that wants
  to expose data to the host (browser, future curl-shaped
  command-line client) can hit `127.0.0.1:8080`
  uniformly. Local paths serve files, non-local paths
  bridge out. One door.
