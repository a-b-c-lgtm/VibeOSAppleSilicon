# Chapter 112 — TLS options: honest proxy, or BearSSL

**Status:** Stub. Tracking milestone 99.

The book has dodged TLS by running an unencrypted HTTP
proxy on the host (chapters 71 and 64's footnotes). This
chapter is the honest discussion of the trade-offs and
two paths forward.

## What this chapter adds

- A complete write-up of the existing
  `scripts/https_proxy.py` — what it actually does, what
  it cannot do (cert pinning, mTLS, custom roots).
- A path-to-real-TLS plan if the reader wants to take it:
  port BearSSL (≈30 KLOC, freestanding-clean,
  AArch64-friendly) and wire it into a `tls_socket_*` API
  layered above `tcp_socket_*`.
- Discussion of what TLS would *not* automatically give
  us: certificate validation requires a root store; OCSP
  needs a clock + DNS; modern handshakes negotiate
  options whose space we'd have to navigate.

## Prerequisites

- Chapter 71 — browser
- Chapter 91 — wall-clock time (TLS needs notBefore /
  notAfter)
- Chapter 106 — loopback (we can run a TLS listener on
  ourselves to test)

## Plan

- Decide: ship the chapter as a *guide*, not as code.
  The reader picks their own adventure.
- Provide a porting checklist for BearSSL: the four
  freestanding gotchas, the build-system glue, where to
  drop in the kernel's PRNG.
- Document the security caveats of the existing proxy in
  big bold letters.

## What you'll learn

- Why TLS is not just "encrypt the bytes" — it is a
  certificate ecosystem, a clock, a PKI.
- Why a kernel like ours can pretend it does not need
  TLS for so long, and what changes the moment it does.

## What this unlocks

- The optional path to a fully self-contained networking
  stack.
- Honesty about a thing the book has been hand-waving for
  a while.
