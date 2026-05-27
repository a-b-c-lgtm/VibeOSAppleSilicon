/*
 * kernel/core/random.c — kernel CSPRNG, chapter 123.
 *
 * Two-layer design:
 *
 *   1. Seed source: virtio-rng (virtio device id 4) when present.
 *      Each seed-pull asks the host for 32 fresh bytes.  When the
 *      device is missing we fall back to CNTVCT_EL0 plus a couple
 *      of stack/heap addresses — enough to keep the boot path
 *      alive in test harnesses that don't add the device, but NOT
 *      cryptographically useful.  random_is_strong() returns 0 in
 *      that case so TLS code (chapter 140+) can refuse to start.
 *
 *   2. Stretching: ChaCha20 keystream.  We keep a 256-bit key, a
 *      96-bit nonce (zeroed; the counter discriminates blocks
 *      within a key generation), and a 32-bit block counter.
 *      Output is the keystream itself (block_i = ChaCha20(key,
 *      nonce, counter)); we reseed the key from virtio-rng every
 *      RESEED_BYTES bytes so that an attacker who somehow recovers
 *      the current state can only predict the next chunk, not the
 *      whole session.
 *
 * Locking: a single plain spinlock_t serialises state updates.
 * `random_bytes` runs in thread context only (it may yield while
 * waiting on virtio-rng during reseed) — never call it from an
 * IRQ handler.  We drop the lock around virtio_rng_get so the
 * yield inside the driver can't deadlock other threads waiting
 * for the CSPRNG.
 */

#include "random.h"
#include "serial.h"
#include "thread.h"
#include "timer.h"
#include "../arch/spinlock.h"
#include "../device/virtio_rng.h"

#include <stdint.h>
#include <stddef.h>

/* Re-seed after this many output bytes.  256 KiB is a Linux-ish
 * default; we never expect a single boot to use more than a few
 * MiB of randomness so this is a non-issue for perf. */
#define RESEED_BYTES   (256u * 1024u)

/* ---- ChaCha20 (RFC 7539) keystream-only ---- */

static inline uint32_t rotl32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d) \
    do { \
        a += b; d ^= a; d = rotl32(d, 16); \
        c += d; b ^= c; b = rotl32(b, 12); \
        a += b; d ^= a; d = rotl32(d,  8); \
        c += d; b ^= c; b = rotl32(b,  7); \
    } while (0)

static void chacha20_block(const uint32_t key[8],
                           const uint32_t nonce[3],
                           uint32_t counter,
                           uint8_t out[64])
{
    /* Constants "expand 32-byte k" little-endian. */
    uint32_t s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu;
    s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) s[4 + i]  = key[i];
    s[12] = counter;
    s[13] = nonce[0]; s[14] = nonce[1]; s[15] = nonce[2];

    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int round = 0; round < 10; round++) {
        /* Column rounds */
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        /* Diagonal rounds */
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; i++) x[i] += s[i];
    for (int i = 0; i < 16; i++) {
        out[i * 4 + 0] = (uint8_t)( x[i]        & 0xFF);
        out[i * 4 + 1] = (uint8_t)((x[i] >>  8) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((x[i] >> 16) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((x[i] >> 24) & 0xFF);
    }
}

/* ---- State ---- */

static int       g_ready    = 0;
static int       g_strong   = 0;     /* 1 iff seeded from virtio-rng */
static uint32_t  g_key[8];           /* 256-bit ChaCha20 key         */
static uint32_t  g_nonce[3] = {0,0,0};
static uint32_t  g_counter  = 0;
static uint32_t  g_since_reseed = 0;

static spinlock_t g_lock = SPINLOCK_INIT;

static inline uint64_t cntvct_el0_read(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* Fold a 32-byte buffer into the ChaCha20 key (caller holds the
 * lock).  XOR so that an already-strong key can only get stronger
 * (or stay the same) on reseed; never replaces secret with
 * predictable. */
static void mix_into_key_locked(const uint8_t seed[32])
{
    for (int i = 0; i < 8; i++) {
        uint32_t w = ((uint32_t)seed[i * 4 + 0]      ) |
                     ((uint32_t)seed[i * 4 + 1] <<  8) |
                     ((uint32_t)seed[i * 4 + 2] << 16) |
                     ((uint32_t)seed[i * 4 + 3] << 24);
        g_key[i] ^= w;
    }
    g_counter = 0;
    g_since_reseed = 0;
}

/* Build a 32-byte fallback seed from CNTVCT_EL0 plus a few
 * address-derived values.  Not secret in any meaningful sense
 * (an attacker who can read /proc/uptime or the kernel ELF can
 * predict it), but at least it varies boot-to-boot so a test
 * harness that loops getrandom doesn't get all zeros. */
static void fallback_seed(uint8_t out[32])
{
    uint64_t t = cntvct_el0_read();
    uint64_t a = (uint64_t)(uintptr_t)&t;
    uint64_t b = (uint64_t)(uintptr_t)&fallback_seed;
    uint64_t c = (uint64_t)(uintptr_t)g_key;
    uint64_t d = timer_ticks();
    for (int i = 0; i < 8; i++) out[i +  0] = (uint8_t)(t >> (i * 8));
    for (int i = 0; i < 8; i++) out[i +  8] = (uint8_t)(a >> (i * 8));
    for (int i = 0; i < 8; i++) out[i + 16] = (uint8_t)(b >> (i * 8));
    for (int i = 0; i < 4; i++) out[i + 24] = (uint8_t)(c >> (i * 8));
    for (int i = 0; i < 4; i++) out[i + 28] = (uint8_t)(d >> (i * 8));
}

/* Fetch 32 fresh bytes of seed, blocking on virtio-rng if present.
 * Runs WITHOUT g_lock held so virtio_rng_get's yield()s don't
 * stall other CPUs trying to acquire the CSPRNG. */
static void fetch_seed(uint8_t out[32])
{
    if (virtio_rng_present()) {
        long n = virtio_rng_get(out, 32);
        if (n == 32) return;
        serial_puts("[random] virtio-rng request failed, using fallback\n");
    }
    fallback_seed(out);
}

void random_init(void)
{
    if (g_ready) return;
    /* virtio_rng_init has already been called by main(); we just
     * check whether it succeeded. */
    g_strong = virtio_rng_present();

    /* Start from an all-zero key, then fold in the first seed.
     * mix_into_key is XOR so this gives us key = seed directly. */
    uint8_t seed[32];
    fetch_seed(seed);
    spin_lock(&g_lock);
    for (int i = 0; i < 8; i++) g_key[i] = 0;
    mix_into_key_locked(seed);
    g_ready = 1;
    spin_unlock(&g_lock);

    if (g_strong)
        serial_puts("[random] CSPRNG seeded from virtio-rng (strong)\n");
    else
        serial_puts("[random] WARNING: no virtio-rng device — "
                    "CSPRNG seeded from CNTVCT (NOT strong, do NOT use "
                    "for TLS)\n");
}

int random_is_strong(void) { return g_strong; }

long random_bytes(void *out, size_t len)
{
    if (!g_ready) return -1;
    if (!out && len) return -1;
    if (len == 0) return 0;

    uint8_t *dst = (uint8_t *)out;
    size_t   got = 0;

    while (got < len) {
        /* Reseed check is intentionally racy: if two threads see
         * the threshold simultaneously they each pull 32 bytes
         * from virtio-rng and both mix into the key.  Extra
         * entropy never hurts. */
        if (g_since_reseed >= RESEED_BYTES) {
            uint8_t seed[32];
            fetch_seed(seed);
            spin_lock(&g_lock);
            mix_into_key_locked(seed);
            spin_unlock(&g_lock);
        }

        uint8_t blk[64];
        spin_lock(&g_lock);
        chacha20_block(g_key, g_nonce, g_counter, blk);
        g_counter++;
        g_since_reseed += 64;
        spin_unlock(&g_lock);

        size_t take = len - got;
        if (take > 64) take = 64;
        for (size_t i = 0; i < take; i++) dst[got + i] = blk[i];
        got += take;
    }
    return (long)got;
}
