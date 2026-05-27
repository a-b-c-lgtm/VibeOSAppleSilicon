# Chapter 110 — Browser uses the in-guest httpd as its proxy

> **Milestone in this chapter:** 97 — retarget the browser's
> proxy address from the SLIRP gateway to the in-guest httpd.
> **Code referenced:**
> - [userspace/browser/](../../../userspace/browser/) (proxy
>   address constants)
> - [userspace/httpd/](../../../userspace/httpd/)
>
> **At the end of this chapter** you will have the browser
> reaching its TLS bridge through `127.0.0.1/8080` instead of
> `10.0.2.2/8080`, decoupling it from QEMU's network topology.
> Prerequisites: chapter 108 (loopback) and chapter 109
> (httpd TLS bridge).

This is a small chapter with a big architectural payoff.
Before chapter 110 the browser knew that "the proxy" lives
at `http://10.0.2.2:8080/` -- a host-side address. That
coupled the browser to QEMU's network topology (`10.0.2.2`
is SLIRP's gateway-of-the-guest IP, not anything universal)
and leaked the existence of a host-side TLS bridge into the
browser's source.

The chapter changes the default proxy address to
`http://127.0.0.1:8080/` (the in-guest httpd we just taught
to splice in [Chapter 109](109-httpd-tls-bridge.md)) and
adds two userspace pieces around that: a `load_proxy_from_env`
helper that runs in every mode (plain / ansi / paint / gui),
and a new [`/bin/proxytest`](../../../userspace/proxytest/proxytest.c)
orchestrator that demonstrates the whole chain in a single
command.

The line of code that does the work is exactly one byte
shorter than the line it replaced. The chapter exists to
explain why the change matters and to put proof under it.

## Prerequisites

- [Chapter 107 -- `/bin/httpd`](107-bin-httpd.md) -- the
  static-file server.
- [Chapter 108 -- TCP loopback](108-tcp-loopback.md) --
  so `127.0.0.1:8080` actually resolves to something inside
  the guest. Without this chapter, the browser's
  `socket_connect(127.0.0.1, 8080)` would time out.
- [Chapter 109 -- httpd as forwarding proxy](109-httpd-tls-bridge.md)
  -- so requests for non-VFS paths get bridged out via
  `HTTPD_UPSTREAM`. Without this, httpd would 404 every
  `/news.ycombinator.com/...` URL.
- [Chapter 32 -- env vars and PATH](../05-devices/032-env-vars-and-path.md)
  -- `BROWSER_PROXY` and `HTTPD_UPSTREAM` both flow through
  `getenv`, and the `export` builtin populates them.
- [Chapter 16 -- init, spawn, wait](../04-userspace/016-init-spawn-wait.md)
  -- `proxytest` relies on `spawn` + `waitpid` to drive
  httpd and browser as children.

## The one-byte change

```c
/* userspace/browser/browser.c */
- #define BR_DEFAULT_PROXY "http://10.0.2.2:8080/"
+ #define BR_DEFAULT_PROXY "http://127.0.0.1:8080/"
```

That is the whole behavioural delta. Three things make it
work:

1. `canonicalize_url` in browser.c was already written to
   prepend whatever `g_proxy_prefix` happens to be. It
   doesn't care whether the prefix points to host or guest;
   the URL rewrite rule
   `https://x/y -> $g_proxy_prefix + "x/y"` is the same
   either way.
2. `socket_connect` in the kernel was already loopback-aware
   as of chapter 108. The browser sees no behavioural
   difference between dialing `10.0.2.2:8080` (out through
   virtio-net + SLIRP) and `127.0.0.1:8080` (short-circuited
   through `lo0`).
3. `httpd` was already a splice proxy as of chapter 109.
   The traffic the browser used to send to the host proxy
   directly now reaches the host proxy via httpd's
   `serve_forward`, with `HTTPD_UPSTREAM` controlling where
   "the host proxy" actually lives.

## Plain mode parity

The first iteration of this chapter shipped only the
`#define` change and broke immediately. The chapter-110
test (which uses plain mode so the marker shows on serial)
failed with `https:// not yet supported (no TLS)`.

The bug was that the `BROWSER_PROXY` env read AND the
`canonicalize_url` call lived inside `run_gui` only. Plain,
ansi, and paint modes went straight from `argv[i]` into
`fetch()`, which has a hard "no https" branch. So `--gui`
proxied happily; everything else bailed.

The fix is two lines of refactoring and one helper:

```c
/* New helper near g_proxy_prefix.  Used to live inline in
 * run_gui; lifted out so every mode can call it. */
static void load_proxy_from_env(void)
{
    char tmp[160];
    long got = getenv("BROWSER_PROXY", tmp, sizeof(tmp));
    if (got <= 0) return;
    /* Copy into g_proxy_prefix; force trailing slash. */
    ...
    printf("[browser] proxy: %s\n", g_proxy_prefix);
}

/* main(), BEFORE mode dispatch: */
load_proxy_from_env();
if (mode_gui) return run_gui(src, viewport);

/* main(), AFTER mode dispatch, BEFORE fetch(): */
char *abs_src = canonicalize_url(src, 0);
if (!abs_src) { printf("oom\n"); return 1; }
if (!br_streq(abs_src, src))
    printf("[browser] proxied: %s\n", abs_src);
src = abs_src;
```

Now `browser https://news.ycombinator.com/` in plain mode
produces the same proxied path it would in `--gui`. The
chapter-110 test exercises this on every run; without the
fix it cannot succeed.

The lesson is the same one chapter 29 made about input
multiplexing: a feature that works in one mode but not
another is a behaviour bug, not a missing-feature bug.
Stamp it out by lifting the shared logic above the mode
switch.

## Why httpd is the right place

Three reasons this chapter exists as a default-flip rather
than as a kernel-level transparent-proxy hack:

1. **Network topology is policy, not mechanism.** Putting
   the proxy address inside the browser source code makes
   "what does the network look like" a compile-time
   decision. Putting it inside a userspace daemon makes it
   runtime. The day we move from QEMU SLIRP to a real
   bridge, or to a TAP device, or to running on hardware,
   only httpd's `HTTPD_UPSTREAM` needs to change. The
   browser doesn't recompile.
2. **No TLS in the browser, ever.** The browser is now
   structurally incapable of speaking TLS. It dials a plain
   HTTP loopback address and parses plain HTTP responses.
   Adding TLS (chapter 123) is a change to httpd: stand up
   a BearSSL inside `serve_forward`. The browser doesn't
   know that work happened.
3. **One door for outbound bytes.** Every "the guest
   reaches the outside world" path now goes through
   httpd's `serve_forward`. That makes it the single place
   to add logging, rate limiting, request auth, caching,
   or any other cross-cutting concern. Compare to the
   pre-chapter world where the browser had a direct
   outbound conn and `httpget` had another and any future
   tool would need yet another.

## `/bin/proxytest`: the orchestrator

The shell still doesn't have `&`, so to demonstrate the
whole chain in a hand-test we need a binary that does the
fork/spawn dance:

```c
/* userspace/proxytest/proxytest.c, abridged */
int main(int argc, char **argv)
{
    const char *url = "https://m97.proxy.test/path";

    /* (1) httpd must be running before browser dials it. */
    int httpd_pid = spawn("/bin/httpd", "8080 --once");

    /* (2) Give httpd a moment to bind its listen socket. */
    sleep_ms(300);

    /* (3) Browser dials BR_DEFAULT_PROXY (127.0.0.1:8080)
     *     via the chapter-106 loopback short-circuit. */
    int br_pid = spawn("/bin/browser", "https://m97.proxy.test/path 600");

    /* (4) Reap browser first -- its dial unblocks accept(). */
    waitpid(br_pid, ..., 0);
    waitpid(httpd_pid, ..., 0);

    printf("[proxytest] done\n");
}
```

That's the full design. Three subtleties worth calling out:

- **Spawn order matters.** httpd must be listening before
  the browser dials, otherwise the browser's `connect()`
  fails with `ECONNREFUSED`. `sleep_ms(300)` is the cheap
  way to handle it; a poll-connect loop would be more
  robust but would consume the `--once` slot and starve the
  browser. The book leaves polling for chapter 10X when
  the shell gets `nc -z`-style helpers.
- **`HTTPD_UPSTREAM` flows through `spawn`.** The kernel
  env table is per-process and inherited across `spawn` and
  `fork`. So `export HTTPD_UPSTREAM=...` in the shell, then
  `proxytest` -- proxytest inherits, then httpd inherits
  from proxytest, then `load_upstream_from_env()` sees it.
  Two inheritance hops, end to end.
- **Reap order also matters.** If we waited for httpd
  first, we'd block forever (httpd doesn't exit until
  the browser disconnects). Reaping browser first lets the
  whole chain unwind cleanly.

`proxytest` is on PATH (it's in `/bin/`), so a developer
can just type `proxytest` at the shell prompt and watch
the chain work.

## Why this is "encapsulation by routing"

The architecture chapter 110 lands is the same one big
companies use at much bigger scale:

- **Service mesh sidecars** (Envoy, Linkerd) -- every
  service inside a kubernetes pod talks to `127.0.0.1:8001`
  for all outbound traffic. The sidecar does TLS, mutual
  auth, retries, circuit-breaking, tracing, rate
  limiting. The application doesn't know any of that
  exists.
- **macOS / iOS app transport security** -- the kernel
  redirects insecure connections through a system-level
  TLS terminator.
- **HTTP forward proxies** in corporate networks -- every
  browser dials `proxy.corp:3128`, the proxy upgrades the
  outbound conn to TLS if necessary and does the URL
  filtering / logging.
- **Cloudflare Workers / Lambda@Edge** -- the request
  goes to a generic edge endpoint; the work that decides
  what backend to hit happens at the edge, not in the
  client.

All four have the same shape: client doesn't know about
transport, daemon does. Our chapter-110 version is the
simplest honest instance of that pattern -- one defined
constant in browser.c, one running daemon on a fixed
local port.

## What chapter 109 unlocks that this chapter uses

The chapter 109 `is_local_path` dispatch table is what
makes the new default safe:

```c
static int is_local_path(const char *target)
{
    return s_starts_with(target, "/mnt/")  ||
           s_starts_with(target, "/data/") ||
           s_starts_with(target, "/proc/");
}
```

When the browser asks for `/mnt/test.html` via the proxy
(say, in `--gui` mode after the user pastes a path into
the URL bar), httpd will serve it from the local VFS via
`serve_get`. When the browser asks for `/m97.proxy.test/...`,
that's not a local prefix, so httpd routes it through
`serve_forward` to `HTTPD_UPSTREAM`. The same in-guest
address handles both cases. From the browser's
perspective there is one door.

## The test

[`scripts/test_browser_proxy.py`](../../../scripts/test_browser_proxy.py)
exercises the full chain in one QEMU boot:

```
1. Spawn host-side fake upstream on 127.0.0.1:18083 returning
   "M97-BROWSER-PROXY-OK|path=<requested-path>\n".
2. Boot QEMU with outbound NAT only (no hostfwd needed --
   the whole flow is in-guest).
3. Wait for shell prompt.
4. `export HTTPD_UPSTREAM=10.0.2.2:18083`.
5. `proxytest`.
6. Assert proxytest logs spawning httpd.
7. Assert httpd binds port 8080 (the default
   BR_DEFAULT_PROXY port).
8. Assert httpd's "forward upstream" line names our fake.
9. Assert proxytest logs spawning browser.
10. Wait for the marker on serial -- if the browser
    rendered the upstream body, the chain works end to
    end.
11. Assert httpd logged "forward GET ..." (chapter-109
    dispatch picked the forward arm).
12. Assert that forward returned HTTP 200.
13. Assert proxytest reaped both children cleanly.
```

10 PASS lines. The marker assertion is the strongest:
it's the byte-for-byte proof that

```
browser plain mode
  -> 127.0.0.1:8080            (chapter 108 loopback)
  -> httpd serve_forward       (chapter 109)
  -> 10.0.2.2:18083            (SLIRP NAT)
  -> host's Python http.server
  -> response splice
  -> browser plain-mode render
  -> serial UART
  -> Python test asserts
```

works without anyone in that chain except the host
test harness knowing TLS exists.

## Failure modes

If chapter 108 is reverted, step 10 fails: the browser's
`connect(127.0.0.1)` times out.

If chapter 109 is reverted, step 11 fails: httpd 404s
the unknown path, browser renders the 404 page, marker
never appears.

If chapter 110 is reverted (or someone reinstates the
`10.0.2.2` default), step 10 fails: the browser dials
out to a SLIRP gateway that has no listener, since the
test deliberately doesn't start `scripts/https_proxy.py`.

If the load_proxy_from_env / canonicalize-in-main fix is
reverted, step 10 fails the same way the first iteration
of the test failed -- the browser bails on `https://`
without ever dialing.

All three regressions show as a missing marker. The test
is therefore a triple-feature regression bell.

## Hand-test

A developer can verify the chain by hand with two
commands at the guest shell:

```sh
$ export HTTPD_UPSTREAM=10.0.2.2:8080
$ proxytest
[proxytest] spawning /bin/httpd 8080 --once
httpd: listening on port 8080 (once=1)
httpd: forward upstream 10.0.2.2:8080
[proxytest] spawning /bin/browser https://m97.proxy.test/path 600
[browser] src=https://m97.proxy.test/path viewport=600 mode=plain
[browser] proxied: http://127.0.0.1:8080/m97.proxy.test/path
... rendered page text ...
[proxytest] done
$
```

For the `https_proxy.py`-routed case (real HTTPS sites
via the host proxy):

```sh
# on the host:
$ python3 scripts/https_proxy.py 8080 &

# inside the guest:
$ proxytest --url https://news.ycombinator.com/
... real HN page text ...
```

The `--url` flag was added to `proxytest` for exactly this
hand-test, so a developer can prove the chain works
against a real HTTPS endpoint without rebuilding.

## Applied to / what gets exercised in tests

Per the project's "apps must use the OS features" discipline:

- **Modified app:** [`userspace/browser/browser.c`](../../../userspace/browser/browser.c)
  flips `BR_DEFAULT_PROXY`, lifts the env / canonicalize
  pipeline above the mode switch, and adds the
  `load_proxy_from_env` helper. Roughly 30 net lines.
- **New app:** [`userspace/proxytest/proxytest.c`](../../../userspace/proxytest/proxytest.c)
  -- single-file orchestrator (~160 lines including
  comments) that drives the chain from one shell command.
  Packed into the OSFS at `/bin/proxytest`.
- **New test:** [`scripts/test_browser_proxy.py`](../../../scripts/test_browser_proxy.py)
  -- 10 PASS assertions, picked up automatically by
  [`scripts/sweep.sh`](../../../scripts/sweep.sh)'s
  `test_*.py` glob.
- **OSFS bump:** [`kernel/core/osfs.h`](../../../kernel/core/osfs.h)
  and [`scripts/mkosfs.py`](../../../scripts/mkosfs.py)
  bumped `OSFS_MAX_FILES` 64 -> 128 (and the directory
  region 4 sectors -> 8) because proxytest pushed us over
  the chapter-60 cap. Backward-compatible -- old images
  still mount because the layout fields are explicit in
  the kernel header.
- **Existing tests unchanged:** every chapter-105/106/109
  regression continues to pass. The sweep re-verified
  this.

## Files changed

- [`userspace/browser/browser.c`](../../../userspace/browser/browser.c)
  -- `BR_DEFAULT_PROXY` flipped; `load_proxy_from_env`
  helper added near `g_proxy_prefix`; `main()` calls it
  before mode dispatch and canonicalises the input URL
  before `fetch()` in plain/ansi/paint modes; the
  https-reject hint in `fetch()` updated to point at
  httpd.
- [`userspace/proxytest/proxytest.c`](../../../userspace/proxytest/proxytest.c)
  (new) -- the orchestrator.
- [`Makefile`](../../../Makefile) -- new `PROXYTEST_*`
  build vars, link rule, strip rule, and an
  `OSFS_BIN_FILES` / `mkosfs.py` entry.
- [`kernel/core/osfs.h`](../../../kernel/core/osfs.h),
  [`kernel/core/osfs.c`](../../../kernel/core/osfs.c),
  [`scripts/mkosfs.py`](../../../scripts/mkosfs.py)
  -- OSFS directory bumped from 4 sectors / 64 files to
  8 sectors / 128 files. `FIRST_DATA_SECTOR` shifted
  from 5 to 9.
- [`scripts/test_browser_proxy.py`](../../../scripts/test_browser_proxy.py)
  (new) -- regression.
- [`book/INDEX.md`](../../INDEX.md) -- row updated.

## What this unlocks

- **Chapter 111** -- the end-to-end celebration. Browser
  navigates to `http://127.0.0.1:8080/mnt/test.html`, no
  hostfwd, no SLIRP trombone, no env fiddling. The chain
  is so transparent the user doesn't see it at all.
- **Chapter 123 (TLS in httpd)** -- when we add a real
  in-guest TLS stack inside `serve_forward`, the browser
  doesn't change. It still dials `127.0.0.1:8080`; the
  proxy still splices. The TLS upgrade is invisible
  upstream of the proxy.
- **Future "proxyctl" tool** -- a small CLI that
  re-points httpd's upstream at runtime without
  restarting the browser. Useful for offline-first
  development (`proxyctl --local` flips httpd's upstream
  to a snapshot directory; `proxyctl --internet` flips
  back).

## Postscript -- the eight bugs between "works" and "works on the desktop"

The chapter above is true, but it's the version of the
story that omits the messy middle. When the OS-wide
discipline is "apps must use the OS features the book
builds", you don't get to call a chapter done until
`/bin/browser news.ycombinator.com` works from inside
`gui_term` on the actual desktop -- not just from the
bare-kernel serial sh that the regression harness sees.

The first time we tried that, the HTML fetch took 30+
seconds. Subsequent refreshes also took 30 seconds.
Tracking that down surfaced eight separate bugs across
the kernel, the shell, the timer, and two test scripts.
Each one is small. Together they explain why every
chapter from here on ends with a postscript like this
one: integration is where the bugs live.

### How the eight bugs were found

The hunt has two scripts at its heart, both kept in the
repo per the debug-scripts policy:

- [`scripts/test_browser_hn_timings.py`](../../../scripts/test_browser_hn_timings.py)
  -- bare-kernel one-shot fetch. The ground-truth speed
  reference.
- [`scripts/test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py)
  -- boots with virtio-gpu/keyboard/tablet and the
  desktop/launcher/taskbar running, but pipes browser
  commands through the SERIAL sh so `[timing]` lines
  land on serial where the harness can grep them.

The whole investigation reduces to "why does (B) take 10x
as long as (A) on the same kernel?" Every bug below was
identified by binary-searching that gap.

### (1) Window-update gate too narrow

`tcp.c::apply_ack` only honoured window-update ACKs in
`ESTABLISHED`. After our active-close, FIN-WAIT-{1,2}
saw a zero window forever and we'd retransmit FIN until
the peer gave up. Fix: widen the gate to
`ESTABLISHED | FIN_WAIT_1 | FIN_WAIT_2`.

### (2) `apply_ack` FIN off-by-one

`data_in_flight -= len` treated the FIN sequence number
as if it were data. Fix: only decrement by 1 for the
FIN itself, and only when the ACK actually advances
past it (`in_flight > tx_len`).

### (3) `tcp_poll` dropped FIN on RTO

If we retransmitted while a FIN was in flight, `fin_sent`
stayed set and the FIN was never resent. The receive side
hung in `read()` for the full TIME_WAIT. Fix: clear
`fin_sent` when `fin_was_in_flight` inside the data-rewind
RTO branch.

These three together account for the original "shutdown
plateau" we hit before any GUI bug came into view. The
diagnostic that paid for itself was a per-connection
counter:

```c
/* kernel/core/tcp.c */
struct tcp_conn {
    ...
    uint32_t dbg_rtx_fired;   /* bumped in each RTO branch */
};

/* release_conn prints it on close */
```

With `dbg_rtx_fired` visible on every `[tcp] release` line,
we could disprove the "premature RTO" hypothesis in one
test run (count = 0 or 1 in both bare and desktop, ruling
out retransmission storms as the cause of the 10x gap).

### (4) Makefile -- `.DEFAULT_GOAL := all`

`make` without arguments was building the kernel only,
silently regressing every `scripts/test_*.py` whenever
someone forgot to type `make all`. One-line fix at the
top of the Makefile:

```makefile
.DEFAULT_GOAL := all
```

This had nothing to do with the browser. It mattered
because chasing the desktop slowdown involved hundreds
of edit-rebuild-test cycles, and intermittently testing
a kernel against a stale userspace is exactly the kind
of compounding noise that hides the real bug.

### (5) `uptime_ms()` was 2x under SMP

The first thing we did once `dbg_rtx_fired` cleared the
TCP hypothesis was add `[timing]` lines to the browser's
fetch path. The numbers came back 16-second HTML on
desktop, 1.4-second bare. A bigger gap than the wall-time
test showed.

The reason: `timer_tick()` fires from each CPU's PPI 27
IRQ, and each call bumped `g_ticks`. On `-smp 2`, wall
time advanced at 2 Hz for every 1 Hz of real time, and
every `[timing]` line in every program in the system was
inflated 2x. The slowdown was real, just half as bad as
it looked.

Fix in [`kernel/core/timer.c`](../../../kernel/core/timer.c):
`timer_ticks()` now reads `CNTVCT_EL0` directly (via a
new `cntvct_el0_read()` helper) and divides by the
per-quantum cycle count. `g_ticks` keeps being
incremented in the IRQ because some kernel-internal
callers still want "quanta seen on this CPU", but it
is no longer the wall-clock authority.

Generalisable rule -- written into repo memory under
`chapter-86-smp-psci`: any global state advanced from
per-CPU IRQ handlers needs an SMP-correct accessor, not
just an SMP-correct *update*.

### (6) Shell needed `&`

A small infrastructure fix unmasked by the new tests.
[`scripts/test_browser_hn_serial_with_desktop.py`](../../../scripts/test_browser_hn_serial_with_desktop.py)
needs to spawn the in-guest httpd in the background so
the serial sh stays available for the browser command:

```sh
$ httpd 8080 &
[bg] tid=12
$ browser --timing news.ycombinator.com 600
```

Implementation in [`userspace/sh/sh.c`](../../../userspace/sh/sh.c)
is deliberately minimal: strip the trailing `&`, set a
`bg` flag, at the spawn site print `[bg] tid=N\n` and
skip the `set_fg_pid`/`wait`/timed-print sequence that
foreground commands run. No `jobs`/`fg`/`wait %N` yet --
that lives in chapter 78.

### (7) The big one -- `pump_input_into_wm` on every `sys_yield`

`kernel/core/syscall.c::sys_yield` looks like this:

```c
static long sys_yield(void) {
    pump_input_into_wm();
    yield();
    return 0;
}
```

`pump_input_into_wm` drains both virtio-keyboard and
virtio-tablet. Each driver did, on every call:

```c
uint32_t istat = r32(VIRTIO_MMIO_INTERRUPT_STATUS);  /* HVF trap */
if (istat) w32(VIRTIO_MMIO_INTERRUPT_ACK, istat);    /* HVF trap */
... drain used ring (usually empty) ...
w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);                    /* HVF trap */
```

Three HVF traps per device per yield. Multiply by:

- `/bin/desktop` deliberately spinning
  `while(1) { gui_poll_event; yield(); }` (chapter
  31's compromise to keep the cursor responsive).
- `/bin/launcher` doing the same.
- the browser thread blocking in `tcp_read` (which
  yields on every poll).

The browser alone yields ~10,000 times during one HN
fetch. With the two desktop apps also yielding flat-out,
we were running tens of thousands of MMIO trap pairs
per second of fetch. At HVF's ~50us trap-and-return cost
on M-series silicon, that is the 7.5 seconds of
overhead we were measuring.

**Fix** -- in both
[`kernel/device/virtio_input.c::virtio_input_poll`](../../../kernel/device/virtio_input.c)
and
[`kernel/device/virtio_tablet.c::virtio_tablet_poll`](../../../kernel/device/virtio_tablet.c):
early-return when `u->idx == g_used_idx_seen`. `u->idx`
lives in shared RAM and is free to read; we only run
the MMIO sequence when the device has actually produced
new events.

```c
void virtio_input_poll(void) {
    if (!g_in_mmio_base) return;
    struct vring_used *u = used_ring();
    /* Fast path: shared-RAM index, no trap. */
    if ((uint16_t)(u->idx - g_used_idx_seen) == 0) return;
    /* ... slow path with MMIO unchanged ... */
}
```

After the fix, the desktop fetch matches bare: 750 ms
HTML vs 700 ms bare, deterministic across three runs.

**Generalisable rule for any virtio polling code under
HVF:** read the device's RAM-resident producer index
*first*. Only touch MMIO when it differs from the
consumer index. The cooperative-yield model multiplies
wasted traps by every scheduler cycle, and HVF's
per-trap cost is enough that "the call is cheap" stops
being true around ~100 calls/sec per device.

### (8) `sys_socket_accept` dropped SIGTERM

[`scripts/test_browser_hn_repeat.py`](../../../scripts/test_browser_hn_repeat.py)
drives `proxytest --repeat 3 --timing`. In repeat mode
proxytest spawns httpd *without* `--once` (so it survives
across iterations) and then `kill(httpd_pid, SIGTERM)`s
it at the end. After bug (7) was fixed, the per-iteration
timings dropped to ~1 s each -- and the test still timed
out, because `proxytest` hung in `waitpid(httpd_pid)`
forever.

The cause:

```c
/* kernel/core/syscall.c::sys_socket_accept */
for (;;) {
    (void)net_poll();
    child = tcp_accept(e->socket_cid);
    if (child >= 0) break;
    if (child == -1) return -EBADF;
    yield();   /* no sig_pending check */
}
```

`thread_signal_pid` raises `sig_pending` and wakes the
thread, but the signal-tail in `svc_dispatch` only runs
when a syscall *returns*. This loop never returns, so
the signal was effectively dropped.

Fix: re-check `sig_pending` after the yield, return
`-EINTR` on a pending signal:

```c
if (thread_current()->sig_pending) return -EINTR;
yield();
```

**Generalisable rule** -- written into the chapter-77
repo memory: every kernel-side spin-yield loop that
camps on userspace progress (accept, future select,
blocking pipe_read on empty pipes, ...) must re-check
`sig_pending` after the yield. The signal-delivery tail
in `svc_dispatch` is necessary but not sufficient on
its own; loops that never return need to opt in.

### One last thing -- the test-harness wait_for race

After bug (7) was fixed, `test_browser_proxy.py` and
`test_browser_hn_repeat.py` started failing with
"never saw the next marker" even though both markers
were in the captured transcript. Cause: each script's
`wait_for` had its own local accumulator that was
discarded on return. When two markers arrived in the
same 400 ms drain, the second `wait_for` never saw
the bytes between them.

Fix: a module-level
`_wait_for_carry: bytearray` that survives across
calls. Each `wait_for` consumes up to
`needle + len(needle)`; anything after is preserved.
Both scripts now use this pattern; future test authors
should reach for it whenever the guest can plausibly
emit two markers inside a single drain window.

### Numbers, before and after

For posterity, on `-cpu host -accel hvf -smp 2 -m 8G`,
fetching `news.ycombinator.com` through the in-guest
httpd via the host TLS proxy:

| run env                                | HTML fetch | wall (key→exit) |
|----------------------------------------|-----------:|----------------:|
| bare-kernel serial sh                  |     700 ms |        ~4000 ms |
| desktop (gui_term + desktop + launcher + taskbar) BEFORE |   8300 ms |        ~8900 ms |
| desktop AFTER                          |     750 ms |        ~1450 ms |

The bare wall is dominated by the chapter-103
boot-self-test camping on `accept` for ~3 s (see
that chapter's repo memory). Desktop skips that
because the test sends the browser command after
the wallpaper paints, not after the boot-self-test
finishes. Both numbers are now bound by the same
two things: the HTML body fetch and the 300 ms
font-CSS round trip.

### What this postscript demonstrates

Three things, in order of importance:

1. **The "apps must use the OS features" rule pays for
   itself.** None of bugs (1) through (8) were visible
   from the regression suite as it stood at the end of
   chapter 110. They all surfaced from one new
   end-to-end test -- "boot the desktop, open the
   browser, fetch a real page" -- that exercised the
   real composition of the system.
2. **Per-trap cost is the dominant fixed cost in
   cooperative kernels under hypervisor-based
   virtualisation.** The bug-(7) fix is 6 lines of
   code; it bought 10x. Future virtio drivers should
   start with the same RAM-index fast path, not bolt it
   on after a regression.
3. **Diagnostic helpers earn their keep.** Adding
   `dbg_rtx_fired` was an hour; reading it ruled out the
   wrong hypothesis in 30 seconds. The same applies to
   the `[timing]` lines in the browser, the `[tcp]
   release` log, and the new desktop test harness.
   None of them are clever. They are just *cheap to
   add and cheap to read*.

The 68-test regression sweep passes end-to-end after
all eight fixes. Chapter 111 picks up from here.
