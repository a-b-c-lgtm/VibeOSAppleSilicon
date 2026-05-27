# Chapter 120 — Cookies and the Same-Origin Policy

Forms (chapter 119) let users log in. Without cookies there
is no way to *stay* logged in: the server has no thread to
pin "you" to between requests, every TCP connection looks
identical, and the second page load is anonymous again.

This chapter adds a real cookie jar — on-disk, durable
across reboots, shared by every userspace HTTP client we
ship — and the cookie-side half of the Same-Origin Policy.
It deliberately stops short of the cross-origin-form half
of SOP; that's the next chapter. The two halves are usually
described together, but they are independent mechanisms and
landing them one at a time keeps the test surface small.

## What ships in this slice

### 1. A header-only cookie jar at `userspace/libc/cookies.h`

The jar is a header-only library, same convention as
[printf.h](../../../userspace/libc/printf.h),
[malloc.h](../../../userspace/libc/malloc.h), and
[url.h](../../../userspace/libc/url.h). Any userspace binary that
talks HTTP picks it up with one `#include` and gets:

```c
struct cookie_attr {
    char     name [64];
    char     value[256];
    char     path [64];
    time_t   expires;   /* 0 = session, treated as "never" */
};

int  cookie_parse_set(const char *val, size_t vlen,
                      time_t now, struct cookie_attr *out);
int  cookie_store_set(const char *host,
                      const struct cookie_attr *c);
int  cookie_store_get(const char *host, const char *path,
                      time_t now, char *out, size_t cap);
```

The three calls are the entire public surface. `parse_set`
turns a raw `Set-Cookie:` value into a struct. `store_set`
writes it to disk. `store_get` returns the rendered
`Cookie: ...` header value the client should send on its
next request — empty string if there's nothing to send.

### 2. Disk format: one file per host, plain text

The jar lives at `/data/cookies/<sanitised-host>`. The
directory is created lazily on first write. Each cookie is
one line:

```
name<TAB>value<TAB>expires_epoch<TAB>path<LF>
```

`expires_epoch` is a decimal POSIX `time_t` — the wall
clock that landed in [chapter 96](../12-system-services/096-rtc-and-wallclock.md).
`0` means "session cookie" and is treated as "expires
never" because this OS has no notion of a browser session
distinct from the box's uptime.

Replacing a cookie is a full file rewrite, not an append.
Append-only would grow unbounded under churn (think: every
page load re-issues `Set-Cookie: id=...`). The whole jar is
slurped, the matching name's line is swapped, and the file
is written back via the OSFS-2 write path with `fsync()`
before close. That fsync is what makes the persistence
sub-test (see below) pass.

The format is cat-friendly. `cat /data/cookies/127.0.0.1`
shows the literal lines, no decoder needed. That's the
whole point of choosing plain text over a binary blob —
when something breaks, the user (or the book) can read
the state with the tools that already exist.

### 3. SOP, cookie side: exact host equality

`cookie_store_get("b.example.com", ...)` never returns
cookies stored under `"a.example.com"`. There is no
Domain-attribute fan-out. Real browsers honour
`Set-Cookie: Domain=.example.com` to share cookies up the
DNS tree; we don't.

The reasoning: host-only cookies are the modern
recommended default (the RFC 6265bis draft already
deprecates Domain), the server-side test fixtures in this
book don't need cross-subdomain cookies, and "exact host
string equality" is one line of C that is impossible to
get subtly wrong. The Domain attribute is the historical
source of half the cookie security bugs ever filed.
We stay out.

The Path attribute *is* honoured: a cookie stored with
`Path=/api` is only sent on requests whose path starts
with `/api` (with the usual `/` or end-of-string boundary
check, so `/apifoo` doesn't match). Path filtering is
much less foot-gunny than Domain and turns out to be
useful for the `/cookie/...` test endpoints.

### 4. Attributes honoured, ignored, and why

| attribute    | what we do                              |
| ------------ | --------------------------------------- |
| `name=value` | required                                |
| `Max-Age=N`  | parsed, expiry = now + N seconds        |
| `Path=...`   | stored, outgoing filter applied         |
| `Expires=`   | **ignored** — no HTTP-date parser yet   |
| `Domain=`    | **ignored** — host-only only            |
| `Secure`     | **ignored** — no TLS in-guest           |
| `HttpOnly`   | **ignored** — no JS DOM API yet         |
| `SameSite=`  | **ignored** — no third-party iframes    |

The "ignored" entries each have a real reason rather than
"we couldn't be bothered". `Secure` is a hint to the
client that it should never send the cookie over plaintext
HTTP; in our world there is no other transport, so the
hint is a no-op. `HttpOnly` is a hint that JavaScript's
`document.cookie` should not return the cookie;
chapter 122 will ship a JS evaluator but it doesn't have a
DOM API surface to gate. `SameSite` controls whether the
cookie is sent on requests originating from a different
origin; we don't have iframes or window-opener relationships,
so every request is same-site by construction.

The deliberate one we *won't* fix later: `Expires=`. We
have `Max-Age=` and that's enough. Adding an HTTP-date
parser ("Wdy, DD Mon YYYY HH:MM:SS GMT") for one obscure
case is not a good use of the next 200 lines of C.

### 5. Browser and httpget both share the jar

The cookie wiring lives in two near-identical blocks, one
in [userspace/browser/browser.c](../../../userspace/browser/browser.c)
and one in [userspace/httpget/httpget.c](../../../userspace/httpget/httpget.c).

**Outbound** (just before the request goes on the wire):

```c
char cookhdr[2048];
cookie_store_get(u.host, u.path, time(0),
                 cookhdr, sizeof(cookhdr));
if (cookhdr[0]) {
    /* Cookie: hobbyos_session=alice; ... */
    /* spliced into the request between Accept-Encoding */
    /* and Connection. */
}
```

**Inbound** (right after the response is parsed):

```c
for (size_t i = 0; i < resp->header_count; i++) {
    if (!s_ieq(resp->headers[i].name, "set-cookie")) continue;
    struct cookie_attr ck;
    if (cookie_parse_set(resp->headers[i].value,
                         resp->headers[i].value_len,
                         time(0), &ck) == 0) {
        cookie_store_set(u.host, &ck);
    }
}
```

The loop matters because `http_get_header()` in
[userspace/libc/http.h](../../../userspace/libc/http.h) returns
*first* match only. A real server can emit several
`Set-Cookie:` lines in one response. Walking
`resp->headers[]` directly captures all of them.

Both binaries log a one-line summary so the regression
test has something to grep for:

```
[browser] sending 1 cookie(s) to 127.0.0.1
[browser] stored 1 cookie(s) from 127.0.0.1
[httpget] sending 1 cookie(s) to 127.0.0.1
[httpget] stored 1 cookie(s) from 127.0.0.1
```

### 6. Server-side `/cookie/{set,whoami,clear}` in httpd

The cookie machinery is hard to test without a server
that participates in the protocol. Rather than spin up
something in the host OS, [userspace/httpd/httpd.c](../../../userspace/httpd/httpd.c)
gains three test endpoints — entirely in-guest, dispatched
before the chapter-105 `is_local_path` check so they don't
hit the VFS:

- `GET /cookie/set` → 200 OK with
  `Set-Cookie: hobbyos_session=alice; Path=/; Max-Age=3600`
  and body `session=alice\n`.
- `GET /cookie/whoami` → reads the request's `Cookie:`
  header, looks for `hobbyos_session=NAME`, returns
  `hello NAME\n` or `anonymous\n`.
- `GET /cookie/clear` → 200 OK with
  `Set-Cookie: hobbyos_session=; Path=/; Max-Age=0` and
  body `cleared\n`.

These are obvious test fixtures and the chapter is
explicit about that — they live in httpd because every
other HTTP server we have is the host's, which means it
doesn't share the guest's cookie jar and isn't useful for
end-to-end verification.

The `whoami` parser does one subtle thing worth calling
out: it only matches `hobbyos_session=` if it appears at
the start of a cookie, i.e. preceded by `;`, space, or
start-of-header. Without that guard, a cookie named
`x-hobbyos_session-fake=evil` would be returned as the
session. Cookies aren't keyed structures over the wire;
they're a `key=value; key=value` string, and parsing them
naively is one of the classic cookie footguns.

### 7. A `/bin/cookies` user tool

The user-facing front for the jar lives at
[userspace/cookies/cookies.c](../../../userspace/cookies/cookies.c).
This is the equivalent of `find ~/Library/Cookies` on a
real OS, except plain text:

```
$ cookies
# 127.0.0.1 (54 bytes)
  hobbyos_session = alice  [path=/]

$ cookies 127.0.0.1
# 127.0.0.1 (54 bytes)
  hobbyos_session = alice  [path=/]

$ cookies clear
cleared 1 jar(s)

$ cookies clear 127.0.0.1
cleared 1 jar
```

Expired cookies are shown but tagged `(expired)` so the
user can see them before the next outbound write removes
them. Session cookies (expiry `0`) are tagged `(session)`.
This is one of those tools you write once and then forget
about until something goes weird with login state, at
which point cd-ing into `/data/cookies` and `cat`ing the
file would tell you the same thing — but having the
inspector makes the chapter's regression test legible
without knowing the on-disk format.

## What this slice does not do yet

- **Cross-origin form blocking.** That's the other half of
  the policy and ships as [chapter 121](121-cross-origin-form-blocking.md):
  the cookie side is in this chapter, the form side is in
  the next one.
- **`Set-Cookie: Domain=` support.** Cookies are
  host-only. Adding Domain would mean writing the
  public-suffix-list lookup logic to stop
  `Domain=com` setting a cookie for every .com site, and
  that's a lot of code for very little value here.
- **`Expires=` parsing.** Only `Max-Age=N` is honoured.
- **Cookie size and count limits per RFC.** We cap at
  64 cookies per host and 512 bytes per line, but don't
  enforce the RFC's "all cookies for a site combined ≤
  4096 bytes" rule.
- **A cookie-management GUI.** The shell tool is enough.

## Key implementation points

### Why a header-only library, not a .o

Every userspace binary that needs HTTP gets the cookie
jar today: `browser` and `httpget`. Adding a third client
later means one `#include`, not a Makefile dance. The
disk format is the API; the C surface is just a typed
wrapper around the file. Header-only also keeps the jar
implementation co-located with its documentation, which
matters here because most of the file is the explanation
of *why* attributes are ignored.

The cost — every binary linking against `cookies.h` gets
its own copy of `cookie_store_set` etc — is irrelevant on
the scale of this OS. Each binary is already statically
linked with its own copy of `printf`, `malloc`,
everything.

### The `freestanding.h` extraction

While bringing this chapter up, the build hit:

```
ld: error: cookies.o: multiple definition of memcpy;
    layout.o: previous definition
```

Both [layout.h](../../../userspace/libc/layout.h) and `cookies.h`
were defining their own private `static memcpy` so that
GCC's optimiser had something to call when it lowered a
big struct copy to a `memcpy` call (the well-known
freestanding-C memset trap). That works fine when only
one of them is included per TU, but `browser.c`
legitimately includes both — layout for rendering,
cookies for the jar — and the two static definitions
clashed.

The fix was a one-file extraction:
[userspace/libc/freestanding.h](../../../userspace/libc/freestanding.h)
holds `memcpy`, `memset`, `memmove` once, marked
`static __attribute__((used))`. Both `layout.h` and
`cookies.h` now `#include "freestanding.h"`. The `used`
attribute keeps `-Werror=unused-function` happy when the
including TU never actually triggers an implicit
mem-call; the `static` keeps each TU's copy private so
two binaries linking the same header don't collide.

This is the kind of refactor we'd otherwise be paying for
over and over each time we add a header-only library
that handles a struct bigger than GCC's inline threshold.
Doing it now means future chapters don't have to think
about it.

### The path-prefix boundary check

`cookie_store_get` filters by path. The check is:

```c
if (request_path starts with cookie.path AND
    (request_path[len(cookie.path)] == '/' OR == 0 OR
     cookie.path ends with '/')) include();
```

The `'/'` boundary check is what stops a `Path=/api`
cookie being sent on a request for `/apifoo`. Without it,
the test fixture would still pass — the test only uses
`Path=/` — but real-world hosts use sub-path scoping and
getting it wrong means cross-app cookie leakage on shared
hostnames.

### Stationary-index directory iteration

[/bin/cookies clear](../../../userspace/cookies/cookies.c) deletes
every jar file by looping on `listdir_at(COOKIE_DIR, 0,
...)` until it returns "no more entries". The trick: it
always asks for index 0 because `unlink` shifts later
entries down. Asking for index 1 after deleting index 0
would skip every other file. Stationary-index iteration
over a shifting directory is one of those things that
looks weird, works correctly, and is much easier to read
than the "stash all names first, then delete"
alternative.

## Regression test

The end-to-end test lives at
[scripts/test_browser_cookies.py](../../../scripts/test_browser_cookies.py).
It mirrors the [test_directories.py](../../../scripts/test_directories.py)
boot harness with one addition — a virtio-net device,
because httpget dials `127.0.0.1:80` and that goes
through the kernel TCP stack which needs the netdev to
have been initialised.

The thirteen checks walk the state machine end-to-end:

1. Boot, `httpget /cookie/whoami` → `anonymous`.
2. `httpget /cookie/set` → body `session=alice`.
3. The same call logs `stored 1 cookie`.
4. `cat /data/cookies/127.0.0.1` contains
   `hobbyos_session`.
5. …and contains the value `alice`.
6. `httpget /cookie/whoami` → `hello alice`.
7. …and logs `sending 1 cookie`.
8. `cookies` lists the host `127.0.0.1`.
9. …and shows `hobbyos_session = alice`.
10. `cookies clear` reports `cleared 1 jar`.
11. `httpget /cookie/whoami` is back to `anonymous`.
12. `httpget /cookie/set` writes the cookie; pre-reboot
    `cat /data/cookies/127.0.0.1` shows it.
13. Reboot the guest. `httpget /cookie/whoami` still
    returns `hello alice` — the durable proof.

That last check is the one that ties everything together.
It's only possible because the OSFS-2 write path from
[chapter 83](../10-filesystem/083-write-back-and-fsync.md)
already gives us `fsync()`, and because `time(NULL)` from
[chapter 96](../12-system-services/096-rtc-and-wallclock.md)
gives the new boot a real wall clock that hasn't rewound
past the cookie's expiry.

## Manual testing

The regression test boots a headless QEMU and drives the
serial console, which is exactly what you want for CI.
For *poking* at cookies — sanity-checking a change, seeing
what httpbin sends, watching the jar fill up — the live OS
is more fun. Two flavours follow: against the in-guest
httpd (deterministic, works offline), and against an
external host (needs networking to reach the wider web).

Either flavour starts from `make run`. Once the desktop is
up, open a `gui_term` window from the launcher; that's
the shell you'll use to inspect the jar.

### Against the in-guest httpd (`/cookie/*`)

init spawns `/bin/httpd` on port 80 at boot, with the
three chapter-110 test endpoints
[wired in](../../../userspace/httpd/httpd.c). No network is required
— traffic stays on the loopback interface.

From a `gui_term`:

```sh
$ httpget http://127.0.0.1/cookie/whoami        # anonymous
$ httpget http://127.0.0.1/cookie/set           # server sends Set-Cookie
$ cat /data/cookies/127.0.0.1                   # jar landed on disk
hobbyos_session  alice  0  /
$ httpget http://127.0.0.1/cookie/whoami        # hello alice (Cookie: sent)
$ httpget http://127.0.0.1/cookie/clear         # server sends Max-Age=0
$ cookies                                       # jar inspector: empty
```

Watch the serial log while you run those — every fetch
logs `[httpget] stored N cookie(s) from <host>` on the way
in and `[httpget] sending N cookie(s) to <host>` on the
way out. Those two lines are the cookie machinery's only
ground truth; if they don't print, nothing landed.

To exercise the same path through the browser app: open
the browser from the launcher, type
`http://127.0.0.1/cookie/set` into the address bar, then
navigate to `http://127.0.0.1/cookie/whoami`. The second
page should render `hello alice`. Same jar — the
`gui_term` can `cat /data/cookies/127.0.0.1` to see what
the browser wrote.

### Against an external host (httpbin)

For a real-world round-trip, [httpbin.org](https://httpbin.org)
exposes `/response-headers` which echoes any query-string
parameter back as a response header. That gives us a
no-redirect, plain-HTTP, real-Set-Cookie endpoint without
having to register an account anywhere:

```sh
$ httpget 'http://httpbin.org/response-headers?Set-Cookie=foo%3Dbar'
httpget: resolved httpbin.org -> 18.233.255.213
[httpget] HTTP/1.1 200 OK (application/json, body=96 bytes)
[httpget] stored 1 cookie(s) from httpbin.org

$ ls /data/cookies
        12  /data/cookies/httpbin.org
$ cat /data/cookies/httpbin.org
foo  bar  0  /

$ httpget 'http://httpbin.org/response-headers?Set-Cookie=foo%3Dbar'
[httpget] sending 1 cookie(s) to httpbin.org           # the proof
[httpget] stored 1 cookie(s) from httpbin.org
```

The same URL in the browser's address bar produces
`[browser] sending 1 cookie(s) to httpbin.org` on the
second navigation — the cookie code is shared, so the
output is identical apart from the prefix.

#### The `http://` matters

Always type the scheme explicitly. Without it the URL
falls into case (6) of
[`canonicalize_url`](../../../userspace/browser/browser.c), which
prepends the proxy prefix `http://127.0.0.1:80/` — so
`httpbin.org/foo` becomes
`http://127.0.0.1:80/httpbin.org/foo`. That goes to the
in-guest httpd, gets forwarded upstream as opaque bytes,
and any `Set-Cookie` in the response ends up keyed under
`127.0.0.1` (or nowhere at all, if there's no upstream
proxy running on the host). The cookie *machinery* still
works, it just stores under the wrong host.

If the browser shows you content but `/data/cookies/`
stays empty — or fills up under `127.0.0.1` instead of
the host you typed — this is what happened.

### One-shot reproducer

[scripts/\_dbg\_external\_cookie.py](../../../scripts/_dbg_external_cookie.py)
automates the whole external-host round-trip: it
reformats `/data`, boots QEMU with the data disk and
virtio-net, runs the three commands above through the
serial console, and dumps the captured `[httpget]` /
`[browser]` log lines plus the jar contents. Re-run
after any change to the cookie path to confirm the
end-to-end loop is still closed.

## Applied to

- Existing apps modified:
  - [userspace/browser/browser.c](../../../userspace/browser/browser.c) —
    inbound Set-Cookie capture + outbound Cookie injection.
  - [userspace/httpget/httpget.c](../../../userspace/httpget/httpget.c) —
    same hook pair; the CLI client now participates in
    the same jar the browser uses.
  - [userspace/httpd/httpd.c](../../../userspace/httpd/httpd.c) —
    new `/cookie/{set,whoami,clear}` dispatch and the
    `send_status_with_extra` / `find_header` helpers.
  - [userspace/libc/layout.h](../../../userspace/libc/layout.h) —
    private mem* shims replaced with
    `#include "freestanding.h"`.
- New apps:
  - [userspace/cookies/cookies.c](../../../userspace/cookies/cookies.c) —
    the `/bin/cookies` jar inspector.
- New libc headers:
  - [userspace/libc/cookies.h](../../../userspace/libc/cookies.h) —
    the jar itself.
  - [userspace/libc/freestanding.h](../../../userspace/libc/freestanding.h) —
    extracted mem* shims, shared between libc headers.
- Build wiring:
  - [Makefile](../../../Makefile) — `COOKIES_OBJS` / `COOKIES_ELF`
    / `COOKIES_STRIPPED` block + master STRIPPED list +
    `cookies=` arg to `mkosfs.py`.
- Tests added:
  - [scripts/test_browser_cookies.py](../../../scripts/test_browser_cookies.py) —
    13 PASS / 0 FAIL on first green run.
  - [scripts/\_dbg\_external\_cookie.py](../../../scripts/_dbg_external_cookie.py) —
    one-shot external-host probe used to confirm
    end-to-end cookie storage against
    `httpbin.org/response-headers`. Kept per the
    debug-scripts-policy.

## What this unlocks

- A user can log into the in-guest httpd and stay logged
  in across navigations.
- The browser can persist preferences (when chapter 122
  ships JS and `document.cookie` lands, the storage path
  is already done).
- `/bin/httpget` becomes a useful tool for poking at
  cookie-protected endpoints; same jar as the browser
  means you can debug a logged-in session from the shell.
- [Chapter 121](121-cross-origin-form-blocking.md)
  (cross-origin form blocking) lands as a small slice on
  top of this one: the cookie half — which is the hard
  half, because it needs disk + parser + protocol — is done.
- Chapter 122 (pocket JavaScript) gets to assume the jar
  exists when wiring `document.cookie`.
