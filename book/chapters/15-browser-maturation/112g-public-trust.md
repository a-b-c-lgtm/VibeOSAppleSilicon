# Chapter 112g — Direct outbound HTTPS: real public CAs

## What we built before this chapter

By the end of chapter 112f the in-guest TLS stack does, on
paper, everything a "real" TLS client does: BearSSL `_full`
profile for cipher coverage, the minimal X.509 engine, recursive
chain validation rooted at a list of anchors loaded from
`/mnt/ca.bundle`, SAN/CN matching against the SNI hostname, and
notBefore/notAfter checking against the PL031 wall-clock from
chapter 95.

And yet `browser https://news.ycombinator.com/` still printed:

```
browser: TLS handshake to news.ycombinator.com:443 failed
         (rc=58, BR_ERR if positive)
```

`58` is `BR_ERR_X509_NOT_TRUSTED`. The plumbing was fine. The
**trust list** was wrong — it contained two anchors, both
BearSSL sample roots. Neither one ever signed anything HN's CA
signed. The validator was doing exactly its job: it walked the
chain, hit the root, and refused to honour a self-signed
certificate it had never been told to trust.

The fix is not more code. The fix is to tell the validator
about the same roots a real browser trusts.

## What ships

### 1. `scripts/fetch_public_roots.sh`

A 70-line cross-platform shell script that copies the host's
system CA bundle to `vendor/testcerts/public-roots.pem`. It
probes, in order:

| Path | Distro / source |
|---|---|
| `/etc/ssl/cert.pem` | macOS (LibreSSL), Alpine, FreeBSD |
| `/etc/ssl/certs/ca-certificates.crt` | Debian, Ubuntu |
| `/etc/pki/tls/certs/ca-bundle.crt` | RHEL, Fedora, CentOS |
| `/etc/ca-certificates/extracted/tls-ca-bundle.pem` | Arch |
| `/usr/local/etc/openssl@3/cert.pem` | Homebrew |
| `/opt/homebrew/etc/openssl@3/cert.pem` | Apple Silicon Homebrew |

If none are found, the script prints actionable guidance pointing
the developer at `curl -o vendor/testcerts/public-roots.pem
https://curl.se/ca/cacert.pem` and exits non-zero. The Makefile
will not silently ship a build without a trust store.

The copy is atomic (`cp` to `.tmp.$$` then `mv`) so an
interrupted run can't leave a half-written PEM that
`mkcabundle.py` would silently accept.

### 2. Bundle assembly

`Makefile`'s `assets/osfs/ca.bundle` rule now includes a third
`--pem` input:

```make
assets/osfs/ca.bundle: scripts/mkcabundle.py \
                      vendor/bearssl/samples/cert-root-rsa.pem \
                      vendor/bearssl/samples/cert-root-ec.pem \
                      vendor/testcerts/public-roots.pem
	python3 scripts/mkcabundle.py $@ \
	    --pem vendor/bearssl/samples/cert-root-rsa.pem \
	    --pem vendor/bearssl/samples/cert-root-ec.pem \
	    --pem vendor/testcerts/public-roots.pem
```

The BearSSL sample roots stay in (so the chapter 112d/e/f tests
against the in-guest `/bin/httpsd` keep working without
modification); the public roots get folded in alongside them.
On a current macOS host the result is:

```
mkcabundle: --pem cert-root-rsa.pem:      1 cert(s)     794 DER B
mkcabundle: --pem cert-root-ec.pem:       1 cert(s)     398 DER B
mkcabundle: --pem public-roots.pem:     128 cert(s) 141,141 DER B
mkcabundle: wrote assets/osfs/ca.bundle (142,861 bytes, 130 anchors)
```

### 3. The two compile-time bumps

```c
/* userspace/libc/tls_socket.h */
#define TLS_MAX_ANCHORS       256        /* was 32 */

/* userspace/browser/browser.c */
#define BR_CA_BUNDLE_MAX (512 * 1024)    /* was 256 KiB */
```

These are *capacity* changes; no new logic. `TLS_MAX_ANCHORS`
sets the inline-array length of the parallel
`anchor_dn[]/anchor_pk_n[]/anchor_pk_e[]/anchor_pk_q[]` rows
inside `tls_socket_t`. At 256 slots × ~1.2 KiB/slot the struct
weighs about 360 KiB on the heap; each in-flight `https://`
fetch allocates one. The user heap eats this comfortably.

`BR_CA_BUNDLE_MAX` is the cap on the file the browser will
agree to read from `/mnt/ca.bundle`. 512 KiB gives 3.5x
headroom over today's 140-KiB bundle, enough for a full curl.se
NSS drop without another recompile.

### 4. Why we don't commit `public-roots.pem`

It's `.gitignore`d on purpose:

* Roots churn — Mozilla distrusts CAs (TrustCor, Camerfirma,
  Symantec), adds new ones (Let's Encrypt's ISRG Root X2,
  Google Trust Services), and rotates intermediates on a
  quarterly cadence. A repo-pinned PEM goes stale fast.
* The whole point is "trust what the host trusts". Every
  developer machine already has a curated, signed bundle
  maintained by Apple, Debian, or Mozilla — re-distributing
  it would just create two sources of truth that disagree.
* It's 333 KiB of base64 churn that would dominate every
  `git log -p`.

### 5. `scripts/_dbg_tls_outbound.py`

A debug harness — per `/memories/debug-scripts-policy.md` named
`_dbg_*` so it stays out of the regression sweep — that boots
the guest with networking, runs `browser <URL>` against a real
public site, and asserts the `TLS handshake OK` line plus
clean body bytes:

```
$ python3 scripts/_dbg_tls_outbound.py https://news.ycombinator.com/
[outbound] target: https://news.ycombinator.com/
[outbound] shell prompt reached
[outbound] TLS handshake OK
PASS: outbound HTTPS round-trip succeeded against https://news.ycombinator.com/
```

This is **not** wired into `make test` because live internet
is too brittle for CI (DNS outages, captive portals, CA
rotations, corporate proxies that block QEMU's SLIRP source).
It IS the manual test you reach for when you want to confirm
the OS still talks to the real world after a TLS-touching
change.

## What did *not* need to change

A surprising amount. Worth listing because the temptation to
"clean up" code that's already correct is real:

* **SNI**. `tls_socket_connect(t, ip, port, sni)` (chapter 112b)
  already passes the hostname into
  `br_ssl_client_reset(&t->cc, sni, 0)`. BearSSL's
  `br_ssl_client_init_full` configures the minimal X.509 engine
  to require SAN-DNS / CN-fallback matching against that name.
  The first time SNI got exercised against a hostname BearSSL
  hadn't seen at compile time was the first `https://example.com/`
  fetch in this chapter — and it just worked.
* **Hostname → IP**. The browser's `resolve()` call goes through
  the chapter-57 DNS resolver, which UDP-queries the DNS server
  whose address was filled in by chapter-56's DHCP client. SLIRP
  hands out `10.0.2.3` as the server, which forwards to the
  host. No new code.
* **Outbound TCP to public IPs:port 443**. The chapter-39 socket
  layer asks the kernel for `sys_connect(ip, port)`; the kernel's
  TCP code (chapter 39 / 55) doesn't care whether the destination
  is loopback or routable. SLIRP NATs it out the host's TCP
  stack.
* **The validator**. Chapter 112f's recursive chain walk is the
  same code whether the leaf is signed by a BearSSL sample CA
  or by E1 (Let's Encrypt's ECDSA intermediate). The walk
  doesn't know that the certificate it's working on came from
  a "real" site.

## Traps caught

1. **`BR_ERR_X509_NOT_TRUSTED` (rc=58) means "the validator did
   its job"**. Pre-chapter the obvious-looking response was to
   stare at `tls_socket.c`. The actual fix was in the bundle —
   no C code touched. When you see an X.509 error code from
   BearSSL, check the **trust list** before the **validator**.
2. **macOS `/etc/ssl/cert.pem` is the right file**. It's
   maintained by the OS and updated through Apple security
   patches. The alternative is `certifi` (the Python
   bundle) which would also work but adds a Python
   dependency to the build.
3. **`TLS_MAX_ANCHORS` overflow is silent in the bundle parser**
   — chapter 112f noted this and bumped to 32; chapter 112g
   bumps to 256 because the macOS bundle has 128. A real-world
   trip-wire: a developer with a Linux box that has 200+ roots
   in `ca-certificates.crt` would have silently fallen back to
   "no trust list at all" if we'd left it at 32.
4. **Atomic write matters**. Without the `.tmp.$$ → mv` pattern
   in `fetch_public_roots.sh`, an interrupted `cp` leaves a
   half-written PEM. `mkcabundle.py` validates each base64 block
   so it would catch a truncated cert mid-block, but a
   *truncated-on-cert-boundary* PEM would silently bundle fewer
   anchors than the developer expects.

## Applied to

* `userspace/browser/browser.c`: continues to use the same
  `br_conn_open` path from chapter 112e/f. `BR_CA_BUNDLE_MAX`
  bumped so the bigger bundle loads. No new branches.
* `userspace/libc/tls_socket.h`: `TLS_MAX_ANCHORS` bumped from
  32 to 256 to cover real-world bundle sizes. Header comment
  updated.
* `Makefile`: bundle rule gains `vendor/testcerts/public-roots.pem`
  dependency and `--pem` argument.

## What gets exercised in tests

* `scripts/test_tls_pem_bundle.py` (chapter 112f, now loosened):
  the host-side `count` assertion changed from `== 2` to
  `>= 2`, since the production bundle now has 128 extra
  anchors. The end-to-end RSA + ECDSA chain-walk assertions
  against `httpsd` stay exactly as they were. **PASS.**
* `scripts/test_browser_https.py`,
  `scripts/test_browser_https_multi.py`,
  `scripts/test_tls_chain.py`,
  `scripts/test_tls_handshake.py`,
  `scripts/test_tlstest.py`: all unchanged, all still **PASS**.
  These exercise the in-guest `/bin/httpsd` over loopback and
  prove that adding 128 unrelated public roots to the trust
  list doesn't perturb the BearSSL-root validation paths.
* `scripts/_dbg_tls_outbound.py` (new, manual): drives the
  browser against `https://example.com/` and
  `https://news.ycombinator.com/` over the public internet
  through SLIRP. Both **PASS** on the chapter-author's machine.

## Limitations and what's next

* **TLS 1.2 only.** BearSSL doesn't implement TLS 1.3. Most
  large CDNs (Cloudflare, Fastly) still accept 1.2 connections,
  but some hardened endpoints are 1.3-only. Adding 1.3 means
  switching crypto libraries; out of scope.
* **No ALPN, no h2.** We negotiate HTTP/1.1 only. Cloudflare
  is happy to downgrade; some HTTP/3-only origins won't talk
  to us at all.
* **No session resumption.** Every connection is a full
  handshake — ~5 RTTs and ~50 ms of asymmetric crypto. A repeat
  visit to HN does the same work. A future chapter could wire
  BearSSL's session-cache API.
* **No revocation checking.** We don't fetch CRLs or speak
  OCSP. A compromised intermediate stays trusted until the
  next time `fetch_public_roots.sh` runs and the host has
  pulled the distrust list. This matches what most real
  browsers do in practice (CRLite / OneCRL is a desktop
  Firefox feature, not a TLS-library feature).
* **No HSTS, no HTTPS-only mode.** The browser still happily
  fetches `http://`. A future chapter could add HSTS pinning
  and a preload list.
