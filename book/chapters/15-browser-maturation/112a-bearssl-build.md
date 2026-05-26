# Chapter 112a — BearSSL builds for our userspace

> **Milestone in this chapter:** 112a — vendor BearSSL and prove
> one primitive works in-guest.
> **Code referenced:**
> - [vendor/bearssl/](../../../vendor/bearssl/) (vendored source)
> - [userspace/tlstest/tlstest.c](../../../userspace/tlstest/tlstest.c)
> - [scripts/test_tlstest.py](../../../scripts/test_tlstest.py)
>
> **At the end of this chapter** `libbearssl.a` is in the build tree,
> the cross toolchain links it against a freestanding userspace
> binary, and the regression boots the kernel, runs `/bin/tlstest`,
> and verifies that SHA-256 of a fixed input matches the expected
> digest. No TLS handshake yet — chapter 112b runs the first one.

Chapter 112 added entropy. The kernel can now hand out
cryptographically unguessable bytes via `SYS_GETRANDOM`, and
`/bin/getrand` prints them. That solves the *first* prerequisite
for TLS — a real PRNG. Several more remain, and they all reduce to
the same problem: a TLS *implementation* is needed. A library that
can produce a ClientHello, agree on an ECDHE shared secret, derive
AES-GCM keys, parse and verify an X.509 chain, and feed the
application 16-KiB plaintext records. That is a meaningful amount
of code — five-to-six-figure lines of it — and the entire stack has
to compile under the cross toolchain (`aarch64-elf-gcc`, no libc,
no syscalls beyond those built earlier), run inside a thread with
no FP/NEON registers, and produce deterministic output that can be
KAT-tested on the host.

This chapter takes the first step of that port: vendor the library,
get it to *build* against the freestanding userspace, and prove that
one cryptographic primitive (SHA-256) computes the right answer when
called from an osdev `/bin/` binary.

```
/$ tlstest
tlstest sha256(empty): e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
tlstest sha256(abc)  : ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
tlstest: PASS bearssl sha256 matches NIST vectors
```

and exits 0. From here, every later chapter in the TLS series
gets to assume "the library is there, it builds, the round
functions are correct" and focus on the harder questions of
plumbing, sockets, certificates, and trust.

## Why BearSSL

The big-name TLS libraries — OpenSSL, BoringSSL, GnuTLS — are
all built for hosted Unix systems. They assume libc, a
filesystem they can `fopen`, a process model with `fork`, often
`pthreads`, sometimes even `epoll`. To compile any of them on
our cross toolchain we would have to either port an enormous
amount of POSIX or hack out an enormous amount of #ifdef'd
infrastructure. Neither is in keeping with the spirit of this
book.

BearSSL is different. From [bearssl.org](https://bearssl.org):

> BearSSL is an implementation of the SSL/TLS protocol
> (RFC 5246) written in C. It aims at offering the following
> features: ...
> - **Be small**, both in code footprint and RAM usage.
> - **Be highly portable**. BearSSL targets not only "big"
>   operating systems like Linux and Windows, but also tiny
>   embedded systems and even special runtime environments.
> - **Be feature-rich enough** to allow for production use, but
>   not so much as to be bloated.

It is written by Thomas Pornin (one of the people who actually
implemented TLS 1.3 in production), it is permissively licensed
(MIT), and crucially the entire library is designed to be
*embeddable*. There is no built-in network code. There is no
built-in filesystem code. There is no built-in PRNG that
*has* to come from `/dev/urandom`. Every one of those is an
adapter the user provides. That is *exactly* the shape that fits
a hobby kernel: BearSSL does the cryptography, our kernel
provides the bytes-in, bytes-out, and entropy callbacks.

The numbers: the full source tree is 277 `.c` files. None of
them are larger than a few hundred lines. The single file
that wants OS bytes is `src/rand/sysrng.c` — and we exclude it
from our build, because chapter 112 already provided
`SYS_GETRANDOM` and BearSSL is happy to use any callback for
seeding (chapter 112b wires that up).

## Vendoring

BearSSL is vendored at `vendor/bearssl/` at the unmodified
upstream 0.6 release. We do not patch a single byte of the
library; everything that needs to differ goes in *adjacent*
shims at `vendor/bearssl-shim/`. The checksum is recorded for
later integrity verification:

```
sha256(bearssl-0.6.tar.gz) =
  6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14
```

The tree:

```
vendor/
├── bearssl/                  (the upstream 0.6 release)
│   ├── inc/
│   │   ├── bearssl.h
│   │   └── bearssl_*.h
│   ├── src/
│   │   ├── hash/   sha2small.c sha2big.c md5.c ...
│   │   ├── codec/  enc32be.c dec32be.c ...
│   │   ├── symcipher/ aes_big_*.c chacha20_ct.c ...
│   │   ├── mac/    hmac.c ...
│   │   ├── kdf/    hkdf.c ...
│   │   ├── ec/     ec_p256_*.c ec_curve25519_*.c ...
│   │   ├── rsa/    rsa_*.c ...
│   │   ├── x509/   x509_minimal.c x509_decoder.c ...
│   │   ├── ssl/    ssl_engine.c ssl_io.c ssl_*.c ...
│   │   └── rand/   sysrng.c          ← excluded from our build
│   └── ... (LICENSE, README, COPYING)
└── bearssl-shim/
    └── string.h              (5-symbol shim, see below)
```

Vendoring without patching has two consequences worth being
explicit about:

1. **Upstream upgrades are easy.** Drop a new release into
   `vendor/bearssl/`, re-run the build, fix anything that
   breaks. The shim layer is small enough that it almost
   never needs to change.
2. **Our build can't trust the source not to have warnings.**
   BearSSL is clean code, but it was written for several
   different toolchains, and on each toolchain it accepts a
   few "harmless" diagnostics that wouldn't fly under our
   `-Werror`. We turn warnings off for vendor code (`-w`)
   rather than censor or rewrite the upstream sources.

## The freestanding port: three small pieces

Our cross toolchain (`aarch64-elf-gcc 14.2.0` from Homebrew)
is a *bare-metal* compiler. It ships only freestanding headers:
`<stddef.h>`, `<stdint.h>`, `<limits.h>`, `<stdarg.h>`, and a
handful more. It deliberately ships *no* `<string.h>`, *no*
`<stdio.h>`, *no* `<time.h>`. BearSSL's
[`src/inner.h`](../../../vendor/bearssl/src/inner.h) does:

```c
#include <string.h>
```

…and that breaks the build immediately. We fix it not by
patching `inner.h` but by providing our *own* `<string.h>`,
placed earlier in the include path:

### Piece 1 — the shim header

[`vendor/bearssl-shim/string.h`](../../../vendor/bearssl-shim/string.h):

```c
#ifndef BEARSSL_SHIM_STRING_H
#define BEARSSL_SHIM_STRING_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#ifdef __cplusplus
}
#endif

#endif
```

That is the entire ABI BearSSL needs from `<string.h>`. Five
declarations. No definitions. When BearSSL's `.c` files include
`<string.h>`, the compiler finds these prototypes, accepts the
call sites, and emits external references to the linker. The
linker resolves those references against piece 2.

### Piece 2 — extern definitions in userspace libc

[`userspace/libc/cstring.c`](../../../userspace/libc/cstring.c) provides
the actual implementations:

```c
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
/* memmove, memset, memcmp, strlen analogous */

/* time(NULL) is dead code under our autodetect (none of
 * __APPLE__, __unix__, __linux__, _POSIX_*, _WIN32 are
 * defined when cross-compiling aarch64-elf), but x509_minimal.c
 * still has a call site that the linker won't tree-shake.
 * Return 0 — "epoch" — and let chapter 112c plug in the real
 * wall clock via SYS_GETTIMEOFDAY. */
long time(long *out) { if (out) *out = 0; return 0; }
```

These are byte-at-a-time implementations and they are very
deliberately *not* fast. Hand-tuned word-at-a-time memcpy is
not the point of this chapter. The byte loop is one screenful
of code, it is obviously correct, and it gives us a stable
ground floor on which to debug everything else.

### Piece 3 — keeping the BearSSL build settings separate

Our regular `USER_CFLAGS` are strict — `-Werror`,
`-fno-asynchronous-unwind-tables`, `-Wall -Wextra`. BearSSL
inherits the strictness but trips a few warnings (`-Wsign-compare`,
unused parameters, a couple of `-Wmaybe-uninitialized`
hits that the static analysis can't see through). Rather
than fight the upstream code, we give BearSSL its own
`CFLAGS` variable in [`Makefile`](../../../Makefile):

```make
BEARSSL_CFLAGS := -ffreestanding -nostdlib \
                  -mcpu=cortex-a72 -mgeneral-regs-only \
                  -fno-stack-protector -fno-pie -fno-pic \
                  -fno-asynchronous-unwind-tables \
                  -O2 -g -MMD -MP -w $(BEARSSL_INC)
```

The two flags that matter most are:

- `-mgeneral-regs-only` — disable FP/NEON. Our context switch
  doesn't save/restore FP, so any TU that emits a `fmov` will
  silently corrupt the next thread's float state. BearSSL is
  pure integer arithmetic, but the compiler will sometimes
  auto-vectorise *if you let it*. We don't let it.
- `-w` — turn warnings off for vendor code. Combined with
  `-O2 -g`, this gives us a release-quality build at a
  reasonable size (~4 MB static archive) with debug symbols
  in case the round functions ever do go wrong.

## The Makefile glue

A complete second-archive-in-the-tree wants exactly five things:
the source list, the object list, the archive itself, a pattern
rule that uses the right CFLAGS, and an archive rule that
collects everything.

```make
BEARSSL_SRCS := $(shell find vendor/bearssl/src -name '*.c' \
                    -not -path 'vendor/bearssl/src/rand/sysrng.c')
BEARSSL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(BEARSSL_SRCS))
BEARSSL_LIB  := $(BUILD)/vendor/bearssl/libbearssl.a
BEARSSL_INC  := -I vendor/bearssl-shim \
                -I vendor/bearssl/inc \
                -I vendor/bearssl/src

$(BUILD)/vendor/bearssl/%.o: vendor/bearssl/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(CROSS)ar rcs $@ $^
```

Three details earned a black eye in earlier projects:

- The `-not -path ...` in the `find` skips `sysrng.c`. If we
  let it through, it pulls in `<windows.h>` or
  `<sys/random.h>` depending on autodetect, neither of which
  we provide. (Chapter 112b will substitute a BearSSL
  `br_prng_seeder` callback that reads from `SYS_GETRANDOM`.)
- The `-I vendor/bearssl-shim` *must* come before
  `-I vendor/bearssl/inc` so our `<string.h>` wins the
  include resolution. We achieve that by listing it first in
  `BEARSSL_INC`.
- The `@rm -f $@` before `$(CROSS)ar` is critical. `ar rcs`
  *appends* if the archive already exists; without the
  pre-clean, an iterative build that recompiles one TU
  doesn't replace the old object inside the archive, it
  duplicates it. The next link picks an arbitrary copy and
  the build becomes mysteriously non-reproducible.

The `tlstest` binary itself follows the same five-line pattern
as every other osdev userspace tool (the `BEEP_*` template
from chapter 96):

```make
CSTRING_OBJ      := $(BUILD)/userspace/libc/cstring.o
TLSTEST_OBJS     := $(BUILD)/userspace/crt/crt0.o \
                    $(BUILD)/userspace/tlstest/tlstest.o \
                    $(CSTRING_OBJ)
TLSTEST_ELF      := $(BUILD)/userspace/tlstest/tlstest.elf
TLSTEST_STRIPPED := $(BUILD)/userspace/tlstest/tlstest.stripped.elf

$(BUILD)/userspace/tlstest/tlstest.o: userspace/tlstest/tlstest.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(USER_CFLAGS) \
	    -I vendor/bearssl-shim -I vendor/bearssl/inc \
	    -c $< -o $@

$(TLSTEST_ELF): $(TLSTEST_OBJS) $(BEARSSL_LIB)
	@mkdir -p $(dir $@)
	$(CROSS)ld $(USER_LDFLAGS) \
	    --start-group $(TLSTEST_OBJS) $(BEARSSL_LIB) --end-group \
	    -o $@
```

`--start-group ... --end-group` is the *one* link-line trick this
chapter introduces. BearSSL is a static archive, so the linker
walks it in order and pulls in each `.o` exactly once. Inside
the archive, TUs reference each other: `sha2small.c` calls into
`enc32be.c`; `ssl_engine.c` calls into half the world. Without
`--start-group`, the linker visits each `.o` once and an
inter-archive reference that wasn't yet known to be needed when
its TU was visited becomes an "undefined reference". Wrapping
the whole list in a group tells the linker to loop until the
unresolved-references set stops shrinking. For a single static
archive linked once, the cost is negligible.

## The smoke binary

[`userspace/tlstest/tlstest.c`](../../../userspace/tlstest/tlstest.c) is
roughly 130 lines. It uses BearSSL's `br_sha256_*` API directly:

```c
#include "bearssl.h"

static const unsigned char k_empty_expected[32] = {
    0xe3,0xb0,0xc4,0x42,/* ... */ 0x78,0x52,0xb8,0x55,
};
static const unsigned char k_abc_expected[32] = {
    0xba,0x78,0x16,0xbf,/* ... */ 0xf2,0x00,0x15,0xad,
};

int main(int argc, char **argv)
{
    br_sha256_context ctx;
    unsigned char     digest[32];

    br_sha256_init(&ctx);
    br_sha256_out(&ctx, digest);
    print_hex32("tlstest sha256(empty)", digest);
    if (!eq32(digest, k_empty_expected)) { /* fail */ }

    br_sha256_init(&ctx);
    br_sha256_update(&ctx, "abc", 3);
    br_sha256_out(&ctx, digest);
    print_hex32("tlstest sha256(abc)  ", digest);
    if (!eq32(digest, k_abc_expected)) { /* fail */ }

    printf("tlstest: PASS bearssl sha256 matches NIST vectors\n");
    return 0;
}
```

We test two vectors, not one, on purpose. The empty-string
digest exercises only `br_sha256_init` plus the final
`br_sha256_out` (one compression round on the all-zero
post-padding block). The `"abc"` vector also exercises
`br_sha256_update` (the 3-byte memcpy into the per-context
buffer plus the count bookkeeping). Together they give the
KAT meaningful coverage of the data path *and* the
initialization path — and any later regression that breaks
either will be caught at boot.

NIST publishes both digests in FIPS 180-4 Appendix B. They are:

```
SHA256("")    = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
```

That second value is the one we will return to in the war
story below.

## Two debugging stories worth telling

The chapter's title says "BearSSL builds for our userspace",
and once it built it was supposed to be a five-minute write-up.
Two bugs lengthened the day; both are educational enough to be
worth recording in plain sight.

### War story 1 — `char buf[80]` was nine bytes too short

The first `print_hex32` looked like this:

```c
static void print_hex32(const char *label, const unsigned char *d)
{
    char buf[80];
    /* ... write label, ': ', 64 hex chars, '\n', '\0' ... */
    write(1, buf, i);
}
```

The arithmetic for that buffer goes:

```
21 (label) + 2 (": ") + 64 (32 bytes * 2 hex chars) + 1 ('\n') + 1 ('\0') = 89
```

We wrote 89 bytes into an 80-byte stack array. The nine bytes
of overflow walked off the end of `buf[]` and into the next
thing on the stack — main's saved return address. So when
`main` returned, it returned not to `crt0`'s `SYS_EXIT` glue
but back to *its own entry point*. main looped on itself
forever.

The symptom in the serial log was an infinite stream of the
same five lines:

```
tlstest
tlstest sha256(empty): e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
tlstest sha256(abc)  : ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
tlstest sha256(empty): e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
tlstest sha256(abc)  : ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
...
```

This is a textbook stack-smash. It is a useful lesson because
in a normal hosted program, an 80-byte buffer overrun would
either be invisible (caught by stack canaries you didn't ask
for) or fatal (caught by a guard page). Our freestanding
userspace has neither — `-fno-stack-protector` is on
deliberately, and we don't map a guard page below each stack
yet. So the overrun just *executes*. Future chapters that need
real stack hygiene will need to add either canaries or guard
pages or both; for now, every userspace tool's stack buffer
must be hand-counted with margin.

Fix:

```c
static void print_hex32(const char *label, const unsigned char *d)
{
    char buf[160];   /* 89 minimum, doubled for label tweaks */
    ...
}
```

Related: `/memories/freestanding-c-memset-trap.md` is the
companion lesson — implicit `memset` calls from `= {0}` struct
inits in freestanding C.

### War story 2 — the expected vector was wrong by one nibble

The infinite loop went away. The empty-string digest matched.
The `"abc"` digest didn't:

```
got:      ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad
expected: ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 39617 7a9 cb410f f6 1f200 15a
```

The first 20 bytes matched and bytes 20 through 31 disagreed
with what looked like a *4-bit left shift*: the expected
sequence `39 61 77 a9 cb 41 0f f6 1f 20 01 5a` interpreted as
a 96-bit number, shifted left by one nibble, with a stray
`d` nibble appearing at the low end. That is not a *plausible*
compiler bug — a byte-aligned store can't slip a nibble — but
it *also* isn't a plausible algorithmic bug, because the first
20 bytes were correct. SHA-256 doesn't have an "early-state
correct, late-state subtly wrong" failure mode: it's a
forward-only state mix where every output word depends on
every input word.

The debugging path went, in order:

1. Disassembled `print_hex32` (correct).
2. Re-checked the SHA-2 sigma macros (standard).
3. Re-checked the IV tables and the K constants (correct).
4. Verified the `br_sha256_context` struct layout (112 bytes:
   vtable + 64-byte buf + 8-byte count + 8×4-byte state).
5. Toggled BearSSL's CFLAGS from `-O2` to `-O0` (no change —
   still the same wrong bytes).
6. Forward-declared `br_sha2small_round` and
   `br_range_enc32be` in tlstest.c and hand-rolled the
   compression on a manually-built padded block (correct
   bytes returned by BearSSL).
7. Printed `val[0..7]` as eight plain 8-hex words *without*
   going through `br_range_enc32be`. Output:

   ```
   tlstest val[0..7]: ba7816bf 8f01cfea 414140de 5dae2223
                      b00361a3 96177a9c b410ff61 f20015ad
   ```

That last step was the moment. The eight state words BearSSL
computed *were the canonical NIST result*. Our hardcoded
expected vector was wrong. Looking at it byte by byte:

```c
0xb0,0x03,0x61,0xa3,
    0x39,0x61,0x77,0xa9,    /* <-- shifted one nibble */
    0xcb,0x41,0x0f,0xf6,    /* <-- shifted one nibble */
    0x1f,0x20,0x01,0x5a,    /* <-- shifted one nibble, last d lost */
```

The correct bytes are `0x96,0x17,0x7a,0x9c, 0xb4,0x10,0xff,0x61,
0xf2,0x00,0x15,0xad`. The wrong bytes are those same bytes
written out as nibbles, *shifted right by one nibble in the
typing*, with the leading `9` re-typed as `3` (a column slip
between `b00361a3 9` and `b00361a3 3`) and the trailing `d`
dropped. Pure human transcription error. BearSSL was right
all along.

The lesson here is general enough to live above the BearSSL
port. If a "wrong" output and the "expected" value differ by
a *shape* (a constant byte offset, a constant bit shift, an
endian flip, a missing byte plus an invented byte), the most
likely cause is **the human-typed expected** value, not the
machine-computed actual. The 30-second fix:

```
$ echo -n abc | sha256sum
ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  -
```

Run that *before* you instrument the library. The lesson is
permanent in `/memories/verify-expected-vector-first.md`.

## What the regression script does

[`scripts/test_tlstest.py`](../../../scripts/test_tlstest.py) follows the
same shape as `test_getrand.py`:

1. Boot the kernel under QEMU with the full kitchen-sink device
   line (rng, sound, net, two disks). Wait for the shell
   prompt.
2. Send `tlstest\n`.
3. Wait for either `tlstest: PASS bearssl sha256 matches NIST
   vectors` or any line beginning with `FAIL`.
4. Slice the captured output forward from the *first*
   occurrence of `b"tlstest"` (the shell command echo). Using
   `rfind` instead of `find` skips the digest lines entirely —
   the last `tlstest` substring is in the trailing PASS line.
5. Strip whitespace from the slice (`re.sub(rb"\s+", b"", tail)`).
   The 64-char digest wraps at the 80-column serial console;
   without the strip, a substring search for the full 64-char
   hex never matches.
6. Assert both digests are present and the summary PASS line
   was emitted. Exit 0 on success.

Both of those slicing rules — first-occurrence and
whitespace-strip — are the kind of "obvious in retrospect"
gotchas that a smoke test for any wrap-prone serial output
should handle. Future TLS tests in this section will reuse the
same two patterns.

## Applied to / new in this chapter

**New vendored code**
- `vendor/bearssl/` — BearSSL 0.6 unmodified release.
- `vendor/bearssl-shim/string.h` — 5-symbol shim header.

**New osdev code**
- `userspace/libc/cstring.c` — extern `mem*`, `strlen`,
  `time` stub.
- `userspace/tlstest/tlstest.c` — chapter 112a SHA-256 KAT.

**Makefile additions**
- `BEARSSL_SRCS`, `BEARSSL_OBJS`, `BEARSSL_LIB`,
  `BEARSSL_INC`, `BEARSSL_CFLAGS`.
- Pattern rule `$(BUILD)/vendor/bearssl/%.o:
  vendor/bearssl/%.c` using `BEARSSL_CFLAGS`.
- Archive rule for `libbearssl.a` with `@rm -f $@`.
- `CSTRING_OBJ`, `TLSTEST_OBJS`, `TLSTEST_ELF`,
  `TLSTEST_STRIPPED`.
- Explicit `tlstest.o` compile rule with the BearSSL include
  path.
- Link rule with `--start-group ... --end-group`.
- New entry `tlstest` in `OSFS_BIN_FILES`.

**Regression and debug scripts**
- `scripts/test_tlstest.py` — kernel-boot KAT.
- `scripts/_dbg_tlstest_verbose.py` — captures the full boot
  log plus tlstest serial output. Kept per
  `/memories/debug-scripts-policy.md`.

**Existing apps modified**
- None yet. `tlstest` is purely a regression binary. The first
  real app to use BearSSL is the browser, in chapter 112d.

## What this unlocks

We now have a static archive of TLS-grade cryptography in our
build tree. The next four chapters fan out from here:

- **Chapter 112b — `tls_socket_*` and the SSL engine.** Wire
  BearSSL's `br_ssl_client_*` to our `tcp_*` socket layer.
  Plug `SYS_GETRANDOM` into BearSSL's PRNG seeder. End-state:
  `tlstest` performs a handshake against an in-guest
  `httpd-tls` over loopback.
- **Chapter 112c — root CA store and chain validation.**
  Ship a small set of CA certificates as a userspace constant
  blob, plumb `br_x509_minimal_set_time` from the new
  `SYS_GETTIMEOFDAY` (chapter 95 wall clock).
- **Chapter 112d — browser `https://`.** Teach the browser
  URL parser to recognise `https://`, route through the
  `tls_socket_*` layer, and present a padlock indicator.
  Remove `scripts/https_proxy.py` from the boot path (keep
  the file per debug-scripts-policy).
- **Chapter 112e — end-to-end public HTTPS.** A regression
  that fetches a real page from a public HTTPS site through
  the guest browser, validates the chain against our shipped
  root store, and renders.

Together, those four chapters complete the user-visible
promise that motivated chapter 112: the browser can speak to
the real web.
