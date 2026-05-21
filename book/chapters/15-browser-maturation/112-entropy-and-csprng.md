# Chapter 112 — Entropy: virtio-rng and a kernel CSPRNG

Chapter 111 closed the JavaScript loop. Forms could now react to
clicks; pages could mutate their own DOM. What was still missing
from anything resembling a real browser was the green padlock —
the ability to talk to `https://` without laundering the
connection through an unencrypted proxy on the host. Chapters
113 through 117 will port BearSSL and wire it into our `tcp_*`
socket layer. But TLS — every TLS — needs an honest source of
randomness *first*. The client random in the ClientHello, the
ephemeral key for ECDHE, the IV for every AEAD record: all of
them must be unguessable. A predictable PRNG in a TLS stack is
not a bug, it is a backdoor. We can't even *start* the BearSSL
port until the kernel can fill a buffer with bytes that an
attacker reading the kernel binary cannot reconstruct.

This chapter is that prerequisite. We:

1. Bring up the `virtio-rng` device — virtio device id 4, the
   simplest device in the whole specification — so the kernel
   can ask the host for fresh bytes from `/dev/urandom`.
2. Build a small ChaCha20 keystream CSPRNG that stretches each
   32-byte seed pull into as many output bytes as the system
   wants, and re-keys itself every 256 KiB.
3. Add `SYS_GETRANDOM` (syscall 94) so userspace can ask for
   `n` cryptographic bytes. BearSSL's libtls will call this. So
   will any future code that needs an unguessable cookie, a
   session id, or a salt.
4. Ship a tiny userspace tool, `/bin/getrand`, that prints `n`
   bytes as hex. It's the visible end of the stack and the
   thing the regression test runs.

By the end the kernel boots with a single new line in the log:

```
probing virtio-mmio bus for an RNG ... ok (entropy online)
[random] CSPRNG seeded from virtio-rng (strong)
```

And `/bin/getrand 16` is a working coin-flip:

```
/$ getrand 16
46230fa905588ae6abd2762caa6ac5cb
/$ getrand 16
eb6d3c5097f7a770f443dffe5b7dfcbb
```

## Why we need a hardware source at all

A pseudo-random generator alone is not enough. Every PRNG
output is a deterministic function of its seed. If the seed is
guessable, the output is guessable; if the seed is the same
across boots, the output is the same across boots. A kernel
that seeded its PRNG from `CNTVCT_EL0` and a few stack pointers
would, in principle, produce different bytes each run — but
those bytes are entirely a function of values an attacker who
has the kernel ELF and a guess at boot time can reproduce.
TLS session keys derived that way are not secret.

The fix is to take the entropy from somewhere the attacker
can't see: the host's OS. On a real machine that would be RDRAND
or a TRNG IP block. Under QEMU it's the host kernel's
`/dev/urandom`, exposed as a virtio device:

```
-object  rng-random,id=rng0,filename=/dev/urandom
-device  virtio-rng-device,rng=rng0
```

QEMU treats `filename=/dev/urandom` as "read from this when the
guest asks for bytes". The guest can't predict those bytes
without compromising the host. Good enough.

## The virtio-rng device

Virtio-rng is the simplest virtio device in the spec. It has:

- One virtqueue.
- No config space worth reading.
- No driver features to negotiate beyond `VERSION_1`.
- A single request shape: "host, here is a device-writable
  buffer of length N; fill it."

That makes it a clean place to write our second
virtio-mmio-v2 driver (the first was `virtio_snd`, chapter 96).
The whole driver lives in [kernel/device/virtio_rng.c](kernel/device/virtio_rng.c).

### Probe

The MMIO bus has 32 slots starting at `0x0A000000`, each 0x200
bytes wide. For each slot we read three magic registers:

```c
static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_ENTROPY)
        return 0;
    return 1;
}
```

`0x74726976` is `"virt"` little-endian; `VERSION == 2` keeps us
on the modern transport (chapter 96 explained the v1 vs v2
trap); `DEVICE_ID == 4` is the entropy device.

On QEMU the entropy device shows up at slot `0x1c` — i.e.
`0x0A003800` — because the audio device occupies an earlier
slot. The boot log records exactly where:

```
[virtio-rng] found device at slot 0x000000000000001c base=0x000000000a003800
```

### Handshake

The handshake matches every other virtio-mmio device we've
written. Set `STATUS` to `0` to reset; then `ACKNOWLEDGE`, then
`DRIVER`. Read the device feature words. Virtio-rng exposes
exactly one driver-visible bit: `VIRTIO_F_VERSION_1`, which
lives in the upper word (feature index 32). We accept it,
write `FEATURES_OK`, and confirm the device didn't clear the
bit back on us:

```c
w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
uint32_t feat_hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
if (!(feat_hi & 1u)) { /* device is broken */ return -1; }
w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
w32(VIRTIO_MMIO_DRIVER_FEATURES, 1u);
```

### The ring page and the bounce page

For each virtqueue, virtio wants three physically-contiguous
regions in guest memory: a descriptor table, an avail ring, and
a used ring. With queue size 8 each of those is well under
128 bytes, so we pack all three into a single 4 KiB page:

```
+0x000  REQ desc table  (8 * 16 = 128)
+0x080  REQ avail ring  (4 + 16 + 2 = 22)
+0x0C0  REQ used ring   (4 + 64 + 2 = 70)
```

Then we allocate a **second** 4 KiB page as the bounce buffer.
Descriptors point at this page; the host writes our random bytes
into it; we `memcpy` from there into the caller's buffer. The
bounce-page-as-separate-page is a small defence in depth: if the
device ever wrote a few bytes past the requested length we'd
clobber an unused part of a dedicated page, not our ring
metadata.

Both pages come from `pmem_alloc_page`, which returns
physically-contiguous 4 KiB. We hand the device the physical
address of each:

```c
g_rng_ring_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
g_rng_bounce_pa = pmem_alloc_page();
g_rng_bounce    = (uint8_t *)(uintptr_t)g_rng_bounce_pa;
```

(The identity-mapped MMU layout means the physical and virtual
addresses of a `pmem_alloc_page` result are the same; the cast
is what conveys the address-space switch.)

### One request, one wait

The whole driver request path is a single function:

```c
static long submit_one(uint32_t chunk)
{
    struct vring_desc *d = req_desc();
    d[0].addr  = g_rng_bounce_pa;
    d[0].len   = chunk;
    d[0].flags = VRING_DESC_F_WRITE;     /* device writes us */
    d[0].next  = 0;

    struct vring_avail *av = req_avail();
    av->ring[g_avail_idx % REQ_QSIZE] = 0;
    dmb();
    g_avail_idx++;
    av->idx = g_avail_idx;
    dsb();
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, REQ_QID);

    uint64_t deadline = timer_ticks() * TICK_INTERVAL_MS + 1000;
    while (g_used_seen == req_used()->idx) {
        if (timer_ticks() * TICK_INTERVAL_MS > deadline)
            return -1;
        yield();
        dmb();
    }
    uint32_t written = req_used()->ring[g_used_seen % REQ_QSIZE].len;
    g_used_seen++;
    return (long)written;
}
```

Three things worth pointing at:

- **`VRING_DESC_F_WRITE`**: we are telling the device "this
  descriptor is for *you* to write into". The opposite flag
  (driver-writes-device) is what we use for, e.g., block-write
  requests.

- **The wait yields**. The driver is called from thread context
  (we never put `getrandom` on an IRQ path), so we can give the
  CPU up to other threads while we wait for the host. A busy
  spin would freeze the GUI for the duration of every random
  read.

- **We trust `used->ring[].len`, not our requested `chunk`.**
  Virtio-rng is allowed to short-fill if its host-side pool
  ran shallow; the device reports the actual number of bytes
  it managed. The outer loop in `virtio_rng_get` re-submits
  until the caller's whole buffer is full.

### The public API

```c
int  virtio_rng_init(void);                 /* probe + handshake */
int  virtio_rng_present(void);              /* true after init   */
long virtio_rng_get(void *out, size_t len); /* blocking read     */
```

That's the entire driver. `virtio_rng_get(buf, n)` returns `n`
on success, `-1` if the device is missing or times out.

## Stretching the seed: ChaCha20

A virtio-rng request goes out over the queue, the host
schedules itself, reads `/dev/urandom`, writes us back. Even
under HVF that's measured in tens of microseconds per request.
That's fine for the occasional 32-byte seed pull; it would be
miserable if every byte of every TLS record took a round trip
through the host.

So we layer a CSPRNG on top: ChaCha20 in keystream mode. Given
a 256-bit key and a 96-bit nonce, ChaCha20 produces an
indistinguishable-from-random byte stream as fast as the CPU
can compute the round function (a few hundred MB/s on this
hardware, which is many orders of magnitude faster than the
device path).

The full CSPRNG lives in [kernel/core/random.c](kernel/core/random.c).
It is intentionally minimal: ~230 lines including the ChaCha20
permutation, the seeding logic, the fallback path, and the
locking. The block function is RFC 7539 verbatim:

```c
#define QR(a, b, c, d) \
    do { \
        a += b; d ^= a; d = rotl32(d, 16); \
        c += d; b ^= c; b = rotl32(b, 12); \
        a += b; d ^= a; d = rotl32(d,  8); \
        c += d; b ^= c; b = rotl32(b,  7); \
    } while (0)

static void chacha20_block(const uint32_t key[8],
                           const uint32_t nonce[3],
                           uint32_t counter, uint8_t out[64])
{
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                       key[0], key[1], key[2], key[3],
                       key[4], key[5], key[6], key[7],
                       counter, nonce[0], nonce[1], nonce[2] };
    uint32_t x[16]; for (int i=0;i<16;i++) x[i]=s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0],x[4],x[ 8],x[12]); QR(x[1],x[5],x[ 9],x[13]);
        QR(x[2],x[6],x[10],x[14]); QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]); QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[ 8],x[13]); QR(x[3],x[4],x[ 9],x[14]);
    }
    for (int i = 0; i < 16; i++) x[i] += s[i];
    /* serialise x[] little-endian into out[0..64] */
}
```

That's a sentence-and-a-half of cryptography. The interesting
work is the seeding policy around it.

### State

```c
static uint32_t  g_key[8];           /* 256-bit key       */
static uint32_t  g_nonce[3] = {0,0,0};
static uint32_t  g_counter  = 0;
static uint32_t  g_since_reseed = 0;
static spinlock_t g_lock = SPINLOCK_INIT;
```

The nonce is permanently zero. That's safe because we use the
nonce-counter discriminator differently from the protocol:
each *reseed* logically gives us a fresh ChaCha instance with
a brand-new key, and within one instance the 32-bit `counter`
gives us up to 2³² × 64 = 256 GiB of output before a wrap. We
re-seed long before that.

### `mix_into_key`

A reseed doesn't *replace* the key with the new seed — it
XORs the seed into the existing key:

```c
static void mix_into_key_locked(const uint8_t seed[32])
{
    for (int i = 0; i < 8; i++) {
        uint32_t w = /* load seed[i*4 .. i*4+3] LE */;
        g_key[i] ^= w;
    }
    g_counter = 0;
    g_since_reseed = 0;
}
```

The reason is failure-tolerance. If virtio-rng ever returns a
short read, a constant value, or a buffer of zeros (say,
because the host-side pool was empty and the device misbehaved),
XOR-mixing means the new key is at least as strong as the old
key — never weaker. A naive `memcpy` would replace a good key
with a bad one.

### `fetch_seed`

Two paths:

```c
static void fetch_seed(uint8_t out[32])
{
    if (virtio_rng_present()) {
        if (virtio_rng_get(out, 32) == 32) return;
        serial_puts("[random] virtio-rng request failed, "
                    "using fallback\n");
    }
    fallback_seed(out);
}
```

The fallback combines `CNTVCT_EL0`, a stack address, a function
address, the kernel timer tick counter, and the address of the
key array itself. It exists *only* so that test harnesses that
forget to pass `-device virtio-rng-device` can still finish
boot — the warning printed at `random_init` is loud, and
`random_is_strong()` returns 0 so future TLS code will refuse
to start a handshake. The fallback is not, in any meaningful
sense, secret.

Note that `fetch_seed` runs **without** `g_lock` held. The lock
guards the ChaCha state; calling `virtio_rng_get` while holding
a spinlock would deadlock the second yield. The race between
two threads each fetching their own 32-byte seed and each
XOR-mixing it is benign — extra entropy can only help.

### `random_init`

```c
void random_init(void)
{
    g_strong = virtio_rng_present();
    uint8_t seed[32];
    fetch_seed(seed);
    spin_lock(&g_lock);
    for (int i = 0; i < 8; i++) g_key[i] = 0;
    mix_into_key_locked(seed);
    g_ready = 1;
    spin_unlock(&g_lock);
    if (g_strong) serial_puts("[random] CSPRNG seeded from "
                              "virtio-rng (strong)\n");
    else          serial_puts("[random] WARNING: no virtio-rng "
                              "device — CSPRNG seeded from "
                              "CNTVCT (NOT strong, do NOT use "
                              "for TLS)\n");
}
```

The starting key is all zeros; mix-into = XOR means the
post-mix key equals the seed exactly, but every subsequent
reseed preserves accumulated entropy.

### `random_bytes`

The hot path:

```c
long random_bytes(void *out, size_t len)
{
    uint8_t *dst = out; size_t got = 0;
    while (got < len) {
        if (g_since_reseed >= RESEED_BYTES) {
            uint8_t seed[32]; fetch_seed(seed);
            spin_lock(&g_lock); mix_into_key_locked(seed);
            spin_unlock(&g_lock);
        }
        uint8_t blk[64];
        spin_lock(&g_lock);
        chacha20_block(g_key, g_nonce, g_counter, blk);
        g_counter++; g_since_reseed += 64;
        spin_unlock(&g_lock);
        size_t take = len - got > 64 ? 64 : len - got;
        for (size_t i = 0; i < take; i++) dst[got + i] = blk[i];
        got += take;
    }
    return (long)got;
}
```

The threshold check is racy on purpose — two threads can both
see "time to reseed" at the same moment and each pull 32 fresh
bytes. The result is *more* entropy mixed in, not less.

### Locking discipline

A single spinlock, never held across `virtio_rng_get`, never
acquired from interrupt context. The risk we're protecting
against is two threads stepping on `g_counter` (which would let
them get the same 64-byte block, defeating the
no-repeat-output property of a keystream cipher). The risk we
are NOT trying to address is concurrent reseed — that race is
benign by construction.

## Wiring init into the boot sequence

In [kernel/core/main.c](kernel/core/main.c), after the sound
device probe and before the window manager comes up:

```c
serial_puts("probing virtio-mmio bus for an RNG ... ");
if (virtio_rng_init() == 0) serial_puts("ok (entropy online)\n");
else                        serial_puts("none (no hardware RNG)\n");
random_init();
```

This placement matters. `random_init` must run AFTER
`virtio_rng_init` (so `virtio_rng_present()` is accurate when
we check it), and it must run BEFORE anything that calls
`random_bytes` (which today is just userspace, but next chapter
will include the TLS library at module-init time).

Per the kernel-thread-lifetime trap recorded in user memory,
this is all on the main thread before `preemption_demo`'s
reaper loop, so there's no danger of leaving a child unwaitable.

## The `SYS_GETRANDOM` syscall

Syscall 94 — slot picked because the previous highest was 93
(`SYS_WIN_FB_RESIZE`). See [kernel/core/syscall.h](kernel/core/syscall.h):

```c
SYS_GETRANDOM       = 94,  /* fill buf[0..len) with crypto bytes;
                              flags must be 0 (reserved); blocking;
                              never partial-fills on success */
```

The kernel implementation is intentionally boring. It validates
the arguments, then loops a small kernel scratch buffer through
`copy_to_user`:

```c
#define GETRAND_MAX     (1u * 1024u * 1024u)
#define GETRAND_CHUNK   256u

static long sys_getrandom(uintptr_t buf, size_t len, unsigned flags)
{
    if (flags != 0)        return -EINVAL_VFS;
    if (len == 0)          return 0;
    if (len > GETRAND_MAX) return -EINVAL_VFS;
    if (!buf)              return -EFAULT;

    uint8_t chunk[GETRAND_CHUNK];
    size_t  total = 0;
    while (total < len) {
        size_t want = len - total;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        long got = random_bytes(chunk, want);
        if (got <= 0) return total > 0 ? (long)total : -EIO;
        if (copy_to_user(buf + total, chunk, (size_t)got) < 0)
            return -EFAULT;
        total += (size_t)got;
    }
    return (long)total;
}
```

Design choices, in order of importance:

- **`flags != 0` is reserved**. Linux defines `GRND_NONBLOCK`
  and `GRND_RANDOM`; we don't need them today (our entropy
  source is essentially infinite under QEMU's `/dev/urandom`),
  but rejecting non-zero flags now means we can add them later
  without breaking ABI.
- **`GETRAND_MAX` of 1 MiB**. Caps the amount of kernel-side
  work a single syscall can demand. Linux caps at 33 MiB; we
  pick smaller because we have less.
- **256-byte chunks**. Big enough that the per-iteration
  overhead is irrelevant; small enough that a 1 MiB request
  doesn't keep a 1 MiB kernel-scratch alive on the stack of
  whatever thread happened to call us.
- **`-EIO` if the CSPRNG ever returns ≤ 0**. Only possible if
  `random_init` never ran or the kernel ran out of memory at
  init — both are boot-time failures the user can't recover
  from at runtime, but failing the syscall is better than
  silently giving the caller zeroes.

Dispatch sits in the big `do_syscall` switch:

```c
case SYS_GETRANDOM:
    ret = sys_getrandom((uintptr_t)a0, (size_t)a1, (unsigned)a2);
    break;
```

## The libc wrapper

[userspace/libc/syscall.h](userspace/libc/syscall.h) gets one
new entry in the syscall-number enum and one new wrapper:

```c
SYS_GETRANDOM = 94,

static inline long getrandom(void *buf, unsigned long len,
                             unsigned int flags)
{ return _svc3(SYS_GETRANDOM, (long)(uintptr_t)buf,
               (long)len, (long)flags); }
```

That's it. The same shape as `getuid`, `kill`, every other
3-arg syscall.

## `/bin/getrand`

The userspace surface is [userspace/getrand/getrand.c](userspace/getrand/getrand.c).
It exists for two reasons: to make the entropy stack
demonstrable from the shell, and to give the regression test
something to assert on.

```c
int main(int argc, char **argv)
{
    unsigned int n = (argc >= 2) ? parse_uint(argv[1], 16u) : 16u;
    if (n > MAX_BYTES) n = MAX_BYTES;        /* MAX_BYTES = 1024 */

    unsigned char buf[MAX_BYTES];
    long got = getrandom(buf, n, 0u);
    if (got < 0 || (unsigned long)got != n) { /* error path */ }

    char out[MAX_BYTES * 2u + 2u];
    for (unsigned int i = 0; i < n; i++) {
        out[i*2 + 0] = hex_char((buf[i] >> 4) & 0xF);
        out[i*2 + 1] = hex_char( buf[i]       & 0xF);
    }
    out[n*2] = '\n';
    write(1, out, n*2 + 1);
    return 0;
}
```

Two details worth mentioning:

- We accumulate the entire hex string in a stack buffer and
  emit it with a single `write`. The earlier draft did a
  `printf("%02x", buf[i])` per byte, which under load
  interleaved with kernel serial output and produced lines
  like `461a82` where `[virtio-snd] ...` had cut in between
  two hex digits. The atomic `write` keeps the hex line on
  one row.

- The MAX_BYTES of 1024 is below the syscall's 1 MiB cap; it's
  the upper bound on what makes sense to dump as hex at the
  shell. Anyone needing more would write a different tool.

## Makefile glue

A new userspace target follows the existing per-tool pattern:

```make
GETRAND_OBJS     = $(OUT)/userspace/crt0.o \
                   $(OUT)/userspace/getrand/getrand.o
GETRAND_ELF      = $(OUT)/userspace/getrand/getrand.elf
GETRAND_STRIPPED = $(OUT)/userspace/getrand/getrand.stripped
```

with the usual link, strip, and embed-in-OSFS rules. The
kernel-side additions are two new `.c` files in `C_SRCS`:

```make
kernel/core/random.c
kernel/device/virtio_rng.c
```

And a new QEMU device line, hoisted into a variable so every
`run` target can use it:

```make
QEMU_RNG ?= -object rng-random,id=rng0,filename=/dev/urandom \
            -device virtio-rng-device,rng=rng0
```

`run`, `run-graphical`, and `run-tcg` each append `$(QEMU_RNG)`.

## The regression test

[scripts/test_getrand.py](scripts/test_getrand.py) boots a
fresh kernel with the RNG attached, waits for `/$`, runs
`getrand 16` twice, and asserts:

1. The boot log contains `[virtio-rng] online` and
   `[random] CSPRNG seeded from virtio-rng`.
2. Each invocation prints exactly 32 lowercase hex chars.
3. The two invocations produce *different* outputs.

That last assertion is the heart of the test. If the CSPRNG
ever returned the same keystream block twice — because the
counter wasn't being incremented, because the key was a
constant, because the lock was being skipped — the comparison
would fail. The current implementation passes it on the first
boot, and continues to pass when the test is looped.

The harness pattern (UNIX-domain serial socket + retry connect
+ `read_until`) is the same one introduced in chapter 96 for
`test_beep.py`; the only difference is the QEMU device flags
and the assertions.

## What's still missing for actual TLS

The bytes are real; everything else is still to come. Before
the browser can speak `https://` to a remote server we need,
in order:

- **Chapter 113**: build BearSSL out-of-tree, package it as
  `libbearssl.a`, wire it into the userspace link line. BearSSL
  is freestanding-clean (no malloc, no FILE*), but it does
  want a few I/O callbacks and a PRNG callback — that PRNG
  callback is `getrandom`.
- **Chapter 114**: design `tls_socket_*` as a layer over
  `tcp_socket_*` and run the BearSSL handshake to completion
  against a test server.
- **Chapter 115**: ship a small built-in root certificate
  store and the chain-validation glue. We can't trust a TLS
  connection we can't authenticate.
- **Chapter 116**: teach the browser's URL parser, fetcher,
  and cache about `https://`. Remove `scripts/https_proxy.py`
  from the boot path; it lives on only as a tool for inspecting
  TLS traffic during debugging.
- **Chapter 117**: end-to-end test against a public HTTPS site.

Each of those builds directly on the entropy we shipped here.
Without `SYS_GETRANDOM` and a CSPRNG behind it, BearSSL refuses
to run.

## Applied to

- **Kernel**: new files
  [kernel/device/virtio_rng.c](kernel/device/virtio_rng.c),
  [kernel/device/virtio_rng.h](kernel/device/virtio_rng.h),
  [kernel/core/random.c](kernel/core/random.c),
  [kernel/core/random.h](kernel/core/random.h).
  [kernel/core/main.c](kernel/core/main.c) now probes the
  device and seeds the CSPRNG before WM init.
  [kernel/core/syscall.h](kernel/core/syscall.h) gains
  `SYS_GETRANDOM = 94`; [kernel/core/syscall.c](kernel/core/syscall.c)
  gains its implementation and dispatch case.
- **Userspace**: new tool [userspace/getrand/getrand.c](userspace/getrand/getrand.c).
  [userspace/libc/syscall.h](userspace/libc/syscall.h) gains
  the `getrandom` wrapper.
- **Build**: [Makefile](Makefile) gains the two new C files,
  the `GETRAND_*` target set, the `$(QEMU_RNG)` device block
  appended to every run target, and the OSFS embed line.
- **Test surface**: new regression
  [scripts/test_getrand.py](scripts/test_getrand.py).
  Kitchen-sink boot configs in
  [scripts/test_browser_forms.py](scripts/test_browser_forms.py),
  [scripts/test_browser_intrinsic_size.py](scripts/test_browser_intrinsic_size.py),
  [scripts/test_browser_direct_png.py](scripts/test_browser_direct_png.py),
  [scripts/test_browser_image.py](scripts/test_browser_image.py),
  [scripts/test_png.py](scripts/test_png.py), and
  [scripts/test_png_palette.py](scripts/test_png_palette.py)
  now pass the virtio-rng device through so their kernel logs
  match the production path.

## What this unlocks

- Any future kernel or userspace code that needs unguessable
  bytes can call `random_bytes` or `getrandom` — session
  cookies, request IDs, ASLR offsets (when we add them), salts
  for password hashing, IVs.
- The next five chapters' port of BearSSL has a working PRNG
  callback to plug into the handshake.
- The `[random] CSPRNG seeded from virtio-rng (strong)` line at
  boot is a guarantee TLS code can act on: if it isn't there,
  `random_is_strong()` returns 0 and the eventual TLS stack
  will refuse to come up.
