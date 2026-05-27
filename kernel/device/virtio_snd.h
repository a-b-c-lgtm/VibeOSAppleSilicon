/*
 * kernel/device/virtio_snd.h — chapter 97 virtio-snd driver.
 *
 * The narrowest possible audio output: ONE PCM stream, mono S16
 * at a fixed 44_100 Hz, blocking submission, no event-queue
 * handling.  Enough to play a boot chime and synthesise a square
 * wave for SYS_BEEP; nothing more.
 *
 * The driver owns three virtqueues out of virtio-snd's full
 * four:
 *
 *   #0  CONTROLQ — 8 slots, used for the request/response RPC
 *                  (jack/pcm/chmap info, set_params, prepare,
 *                   start, stop, release).
 *   #1  EVENTQ   — never set up.  We don't subscribe to async
 *                  jack-state events; the device tolerates an
 *                  unconfigured event queue (we just won't get
 *                  notifications).
 *   #2  TXQ      — 8 slots for outgoing PCM payload.  Each
 *                  message is a 2-descriptor chain: [xfer
 *                  header + samples] (device-readable) and
 *                  [pcm_status] (device-writable).
 *   #3  RXQ      — never set up.  No capture path.
 *
 * The PCM stream is configured, prepared, and started ONCE during
 * init with a 64 KiB device-side buffer.  Subsequent
 * `virtio_snd_play_square / virtio_snd_play_pcm` calls just
 * stream samples into the TXQ; we never STOP / RELEASE / re-
 * SET_PARAMS at runtime.
 *
 * Polling, not interrupts: the TXQ used ring is polled after
 * every submission with a brief `yield()` between iterations.
 * Audio playback durations measured in milliseconds make a
 * proper IRQ path overkill — and it keeps the driver layered
 * the same way as virtio_blk's request path.
 */

#ifndef VIRTIO_SND_H
#define VIRTIO_SND_H

#include <stdint.h>

/* Probe the virtio-mmio bus for a virtio-sound (device id 25)
 * device, run the legacy/v2 handshake, set up CONTROLQ + TXQ,
 * configure and start one mono S16/44_100 Hz output stream.
 *
 * Returns 0 on success or -1 on absent / failed init.  Safe to
 * call when no virtio-sound device is present (chapter 97 floor:
 * the driver is optional, tests that don't add the QEMU
 * argument won't see any of its log lines).
 */
int virtio_snd_init(void);

/* True iff virtio_snd_init succeeded. */
int virtio_snd_present(void);

/* Synthesise a square wave at `freq_hz` for `duration_ms` and
 * stream it through the running PCM stream.  Blocks until the
 * device has consumed every sample (which, for QEMU's virtio-
 * snd implementation, is approximately when the audio finishes
 * playing on the host).
 *
 * Bounds:
 *   freq_hz       :  20 .. 22_050   (clipped on either side)
 *   duration_ms   :   1 .. 5_000    (clipped to keep static
 *                                    buffers bounded)
 *
 * Returns 0 on success or -1 if the device is absent.  Does not
 * mix with concurrent calls — callers are expected to serialise
 * (today only sys_beep + the boot chime; no contention).
 */
int virtio_snd_play_square(uint32_t freq_hz, uint32_t duration_ms);

#endif /* VIRTIO_SND_H */
