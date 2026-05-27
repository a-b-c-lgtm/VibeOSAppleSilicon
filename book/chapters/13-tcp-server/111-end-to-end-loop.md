# Chapter 111 — End to end: the browser fetches from its own kernel

> **Milestone in this chapter:** 98 — the capstone of Part XIII.
> Boot the desktop, open `gui_term`, and fetch a page served
> by `/bin/httpd` running in the same guest.
> **Code referenced:**
> - [userspace/httpd/](../../../userspace/httpd/) (the http
>   server started at boot)
> - [userspace/browser/](../../../userspace/browser/)
>
> **At the end of this chapter** you will have an end-to-end
> in-guest HTTP loop: a browser, a TCP loopback, an in-guest
> server, and a file served back to the renderer. Builds on
> chapters 108 (loopback), 109 (TLS bridge), 110 (proxy
> retarget).

The capstone of Part XIII. The desktop boots, and `/bin/httpd`
is already running on port 80 waiting for connections.  Open
`gui_term`, type:

```
browser http://127.0.0.1/mnt/test.html
```

and the browser fetches from our own kernel.  No host-side
proxy in the request path, no QEMU SLIRP redirect, no
environment variables to set.  Two of our own programs
talking TCP over `127.0.0.1`.

This chapter is almost entirely a celebration.  The
interesting code shipped in the four chapters that built
the pieces:

| Chapter | Brought us |
|---------|------------|
| [105](107-bin-httpd.md)   | `/bin/httpd` -- the server itself |
| [106](108-tcp-loopback.md) | `127.0.0.1` as a real address |
| [109](109-httpd-tls-bridge.md) | httpd as a transparent forwarder |
| [110](110-browser-uses-in-guest-httpd.md) | `BROWSER_PROXY` repoint + the eight-bug postscript |

What was left was three small things:

1. **Spawn httpd at boot** so the desktop is "ready to
   browse" without ceremony.
2. **Move the browser's default proxy from `:8080` to
   `:80`** so the freshly-spawned httpd is actually the
   address `browser` dials by default.  (Without this,
   step 1 leaves an httpd running on a port nobody is
   talking to.)
3. **A regression test** that asserts the loop closes:
   in-guest browser -> kernel TCP -> in-guest httpd ->
   VFS -> back to the browser, all from a single
   `browser ...` command.

Those are the three things this chapter ships.

## Prerequisites

- [Chapter 107](107-bin-httpd.md) -- `/bin/httpd`.
- [Chapter 108](108-tcp-loopback.md) -- loopback short-circuit
  in `ip_send_packet`.
- [Chapter 109](109-httpd-tls-bridge.md) -- the HTTPS
  forwarding bridge (not used by this chapter directly, but
  the architecture it locked in is why port-80 + port-8080
  can coexist).
- [Chapter 110](110-browser-uses-in-guest-httpd.md) -- the
  browser's default-proxy plumbing and the eight bugs that
  the postscript documents.
- [Chapter 70](../08-browser/070-bin-browser.md) -- the browser.

## The change

Open [`userspace/init/init.c`](../../../userspace/init/init.c).
After the `/bin/launcher` spawn and before the boot chime,
insert a fifteen-line block:

```c
puts("[init] launching /bin/httpd 80 (background, loopback)");
int httid = spawn("/bin/httpd", "80");
if (httid < 0) {
    write(1, "[init] spawn /bin/httpd FAILED errno=", 37);
    putd(-httid);
    write(1, "\n", 1);
    /* non-fatal -- desktop is still usable without it */
}
```

That is *literally the whole feature*.  Init already runs as
the chapter-49 background-reaper, so the new child is just
another tid the reap loop tolerates.  httpd's main loop is
the chapter-105 accept/serve cycle; it parks on
`socket_accept`, the cooperative scheduler runs it again
whenever a SYN arrives on its listen fd, and it dispatches
requests via the chapter-105 `serve_get` / chapter-109
`serve_forward` split (since we don't set
`HTTPD_UPSTREAM`, every non-VFS GET 502s -- which is the
right local-only-fileserver default).

## Why port 80, not 8080

This was the only non-trivial decision and it deserves
explanation, because the obvious answer ("8080 -- it's what
all the other chapters use") would have broken the test
sweep.

The existing httpd tests
([`test_httpd.py`](../../../scripts/test_httpd.py),
[`test_httpd_forward.py`](../../../scripts/test_httpd_forward.py),
[`test_browser_proxy.py`](../../../scripts/test_browser_proxy.py),
[`test_browser_hn_repeat.py`](../../../scripts/test_browser_hn_repeat.py),
[`test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py))
all spawn their own httpd on port 8080.  Some of them
configure `HTTPD_UPSTREAM`, some don't.  Either way, they
need a clean port to bind.

`tcp_listen` in [`kernel/core/tcp.c`](../../../kernel/core/tcp.c)
rejects duplicate binds with `-2`, which the syscall layer
translates to `-EADDRINUSE`.  We don't have `SO_REUSEADDR`
and we don't have wildcard binding.  If init's port-80 httpd
collided with a test's port-8080 httpd, the test would
either fail to start or get back an unexpected page from
the wrong server.

So the rule is: **init binds the conventional HTTP port (80)
and tests keep using 8080**.  The two coexist on the same
loopback interface, and the cost is a one-token
difference in the URL you type by hand
(`http://127.0.0.1:80/...` instead of `:8080`, or just
`http://127.0.0.1/...` since the browser defaults to 80).

This is a microcosm of why conventional ports exist at all.
The whole point of port 80 vs 8080 vs 9000 is exactly this
*do-not-collide* problem at a larger scale: distinct services
on the same host claim distinct ports so they can coexist.
The same logic applies at the level of one process tree
sharing one IP stack.

## The matching browser change

Flipping `BR_DEFAULT_PROXY` in
[`userspace/browser/browser.c`](../../../userspace/browser/browser.c)
from `"http://127.0.0.1:8080/"` to `"http://127.0.0.1:80/"`
is the second half of the feature.  Without it, the
auto-spawned port-80 httpd is doing nothing useful for
the default browser invocation -- every `browser
https://...` call would still try to dial `:8080` and
get connection-refused (since the tests that use 8080
are only running for the duration of those tests).

This one-line change has fan-out, because all the test
binaries that *do* run their own port-8080 forwarder
(`proxytest`, the hn-repeat / hn-timings / hn-desktop
suite) used to rely on the same default.  They now need
to override it explicitly:

- [`userspace/proxytest/proxytest.c`](../../../userspace/proxytest/proxytest.c)
  adds one line right after spawning its httpd:
  ```c
  setenv("BROWSER_PROXY", "http://127.0.0.1:8080/");
  ```
  The kernel env table propagates to `spawn()`ed children,
  so the browser child reads it through the same
  `load_proxy_from_env` path chapter 110 introduced.
- [`scripts/test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py)
  and [`test_browser_hn_desktop.py`](../../../scripts/test_browser_hn_desktop.py)
  add a one-line shell `export BROWSER_PROXY=http://127.0.0.1:8080/`
  after the `httpd 8080 &` step, so the desktop-spawned
  browser sees the same override.

The symmetry is the point: **whoever spawns the
forwarding httpd is also the one who tells the browser
about it**.  init spawns a non-forwarding httpd on the
conventional port and so the browser defaults to it;
tests spawn a forwarding httpd on a private port and so
tests override.  There is no shared global config that
has to be kept in sync.

## What this unlocks

- **The desktop is ready to browse the moment it boots.**
  No `httpd &` ceremony.  From the launcher you can drop
  into gui_term and immediately type a `127.0.0.1` URL.
- **Hermetic tests for the rest of the browser.**  Any
  Part XV browser feature that needs a "stable page to
  point at" can drop a file into `/mnt/` and fetch it via
  `http://127.0.0.1/mnt/that-file.html`.  No host network,
  no flaky DNS, no race against a test's own httpd
  starting up.
- **The full pipeline gets exercised on every boot**
  (because the test sweep does), which means future
  regressions in `socket_connect`, `socket_accept`, the
  loopback short-circuit, the VFS dispatch, or the
  browser's network layer will all surface as
  `test_browser_self` failures within a few seconds.

## The test:  `scripts/test_browser_self.py`

The full source is in
[`scripts/test_browser_self.py`](../../../scripts/test_browser_self.py).
Five assertions, in order:

1. **The shell prompt appears.**  Catches any boot-time
   regression in init.c (the most likely victim of this
   chapter's edit).
2. **`[init] launching /bin/httpd 80` and `httpd: listening
   on port 80` appear in the boot transcript**, BEFORE
   the shell prompt.  Proves the auto-spawn happened and
   that httpd's `socket_listen` succeeded.
3. **`browser --timing http://127.0.0.1:80/mnt/test.html 600`
   exits cleanly** within 15 s.  This is the actual fetch.
   The 600-pixel viewport matches the chapter-72
   default.
4. **httpd logged a `local GET /mnt/test.html -> 200`** and
   **the browser logged `HTTP/1.0 200 OK (text/html ...`**.
   Both sides of the request agree on what happened, which
   means the bytes made it through the loopback layer
   intact.
5. **The painted output contains "Hello", "goodbye", and
   "The quick brown fox" as recognisable glyph runs** (the
   chapter-71 plain paint mode spaces every glyph, so the
   assertion is a loose-whitespace regex).  And **the TCP
   release log line for the loopback connection reports
   `rx_total > 0`** -- the cheapest possible proof that
   the kernel actually moved bytes (not just SYN+FIN).

That last assertion is worth dwelling on.  A naive test
would just look for the rendered text and call it a day.
But there's a degenerate failure mode where the browser
renders a *cached* page from a previous run, or where the
test accidentally captures output from a different process.
Asserting on `rx_total=0x27b` (the exact byte count of
"HTTP/1.0 200 OK\r\n...\r\n\r\n<538-byte body>")
proves the bytes moved *this time*, *over loopback*, *for
this connection*.

The test boots without virtio-gpu / keyboard / tablet --
serial only.  That keeps it fast (the entire run, including
QEMU boot, is ~12 seconds on the test host) and isolates the
loopback path from the chapter-110 cursor-pump fix.
The desktop variant of the same fetch path is covered by
[`test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py),
which routes through the chapter-109 forwarding proxy.

## Timing

Bare-kernel, no GUI, loopback fetch: **~430 ms** wall on
Apple Silicon HVF.  Breakdown from the
`browser --timing` output:

```
[timing] fetch                  0 ms
[timing] html size: 538 bytes
[timing] tokenise + DOM         0 ms
[timing] collect/fetch CSS      0 ms
[timing] layout                 0 ms
[timing] paint collect          0 ms
[timing] paint cmds: 26, doc=600x270
```

The "0 ms" line items are sub-millisecond and round to
zero.  Almost the entire 430 ms is QEMU startup, kernel
boot, init, launcher spawn, httpd spawn, listen, shell
prompt -- the fetch itself is essentially instant once
everything is up.

From the desktop (via gui_term), the same fetch takes
**1.2 -- 1.3 s** wall.  That's the cooperative-yielding
overhead documented in the
[chapter 110 postscript](110-browser-uses-in-guest-httpd.md#postscript):
even after the cursor-pump fix, every `sys_yield` from
the browser still has to drain the virtio rings (fast-path
now), re-pick the runnable thread, and resume.  It's not
the kernel's TCP path that's slow; it's the scheduler's
overhead per yield, accumulated across the hundreds of
yields a fetch involves.

## Hand-test recipe

The whole point of this chapter is that the desktop is
ready to browse.  So the recipe is:

```
make -j
make run     # or your usual QEMU invocation with GUI
# wait for desktop
# from the launcher, open gui_term
$ browser http://127.0.0.1/mnt/test.html
# the rendered page appears in the same window
```

Note the URL has no explicit port -- `BR_DEFAULT_PROXY` is
now `:80`, and the browser's URL parser defaults the port
to 80 for `http://` when not specified.  `:80` works too
if you want to be explicit.  For a bare hostname like

```
$ browser news.ycombinator.com
```

the browser canonicalizes through `BR_DEFAULT_PROXY`,
which means it dials init's port-80 httpd, which has no
upstream and 502s.  To make bare hostnames work from the
desktop you have to either (a) kill init's httpd and
respawn it with an upstream, or (b) override
`BROWSER_PROXY` to point at a forwarder you have spawned
yourself:

```
# (a) replace init's local-only httpd with a forwarder
$ kill $(pidof httpd)     # if you have pidof; otherwise read /proc/...
$ HTTPD_UPSTREAM=10.0.2.2:8080 httpd 80 &
$ browser news.ycombinator.com

# (b) leave init's httpd alone, run a side forwarder on 8080,
#     and tell the browser to use it
$ HTTPD_UPSTREAM=10.0.2.2:8080 httpd 8080 &
$ export BROWSER_PROXY=http://127.0.0.1:8080/
$ browser news.ycombinator.com
```

For a more interesting *local* page, try
`http://127.0.0.1/mnt/welcome.html` (the bigger sample),
or `http://127.0.0.1/proc/uptime` (the
[chapter 101](../12-system-services/101-procfs-ps-top.md) procfs file --
served as plain text because httpd's
`looks_like_path` matches `/proc/`).

## Applied to / what gets exercised in tests

The standing rule: every kernel feature has to land in an app
the user actually runs.

- **Existing apps modified to use the feature**:
  - `/bin/init` ([userspace/init/init.c](../../../userspace/init/init.c))
    now spawns `/bin/httpd 80` at boot.
  - `/bin/browser` ([userspace/browser/browser.c](../../../userspace/browser/browser.c))
    now defaults its proxy to `:80` instead of `:8080`,
    matching init's spawn.
  - `/bin/proxytest` ([userspace/proxytest/proxytest.c](../../../userspace/proxytest/proxytest.c))
    now `setenv`s `BROWSER_PROXY=http://127.0.0.1:8080/`
    before spawning the browser child, so its private
    8080-bound forwarder is the one that gets used.
- **New apps added**: None.  The capstone is the
  *configuration*, not a new program.
- **Existing test scripts upgraded**:
  - [`test_browser_hn_desktop.py`](../../../scripts/test_browser_hn_desktop.py)
    and
    [`test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py)
    each gained one `export BROWSER_PROXY=...` line
    after the `httpd 8080 &` step, mirroring the change
    proxytest made on the C side.
- **New test scripts added**:
  [`scripts/test_browser_self.py`](../../../scripts/test_browser_self.py)
  -- the five-assertion end-to-end regression described
  above.

Full sweep post-change: 69/69 PASS.

## Files changed

| File | Change |
|------|--------|
| [`userspace/init/init.c`](../../../userspace/init/init.c) | +15 lines: spawn `/bin/httpd 80` after launcher, before boot chime |
| [`userspace/browser/browser.c`](../../../userspace/browser/browser.c) | `BR_DEFAULT_PROXY` flipped `:8080` -> `:80`; help text updated |
| [`userspace/proxytest/proxytest.c`](../../../userspace/proxytest/proxytest.c) | `setenv("BROWSER_PROXY", "http://127.0.0.1:8080/")` after spawning httpd |
| [`scripts/test_browser_hn_desktop.py`](../../../scripts/test_browser_hn_desktop.py) | added `export BROWSER_PROXY=http://127.0.0.1:8080/` |
| [`scripts/test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py) | added `export BROWSER_PROXY=http://127.0.0.1:8080/` |
| [`scripts/test_browser_self.py`](../../../scripts/test_browser_self.py) | NEW: end-to-end regression |
| [`book/INDEX.md`](../../INDEX.md) | update part marker |

## What chapter 111 does not do

For honesty's sake, here's what someone might *expect* a
"capstone end-to-end chapter" to also ship, and why it
isn't here:

- **A multi-page sample site under `/mnt/www/`.**  The
  browser has a URL bar, back/forward/refresh buttons,
  and link-following (chapter 63 and on), so we could
  absolutely ship a small interlinked site and click
  around it.  The reason this chapter doesn't is just
  scope: authoring a sample site is content work, and
  the assertion we wanted here was about the *plumbing*
  closing the loop.  A future chapter can drop
  `index.html`, `about.html`, `news.html` into
  `assets/osfs/www/` and add a test that follows two
  links without crashing -- it's purely additive.
- **A framebuffer-diff regression that compares the
  rendered page to a checked-in reference image.**  We
  have the building blocks (chapter 99 PNG decode could
  encode in the other direction with a few hundred more
  lines) but we don't have the discipline yet --
  framebuffer hashes are too brittle across font rounding
  and TTF revisions.  The textual-marker assertion in
  `test_browser_self` is the pragmatic substitute.
- **HTTPS termination at httpd.**  Chapter 109 forwards
  HTTPS by giving up and proxying to the host's network
  stack.  Real HTTPS termination requires
  bringing up a TLS library inside the guest, which is a
  whole chapter (or chapters) of its own and outside the
  Part XIII scope.

Each of those is a real chapter someone could write next.
None of them are blockers for "the loop is closed."

## Conclusion of Part XIII

Thirty-something chapters ago Part XIII opened by saying
"we have TCP; let's serve and consume bytes over it."  At
this point the kernel:

- accepts incoming connections on real ports
  (chapters 105-106),
- has its own `/bin/httpd` that serves files from the VFS
  (chapter 107),
- routes loopback traffic without a NIC round-trip
  (chapter 108),
- can forward HTTPS to a host upstream as a development
  affordance (chapter 109),
- has a browser that picks up an in-guest proxy by default
  and survives the desktop's cooperative scheduler
  (chapter 110),
- and now boots ready to serve its own pages to its own
  browser over its own loopback (this chapter).

Part XIV picks up at the storage layer.  But before
leaving, take a moment to type
`browser http://127.0.0.1/mnt/test.html` into gui_term
and watch a page render.  Every byte of that interaction
-- the URL parse, the DNS skip, the loopback short-circuit,
the accept queue wake, the file read, the response write,
the parse, the layout, the paint -- was written in this
project.  That's the loop closed.
