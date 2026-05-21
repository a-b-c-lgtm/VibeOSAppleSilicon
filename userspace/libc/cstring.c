/* userspace/libc/cstring.c — chapter 112a.
 *
 * Tiny extern-symbol implementations of the libc functions that
 * BearSSL's archive references but our freestanding userspace
 * has historically avoided needing.  Previous binaries got away
 * with the static `memcpy`/`memset`/`memmove` shims in
 * libc/freestanding.h because every translation unit that needed
 * them included that header.  An external .a archive can't —
 * BearSSL's .o files were compiled without our headers, so the
 * link line for any binary that pulls libbearssl.a needs an
 * EXTERN definition somewhere.
 *
 * Five functions cover everything BearSSL uses out of <string.h>
 * (verified by `grep -rho 'mem[a-z]*\|str[a-z]*' vendor/bearssl/`);
 * `time(NULL)` is called in exactly one place
 * (`vendor/bearssl/src/x509/x509_minimal.c`).  We stub `time` to
 * return 0 here so the link succeeds even when something pulls
 * x509_minimal in.  Real cert-expiry validation will set the
 * reference time explicitly via `br_x509_minimal_set_time`
 * (chapter 112c).
 *
 * NOTE: these are NOT constant-time and NOT optimised.  They are
 * the slowest-possible byte-at-a-time implementations, kept tiny
 * because BearSSL's own constant-time primitives don't rely on
 * any property of these stubs other than correctness.  If
 * profiling ever shows AES-GCM bottlenecked on memcpy we can
 * replace them with word-at-a-time versions.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* time() stub.  BearSSL's x509_minimal calls time(NULL) only as a
 * default when no validation-time has been pre-set.  Returning 0
 * means "epoch", which makes every certificate look not-yet-valid;
 * callers that actually want validation MUST call
 * br_x509_minimal_set_time(ctx, days_since_epoch, seconds_in_day)
 * before starting a handshake.  Chapter 112c provides a helper
 * built on top of SYS_GETTIMEOFDAY. */
typedef long time_t;
time_t time(time_t *out)
{
    if (out) *out = 0;
    return 0;
}

/* chapter 112b: br_prng_seeder_system() stub.
 *
 * BearSSL's ssl_engine.c calls this from br_ssl_engine_init_rand
 * to find an OS-provided entropy source.  The real implementation
 * lives in vendor/bearssl/src/rand/sysrng.c -- which we deliberately
 * exclude from the build (see Makefile BEARSSL_SRCS) because it
 * probes /dev/urandom, getentropy, and CryptGenRandom, none of
 * which exist in our freestanding userspace.
 *
 * Returning NULL ("no seeder available") is the documented BearSSL
 * contract for "the caller MUST call br_ssl_engine_inject_entropy
 * before the first reset()".  tls_socket.c and httpsd.c both do
 * exactly that, pulling 64 bytes from SYS_GETRANDOM (chapter 112's
 * kernel CSPRNG, seeded from /dev/urandom on the host via the
 * virtio-rng device in the QEMU command line).
 *
 * The signature must match exactly -- br_prng_seeder is a function
 * pointer typedef'd in bearssl_rand.h.  We declare its return type
 * locally as a void(*)() to avoid pulling bearssl_rand.h into
 * cstring.c; the linker only cares about the symbol name. */
typedef int (*br_prng_seeder_fn)(void **ctx);  /* approximate */
br_prng_seeder_fn br_prng_seeder_system(const char **name)
{
    if (name) *name = "none";
    return 0;
}
