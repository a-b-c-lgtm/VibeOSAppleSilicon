# Chapter 110 — Cookies and the Same-Origin Policy

**Status:** Stub. Tracking milestone 97.

Forms (chapter 109) lets users log in. Without cookies
there is no way to *stay* logged in. And without a
Same-Origin Policy, cookies become a security disaster.
This chapter does both at once because doing one without
the other is a trap.

## What this chapter adds

- A cookie store at `/data/cookies/<host>` (one file per
  host, append-only line format).
- Browser sends `Cookie:` on every request whose host has
  a non-empty file and whose cookies have not expired.
- Browser parses `Set-Cookie:` from responses and updates
  the store.
- Same-Origin Policy enforcement points (briefly):
  - Cookies for `a.example.com` are not sent to
    `b.example.com`.
  - Form submission to a different origin is blocked
    *unless* the form's `action=` was already cross-origin
    at parse time (no third-party redirect smuggling).
  - JavaScript (chapter 111) cannot read cross-origin
    documents.

## Prerequisites

- Chapter 84 — persistence on disk
- Chapter 109 — forms (we need somewhere to send cookies)
- Chapter 91 — wall-clock time (cookie expiry)

## Plan

- Cookie attributes we honour: `Expires`, `Path`, `Secure`,
  `HttpOnly`, `SameSite=Strict`. The rest are ignored.
- A `/bin/cookies` user tool to list/delete cookies.
- The browser address bar shows a small lock when
  the connection is "secure" via the proxy (still not
  real TLS — see chapter 112).

## What you'll learn

- Why "just send the cookie" turned out to be a 30-year
  security saga.
- The Same-Origin Policy is more about "prevents the
  default behaviour from being a footgun" than "blocks
  attackers" — attackers still have many tools.

## What this unlocks

- Logged-in browsing.
- Real talk in the JavaScript chapter about why
  document.cookie is so heavily restricted.
