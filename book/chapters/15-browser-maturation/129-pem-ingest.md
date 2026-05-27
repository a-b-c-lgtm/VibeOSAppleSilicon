# Chapter 129 — PEM ingest and the recursive chain walk

Chapter 128 shipped a multi-anchor trust store. The two
anchors it carried were both *intermediate* CAs (CERT1 from
BearSSL's `chain-rsa.h` and `chain-ec.h`). That's a perfectly
valid trust shape, but it sidestepped the most interesting
property of a real PKI: the validator's recursive walk from a
trusted root, through one or more intermediates, down to the
end-entity certificate.

Chapter 129 takes two small steps that unlock that walk:

1. Teach `scripts/mkcabundle.py` to ingest **PEM**, the format
   every real-world CA distribution uses (Mozilla NSS, OS
   bundles, `openssl x509`, every `Let's Encrypt` flow).
2. Re-point the build at the BearSSL sample **root** PEMs
   (`cert-root-rsa.pem`, `cert-root-ec.pem`) instead of the
   intermediates extracted from `chain-*.h`. The intermediates
   are still served by `/bin/httpsd` as part of its handshake
   chain, so the validator now has to walk all the way up.

No guest C code changes. The wire-level behaviour of the TLS
handshake doesn't change either — the same `br_x509_minimal_*`
validator was already capable of multi-link walks; we just
weren't asking it to do one.

## What ships

- `scripts/mkcabundle.py` — adds a `--pem PATH` mode alongside
  the original positional `HEADER.h` mode. PEM mode regexes out
  every `-----BEGIN CERTIFICATE----- ... -----END CERTIFICATE-----`
  block, base64-decodes each, and appends as a fresh DER blob
  to the bundle. Tolerates surrounding whitespace, other PEM
  block types (`PRIVATE KEY`, etc.), and per-file multi-cert
  bundles in the same input. The two modes interoperate: a
  single `mkcabundle.py` invocation can mix any combination of
  `--pem` and `.h` inputs and they all end up in the same
  bundle in argv order.
- `Makefile` — the `assets/osfs/ca.bundle` rule now points at
  `vendor/bearssl/samples/cert-root-rsa.pem` +
  `cert-root-ec.pem` via `--pem`. Both are the BearSSL sample
  ROOT certs (the ones that signed the intermediates that
  signed the leaves in chapter 128's anchors).
- `userspace/libc/tls_socket.h` — `TLS_MAX_ANCHORS` bumped from
  8 to 32 (~1.2 KiB scratch per slot in the `tls_socket_t`
  struct), giving us room for a curated public-root set without
  another recompile.
- `userspace/browser/browser.c` — `BR_CA_BUNDLE_MAX` raised
  from 32 KiB to 256 KiB so a future drop of the full Mozilla
  NSS bundle (~150 KiB) loads as-is.
- `scripts/test_tls_pem_bundle.py` — chapter-129 regression.
  Asserts both the host-side bundle layout (`CAB1` magic, two
  anchors) and the end-to-end recursive validation against
  `https://localhost:8443/` (RSA chain) and
  `https://localhost:8444/` (ECDSA chain). Passing the test
  with the chapter-129 bundle proves the validator walked
  root → intermediate → leaf in both algorithms.

## The PEM grammar in 30 lines

PEM is "base64 with a frame":

```
-----BEGIN <LABEL>-----
<base64, line-wrapped, sometimes with whitespace>
-----END <LABEL>-----
```

Multiple blocks can stream in the same file; arbitrary text
(comments, attribute headers like `Subject: ...`) can sit
between blocks; the inner base64 may or may not be line-wrapped
to 64-char columns. The Mozilla NSS root list, for instance,
ships ~150 cert blocks separated by per-cert metadata lines
that are ignored by the parser.

`scripts/mkcabundle.py::parse_pem_certs` handles this with a
single regex (`-----BEGIN CERTIFICATE-----\s*<body>\s*-----END
CERTIFICATE-----`) plus `b"".join(body.split())` to strip every
whitespace byte before `base64.b64decode(..., validate=True)`.
Other PEM block types (private keys, CRLs) are silently skipped
by limiting the regex to the `CERTIFICATE` label.

The atomic-rename write (`tmp_path` → `out_path`) protects
against half-written bundles if the build is interrupted; the
chapter-128 rationale carries over unchanged.

## What "recursive chain walk" actually means here

Before chapter 129 the validator's job was simple:

```
client trust list: [ intermediate ]
server presents:   leaf, intermediate

walk:
  leaf.signed-by   == intermediate.pubkey  ✓ (sig check)
  intermediate     ∈ trust list             ✓ (direct trust)
  ACCEPT
```

After chapter 129:

```
client trust list: [ root ]
server presents:   leaf, intermediate

walk:
  leaf.signed-by         == intermediate.pubkey   ✓ (sig check)
  intermediate.signed-by == root.pubkey            ✓ (sig check)
  root                   ∈ trust list              ✓ (direct trust)
  ACCEPT
```

Two signature verifications, not one. The validator pulls each
issuer-DN out of the cert it's currently checking, searches the
trust list AND the server-presented chain for a cert whose
subject-DN matches, and recurses. The recursion bottoms out
when the issuer either matches a trust anchor (success) or
doesn't appear anywhere (failure: `BR_ERR_X509_NOT_TRUSTED`).

This is the same algorithm Firefox/Chrome/etc run hundreds of
times a second. Our `br_x509_minimal_*` is a faithful textbook
implementation; chapter 126 already wired it in correctly,
but 128's "trust the intermediate" shortcut hid the recursive
case. 129 unhides it.

## Traps caught

### PEM whitespace normalization

The first cut of the parser called `base64.b64decode(body,
validate=True)` directly. That barfed on the line-wrapped PEM
output because base64 `validate=True` rejects whitespace.
Fix: `b"".join(body.split())` collapses every kind of
whitespace (spaces, tabs, CR, LF) before decoding.

### PEM `validate=True` is not "validate the cert"

The argument name is misleading. `base64.b64decode(validate=True)`
just rejects characters outside the base64 alphabet — it has
no awareness of X.509. We still want it (it surfaces a
copy-paste error in the source PEM clearly) but it doesn't
replace `br_x509_decoder_*` on the guest side.

### Forgetting to bump `TLS_MAX_ANCHORS`

If a future bundle ingests more than 8 anchors, chapter-128's
`TLS_MAX_ANCHORS = 8` causes
`tls_socket_init_chain_from_bundle` to bail with `-1` and
`load_ca_bundle_once` to silently fall back to the in-binary
single anchor. The browser still works — but only against the
sample CN=localhost host — which would be a frustrating
debugging session. Chapter 129 raises the cap to 32 and
documents the 256 KiB raw-bundle size cap that pairs with it.

### Anchor DN buffer is per-row, not pool

Each row of `anchor_dn[i]` is a fixed-size 512-byte slab. Most
real roots fit (Mozilla NSS roots are 80-300 bytes), but a
poorly-formatted enterprise root can blow the limit. The
overflow path is detected in `tls_dn_append` and the whole
init call fails cleanly with `-1`. There is no pool — making
DN storage dynamic would mean a malloc per anchor which is
both more code and more memory pressure than the current shape
for no observable benefit at the ~150-anchor scale.

## Applied to

- `scripts/mkcabundle.py` — gains `--pem` ingest; existing `.h`
  ingest preserved unchanged for backward compatibility with any
  hand-written tests.
- `assets/osfs/ca.bundle` — rebaked from PEM root certs; same
  framed `CAB1` layout, same byte-stream consumer in the guest.
- `userspace/libc/tls_socket.h` — anchor cap raised; no API or
  ABI break (the struct grows by ~20 KiB but every consumer
  heap-allocates).
- `userspace/browser/browser.c` — bundle size cap raised; the
  per-fetch flow is unchanged.

## What gets exercised in tests

- `scripts/test_tls_pem_bundle.py` (new) — the chapter-129
  end-to-end: PEM extracts correctly, bundle ships, browser
  loads it, validator walks root → ica → leaf in both
  algorithms.
- `scripts/test_browser_https_multi.py` (unchanged) — still
  passes; nothing in the multi-anchor contract changed.
- `scripts/test_browser_https.py` (unchanged) — still passes;
  the bundle-loaded path keeps the same TLS-OK shape.
- `scripts/test_tls_chain.py` (unchanged) — still passes
  against an intermediate-as-anchor build of `/bin/tlstest`
  (the standalone binary takes an explicit cert argument; it
  doesn't go through the bundle).

All four TLS tests PASS at chapter close.

## Next: chapter 130

A real public-CA trust store. Drop in the Mozilla NSS
`cacert.pem` (~150 KiB, ~150 roots) and add an outbound test
against an actual public site (e.g. `https://example.com/`
via DigiCert Global Root G2). That work is gated on:

- DNS for `example.com` through SLIRP (already works in
  `scripts/test_dns.py` since chapter 37).
- Outbound TCP to port 443 through SLIRP (chapter 35's
  `virtio-net` setup already covers this).
- A regression policy decision: live-internet tests are
  brittle for CI, so chapter 130 will likely ship the
  capability + a manual `_dbg_*` runner rather than a
  hard-required regression script.
