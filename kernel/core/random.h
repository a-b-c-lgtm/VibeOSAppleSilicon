/*
 * kernel/core/random.h — kernel CSPRNG, chapter 123.
 *
 * Public API for getting random bytes inside the kernel.  Backed
 * by virtio-rng when present (driver in kernel/device/virtio_rng.c);
 * falls back to a ChaCha20 keystream seeded from CNTVCT_EL0 +
 * stack addresses when the device is absent — useful enough to
 * keep the boot sequence alive in test harnesses that don't pass
 * `-device virtio-rng-device`, but NEVER trust those bytes with a
 * TLS handshake.  random_is_strong() distinguishes the two.
 *
 * Called by:
 *   - sys_getrandom() (syscall 94, exposed to userspace by
 *     userspace/libc/syscall.h::getrandom)
 *   - future TLS code in libtls (chapter 140+)
 *
 * Thread-safety: a single mutex serialises CSPRNG state updates;
 * concurrent callers see consistent, non-overlapping output.
 */
#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <stdint.h>
#include <stddef.h>

/* Probe virtio-rng, draw an initial 32-byte ChaCha20 key from it
 * (or from a CNTVCT-based fallback seed), and mark the CSPRNG as
 * initialised.  Idempotent; call once during boot. */
void random_init(void);

/* True iff the CSPRNG is seeded from a real entropy device
 * (virtio-rng).  False if we're stretching a weak seed; the
 * caller may want to refuse to perform a TLS handshake or to
 * print a security warning.  Undefined before random_init(). */
int random_is_strong(void);

/* Fill `out` with `len` cryptographically-random bytes.  Never
 * blocks longer than a single virtio-rng request, never returns
 * fewer than `len` bytes unless the underlying device fails;
 * returns the number of bytes written.  Safe to call from any
 * thread after random_init(). */
long random_bytes(void *out, size_t len);

#endif /* KERNEL_RANDOM_H */
