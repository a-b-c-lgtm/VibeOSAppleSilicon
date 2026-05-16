/*
 * kernel/device/virtio_net.h — milestone-52 virtio-net driver.
 *
 * Modern (v2) virtio-mmio NIC driver.  QEMU's `virt` machine
 * with `-device virtio-net-device,netdev=net0` exposes one of
 * these on a free virtio-mmio slot; we probe slots 0..31 looking
 * for device id 1 (NET).
 *
 * Two virtqueues:
 *   queue 0 — receiveq: device fills these descriptors with
 *             incoming Ethernet frames (preceded by a 12-byte
 *             virtio_net_hdr).
 *   queue 1 — transmitq: driver fills these with outgoing
 *             frames (also preceded by a 12-byte zeroed
 *             virtio_net_hdr).
 *
 * Buffer model: ONE descriptor per buffer.  Each buffer is a
 * single contiguous 1536-byte slab containing:
 *   [ 0..11   ]  virtio_net_hdr  (always 12 bytes under
 *                                 VIRTIO_F_VERSION_1)
 *   [12..1525]  Ethernet frame  (up to 1514 bytes)
 *   [1526..1535] slack
 *
 * No descriptor chaining, no MRG_RXBUF, no checksum offload, no
 * GSO.  Feature negotiation accepts only VERSION_1 plus
 * VIRTIO_NET_F_MAC (so the device-config MAC is authoritative)
 * and VIRTIO_NET_F_STATUS (so we can read link state if we ever
 * want to).
 *
 * Polling, not IRQ-driven (consistent with virtio_blk and
 * virtio_input).  Call virtio_net_drain_rx() to harvest any
 * frames the device has handed back to the used ring; the
 * registered RX callback fires synchronously inside that call,
 * one invocation per frame.
 */
#ifndef KERNEL_DEVICE_VIRTIO_NET_H
#define KERNEL_DEVICE_VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>

#define VIRTIO_NET_MAC_LEN          6
/* Maximum L2 (Ethernet) frame size we'll send or accept.  No
 * jumbo frames; no VLAN tags accounted for separately (a tag
 * fits inside the 1514-byte budget). */
#define VIRTIO_NET_FRAME_MAX        1514

/* Bring up the device.  Probes virtio-mmio slots, runs the
 * modern handshake, allocates rings and buffer slabs, and pre-
 * fills the RX queue so the device has somewhere to write
 * incoming frames as soon as the link comes up.
 *
 * Returns 0 on success, -1 if no virtio-net device is present
 * or initialisation failed for any other reason.  Safe to call
 * from any context, but only once. */
int  virtio_net_init(void);

/* Non-zero if virtio_net_init() previously succeeded. */
int  virtio_net_present(void);

/* Copy the device's MAC address (6 bytes) into `out`.  Caller
 * must provide a buffer of at least VIRTIO_NET_MAC_LEN bytes.
 * No-op if the device is not present. */
void virtio_net_get_mac(uint8_t out[VIRTIO_NET_MAC_LEN]);

/* Transmit one Ethernet frame.  `frame` points to the L2 header
 * (destination MAC first), `len` is the frame length excluding
 * the FCS (the device synthesizes the FCS).  Returns 0 on
 * success, -1 if the device is absent / no TX descriptors are
 * available / `len` is out of range.
 *
 * Synchronous in the sense that the buffer is copied into our
 * own descriptor backing store before this function returns —
 * the caller's `frame` pointer does not need to remain valid
 * after the call. */
int  virtio_net_tx(const uint8_t *frame, uint32_t len);

/* RX callback signature.  Called once per harvested frame.  The
 * `frame` pointer is a kernel pointer into our descriptor
 * backing store; it is valid only for the duration of the
 * callback.  `len` is the L2 frame length (the 12-byte virtio
 * header has already been stripped). */
typedef void (*virtio_net_rx_cb)(const uint8_t *frame, uint32_t len);

/* Install or replace the RX callback.  NULL disables delivery
 * (drain still runs to recycle descriptors). */
void virtio_net_set_rx_callback(virtio_net_rx_cb cb);

/* Drain the RX used ring synchronously.  For each used entry,
 * invoke the registered callback (if any), then put the buffer
 * back on the avail ring.  Returns the number of frames
 * processed. */
int  virtio_net_drain_rx(void);

#endif /* KERNEL_DEVICE_VIRTIO_NET_H */
