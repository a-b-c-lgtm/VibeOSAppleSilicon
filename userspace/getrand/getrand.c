/* userspace/getrand/getrand.c — chapter 112: print N random bytes
 * as hex, one line per request.  Usage:
 *
 *   getrand              # 16 bytes -> 32 hex chars
 *   getrand <N>          # N bytes  -> 2*N hex chars (capped at 1024)
 *
 * Calls SYS_GETRANDOM, which the kernel services from
 * kernel/core/random.c (a ChaCha20 CSPRNG re-seeded from
 * virtio-rng).  Returns 0 on success, 1 on error.
 *
 * This is the test surface for chapter 112 — running it twice in
 * a row MUST produce two different outputs (the assertion enforced
 * by scripts/test_getrand.py).  When the kernel boots without the
 * virtio-rng device the warning printed by random_init still
 * applies — the bytes will differ run-to-run (because CNTVCT_EL0
 * varies), but the sequence is NOT cryptographically random and
 * should not be fed into chapter 114's TLS handshake.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

#define MAX_BYTES 1024u

static unsigned int parse_uint(const char *s, unsigned int dflt)
{
    if (!s || !s[0]) return dflt;
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
    }
    return v;
}

static char hex_char(unsigned int nib)
{
    return (char)((nib < 10) ? ('0' + nib) : ('a' + (nib - 10)));
}

int main(int argc, char **argv)
{
    unsigned int n = (argc >= 2) ? parse_uint(argv[1], 16u) : 16u;
    if (n == 0) {
        printf("\n");
        return 0;
    }
    if (n > MAX_BYTES) {
        printf("getrand: clamping %u down to %u bytes\n", n, MAX_BYTES);
        n = MAX_BYTES;
    }

    unsigned char buf[MAX_BYTES];
    long got = getrandom(buf, n, 0u);
    if (got < 0) {
        printf("getrand: kernel returned %d\n", (int)got);
        return 1;
    }
    if ((unsigned long)got != n) {
        printf("getrand: short read: wanted %u got %d\n", n, (int)got);
        return 1;
    }

    /* Print as one line of lowercase hex.  We accumulate into a
     * small stack buffer instead of calling printf("%02x") per
     * byte so the output reaches the serial port as a single
     * write, which keeps interleaved boot messages from cutting
     * the hex line in half. */
    char out[MAX_BYTES * 2u + 2u];
    for (unsigned int i = 0; i < n; i++) {
        out[i * 2u + 0u] = hex_char((buf[i] >> 4) & 0xFu);
        out[i * 2u + 1u] = hex_char( buf[i]       & 0xFu);
    }
    out[n * 2u + 0u] = '\n';
    out[n * 2u + 1u] = '\0';
    write(1, out, n * 2u + 1u);
    return 0;
}
