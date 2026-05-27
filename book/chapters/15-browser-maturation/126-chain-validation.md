# Chapter 126 -- X.509 chain validation

Chapter 125 ended on a working TLS handshake against an
in-guest server, but the trust shape was a shortcut: the client
pinned on the leaf certificate's public key and skipped every
other check. That works for a single hard-wired peer; it does
not generalise. To talk to any server not enrolled ahead of
time we need the production trust shape:

1. Walk the chain from the leaf back to a trust anchor.
2. Verify each link's signature with the issuer's public key.
3. Check the leaf's notBefore / notAfter against a clock we
   believe in.
4. Match the SAN/CN against the host name we asked for (SNI).

BearSSL ships all four. The "minimal" X.509 engine
(`br_x509_minimal_*`) is the production validator that
`br_ssl_client_init_full` wires in by default — in 125 we
deliberately overrode it with `br_x509_knownkey` after
`init_full` returned. This chapter takes the override out and
gives the minimal validator what it needs to do its job: a
trust anchor and a notion of "now".

## What ships

- `userspace/libc/tls_socket.{c,h}` — adds
  `tls_socket_init_chain_from_anchor()` and a `chain_mode`
  branch in `tls_socket_connect()`. The struct grows a
  `br_x509_trust_anchor anchor`, a 256-byte DN buffer, and
  copies for the anchor's RSA `n`/`e` bytes (~830 bytes total).
- `userspace/tlstest/tlstest.c` — adds `--handshake-ca HOST
  PORT` mode that builds the anchor from BearSSL's sample
  intermediate CA cert and validates the chain end-to-end.
- `scripts/test_tls_chain.py` — boots the kernel, runs
  `tlstest --handshake-ca 127.0.0.1 8443` against the
  `/bin/httpsd` that init.c already spawns for 125, asserts
  four progress signals.
- `book/chapters/15-browser-maturation/126-chain-validation.md`
  (this file) + the corresponding `book/INDEX.md` rows.

`/bin/httpsd` is unchanged from 125 — it still presents the
same `(CERT0, CERT1)` chain, and the path is irrelevant to its
response. The validator on the client side is the only thing
that changes.

## The trust anchor at runtime

A `br_x509_trust_anchor` carries three pieces of data:

```c
typedef struct {
    br_x500_name      dn;     /* raw DER bytes of subject DN  */
    unsigned          flags;  /* BR_X509_TA_CA (or 0)         */
    br_x509_pkey      pkey;   /* the CA's public key (n, e)   */
} br_x509_trust_anchor;
```

The minimal validator matches a chain to an anchor when the
last cert's issuer DN equals the anchor's `dn.data` byte-for-
byte and the chain's signature chain terminates in a signature
verifiable by the anchor's `pkey`.

BearSSL's `tools/brssl` ships an offline encoder that turns
PEM CA certs into pre-baked C arrays; we deliberately do not
use it. Instead we build the anchor at runtime from the same
`br_x509_decoder_*` API we already used in 125 to pull the
leaf's public key out:

```c
typedef struct {
    unsigned char *buf;
    size_t         cap;
    size_t         len;
    int            overflow;
} dn_accum_t;

static void tls_dn_append(void *ctx, const void *data, size_t len)
{
    dn_accum_t *a = ctx;
    if (a->overflow) return;
    if (a->len + len > a->cap) { a->overflow = 1; return; }
    /* append `len` bytes from `data` to a->buf */
}

br_x509_decoder_init(&dec, tls_dn_append, &acc);
br_x509_decoder_push(&dec, ca_der, ca_der_len);
br_x509_pkey *pk = br_x509_decoder_get_pkey(&dec);
```

The `append_dn` callback fires once per chunk while the decoder
walks the subject DN's ASN.1 SEQUENCE; we accumulate the bytes
into `tls_socket_t::anchor_dn[256]`. The RSA pubkey is read out
of the decoder's pad and copied to `anchor_pk_n` / `anchor_pk_e`
the same way 125 copies the pinned key (the decoder hands back
pointers into its own scratch — they become invalid the moment
the context is reused).

Two reasons to do this at runtime instead of at build time:

1. We have no host build-time dependency on `brssl` or OpenSSL.
   The same machinery becomes the foundation for an "import a
   CA from disk" UI later.
2. The book's bias is "you can see the bytes go by". A runtime
   decoder pass with an accumulator callback is a much clearer
   teaching shape than `__attribute__((section(".rodata")))`
   over a 6-line opaque blob.

The cost is one X.509 parse at boot — measured at well under
a millisecond on QEMU virt.

## The minimal validator wants a clock

BearSSL's "minimal" engine asks: is `(days, secs)` inside
`[notBefore, notAfter]`? If you never call
`br_x509_minimal_set_time` it leaves the time at `(0, 0)`, which
is interpreted as "no clock available, refuse" — every cert with
a non-zero `notBefore` (i.e. every cert) gets
`BR_ERR_X509_EXPIRED` (54).

The conversion from Unix time is straightforward but the
constant is easy to get wrong. BearSSL counts proleptic
Gregorian days from "0 AD January 1" (the astronomer's year 0,
which the calendar treats as a leap year). The Unix epoch
1970-01-01 sits at day 719528 in that counting — you can see
the exact constant inside the engine itself:

```
$ grep 719528 vendor/bearssl/src/x509/x509_minimal.c
    T0_PUSH((uint32_t)(x / 86400) + 719528);
```

So the conversion is:

```c
#define TLS_UNIX_EPOCH_DAYS 719528u

static void tls_set_validator_time(br_x509_minimal_context *xc)
{
    struct timeval tv;
    if (gettimeofday(&tv) != 0) return;
    if (tv.tv_sec <= 0) return;
    uint64_t s = (uint64_t)tv.tv_sec;
    uint32_t days = (uint32_t)(s / 86400u) + TLS_UNIX_EPOCH_DAYS;
    uint32_t secs = (uint32_t)(s % 86400u);
    br_x509_minimal_set_time(xc, days, secs);
}
```

This is the first user of `SYS_GETTIMEOFDAY` (chapter 96)
outside `/bin/date` and the taskbar clock. The PL031 RTC the
kernel reads at boot now backs cert expiry on every TLS
handshake.

> **Trap.** If you try the obvious offset of 719162 (days from
> year *1* Jan 1 to 1970), the chain validates as if we were a
> year in the past. The sample cert is valid 2010–2037 so the
> bug doesn't fire on its own; it will the first time we try to
> talk to a real server with a tighter `notBefore`. Always
> grep BearSSL for the magic number, don't recompute it.

## Init shape

The new `tls_socket_init_chain_from_anchor` zeroes the struct,
sets `chain_mode = 1`, and bakes the anchor. The actual
validator wiring happens inside `tls_socket_connect`, which
branches on `chain_mode`:

```c
if (t->chain_mode) {
    /* Chain mode: minimal validator IS the active vtable.
     * Pass our anchor in as a one-entry trust list. */
    br_ssl_client_init_full(&t->cc, &t->xc, &t->anchor, 1);
    tls_set_validator_time(&t->xc);
} else {
    /* knownkey mode (125): override the validator. */
    br_ssl_client_init_full(&t->cc, &t->xc, NULL, 0);
    br_ssl_engine_set_x509(&t->cc.eng, &t->xkc.vtable);
}
```

Notice that the field is now called `xc` (not `xc_unused` as in
the 125 draft) — it doubles as the dead-init scratch in
knownkey mode and as the actual validator in chain mode. The
rest of `tls_socket_connect` (entropy injection, client_reset,
socket_connect, sslio_init, sslio_flush) is the same byte-for-
byte; this is the only branch.

## The test

```
$ tlstest --handshake-ca 127.0.0.1 8443
tlstest: built trust anchor from CA cert (993 DER bytes, DN 41 bytes, RSA-2048)
tlstest: wallclock tv_sec=1779386221 (validator anchored to it)
tlstest: connecting 127.0.0.1:8443 (SNI=localhost, chain)
tlstest: handshake complete; sending GET /m112c
tlstest: read 121 bytes from server
tls handshake: PASS chapter 126 end-to-end
```

The four signals the regression script asserts on, in order:

| Signal                                         | Why it matters                                       |
|------------------------------------------------|------------------------------------------------------|
| `built trust anchor from CA cert`              | Decoder + append_dn + pubkey copy worked            |
| `wallclock tv_sec=<n>` and `n in 2010..2037`   | SYS_GETTIMEOFDAY is wired up and in the cert window |
| `handshake complete`                           | The minimal validator accepted the chain            |
| `tls handshake: PASS chapter 126`             | App-data round-trip survived chain validation       |

The `marker` itself ("tls handshake ok") only appears inside
the encrypted record stream that httpsd emits as the response
body. If the chain were rejected, BearSSL would tear the
connection down between `handshake complete` and the body
write, and the marker would never reach the client.

## Traps caught (in order they would normally bite)

1. **Wrong epoch offset.** 719162 vs 719528 — the engine's own
   source is authoritative. Grep it.
2. **`append_dn` writing past the buffer.** Real-world DNs go
   up to ~512 bytes for some enterprise CAs. We cap at 256
   because BearSSL's sample intermediate uses a 41-byte DN, and
   we flag `overflow` so the init refuses rather than truncates.
   Chapter 127 will lift this to a larger buffer (or to a heap
   allocation sized by the cert) when we start trusting real
   public CAs.
3. **Decoder pointer aliasing.** Same trap as 125: the bytes
   pointed at by `br_x509_pkey::key.rsa.n` live inside the
   decoder's pad, not in `cert_der`. Always `memcpy` out.
4. **Forgetting `BR_X509_TA_CA`.** Without the flag the anchor
   is treated as "direct trust this exact EE cert", not "trust
   this CA". The chain walk silently fails with
   `BR_ERR_X509_NOT_TRUSTED` (62) because the engine refuses to
   consider the anchor as a signer.
5. **Skipping `tls_set_validator_time` after a successful
   `gettimeofday`.** Returns `BR_ERR_X509_EXPIRED` (54) on every
   connect, even though the clock is right. Easy to spot
   because the error code matches the cert's `notBefore` rather
   than a parse failure.

None of these bit us during 126 — they all came out of careful
reading first — but they are the failure modes a future
contributor will hit. The regression script's hard-coded
window-check (`1262304000 <= ts <= 2145916800`) catches #5 by
flagging "wallclock is fine, validator must be wrong" instead
of letting the test fail as a generic handshake error.

## What the user-visible machine gains

- `/bin/tlstest` now has a second handshake mode that uses the
  real chain validator. Run from the shell:
  - `tlstest --handshake 127.0.0.1 8443` — pinned (125).
  - `tlstest --handshake-ca 127.0.0.1 8443` — chain (this
    chapter).
- The same path will be used unchanged by the browser in
  chapter 127. The only difference there is the anchor list:
  instead of one self-signed intermediate, we will load a
  curated subset of Mozilla NSS roots from `osfs:/etc/ca/`.
- `/bin/date` is no longer the only userspace consumer of the
  PL031 RTC. Cert expiry now depends on it; chapter 127's
  taskbar will gain a `[!] no RTC` warning if the kernel boots
  without one, since we'd silently lose HTTPS in that case.

## Applied to

| Surface              | Change                                                 |
|----------------------|--------------------------------------------------------|
| `userspace/libc/tls_socket.{c,h}` | + `tls_socket_init_chain_from_anchor`, branched `connect`, +830 B of fields |
| `userspace/tlstest/tlstest.c`      | + `--handshake-ca HOST PORT`, shared `run_handshake_mode(mode)` |
| `scripts/test_tls_chain.py`        | New regression, four PASS lines                |
| `userspace/httpsd/httpsd.c`        | Unchanged. Still answers any GET with the marker. |
| `userspace/init/init.c`            | Unchanged. Still spawns `/bin/httpsd 8443`.    |
| `Makefile`                         | Unchanged. tls_socket.o already in the link.   |

## Next: chapter 127

Wire `tls_socket_init_chain_from_anchor` into the browser's
network layer behind an `https://` URL scheme. Retire
`scripts/https_proxy.py` from the regression boot path
(keeping the file per debug-scripts-policy.md). Replace the
single-anchor test trust with a small curated trust store
loaded from `osfs:/etc/ca/`.
