# Chapter 128 — TLS trust store (multi-anchor)

Chapter 127 wired one trust anchor into `/bin/browser`: the
BearSSL sample CA from `vendor/testcerts/test_chain.c`, hard-
coded into the binary at link time. That was enough to prove
the native HTTPS pipeline end-to-end, but it left two real-world
properties of a trust store unexpressed:

1. A production browser carries **dozens to hundreds** of root
   CAs, not one. Any one fetch picks whichever root signed the
   leaf; the rest sit dormant. The trust *set*, not a single
   trust anchor, is the shape we eventually want.
2. The validator must accept anchors of **multiple key types**
   (RSA today, ECDSA increasingly, and Ed25519 on the horizon).
   BearSSL's minimal validator already does — but `tls_socket.c`
   was only baking RSA anchors.

Chapter 128 fixes both. The trust store becomes a fixed-size
array of `br_x509_trust_anchor` plus per-anchor backing storage;
the anchor baker learns to handle ECDSA pubkeys; the bundle is
shipped on the OSFS image as `/mnt/ca.bundle` in a tiny framed
binary format; and `init` spawns a second `/bin/httpsd` instance
on port 8444 with an ECDSA P-256 chain, so the regression test
exercises both anchors in the same boot.

No kernel code changes. The wire format of TLS itself doesn't
change — this is entirely a userspace trust-store refactor and
a build-time bundle pipeline.

## What ships

- `userspace/libc/tls_socket.h` — the single `anchor` field +
  3 backing buffers becomes a 5-row parallel array sized by
  `TLS_MAX_ANCHORS` (8), with a per-anchor `anchor_pk_q` row
  for ECDSA pubkeys alongside the existing RSA `n` and `e`
  rows. New APIs:
  - `tls_socket_init_chain_multi(t, ders[], lens[], n)` — the
    primitive that bakes N anchors into one socket.
  - `tls_socket_init_chain_from_bundle(t, buf, len)` — parses
    the framed `CAB1` blob and calls `_multi` with the
    extracted DERs. This is the path the browser uses.
  - `tls_socket_init_chain_from_anchor(...)` is unchanged on
    the outside but is now a one-line wrapper around `_multi`.
- `userspace/libc/tls_socket.c` — `tls_bake_anchor_into(t, i,
  cert, len)` replaces the old single-slot baker. The decoder
  branches on `pk->key_type`: `BR_KEYTYPE_RSA` keeps the
  existing path; `BR_KEYTYPE_EC` copies `pk->key.ec.q` into
  `anchor_pk_q[i]` and records `pk->key.ec.curve`. The
  `tls_socket_connect` call site now passes `t->anchors,
  t->anchor_count` instead of `&t->anchor, 1`.
- `userspace/httpsd/httpsd.c` — `handle_one` selects
  `br_ssl_server_init_full_ec` vs `br_ssl_server_init_full_rsa`
  based on a `g_use_ec` flag; `main` parses an optional
  leading `--ec` argv and shifts the port parse to argv[2] in
  that mode. The startup banner now prints `"RSA-2048"` or
  `"ECDSA P-256"` so the regression test can confirm the EC
  chain TU actually got linked in.
- `vendor/testcerts/test_chain_ec.c` — a fresh translation
  unit that re-exports BearSSL's `chain-ec.h` and `key-ec.h`
  as `test_server_chain_ec[]`, `test_server_chain_ec_len`, and
  `test_server_key_ec`. The original `test_chain.c` keeps the
  RSA chain; the two coexist because each `chain-*.h` declares
  its `CERT0/CERT1/CHAIN` arrays `static const`, so they would
  collide if `#include`d in the same TU.
- `Makefile` — adds `TEST_CHAIN_EC_OBJ`, threads it into
  `HTTPSD_OBJS`, adds the `assets/osfs/ca.bundle` build rule
  invoking `scripts/mkcabundle.py` on the two BearSSL chain
  headers, and lists `ca.bundle` in both `OSFS_FILES` and the
  `$(DISK)` recipe so it lands on the OSFS image at boot.
- `scripts/mkcabundle.py` — host-side tool that extracts the
  CA cert (`CERT1[]`) from each BearSSL-style header and
  emits the framed `CAB1` bundle. Documented inline; replaces
  the long-form `--pem` ingest with a single regex for now
  (chapter 129 will add PEM parsing for the Mozilla NSS root
  list).
- `userspace/browser/browser.c` — adds a one-shot
  `load_ca_bundle_once()` at the first `https://` fetch that
  slurps `/mnt/ca.bundle` into a static buffer. `br_conn_open`
  prefers the bundle (`tls_socket_init_chain_from_bundle`) and
  falls back to the chapter-127 in-binary single anchor when
  the bundle is missing or rejected. The TLS-OK line now
  prints `"%d anchors, source=%s"` so a regression can verify
  the bundle path was taken.
- `userspace/init/init.c` — alongside the existing port-8443
  spawn, a second `spawn("/bin/httpsd", "--ec 8444")` brings
  up the ECDSA listener. Same supervised-restart policy as
  the RSA instance (non-fatal on spawn failure; TLS tests
  surface the absence).
- `scripts/test_browser_https_multi.py` — boots the kernel,
  runs `browser https://localhost:8443/` and
  `browser https://localhost:8444/`, asserts that both
  succeed, that the bundle was loaded (`magic=CAB1`), that the
  TLS-OK line shows `source=bundle` (not `built-in`), and that
  the decrypted body tokens reach the renderer in both cases.

## The bundle format

The on-disk format (`/mnt/ca.bundle`, magic `"CAB1"`) is a
deliberate non-standard. PEM would have meant hauling a base64
decoder into userspace just to bootstrap the trust store, and
PKCS#7 / PKCS#12 would have meant ASN.1 parsing of an outer
wrapper that adds nothing beyond what raw DER already provides.
The framed layout is parseable in ~30 lines of guest code (see
`tls_socket_init_chain_from_bundle` in `tls_socket.c`):

```
[4]  magic    = "CAB1"
[4]  count    = u32 little-endian
for each anchor (count times):
    [4]  der_len = u32 LE
    [der_len]    raw DER X.509 certificate bytes
```

The generator (`scripts/mkcabundle.py`) takes the path of the
output bundle followed by one or more BearSSL `chain-*.h`
headers, extracts the `CERT1[]` byte array from each (CERT0 is
the leaf, CERT1 is the CA that signed it — the latter is what
we want as the anchor), and writes the frame.

In chapter 128 the bundle carries two anchors:

- `vendor/bearssl/samples/chain-rsa.h` → 824 DER bytes
  (the BearSSL sample RSA-2048 CA)
- `vendor/bearssl/samples/chain-ec.h` → 429 DER bytes
  (the BearSSL sample ECDSA P-256 CA)

Total bundle size: 1269 bytes. The 32 KiB cap in
`load_ca_bundle_once()` leaves ample room for a future
Mozilla NSS bundle to be dropped in without a recompile.

## Two `httpsd` instances, two CAs, one trust store

The architecture deliberately splits the server side too:

- `/bin/httpsd 8443` (chapter 125) keeps presenting the
  RSA-2048 sample chain.
- `/bin/httpsd --ec 8444` (chapter 128) presents the ECDSA
  P-256 sample chain.

Without this split the multi-anchor refactor would be
unobservable: a single bundle entry would be enough, and the
test couldn't distinguish "browser picks the right anchor" from
"browser has only one anchor and got lucky." With the two
ports, a passing test is direct evidence that:

- The validator walks `t->anchors[0..n)` (not just `&t->anchor`)
- Both the RSA and the EC paths in `tls_bake_anchor_into`
  execute under normal flow.
- The trust *store*, not a single anchor, is what backs the
  fetch.

## Traps caught while writing this chapter

### `static const` collision in BearSSL sample headers

The first attempt put `#include "../bearssl/samples/chain-rsa.h"`
and `#include "../bearssl/samples/chain-ec.h"` in the same
`test_chain.c`. Both headers declare `static const unsigned
char CERT0[]`, `CERT1[]`, and a `static const br_x509_certificate
CHAIN[]`. Even though `static` constrains visibility to the TU,
the names still collide *within* that TU and GCC refuses to
compile.

The fix is one TU per chain header (one for RSA, one for EC).
The two TUs each see their own `static const CERT0/CERT1` and
re-export the chain under a publicly visible non-conflicting
name (`test_server_chain` vs `test_server_chain_ec`).

### EC pubkey doesn't have `n`/`e` fields

The chapter-127 anchor baker assumed `pk->key.rsa.{n,nlen,e,elen}`
unconditionally. With an EC anchor `pk->key_type` is
`BR_KEYTYPE_EC` and the union is occupied by `pk->key.ec.{curve,
q, qlen}` instead. Reading `key.rsa.n` on an EC anchor is
undefined behaviour (in practice: pointer-shaped garbage from
whatever `key.ec.q` happens to hold).

The new `tls_bake_anchor_into` branches early on `pk->key_type`
and uses the correct union arm in each branch.

### `tls_socket_t` field rename broke `/bin/tlstest`

`/bin/tlstest`'s `run_handshake_mode` reaches into the socket
to print stats about the baked anchor (`t->anchor.dn.len`,
`t->anchor.pkey.key.rsa.nlen`). With `anchor` becoming the
array `anchors`, the build failed cleanly with `error: 'tls_socket_t'
has no member named 'anchor'`. The fix is `t->anchors[0]` —
trivial, but worth recording: schema-changing refactors on a
shared struct want a `grep -n "->anchor[^s]"` sweep before the
build.

### `init`'s `spawn()` whitespace-splits the args

`spawn("/bin/httpsd", "--ec 8444")` works because
`kernel/core/syscall.c::sys_spawn` whitespace-splits the `args`
string into individual argv tokens (with the path prepended as
argv[0]). That meant `httpsd` sees `argv = {"/bin/httpsd",
"--ec", "8444"}` and `main`'s `argi` walker handles both the
flag and the port correctly. If `spawn` had passed the args
through as a single string we would have needed an
intermediate shell or a libc-side splitter — neither of which
is wanted at boot.

## Applied to

- `userspace/browser/browser.c` — uses the new multi-anchor
  trust store on every `https://` fetch. Anchor source is
  printed in the TLS-OK line so an observer can tell which path
  was taken (`source=bundle` vs `source=built-in`).
- `userspace/httpsd/httpsd.c` — gains the `--ec` mode and now
  reports key type on the startup banner.
- `userspace/libc/tls_socket.{h,c}` — multi-anchor refactor;
  `tls_socket_init_chain_from_anchor` survives unchanged on the
  outside, so chapter-126's `/bin/tlstest --handshake-ca`
  regression keeps working with a one-anchor chain.
- `userspace/tlstest/tlstest.c` — one-line field-rename fix
  (`t->anchor.dn.len` → `t->anchors[0].dn.len`) so it keeps
  printing the chain-mode diagnostics.
- `userspace/init/init.c` — spawns the second `/bin/httpsd`
  instance on port 8444 with `--ec`.

## What gets exercised in tests

- `scripts/test_browser_https_multi.py` (new) — full
  end-to-end against both ports.
- `scripts/test_browser_https.py` (unchanged) — proves the
  bundle path doesn't regress the chapter-127 single-anchor
  flow: it still passes against `:8443` because the bundle
  contains the RSA anchor too.
- `scripts/test_tls_chain.py` (unchanged) — proves
  `tls_socket_init_chain_from_anchor` still bakes a one-entry
  anchor list correctly after being collapsed into a wrapper
  over `_multi`.
- `scripts/test_tls_handshake.py` (unchanged) — proves the
  knownkey path (chapter 125) survives the struct layout
  change.
- `scripts/test_tlstest.py` (unchanged) — proves the link
  doesn't regress (the `cstring.o` / `bearssl` glue keeps
  resolving).

All five existing TLS tests plus the new multi test PASS at
chapter close.

## Next: chapter 129

A real public-CA bundle. `scripts/mkcabundle.py` grows a `--pem`
mode that ingests the Mozilla NSS root list (~150 root certs,
~150 KiB); the cap on `BR_CA_BUNDLE_MAX` and `TLS_MAX_ANCHORS`
rise accordingly; `init` no longer needs the in-guest `httpsd`
to validate the browser end-to-end because a real outbound
fetch against (e.g.) `https://example.com/` becomes the test.
That chapter is the natural place to retire the
`test_server_chain` in-binary fallback entirely.
