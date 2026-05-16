/*
 * kernel/device/virtio_mmio.h — virtio-mmio register layout (v2/modern).
 *
 * Reference: virtio v1.2 spec, sections 4.2 (mmio transport) and 5.2
 * (block device).  QEMU's `virt` machine puts the virtio-mmio bus at
 * 0x0a000000 with up to 32 slots, each occupying 0x200 bytes, IRQs
 * SPI 16+N for slot N.
 */
#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <stdint.h>

#define VIRTIO_MMIO_BASE        0x0A000000UL
#define VIRTIO_MMIO_STRIDE      0x200UL
#define VIRTIO_MMIO_SLOTS       32

/* Common header (all device types) */
#define VIRTIO_MMIO_MAGIC_VALUE       0x000   /* R: 0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION           0x004   /* R: 2 = modern         */
#define VIRTIO_MMIO_DEVICE_ID         0x008   /* R: 2 = blk            */
#define VIRTIO_MMIO_VENDOR_ID         0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES   0x010   /* R                     */
#define VIRTIO_MMIO_DEVICE_FEAT_SEL   0x014   /* W                     */
#define VIRTIO_MMIO_DRIVER_FEATURES   0x020   /* W                     */
#define VIRTIO_MMIO_DRIVER_FEAT_SEL   0x024   /* W                     */
#define VIRTIO_MMIO_QUEUE_SEL         0x030   /* W                     */
#define VIRTIO_MMIO_QUEUE_NUM_MAX     0x034   /* R                     */
#define VIRTIO_MMIO_QUEUE_NUM         0x038   /* W                     */
#define VIRTIO_MMIO_QUEUE_READY       0x044   /* RW                    */
#define VIRTIO_MMIO_QUEUE_NOTIFY      0x050   /* W                     */
#define VIRTIO_MMIO_INTERRUPT_STATUS  0x060   /* R                     */
#define VIRTIO_MMIO_INTERRUPT_ACK     0x064   /* W                     */
#define VIRTIO_MMIO_STATUS            0x070   /* RW                    */
#define VIRTIO_MMIO_QUEUE_DESC_LO     0x080   /* W                     */
#define VIRTIO_MMIO_QUEUE_DESC_HI     0x084   /* W                     */
#define VIRTIO_MMIO_QUEUE_DRIVER_LO   0x090   /* W: avail ring         */
#define VIRTIO_MMIO_QUEUE_DRIVER_HI   0x094   /* W                     */
#define VIRTIO_MMIO_QUEUE_DEVICE_LO   0x0A0   /* W: used ring          */
#define VIRTIO_MMIO_QUEUE_DEVICE_HI   0x0A4   /* W                     */
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0FC   /* R                     */
#define VIRTIO_MMIO_CONFIG            0x100   /* device-specific       */

/* Status bits (Status register) */
#define VIRTIO_STATUS_ACKNOWLEDGE     1u
#define VIRTIO_STATUS_DRIVER          2u
#define VIRTIO_STATUS_DRIVER_OK       4u
#define VIRTIO_STATUS_FEATURES_OK     8u
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64u
#define VIRTIO_STATUS_FAILED          128u

/* Device IDs we care about */
#define VIRTIO_DEVICE_ID_NET          1u
#define VIRTIO_DEVICE_ID_BLOCK        2u
#define VIRTIO_DEVICE_ID_GPU          16u
#define VIRTIO_DEVICE_ID_INPUT        18u

/* Feature bits we (might) negotiate */
#define VIRTIO_F_VERSION_1            (1ULL << 32)

/* Descriptor flags (split queue) */
#define VRING_DESC_F_NEXT             1u
#define VRING_DESC_F_WRITE            2u   /* device-writable */
#define VRING_DESC_F_INDIRECT         4u

/* Used ring flags */
#define VRING_USED_F_NO_NOTIFY        1u
/* Avail ring flags */
#define VRING_AVAIL_F_NO_INTERRUPT    1u

/* Split-queue structures (legacy / modern w/o packed). */
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];          /* size = QueueNum; followed by used_event */
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[]; /* size = QueueNum; followed by avail_event */
} __attribute__((packed));

#endif /* VIRTIO_MMIO_H */
