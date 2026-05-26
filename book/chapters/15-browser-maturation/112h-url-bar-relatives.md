# Chapter 112h — URL-bar UX after native TLS

## What was suddenly wrong after 112g

Chapter 112g shipped two things: real public CA roots, and an
end-to-end probe that proved `browser https://news.ycombinator.com/`
worked. The very first time a human (not the probe) used the
browser interactively after that landed, two URL-bar bugs
surfaced that the regression sweep had hidden for the entire
TLS arc:

1. **Bare hostnames went through the proxy.**
   Typing `news.ycombinator.com` (no scheme) in the URL bar
   produced `http://127.0.0.1:80/news.ycombinator.com` — the
   chapter 106b in-guest proxy prefix — which then 502'd
   because no upstream was configured. The user expected
   `https://news.ycombinator.com/`, the shape every browser
   from Mosaic on has produced for a bare host.

2. **Relative links from a real HTTPS page went through the
   proxy.**
   On `https://news.ycombinator.com/`, clicking a comment
   link whose `href="item?id=48225838"` navigated to
   `http://127.0.0.1:80/item?id=48225838` instead of
   `https://news.ycombinator.com/item?id=48225838`. The
   page itself was loaded correctly over native TLS; only
   link follows fell off the cliff.

Both bugs lived in the same function — `canonicalize_url` in
[`userspace/browser/browser.c`](../../../userspace/browser/browser.c) —
and both came from the same root cause: that function was
written in chapter 106b, when "fetching anything outside the
guest" meant "send it to the in-guest httpd proxy". Three
chapters of native TLS work later (112d → 112g) the proxy
default was no longer the right default, but `canonicalize_url`
was still the one place every other path funnelled through.

This chapter changes its defaults to match the native-TLS world
the browser now lives in, while keeping the proxy path alive
for the legacy regression callers that explicitly opt in via
`BROWSER_PROXY`.

## What `canonicalize_url` did before this chapter

The function classified its input into six cases (numbered in
the source comment) and produced a freshly-allocated absolute
URL ready for `load_page`:

| # | Pattern | Old behaviour |
|---|---|---|
| 1 | `/mnt/...`, `/bin/...`, `/dev/...` | passthrough — local fs |
| 2 | `http://...` | passthrough (with proxy rewrite when `BROWSER_PROXY` set) |
| 3 | `https://...` | passthrough (with proxy rewrite when `BROWSER_PROXY` set) |
| 4 | `//host/path` | **prepend `g_proxy_prefix`** unconditionally |
| 5 | `/path` (current is http(s)) | combine with current's `scheme://host` |
| 6 | anything else | **prepend `g_proxy_prefix`** unconditionally |

Case (3) had been updated in chapter 112d to passthrough when
no proxy env was set. Cases (4) and (6) had not. And there was
no case for the most common shape of relative link in real HTML:
a bare path-reference like `item?id=42`.

## What changes

### 1. Case (6): default to `https://` when no proxy is set

`g_proxy_was_set` is read once at `main()` from the
`BROWSER_PROXY` env var. When it is set (proxytest,
`scripts/test_browser_hn_*.py`) we keep the chapter 106b
rewrite — those harnesses rely on it and there is no reason
to break them. When it is not set, the default flips:

```c
const char *prefix = g_proxy_was_set ? g_proxy_prefix
                                     : "https://";
```

That is the entire fix for bug 1. Typing
`news.ycombinator.com` in the URL bar now produces
`https://news.ycombinator.com`, which falls into case (3)'s
passthrough on the next call (it is fed back through
`canonicalize_url` from `navigate_to`), goes into
`br_conn_open`, gets a TLS handshake, validates against the
bundle anchors shipped in chapter 112g, and renders. Same
keystrokes as Firefox, same result.

### 2. New case (5a): path-relative resolution

Between the old cases (5) and (6), a new branch handles the
classic relative-reference case from RFC 3986 §5.2.3:

```c
if (current &&
    (br_starts(current, "http://") || br_starts(current, "https://"))) {
    int looks_like_host = 0;
    for (const char *p = input; *p && *p != '/' && *p != '?'; p++) {
        if (*p == '.' || *p == ':') { looks_like_host = 1; break; }
    }
    if (!looks_like_host) {
        char merged[BR_URL_BUF_CAP];
        if (resolve_url(current, input, merged, sizeof merged) == 0)
            return br_strdup(merged);
    }
}
```

The merge itself is delegated to `resolve_url`, the helper
chapter 110a already wrote for form actions. It does the
"strip after the last `/` in the base path, then concat the
ref" dance per §5.2.3 — and it has been on the regression
treadmill (test_browser_proxy.py, test_browser_self.py) ever
since.

The only new logic here is the "looks like a host" heuristic
that decides whether a string with no scheme is a hostname or
a relative path-reference. The rule:

> A bare hostname must contain a `.` (TLD) or a `:` (port) in
> the authority position — that is, before the first `/` or
> `?`. Anything else is a relative path-reference.

| Input | First `.`/`:`? | Treated as |
|---|---|---|
| `news.ycombinator.com` | `.` in `news` | host (→ case 6) |
| `localhost:8443` | `:` after `localhost` | host (→ case 6) |
| `127.0.0.1` | `.` after `127` | host (→ case 6) |
| `item?id=42` | none before `?` | relative (→ case 5a) |
| `vote?id=43&how=up` | none before `?` | relative (→ case 5a) |
| `from?site=example` | none before `?` | relative (→ case 5a) |
| `news` (HN section link) | none | relative (→ case 5a) |
| `dir/file.html` | none before `/` | relative (→ case 5a) |

This is the same heuristic Chrome uses for its omnibox before
the public-suffix list overrides kick in. It has a known
false-positive — `y18.svg` looks like a hostname under this
rule because of the dot — but that path doesn't matter for
us in practice. Image and stylesheet resource fetches go
through `canonicalize_url(src, /*current=*/0)`, so they never
reach case (5a) at all: NULL current means the heuristic
branch is skipped entirely. Documented in the code so future
me doesn't try to "fix" the false positive and break the
opposite class of pages.

## Why both fixes share one place

Every URL the browser ever loads — from argv, from the URL
bar, from a link click, from a form submit, from a history
nav — funnels through `canonicalize_url`. That funnel was
designed in chapter 106b to enforce one rule: "if it isn't
local, route it through the proxy." Three chapters later
that rule was no longer true for `https://`, but the funnel
had three other gates with the same wrong rule baked in.

Putting the fixes anywhere else (in `navigate_to`, in the
URL-bar input handler, in the link-click handler) would
have meant three places to keep in sync forever. Updating
the funnel keeps the policy in one spot.

## Traps for the next chapter to know

### `BROWSER_PROXY` must still work

`scripts/test_browser_proxy.py` and `scripts/test_browser_hn_*.py`
both set `BROWSER_PROXY` before spawning `browser`. They expect
case (6) to rewrite bare hosts into proxy URLs and case (3) to
do the same for `https://`. The `g_proxy_was_set` gate
preserves both behaviours; **do not** drop the legacy path
without first migrating those tests. The regression sweep run
at the end of this chapter includes `test_browser_proxy.py` and
verifies it still exits 0.

### Relative resolution in proxy mode

The new case (5a) fires in **both** modes. In proxy mode the
current page URL is the rewritten transport URL
(`http://127.0.0.1:80/news.ycombinator.com/news`), so
`resolve_url(current, "item?id=42", ...)` produces
`http://127.0.0.1:80/news.ycombinator.com/item?id=42` — which
is exactly the right transport URL for the next proxy fetch.
The merge logic happens to work uniformly across both modes
because it operates on the *current page URL as the browser
already has it*, not on the user-visible URL.

### Case (4) `//host/path` is still wrong

Protocol-relative refs like `<a href="//other.com/path">`
currently unconditionally prepend `g_proxy_prefix`. This is
the same class of bug as the case (6) one this chapter fixed,
but no page we routinely visit (HN, example.com) emits them,
and the user did not report the issue. Left as a follow-up
to keep the diff focused. Fixing it is a one-line change of
"use `g_proxy_prefix`" to "use current page's scheme, or
`https://` if none" — a future chapter can pick it up when a
real site forces the issue.

### Fragment-only and query-only refs are still wrong

`#section` and `?id=42` (with no path) refs both fall into
case (5a) and get merged as if they were path refs, which
RFC 3986 §5.2.3 says they aren't. Same deferral logic as
case (4): real-world pages we visit don't depend on this
yet. `resolve_url` itself would need extending; chapter 110a
already documents this limitation for the form-action path.

## Applied to

- [`userspace/browser/browser.c`](../../../userspace/browser/browser.c) —
  `canonicalize_url` case (5a) inserted, case (6) default
  flipped to `https://` when `BROWSER_PROXY` is unset.
  Additionally, a latent bug in `resolve_url` was uncovered
  the moment case (5a) started routing link-click refs
  through it (see next section). And the vestigial "skip
  https stylesheets" guard in `apply_link_sheets` was
  removed -- the fetch path has been transport-agnostic
  since chapter 112d, the guard just hadn't been cleaned up
  yet. No other callers changed; all six existing call sites
  (`navigate_to`, `run_gui` startup, `run_bench_resize`, the
  plain/ansi mode path, `form_resolve_action_url` and its
  non-http fallback) pick up the new behaviour transparently.

## The `resolve_url` latent bug case (5a) uncovered

First live test of case (5a) against
`https://news.ycombinator.com/` (an https page loaded from a
bare-host URL with **no path component**) printed three things
that all looked unrelated and turned out to share a single
root cause in `resolve_url`:

```
[browser] skip sheet (https not supported): https://news.ycombinator.com
[browser] resolved news.ycombinator.com -> 209.216.230.207
[browser] HTTP/1.1 200 OK (text/html; charset=utf-8, body=34236 bytes)
[png] bad signature at byte 0
[browser] png_decode failed for https://news.ycombinator.com (negative-cached)
```

The stylesheet URL was wrong (truncated to the bare host).
The image fetch URL was wrong (same truncation, the HTML body
got decoded as PNG). And clicking a comment link
re-navigated to the front page instead of `item?id=...`.

All three came from `resolve_url`'s "path-relative" branch:

```c
/* Find the last '/' in [path_start, path_end). */
const char *last_slash = path_start;        // <-- BUG
for (const char *q = path_start; q < path_end; q++) {
    if (*q == '/') last_slash = q;
}
size_t prefix_len;
if (last_slash >= path_start) {             // <-- always TRUE
    prefix_len = (size_t)(last_slash - base_url) + 1;
} else {
    /* No path in base; synthesise "/". */  // <-- dead code
    ...
}
```

`last_slash` was pre-initialised to `path_start` so the
`>=` test was always true and the "no path" else branch was
unreachable. For a base URL like `https://news.ycombinator.com`
(no trailing `/`), `path_start` sits at the trailing `\0`,
the for-loop never assigns, and `prefix_len` becomes
`strlen(base) + 1` — one past the end of the string. The
copy reads the `\0`, plants it into `out[host_len]`, then
writes the ref starting at `out[host_len + 1]`. As a C
string, the output is just the bare host — the ref is
sitting in memory after a NUL that nobody can see.

This bug was latent for the entire 109a → 112g arc because:

- Stylesheets only get fetched for `http://` pages (chapter
  110a forms), and no test page exercised a no-path base.
- Image src resolution started using `resolve_url` (chapter
  98) but every page that had images had a path
  (`/mnt/test_layout.html`, the chapter 112d localhost test,
  ...).
- Link clicks never went through `resolve_url` at all — they
  went through `canonicalize_url` case (6) which prepended
  the proxy prefix.

Case (5a) is the first code path that funnels link-click
refs into `resolve_url`. The very first real public site we
clicked a link on (HN, no trailing slash) immediately
surfaced the bug.

The fix is one local change to the path-relative branch:
leave `last_slash` as `NULL` initially, then `if (last_slash)`
takes the "found a `/`" path and `else` synthesises one.
After the fix the same probe shows the stylesheet URL
correctly as
`https://news.ycombinator.com/news.css?lzHURXftP8FujYvlbdfN`,
image fetches resolve properly, and comment-link clicks
navigate to `https://news.ycombinator.com/item?id=...` as
the user expects.

### Drive-by: drop the legacy https-stylesheet skip guard

With `resolve_url` returning well-formed `https://...` URLs
for HN's relative stylesheet refs, the next thing the user
noticed was that the browser printed

```
[browser] skip sheet (https not supported): https://news.ycombinator.com/news.css?lzHURXftP8FujYvlbdfN
```

That guard was a vestige from before chapter 112d. Since
112d the fetch path is uniform: `fetch()` -> `http_fetch()`
-> `http_fetch_one()` -> `br_conn_open()`, and
`br_conn_open` already picks the right transport (TLS for
`https://`, plain TCP for `http://`) and validates the
chain against the bundle. There is no reason left to skip
https stylesheets.

`apply_link_sheets` in [userspace/browser/browser.c](../../../userspace/browser/browser.c)
used to read:

```c
if (br_starts(abs, "https://")) {
    printf("[browser] skip sheet (https not supported): %s\n", abs);
    continue;
}
if (!br_starts(abs, "http://")) {
    printf("[browser] skip sheet (unsupported scheme): %s\n", abs);
    continue;
}
```

The https branch was deleted, leaving the single
"non-http(s) scheme" gate:

```c
if (!br_starts(abs, "http://") && !br_starts(abs, "https://")) {
    printf("[browser] skip sheet (unsupported scheme): %s\n", abs);
    continue;
}
```

Live probe after the change shows two TLS handshakes (one
for the page, one for the stylesheet) and the stylesheet
body arriving as `text/css, body=7350 bytes` -- HN's actual
rendered CSS is now in the page.

The chapter 110a memory note already documented several
limitations of `resolve_url`; this bug was not among them
because it had never been observed. Now it is fixed.

## What gets exercised in tests

- [`scripts/test_browser_https.py`](../../../scripts/test_browser_https.py)
  (chapter 112d) — PASS. Native TLS to `https://localhost:8443/`
  unaffected; the URL is already absolute so it hits case (3)'s
  passthrough.
- [`scripts/test_browser_https_multi.py`](../../../scripts/test_browser_https_multi.py)
  (chapter 112e) — PASS. Multi-anchor TLS path unaffected.
- [`scripts/test_tls_pem_bundle.py`](../../../scripts/test_tls_pem_bundle.py)
  (chapter 112f) — PASS. Bundle-format invariants unaffected.
- [`scripts/test_browser_proxy.py`](../../../scripts/test_browser_proxy.py)
  (chapter 106b) — PASS. The critical proxy regression: with
  `BROWSER_PROXY` set, case (6) still prepends the proxy
  prefix exactly as before. This is the test that proves the
  legacy gate works.
- [`scripts/test_browser_self.py`](../../../scripts/test_browser_self.py)
  (chapter 106c) — PASS. End-to-end loop through the in-guest
  proxy still closes.
- [`scripts/_dbg_bare_host.py`](../../../scripts/_dbg_bare_host.py)
  (NEW, per `/memories/debug-scripts-policy.md`) — manual
  probe, not in regression sweep. Boots the guest, runs
  `browser news.ycombinator.com` (bare hostname, no proxy
  env), asserts the captured serial log contains
  `[browser] resolved news.ycombinator.com -> <ipv4>` AND
  `TLS handshake OK`, AND that `127.0.0.1` never appears in
  the output (the regression sentinel that the proxy path
  wasn't silently taken). Confirms bare-host → https://
  default flip works against the live HN server.
- [`scripts/_dbg_hn_resources.py`](../../../scripts/_dbg_hn_resources.py)
  (NEW, per `/memories/debug-scripts-policy.md`) — manual
  probe for the `resolve_url` "base has no path" fix and the
  follow-on https-stylesheet enable. Boots the guest, runs
  `browser https://news.ycombinator.com/`, asserts the serial
  log does NOT contain the two truncation symptoms
  (`png_decode failed for https://news.ycombinator.com\n` or
  any `skip sheet (https not supported)` line at all -- the
  guard is gone). It also asserts the POSITIVE outcome: the
  log must contain
  `fetching stylesheet https://news.ycombinator.com/news.css`
  AND at least TWO `TLS handshake OK with news.ycombinator.com:443`
  lines (one for the page, one for the stylesheet). PASS
  confirmed live against HN.

## Limitations

Same shape as the chapter 112g and 110a limitations list:

- No public suffix list; the "dot or colon means hostname"
  heuristic is good enough for the URL bar, not for the
  general web.
- Case (4) (`//host/path`) and the fragment-only / query-only
  refs still go through the old codepaths.
- The URL bar has no search-engine fallback; typing `foo` (no
  dot, no current page) becomes `https://foo/` which dies in
  DNS. That's fine for a teaching browser. Real browsers fall
  back to "did you mean to search for this?" via a configured
  search engine, which we deliberately don't have.

None of these block the next chapter (TLS arc is closing); they
are warts on the URL-bar UX that a future "polish the omnibox"
chapter can mop up.
