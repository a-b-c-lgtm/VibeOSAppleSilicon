# Chapter 112b — In-guest TLS handshake

Chapter 112a got `libbearssl.a` into the build tree and proved
that one of its primitives (SHA-256) computes the right answer
when called from a freestanding userspace binary. That is a
linker-and-codegen milestone, not a TLS milestone. No
ClientHello has been sent. No certificate has been parsed. No
record has been encrypted. This chapter takes the next step:
get the BearSSL *engine* — not just one of its hash primitives
— to run a full TLS 1.2 handshake end to end, between two
processes in our guest. By the end:

```
/$ tlstest --handshake 127.0.0.1 8443
tlstest: pinned RSA-2048 public key from leaf cert (832 DER bytes)
tlstest: connecting 127.0.0.1:8443 (SNI=localhost)
tlstest: handshake complete; sending GET /m112b
tlstest: read 124 bytes from server
tls handshake: PASS chapter 112b end-to-end
```

and `scripts/test_tls_handshake.py` boots the kernel, asserts
each of those lines, and exits 0.

Why "in-guest first"? Because the universe of things that can
go wrong with a TLS handshake is enormous, and the universe of
things that can go wrong with TLS *over the Internet* is even
bigger. If we point our client straight at `https://example.com`
the first time we run it, every failure mode looks the same:
"the handshake didn't complete." Was it our entropy injection?
Our X.509 validator? Our record-layer state machine? Our TCP
stack mishandling out-of-order ACKs? SLIRP doing something
weird with MTU? The remote server speaking TLS 1.3-only? The
debugging cycle would be brutal. Standing up a TLS *server* in
the same address-spaced universe, where we control both halves
and can inspect both halves' state, makes every failure narrowly
attributable. Once the loopback handshake passes, chapter 112c
adds the CA store and chapter 112d points the browser at a real
URL — but neither of those is *that* much more code, because
the engine itself will already have been proven by the work in
this chapter.

## What we have to build

Three new pieces:

```
userspace/libc/tls_socket.{h,c}     # client-side TLS wrapper
userspace/httpsd/httpsd.c           # in-guest TLS server
vendor/testcerts/test_chain.{h,c}   # self-signed cert + key (re-exported)
```

…plus a `tlstest --handshake HOST PORT` mode that exercises the
client, a regression script that boots the kernel and runs it,
and an `init.c` line that spawns `httpsd 8443` so the server is
already listening by the time the shell prompt appears.

The total is about 600 lines of new source. Most of it is
plumbing — error paths, includes, the `--handshake` argv parser.
The *interesting* code is maybe 200 lines and lives in
`tls_socket.c` and `httpsd.c`. The interesting *decisions* are
in this chapter.

## A test certificate, without host tooling

The very first question is: what certificate does the server
present, and where does it come from?

The obvious answer — "generate one with OpenSSL on the host" —
is the wrong shape for this project. It would mean adding
`openssl` to the build prereqs, writing a host-side script
to mint a key and a cert at build time, plus another script
to convert the resulting PEM blobs into C arrays so the server
binary can embed them. Three host-side tools just to start a
test server is too much.

There is a much smaller path: **BearSSL ships its own sample
cert chain**, MIT-licensed, vendored already at
`vendor/bearssl/samples/chain-rsa.h` and `samples/key-rsa.h`.
The chain has two certificates (an end-entity for `CN=localhost`
and an intermediate CA) plus the matching RSA-2048 private key
in CRT form. We don't need to mint anything: we just need to
re-export the sample data with non-`static` linkage so other
translation units can link against it.

That re-export lives in `vendor/testcerts/test_chain.c`:

```c
#include "bearssl.h"
#include "chain-rsa.h"   /* from BearSSL samples, MIT */
#include "key-rsa.h"     /* from BearSSL samples, MIT */

const br_x509_certificate *const test_server_chain     = CHAIN;
const size_t                     test_server_chain_len = CHAIN_LEN;
const br_rsa_private_key  *const test_server_key       = &RSA;
```

`CHAIN` and `RSA` are `static const` in the sample headers
(deliberate, so multiple samples can include them without
colliding). Including them in *this* one translation unit
lands the bytes in `test_chain.o`'s `.rodata` exactly once;
exporting pointers to that static data through `extern const`
symbols means every other binary that links against
`test_chain.o` shares the same copy. No duplication; no host
tooling; no certificate generation in our build.

The chapter-112b client *doesn't validate the chain at all*.
It pins on the public key of the leaf cert. (Real chain
validation comes in 112c, once we have a CA store and a real
time source.) So the chain we present here doesn't need to
chain to anything trusted; it just needs to *exist* so the
server side of `br_ssl_server_init_full_rsa` is happy.

## The four BearSSL state machines you have to know about

When you call into BearSSL it is always in one of two modes:

1. You are **driving the engine** via `br_ssl_engine_recvrec`,
   `br_ssl_engine_recvapp`, `br_ssl_engine_sendapp`, and
   `br_ssl_engine_sendrec` — the four "buffer halves" that
   make up the record state machine. Bytes come off the
   network into `recvrec`, get decrypted into `recvapp`, get
   read by the application from `recvapp`, get written by the
   application into `sendapp`, get encrypted into `sendrec`,
   and finally get written back to the network from `sendrec`.

2. You are **letting the convenience wrapper drive the engine**
   via `br_sslio_init`, `br_sslio_read`, `br_sslio_write`,
   `br_sslio_flush`, and `br_sslio_close`. These take two
   callbacks (`low_read` and `low_write`) and an opaque pointer
   each. They internally do the four-buffer pump so the
   application sees nothing but a stream interface.

We use the second. Hand-rolling the four-buffer pump on top of
our `read()` / `write()` syscalls would have been about another
80 lines of carefully ordered `br_ssl_engine_*` calls and
exactly the wrong place to introduce a bug while we are still
learning BearSSL. The convenience layer is also what BearSSL's
own samples use. The callbacks are tiny:

```c
static int tls_low_read(void *ctx, unsigned char *data, size_t len)
{
    int fd = *(int *)ctx;
    if (len > 16384) len = 16384;
    long n = read(fd, data, len);
    if (n <= 0) return -1;       /* error or peer FIN */
    return (int)n;
}
```

Two things to call out. First, the callback returns `int`,
not `ssize_t`, and BearSSL caps the value at "less than 20000"
— so we clamp `len` to 16 KiB to be safe even if the engine
ever asks for more. Second, **`read() == 0` (peer FIN) maps to
`-1` (error)**. This is correct: a TLS connection cannot end at
a record boundary without a `close_notify`; an unannounced FIN
is a truncation attack and the engine has to refuse it. The
mapping is subtle enough that we have to write it as a comment,
because the C semantics ("0 means EOF, return 0") are exactly
backwards from what BearSSL wants. A bug here would silently
turn truncation attacks into clean-closure responses and the
hosting `br_sslio_read` would return success on a partial
record. Don't put it in the wrong order.

## Entropy is *mandatory* in our build

The third BearSSL state machine is the PRNG. BearSSL's engine
holds an HMAC-DRBG. The DRBG is seeded automatically *if* the
build detected an OS source (`/dev/urandom`,
`getentropy(3)`, `CryptGenRandom`). Our build detected none —
we are freestanding, `BR_USE_URANDOM` / `BR_USE_GETENTROPY` /
`BR_USE_WIN32_RAND` are all `0`, and we excluded `sysrng.c`
from the archive entirely (chapter 112a). The link-time
consequence is one undefined-symbol error:

```
ssl_engine.o:(.text+0x5a4): undefined reference to
                            `br_prng_seeder_system'
```

— because `br_ssl_engine_init_rand` calls it even when the
returned pointer is going to be `NULL`. We satisfy the linker
with a one-function stub in `userspace/libc/cstring.c` that
always returns `NULL`:

```c
typedef int (*br_prng_seeder_fn)(void **ctx);
br_prng_seeder_fn br_prng_seeder_system(const char **name)
{
    if (name) *name = "none";
    return 0;
}
```

A `NULL` return means "the caller must call
`br_ssl_engine_inject_entropy` manually before the first
`br_ssl_client_reset` / `br_ssl_server_reset` call". If they
don't, the reset fails (return value `0`, with
`BR_ERR_BAD_PRNG` as the engine's last error). This is exactly
where chapter 112 — the kernel CSPRNG and the `SYS_GETRANDOM`
syscall — earns its keep. The `tls_socket_t` initialiser pulls
64 bytes (512 bits, comfortably above BearSSL's documented
80-bit minimum) and injects them:

```c
unsigned char seed[64];
long got = getrandom(seed, sizeof seed, 0u);
if (got != (long)sizeof seed) return -1;
br_ssl_engine_inject_entropy(eng, seed, sizeof seed);
```

`tls_socket_connect` does this *before* `br_ssl_client_reset`,
and `httpsd`'s `handle_one` does it before `br_ssl_server_reset`.
Without these two lines, both ends would refuse to handshake
with no diagnostic beyond "reset returned 0".

## The knownkey validator (and the NULL-pointer trap)

The cheapest possible X.509 validator is one that ignores the
*chain* entirely and asks only "does the leaf cert present
*this specific public key*?". BearSSL ships exactly that
validator under the name `br_x509_knownkey_*`. The setup is
two function calls:

```c
br_x509_knownkey_init_rsa(&t->xkc, &t->pinned_pk,
                          BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN);
br_ssl_engine_set_x509(&t->cc.eng, &t->xkc.vtable);
```

…but the pinned public key bytes have to come from *somewhere*.
We don't want to hard-code them in the test binary — they would
fall out of sync the moment anyone regenerated the sample chain.
Instead the client extracts them at startup, by feeding the
leaf cert through the streaming X.509 decoder
(`br_x509_decoder_*`) and copying the resulting `(n, e)` bytes
into a long-lived buffer:

```c
br_x509_decoder_context dec;
br_x509_decoder_init(&dec, 0, 0);
br_x509_decoder_push(&dec, cert, cert_len);
br_x509_pkey *pk = br_x509_decoder_get_pkey(&dec);
/* MUST copy: pk points into dec's internal storage, which
 * is invalidated when dec goes out of scope */
memcpy(t->pinned_pk_n, pk->key.rsa.n, pk->key.rsa.nlen);
memcpy(t->pinned_pk_e, pk->key.rsa.e, pk->key.rsa.elen);
```

The "MUST copy" comment is real. The decoder's pkey accessor
returns pointers into its own context — the moment the decoder
context is recycled (or in our case, falls off the stack at the
end of the init function), those pointers dangle. Forgetting
that copy step works for exactly as long as the stack frame
happens to keep `dec` alive, which is "until the next function
call". Painful to debug; trivial to avoid.

The second trap is in `br_ssl_client_init_full` itself.
The function signature is:

```c
void br_ssl_client_init_full(br_ssl_client_context *cc,
    br_x509_minimal_context *xc,
    const br_x509_trust_anchor *trust_anchors,
    size_t trust_anchors_num);
```

We don't want the minimal validator — we want knownkey — so
the natural assumption is that we can pass `NULL` for `xc`.
We can't. The very first thing inside the function is

```c
br_x509_minimal_init(xc, &br_sha256_vtable, ...);
```

which dereferences `xc` unconditionally. Passing `NULL` crashes
in EL0 with a data abort, `FAR_EL1 = 0`. We learned this the
hard way on the first end-to-end run: the boot got as far as
the shell prompt, `tlstest --handshake` printed its banner,
connected the TCP socket, then died inside `init_full`.

The fix is small but visible. We carve out a `br_x509_minimal_context`
field in our struct — explicitly named `xc_unused` so the
intent is documented — pass its address to `init_full`, let
`init_full` populate it with all the cipher-suite, hash, and
verifier wiring, and then *immediately* call
`br_ssl_engine_set_x509(&t->cc.eng, &t->xkc.vtable)` to
overwrite the engine's `x509ctx` pointer with our knownkey
validator. After that line, nothing inside the engine ever
references `xc_unused` again. The 5 KB it costs us in the
struct is the cheapest possible price for staying on the
documented init path.

## The two-process flow

The runtime picture is:

```
/bin/init  -+----- spawn /bin/httpd   80    (chapter 106c)
            +----- spawn /bin/httpsd  8443  (this chapter)
            '----- spawn /bin/sh

shell:    tlstest --handshake 127.0.0.1 8443
            |
            v
       tls_socket_init_knownkey_from_cert(t, CERT0, sizeof CERT0)
           -> br_x509_decoder_*  extracts (n, e)
           -> br_x509_knownkey_init_rsa
       tls_socket_connect(t, 127.0.0.1, 8443, "localhost")
           -> getrandom(64) -> inject_entropy
           -> br_ssl_client_init_full
           -> br_ssl_engine_set_x509 (override -> knownkey)
           -> br_ssl_engine_set_buffer (bidi, 33 KiB)
           -> br_ssl_client_reset(cc, "localhost", 0)
           -> socket_connect  (TCP 127.0.0.1:8443)
           -> br_sslio_init   (wires low_read / low_write)
           -> br_sslio_flush  (drives the handshake)
                  ⇄ httpsd's accept loop completes its half
       br_sslio_write_all(req); br_sslio_flush
       br_sslio_read loop until peer FIN
       assert("tls handshake ok" in body)
       tls_socket_close(t)   -> close_notify exchange
```

Both halves of the pipe — client and server — use the same
`tls_low_read` / `tls_low_write` pattern, the same 64-byte
entropy injection, and the same `br_sslio_*` convenience
layer. The only structural difference is that the server side
is constructed by `br_ssl_server_init_full_rsa(&sc, chain,
chain_len, &key)` instead of `br_ssl_client_init_full`. Once
both engines are reset and the I/O wrappers are wired, the
first `br_sslio_read` on either side drives the whole
ClientHello → ServerHello → Certificate → ServerHelloDone →
ClientKeyExchange → ChangeCipherSpec → Finished sequence to
completion. None of that is code *we* write; it is all inside
BearSSL.

## httpsd, the in-guest TLS server

`httpsd.c` is about 200 lines, half of which is comments and
the rest of which is the accept loop plus the canned response.
The structure is deliberately the same as `httpd.c` from
chapter 105 — a single-threaded `for(;;) socket_accept()` —
plus a per-connection TLS context. The TLS context is too big
for the stack (~36 KB, mostly the bidi buffer), so each
connection malloc's its own:

```c
conn_t *c = (conn_t *)malloc(sizeof *c);
br_ssl_server_init_full_rsa(&c->sc,
                            test_server_chain,
                            test_server_chain_len,
                            test_server_key);
br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
srv_inject_entropy(&c->sc.eng);
br_ssl_server_reset(&c->sc);
br_sslio_init(&c->ioc, &c->sc.eng,
              srv_low_read,  &c->fd,
              srv_low_write, &c->fd);
```

After that the handler reads up to 1 KiB of request, ignores it
("any GET gets the same response"), and writes back a fixed
response body containing the marker `tls handshake ok`. The
marker is the actual evidence the regression script checks: it
only appears *inside* the TLS-encrypted record stream, so if
the client sees it, the handshake decrypted correctly and the
record layer worked end to end.

## Wiring it into init

`init.c` already spawns `/bin/httpd 80` (chapter 106c) so that
the desktop comes up with a usable local web server. Chapter
112b adds the matching line for TLS:

```c
puts("[init] launching /bin/httpsd 8443 (background, loopback TLS)");
int httsid = spawn("/bin/httpsd", "8443");
if (httsid < 0) { /* non-fatal, regression will surface this */ }
```

Port 8443 (conventional "alternate HTTPS") avoids the
host-side conflict that 443 might bring through SLIRP and
sidesteps the inevitable "needs root" surprise on hosted ports.
The two listeners coexist: `httpd` on 80 serves cleartext
files; `httpsd` on 8443 serves the canned TLS body. Both bind
loopback only — neither is reachable from the host network,
both are reachable from anything else in the guest.

## The regression: `scripts/test_tls_handshake.py`

The test script mirrors `scripts/test_tlstest.py` (chapter 112a)
with the same QEMU command line — crucially including
`-object rng-random,id=rng0,filename=/dev/urandom -device
virtio-rng-device,rng=rng0` so the kernel CSPRNG can satisfy
`getrandom()`. It waits for the shell prompt, types
`tlstest --handshake 127.0.0.1 8443`, and asserts three lines
in order:

```
pinned RSA-2048 public key from leaf cert
handshake complete
tls handshake: PASS
```

A failure on any of them prints the surrounding 2000 bytes of
serial output, which is usually enough to localise the bug
(getrandom path, handshake state machine, or the response
read). The script also screens for `[svc] unknown syscall` and
`[svc] FATAL` — the canonical "tlstest tried to do something
the kernel can't yet" and "tlstest dereferenced something bad"
markers respectively.

## Things that bit us

- **`br_ssl_client_init_full(cc, NULL, ...)` crashes.** Pass
  a real `br_x509_minimal_context*`, even if you intend to
  override the validator immediately afterward. Putting one
  in the struct as a named `xc_unused` field is the cheapest
  and most legible fix.
- **`br_ssl_engine_inject_entropy` is mandatory** in a
  freestanding build where `BR_USE_*` is all `0`. Without it
  reset returns 0 silently; you only see the failure when
  `br_sslio_flush` returns `-1` with `last_error = BR_ERR_BAD_PRNG`.
- **The shim and `syscall.h` both declare `strlen`.** We
  cross-guard them with a shared `OSDEV_STRLEN_PROVIDED`
  macro so whichever header is included first installs the
  symbol and the other one skips. Without the guard,
  including both in the same TU is a hard compile error.
- **`br_prng_seeder_system` is referenced from `ssl_engine.o`**
  even when no seeder will ever be used. A 5-line stub in
  `cstring.c` returning `NULL` makes the linker happy and
  matches BearSSL's "caller must inject" contract.
- **Read callbacks must return `-1` on peer FIN**, not `0`.
  The C convention and the BearSSL convention are exactly
  opposite here.
- **`br_x509_decoder_get_pkey` returns pointers into the
  decoder's own storage.** Copy them out before the decoder
  context goes out of scope.

## What this unlocks (applied-to inventory)

Per the project rule that "OS features the book builds need to
be incorporated into the existing applications, and new
applications should be added to demonstrate anything that can't
be used in the existing ones":

- **New application: `/bin/httpsd`** — in-guest TLS server.
  Spawned by `init.c` on port 8443 at every boot, so anything
  the user (or a future test) wants to point at a known
  good TLS endpoint can use it. Useful to chapter 112d
  (browser HTTPS) and 112e (end-to-end public HTTPS).
- **New libc surface: `tls_socket.{h,c}`** — heap-allocated
  TLS client object with `init_knownkey_from_cert`,
  `connect`, `send`, `recv`, `flush`, `close`. Picked up by
  chapter 112d when the browser learns the `https://` scheme,
  and by any future userspace tool that wants TLS over our
  chapter-104 sockets.
- **`tlstest`** — gains a `--handshake HOST PORT` mode that
  exercises the full client surface (the SHA-256 KAT mode
  stays as the 112a smoke test).
- **`scripts/test_tls_handshake.py`** — new regression script
  added to the sweep.
- **Out of scope until 112c**: real CA store, chain
  validation, name matching beyond SNI passthrough, time-based
  expiry checks. The knownkey validator is the right shape for
  a loopback test; it is the wrong shape for the open Internet.
  Chapter 112c builds the validator we'll actually trust.

## Next: chapter 112c — root certificates and chain validation

The knownkey shortcut works because the client knows the
server's public key by other means (it extracted it from a
cert it also knows). Real TLS works because the client knows
a small set of *root certificates* and trusts any server
whose chain ends at one of them. Chapter 112c brings that in:
encode the Mozilla NSS root store as a static `br_x509_trust_anchor[]`
array, switch `br_ssl_client_init_full` to use the real minimal
validator instead of knownkey, and replace the time-stub with
`SYS_GETTIMEOFDAY` so cert expiry checks become honest. That
opens the door to chapter 112d — pointing the browser at a
real `https://` URL — and chapter 112e, an end-to-end public
HTTPS fetch.
