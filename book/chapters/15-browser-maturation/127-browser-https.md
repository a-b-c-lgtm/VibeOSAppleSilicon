# Chapter 127 — Browser https:// (native TLS)

Chapter 126 built a chain-validating TLS client and proved it
against `/bin/httpsd` from a standalone binary (`tlstest
--handshake-ca`). Until this chapter the only way the *real*
browser could fetch an `https://` URL was through the host-side
Python TLS-terminating proxy from chapter 109 — a development
prop that lived outside the guest and silently rewrote URLs
through `BROWSER_PROXY`. Chapter 127 wires the chapter-126
client into `/bin/browser` itself, retires the host proxy from
the regression boot path, and demonstrates an end-to-end
guest-only HTTPS pipeline: argv → URL parse → loopback resolve
→ TCP → BearSSL handshake → HTTP/1.1 GET → chunked drain →
tokeniser → DOM → layout → plain-text render → stdout.

The kernel and the network stack do not change. Every new line
of code lives in `userspace/browser/browser.c`, in a single
new abstraction.

## What ships

- `userspace/browser/browser.c` — adds a five-function
  `br_conn_t` abstraction that wraps either a raw TCP fd or a
  heap-allocated `tls_socket_t`. The two transports share a
  common `write / read / close` surface so the rest of the
  fetch path (`drain_conn`, the HTTP request builder, the
  response splitter) sees no difference between `http://` and
  `https://`. Adds a literal-`localhost` short-circuit so the
  loopback test doesn't need a real DNS server.
- `userspace/browser/browser.c` — `canonicalize_url` case (3)
  now lets `https://` URLs through to the native TLS path by
  default, and only rewrites them through `BROWSER_PROXY` if
  the environment variable was explicitly set at startup
  (`g_proxy_was_set`). This preserves every existing caller
  that does `setenv("BROWSER_PROXY", …)` before spawning the
  browser (`proxytest`, `test_browser_hn_*`) while making the
  bare browser binary use real TLS.
- `Makefile` — `BROWSER_OBJS` grows three members
  (`tls_socket.o`, `test_chain.o`, `cstring.o`), the
  `BROWSER_ELF` link rule pulls `libbearssl.a` inside the
  `--start-group … --end-group` window the linker uses for
  resolving the dense BearSSL symbol graph, and a per-file
  pattern rule supplies `BEARSSL_INC` when compiling
  `browser.o` (the umbrella `bearssl.h` header isn't on the
  default user include path).
- `scripts/test_browser_https.py` — boots the kernel, drives
  `browser https://localhost:8443/` from the shell, asserts the
  four progress signals: shell prompt, in-guest `httpsd`
  visible in the boot log, the new chapter-127 TLS-OK line
  from the browser, and the decrypted response tokens reaching
  the renderer.
- `book/chapters/15-browser-maturation/127-browser-https.md`
  (this file) and the corresponding `book/INDEX.md` rows.

`scripts/https_proxy.py`, the host-side TLS terminator from
chapter 109, stays in the tree (debug scripts in this
repo are never deleted, even when no regression script
still drives them) but no regression script
launches it any more. The browser no longer needs it.

## The br_conn_t abstraction

The fetch path before 127 worked on a single integer:

```c
int fd = socket_connect(ip_be, u.port);
…
write(fd, req, n);
drain_fd(fd, &total);
close(fd);
```

A TLS connection is structurally similar — bytes in, bytes out
— but the bytes are encrypted on the wire and the lifecycle
includes a handshake. Rather than fork the entire fetch path
into a "TCP" and a "TLS" variant, 127 introduces a tagged
union:

```c
#define BR_CONN_TCP 0
#define BR_CONN_TLS 1

typedef struct {
    int            kind;   /* TCP | TLS */
    int            fd;     /* TCP: the socket; TLS: -1 */
    tls_socket_t  *tls;    /* TLS: heap-allocated engine; TCP: NULL */
} br_conn_t;
```

Five helpers do all the transport work:

| helper             | TCP path                | TLS path                     |
|--------------------|-------------------------|------------------------------|
| `br_conn_open`     | `socket_connect`        | `malloc(tls_socket_t)`, `tls_socket_init_chain_from_anchor`, `tls_socket_connect` |
| `br_conn_write`    | `write`                 | `tls_socket_send` + `tls_socket_flush` |
| `br_conn_read`     | `read`                  | `tls_socket_recv` (with last-error check below) |
| `br_conn_close`    | `close`                 | `tls_socket_close` + `free`  |
| `drain_conn`       | loops on `br_conn_read` | (unchanged loop body)        |

`http_fetch_one` keeps its old shape end-to-end — `connect`,
`write` the request, `drain` the response, `close`. The
unionised `br_conn_t` lets one body serve both schemes.

## tls_socket_recv: EOF or error?

The one subtle place where TCP and TLS diverge is the read
return code. `recv` on a TCP socket returns 0 at EOF and a
negative errno on error. `tls_socket_recv` returns -1 for
*both* a clean close-notify and a real handshake / record
failure, because the BearSSL IO layer signals both as
"engine no longer producing data". The right thing to do
on -1 is to ask the engine what it was thinking:

```c
int err = br_ssl_engine_last_error(&c->tls->cc.eng);
if (err == BR_ERR_OK)
    return 0;     /* clean close_notify, treat as EOF */
printf("browser: TLS read error: BR_ERR=%d\n", err);
return -1;
```

`BR_ERR_OK` after a -1 means the engine drained itself
cleanly; any other value is a real problem and the print line
gives us a single grep target for failure debugging. The
chapter-127 test screens for `browser: TLS read error` in
its fail-fast list precisely so we notice if this branch ever
fires unexpectedly.

## The single-anchor trust shape

127 still uses the chapter-126 sample chain
(`test_server_chain`) and the sample intermediate CA as the
sole trust anchor:

```c
if (test_server_chain_len < 2) { … fail … }
if (tls_socket_init_chain_from_anchor(
        t,
        test_server_chain[1].data,
        test_server_chain[1].data_len) < 0) { … }
```

The anchor lives in the browser binary, not on disk. This is
the simplest possible shape that exercises the production
validator path end-to-end — the same shape `tlstest
--handshake-ca` proved out in 126. Chapter 128 takes the
next step: load a curated subset of Mozilla NSS roots from
`osfs:/etc/ssl/` so the browser can talk to a real public
server.

## Why "localhost" and not 127.0.0.1

The sample chain's leaf has `CN=localhost`. The minimal
validator does an SNI-against-SAN/CN match as the last step;
calling it with SNI = "127.0.0.1" fails because that string
isn't in the cert. The honest fix would be to either:

1. Regenerate the sample chain with a SAN that includes the
   loopback IP (rejected: we want to track upstream BearSSL's
   sample data byte-for-byte, and real servers don't tend to
   issue certs for raw IPs anyway).
2. Make the browser use `localhost` as SNI when the URL host
   is `127.0.0.1` (rejected: silent SNI overrides hide bugs).
3. Let the user write `https://localhost:8443/` and have the
   browser short-circuit `localhost` straight to 127.0.0.1
   without going through `SYS_RESOLVE` (taken).

The short-circuit is six lines in the resolve block. QEMU
slirp's built-in DNS forwarder doesn't answer for the literal
word "localhost", so the alternative — letting `SYS_RESOLVE`
attempt a real lookup — would time out for ~3 seconds before
failing. The shortcut makes loopback testing instant and
matches what every other operating system does for the same
hostname.

## The proxy-rewrite gate

`canonicalize_url` historically rewrote `https://example.com/`
into `http://127.0.0.1:8080/example.com/` so the host-side
proxy could terminate TLS for us. That rewrite still works,
but now it only fires when the caller explicitly set the
`BROWSER_PROXY` environment variable:

```c
static int g_proxy_was_set = 0;
…
if (br_starts(input, "https://")) {
    if (!g_proxy_was_set)
        return br_strdup(input);   /* native TLS path */
    /* else fall through to legacy proxy rewrite */
}
```

`g_proxy_was_set` is flipped to 1 the first time
`load_proxy_from_env` finds a non-empty `BROWSER_PROXY`. The
two production users of the old rewrite both pre-date 127 and
still work unmodified:

- `userspace/proxytest/proxytest.c` calls
  `setenv("BROWSER_PROXY", "http://127.0.0.1:8080/")` before
  spawning the browser (line 152). Its regression
  (`scripts/test_browser_proxy.py`) is unchanged and still
  passes.
- `scripts/test_browser_hn_desktop.py` and friends
  `export BROWSER_PROXY=…` in the shell before running the
  browser. Unchanged.

`BR_DEFAULT_PROXY` (`"http://127.0.0.1:80/"`) is kept for
case (6) — the "bare hostname" URL form — so typing
`browser news.ycombinator.com` from a shell that has no
`BROWSER_PROXY` set still goes through the old in-guest
`/bin/httpd` forwarder we built in 110. That path is `http`,
not `https`, and is unaffected by 127.

## Traps caught

1. **`bearssl.h` not on the default user include path.** The
   generic `$(BUILD)/userspace/%.o` pattern rule supplies
   `USER_CFLAGS` only. Without a per-file override the build
   fails at the `cc -c browser.c` step. Fix: a pattern-
   specific rule for `$(BUILD)/userspace/browser/browser.o`
   that adds `-I vendor/bearssl-shim -I vendor/bearssl/inc`,
   placed *after* the generic rule (per Make's "most
   specific wins" tiebreaker, which requires this order).
2. **`tls_socket_t` is ~42 KB.** The user stack is 16 KiB, so
   the per-fetch object has to live on the heap. `br_conn_open`
   `malloc`s it on entry and `br_conn_close` `free`s it on
   exit; nothing changes for the TCP branch.
3. **Link-time symbol soup.** BearSSL ships ~100 .o files with
   a dense reference graph (the cipher suites lazily reference
   prefs that reference key types that reference …). The
   `BROWSER_ELF` link rule now wraps the object set in
   `--start-group $(BROWSER_OBJS) $(BEARSSL_LIB) --end-group`
   so the linker resolves transitive references the same way
   `TLSTEST_ELF` and `HTTPSD_ELF` do.
4. **SNI mismatch on IP literal.** Covered above — the
   `localhost` short-circuit sidesteps it cleanly. If we
   forget this when adding a second test target, the symptom
   is `browser: TLS handshake to … failed` after a successful
   TCP connect, with `[browser] TLS handshake OK` *not*
   printed. The test's separate "TLS-OK line" and "decrypted
   body" assertions split this into a clear-failure-mode
   matrix rather than one opaque "the test failed".
5. **`g_proxy_was_set` gating on the wrong helper.** The flag
   lives at file scope, not inside `canonicalize_url`. It is
   written once by `load_proxy_from_env` (which the main
   function runs at startup) and read on every URL. Setting
   it inside `canonicalize_url` would create a chicken-and-egg
   ordering problem; setting it inside `getenv` itself would
   leak browser logic into libc.

## Applied to

| Surface | Change |
|---------|--------|
| `userspace/browser/browser.c` | + `BR_CONN_*` / `br_conn_t` / `br_conn_open` / `br_conn_write` / `br_conn_read` / `br_conn_close`; `drain_fd` renamed `drain_conn`; `http_fetch_one` now opens a `br_conn_t` instead of a raw fd; `fetch` collapses `http://` + `https://` into one branch; `canonicalize_url` case (3) gated by `g_proxy_was_set`; localhost loopback shortcut in the resolver |
| `userspace/httpsd/httpsd.c` | Unchanged. Same chapter-125 response body, same boot launcher in `init.c`. |
| `userspace/init/init.c` | Unchanged. Still spawns `/bin/httpsd 8443` as a background service. |
| `Makefile` | `BROWSER_OBJS` += `TLS_SOCKET_OBJ` + `TEST_CHAIN_OBJ` + `CSTRING_OBJ`; `$(BROWSER_ELF)` rule depends on `$(BEARSSL_LIB)` and uses `--start-group … --end-group`; new per-file override for `browser.o` supplies `BEARSSL_INC`. |
| `scripts/test_browser_https.py` | New regression — boots, drives `browser https://localhost:8443/`, asserts five PASS lines. |
| `scripts/https_proxy.py` | Unchanged on disk. No longer launched by any regression script. Retained as reference per the repo's debug-script convention. |
| `userspace/proxytest/proxytest.c` | Unchanged. Still works because it sets `BROWSER_PROXY` before spawning the browser. |

## What gets exercised in tests

- `scripts/test_browser_https.py` — the new 127 regression.
- `scripts/test_browser_self.py` — unchanged from chapter
  115, proves the bare browser path still works.
- `scripts/test_browser_proxy.py` — unchanged from chapter
  110, proves the legacy `BROWSER_PROXY` rewrite still
  works (so the gate doesn't accidentally block existing
  users).
- `scripts/test_tls_chain.py` — chapter 126 regression,
  proves `tls_socket_init_chain_from_anchor` still works in
  isolation.
- `scripts/test_tls_handshake.py` — chapter 125 regression,
  pinned-key handshake.
- `scripts/test_tlstest.py` — chapter 124 link/run smoke.

All six pass on `make -j8 && python3 scripts/<test>.py`.

## What the user-visible machine gains

- `browser https://localhost:8443/` works from any shell with
  no environment setup.
- The chapter-110 host proxy is now genuinely optional. It
  remains the recommended path for hostnames that QEMU slirp
  can route to the real internet (we'll keep using it for
  rendering tests against arbitrary sites until the in-guest
  trust store ships), but for guest-local servers the browser
  goes direct.
- The browser is now a useful guest-local TLS reference. Any
  future userspace process that wants to do client TLS can
  read `br_conn_open` as a five-call example and copy it.

## Next: chapter 128

Lift the trust anchor out of `test_chain.c` and into a small
curated trust store under `osfs:/etc/ssl/`. Add a multi-
anchor loader to `tls_socket.h` that scans the directory at
init time. Add a single public-internet target to the
regression set so we have at least one test that proves the
client can negotiate TLS 1.2 with a real CA-issued cert. The
`localhost` shortcut from 127 stays as the loopback path for
in-guest httpsd; the public path joins it as a peer.
