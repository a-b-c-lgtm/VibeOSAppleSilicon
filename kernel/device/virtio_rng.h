/*
 * kernel/device/virtio_rng.h — chapter 123 entropy source.
 *
 * Spec reference: virtio v1.2 §5.4 (entropy device, device id 4).
 * On the QEMU `virt` board we attach
 *
 *   -object rng-random,id=rng0,filename=/dev/urandom
 *   -device virtio-rng-device,rng=rng0
 *
 * which gives the guest a single virtqueue: descriptors point
 * at device-writable buffers, the host fills them with random
 * bytes drawn from the host kernel's CSPRNG.
 *
 * Why a hardware-style device for randomness?
 *
 *   TLS — which the next few chapters wire up — derives its
 *   keys, nonces, and IVs from the client.  A single predictable
 *   nonce in AES-GCM exposes the underlying plaintext; a guessable
 *   key is total game over.  Without a real entropy source we'd
 *   be writing security theatre.  Asking the host kernel for the
 *   bytes is the most honest answer available to us: the host
 *   /dev/urandom is itself a properly-seeded CSPRNG, which is
 *   what every "real" OS does when it doesn't have its own
 *   thermal-noise / jitter / interrupt-pool entropy collection
 *   built out yet.
 *
 * The public surface is intentionally tiny: a blocking get that
 * fills the caller's buffer with N device-supplied bytes.  All
 * the actual stretching / CSPRNG state lives in kernel/core/random.c,
 * which uses virtio_rng_get as its seed (and re-seed) source.
 */
#ifndef KERNEL_VIRTIO_RNG_H
#define KERNEL_VIRTIO_RNG_H

#include <stdint.h>
#include <stddef.h>

/* One-time probe.  Walks the virtio-mmio bus for device id 4
 * (entropy source), runs the v1 handshake, sets up a single
 * virtqueue.  Returns 0 on success, -1 if no device was found
 * or the handshake failed.
 *
 * Safe to call when no `-device virtio-rng-device` was passed
 * to QEMU; the caller should fall back to a less-trustworthy
 * software seed (see kernel/core/random.c). */
int virtio_rng_init(void);

/* True after virtio_rng_init found a device. */
int virtio_rng_present(void);

/* Block until `len` random bytes have been written into `out`.
 * The implementation may issue more than one device request
 * (the bounce buffer is one page; larger requests are chunked).
 *
 * Returns the number of bytes written (== len on success), or
 * a negative errno on failure.  Not safe to call before
 * virtio_rng_init() returned 0. */
long virtio_rng_get(void *out, size_t len);

#endif /* KERNEL_VIRTIO_RNG_H */
