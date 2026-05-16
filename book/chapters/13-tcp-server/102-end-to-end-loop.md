# Chapter 102 — End to end: the browser fetches from its own kernel

**Status:** Stub. Tracking milestone 95.

The capstone of Part XIII. We finally close the loop:
`/bin/httpd` runs in one window, `/bin/browser` in another,
and the browser fetches a page from the same kernel — no
host-side proxy, no QEMU SLIRP redirect, no external
program in the path. Just two of our own programs talking
TCP over `127.0.0.1`.

## What this chapter adds

- Loopback support in net.c (deliver-locally if dest IP is
  127/8 or our own IP).
- A `/data/www/index.html` that links to a few other pages
  in the same tree.
- A boot-time auto-spawn for `/bin/httpd` (one-line addition
  to init).
- A scripted regression: spawn both, navigate, screenshot,
  diff.

## Prerequisites

- Chapter 101 — httpd
- Chapter 71 — browser

## Plan

- Loopback shortcut bypasses virtio-net entirely (handled
  in `net_send_ipv4`).
- One-time-only chapter: it's mostly a celebration of the
  layers all working together.

## What you'll learn

- The "loopback is a special case of routing" framing.
- Why even single-machine setups want a loopback path
  (testability, daemon discoverability).

## What this unlocks

- A safer way to develop browser features (no host
  network needed for testing).
- The Part XIV browser-maturation work has somewhere to
  point its requests.
