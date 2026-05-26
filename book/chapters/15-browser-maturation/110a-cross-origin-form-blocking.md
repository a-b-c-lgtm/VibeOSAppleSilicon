# Chapter 110a — Cross-origin form submission blocking

[Chapter 110](110-cookies-and-sop.md) shipped the cookie half
of the Same-Origin Policy: cookies are keyed by exact host
string, so a page on host A can never read or send host B's
cookies. That stops a whole class of cookie-leakage bugs, but
it doesn't stop the *other* SOP problem: a page that talks
the user into typing their password into a form, then submits
that form to a different host. Cookies aren't involved at
all — the user is the leak.

This chapter is the form-submission half. It's a tight slice:
one new header, one branch in the form-submit path, one
debug subcommand, one regression test, no new applications.

## What ships in this slice

### 1. Origin equality at `userspace/libc/origin.h`

"Origin" is the trust boundary the SOP guards: the
`<scheme, host, port>` triple of a URL. Two URLs share an
origin iff all three are byte-equal (host folded to lower
case, port defaulted from scheme). The new header gives the
browser one call:

```c
int origin_eq(const char *url_a, const char *url_b);
```

It also exposes one tiny syntactic helper:

```c
int origin_action_is_absolute(const char *action_attr);
```

— which returns 1 if the raw `<form action=...>` string
starts with `http://`, `https://`, or `//`. That's the "author
wrote an absolute URL with an explicit authority" signal the
policy needs.

The header itself is a thin wrapper over
[`url.h`](../../../userspace/libc/url.h)'s existing `url_parse`. The
parse work is already done; this header just calls it twice
and compares the results.

### 2. The three-branch policy

`form_submit_url_at()` in [browser.c](../../../userspace/browser/browser.c)
now runs an origin compare every time the user clicks a submit
control. Three outcomes:

| page origin → action origin                | what happens                  |
| ------------------------------------------ | ----------------------------- |
| same                                       | silent allow                  |
| different, action attr is absolute         | allow + log                   |
| different, action attr is relative         | **BLOCK** + log               |

The log line shows the destination URL so the user (and the
regression test) can tell which origin was involved:

```
[browser] SOP: cross-origin form submit to http://example.com/login (author-declared, allowed)
[browser] SOP: blocked cross-origin form submit to http://evil.com/login (relative action resolved cross-origin)
```

The block branch is currently **unreachable** in this
codebase: there's no `<base href>` (chapter 113+ territory)
and no JavaScript (chapter 111+), so a relative action can
only resolve to the same origin as the page. The check is
defense-in-depth — it costs ten lines now and means the
chapter 111 author doesn't have to remember to wire SOP
themselves when `form.action = ...` becomes assignable from
JS.

### 3. SOP compares the *raw* URL, not the proxy form

A subtle bug found during bring-up: chapter 106b's HTTPS
proxy rewrites `https://example.com/x` into the loopback
transport URL `http://127.0.0.1:80/example.com/x`. If we
ran the SOP compare on the post-canonicalisation URL, a
cross-scheme action (`https://` from an `http://` page)
would compare same-origin because both got rewritten into
`http://127.0.0.1:80/...`.

The fix is a sister helper to `form_resolve_action_url`:

```c
static char *form_resolve_action_url_raw(const char *current_url,
                                         const char *action);
```

Same resolve logic, no canonicalisation. The submit path
uses the raw version for the SOP decision and keeps using
the canonicalised version for the actual network fetch. SOP
is about author intent; canonicalisation is a transport
detail. They genuinely are two different concerns and
braiding them caused the bug.

### 4. Headless debug subcommand: `browser --check-sop`

The full path through `form_submit_url_at` only runs in GUI
mode after a virtio-tablet click event. That's
heavyweight to test ([test_browser_forms.py](../../../scripts/test_browser_forms.py)
does it; the harness is 200+ lines). For chapter 110a we
want fast feedback on the *policy*, separately from the
GUI plumbing. The new flag:

```
browser --check-sop <page-url> <action-attr> [resolved-override]
```

…runs the same `origin_eq` + `origin_action_is_absolute`
calls the production path uses, prints one of three
deterministic lines, and exits. No network, no parser, no
window server:

```
$ browser --check-sop http://a.com/ /login
SOP: same-origin (http://a.com/login)

$ browser --check-sop http://a.com/ http://b.com/login
SOP: cross-origin allowed (http://b.com/login)

$ browser --check-sop http://a.com/ /login http://b.com/login
SOP: blocked (http://b.com/login)
```

The third positional argument is the synthetic override: if
provided, it skips `resolve_url` and uses the given string
as the "resolved" URL directly. That's how the regression
test reaches the blocked branch — it simulates what chapter
113's `<base href>` would surface today.

## What this slice does not do yet

- **`<base href>` parsing.** Without it the blocked branch
  is unreachable in production. Chapter 113 ships this.
- **GUI affordances** when a block happens. The current
  block is silent except for a serial log line; a future
  chapter could surface it as a browser-chrome banner.
- **Redirect-following SOP.** If the action's host
  302-redirects to a third host, the browser follows the
  redirect without re-running SOP. (We don't follow
  redirects at all yet — chapter 106b explicitly punts.)
- **CSRF token plumbing.** SOP blocks the gross case
  (cross-origin password submission); CSRF tokens are the
  fine-grained defence and need server-side support. Not
  this OS's problem until we have an app that authenticates
  via cookies *and* has destructive POST endpoints.

## Key implementation points

### Why the policy is "absolute action ⇒ author intent"

The threat model for cross-origin form submission is:

> User trusts page A. User types a password. The form
> submits somewhere unexpected. Password is leaked.

If the form's action attribute literally contains
`http://other-host/login`, the page author wrote that on
purpose. The user might still be deceived ("why is this
login form on a.com pointing at b.com?"), but that's a
phishing-style social problem, not a browser problem; the
browser can't tell from markup whether `<form
action="https://login.bank.com/">` is legitimate (it
might be) or malicious (it might also be).

The browser *can* tell when the markup says one thing and
the request goes somewhere else — that's the cross-origin
*via* relative action case. A relative path that resolves
to a different host is always a sign that something rewrote
the resolution at runtime: a `<base href>` from
attacker-controlled HTML, a JS `form.action = "..."`
assignment, or a redirect-as-action trick. The user
*can't* see that from the page; the browser must.

This is the same logic real browsers use, just hidden under
several layers of spec text. CORS, `<base href>`, the HTML
spec's "form submission algorithm" all hash out the
details, but the underlying intuition is: trust the markup
*as-written*; distrust the markup *as-rewritten*.

### Why exact origin equality, not eTLD+1

Real browsers cluster subdomains via the public suffix list:
`docs.google.com` and `mail.google.com` share an eTLD+1 of
`google.com` and so are "same-site" for some policies (like
SameSite cookies). We don't.

The PSL is a 250 KiB external dataset, updated weekly,
that you must keep in sync with reality or your SOP rule
gets subtly wrong. We don't have a way to ship that, and
exact origin equality is correct (just stricter than real
browsers). A page on `a.example.com` simply cannot share
state with `b.example.com` in this OS; cookies are
host-only, form submits cross-subdomain are "cross-origin
allowed" (you wrote an absolute URL, you meant it). That's
fine for the apps this OS hosts.

### Why a `--check-sop` flag instead of a libtest

Two reasons:

1. The browser binary already exists, already links
   against `origin.h`, already runs in the guest. Adding a
   8-line subcommand exercises the *exact* code paths the
   production submit takes. A separate libtest would need
   to duplicate the resolve logic and could drift.
2. The test harness mirrors
   [test_browser_cookies.py](../../../scripts/test_browser_cookies.py)
   — boot, drive the shell, grep the serial log. That's
   the pattern this book uses everywhere; making chapter
   110a follow it keeps the book's test surface uniform.

The cost — one more flag in `usage()` — is small and the
flag is genuinely useful when poking at SOP behaviour
interactively.

## Regression test

[scripts/test_browser_sop.py](../../../scripts/test_browser_sop.py)
walks eight policy points in one boot:

1. Relative action → same-origin.
2. Absolute action to same `host:port` → same-origin.
3. Absolute action to different host → cross-origin allowed.
4. Absolute action to different port → cross-origin allowed.
5. `https://` action from `http://` page → cross-origin
   allowed (different scheme).
6. Protocol-relative `//host/path` cross-host →
   cross-origin allowed.
7. Synthetic resolved-override (relative action that
   resolved cross-origin) → blocked.
8. Hosts compared case-insensitively
   (`Example.COM` vs `example.com`) → same-origin.

Sub-check 5 is the one that caught the canonicalisation bug
during bring-up. Without the raw-resolve sister helper, the
proxy rewrite folded `https://127.0.0.1/login` and the
`http://127.0.0.1:80/index.html` page into the same
transport URL and SOP wrongly returned "same-origin". The
fix landed before this chapter shipped.

Sub-check 7 is the blocked branch via the resolved-override
arg — the only way to reach it today.

The harness is intentionally minimal: no virtio-net, no
httpd dependency, no GUI. It runs in under 30 seconds.

## Applied to

- Existing apps modified:
  - [userspace/browser/browser.c](../../../userspace/browser/browser.c) —
    SOP check at form submit, raw-resolve helper,
    `--check-sop` debug subcommand.
- New libc headers:
  - [userspace/libc/origin.h](../../../userspace/libc/origin.h) —
    `origin_eq` + `origin_action_is_absolute`.
- Existing book wiring:
  - [book/INDEX.md](book/INDEX.md) — chapter linked under
    Part XV.
- Tests added:
  - [scripts/test_browser_sop.py](../../../scripts/test_browser_sop.py) —
    8 PASS / 0 FAIL.
- Tests still green (verified post-change):
  - [scripts/test_browser_forms.py](../../../scripts/test_browser_forms.py)
    — the same-origin happy path still passes through the
    new SOP gate unchanged.
  - [scripts/test_browser_cookies.py](../../../scripts/test_browser_cookies.py)
    — chapter 110 round-trip still passes, confirming the
    SOP wiring didn't perturb cookie injection or capture.
  - [scripts/test_browser_self.py](../../../scripts/test_browser_self.py)
    — the in-guest browser-to-httpd loop still closes.

## What this unlocks

- Chapter 111's pocket JavaScript can ship `form.action`
  assignment without a security review — the SOP gate is
  already in place and will block a malicious
  `form.action = "http://evil.com/"` mutation.
- Chapter 113's VFS work can land `<base href>` parsing
  without a separate cross-origin form audit; the blocked
  branch becomes reachable but the policy that handles it
  already exists.
- Together with chapter 110, the OS now has both halves of
  the practical SOP: cookies are host-keyed (no
  cross-origin cookie leak) AND form submits respect
  author intent (no cross-origin password leak via
  rewritten action). That's the SOP that matters for the
  apps this OS hosts.
