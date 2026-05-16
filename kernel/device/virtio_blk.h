/*
 * kernel/device/virtio_blk.h — minimal virtio-blk driver public API.
 *
 * Each driver instance maintains one virtqueue (queue 0) of fixed
 * depth 8 and exposes synchronous, polled, single-sector
 * read/write primitives.  Suitable for milestone-11 bring-up; a
 * real driver would batch requests, use IRQ completions, and
 * support multiple queues.
 *
 * Multi-device support (chapter 81): the driver probes the
 * virtio-mmio bus for ALL block devices (up to VIRTIO_BLK_MAX_DEVS)
 * and gives each one a stable index 0..N-1 in probe order.  The
 * single-arg API (`virtio_blk_read(sector, buf)`) is a backward-
 * compatible shim that targets device 0; the device-aware API
 * (`virtio_blk_dev_read(dev, sector, buf)`) lets OSFS-2 reach
 * device 1.  QEMU presents `-drive id=hd0` first and `-drive
 * id=hd1` second, matching that ordering.
 */
#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>

#define VIRTIO_BLK_SECTOR_SIZE   512u
#define VIRTIO_BLK_MAX_DEVS      2

/* Probe the virtio-mmio bus and bring up every virtio-blk
 * device found, in slot order, up to VIRTIO_BLK_MAX_DEVS.
 * Returns the count of devices successfully initialised
 * (0..VIRTIO_BLK_MAX_DEVS).  Device 0 is the boot OSFS-1 disk;
 * device 1 (if present) is the OSFS-2 data disk. */
int virtio_blk_init(void);

/* True after virtio_blk_init found at least one device.  Shim
 * for the single-device API. */
int  virtio_blk_present(void);

/* Total sector count reported by device 0. */
uint64_t virtio_blk_capacity(void);

/* Synchronous polled read of one 512-byte sector at LBA `sector`
 * from device 0 into `buf`.  Returns 0 on success, -1 on device
 * error. */
int virtio_blk_read(uint64_t sector, void *buf);

/* Synchronous polled write of one 512-byte sector to device 0. */
int virtio_blk_write(uint64_t sector, const void *buf);

/* Multi-device API.  `dev` is 0..VIRTIO_BLK_MAX_DEVS-1; out-of-
 * range or absent devices return -1. */
int      virtio_blk_count(void);
int      virtio_blk_dev_present(int dev);
uint64_t virtio_blk_dev_capacity(int dev);
int      virtio_blk_dev_read(int dev, uint64_t sector, void *buf);
int      virtio_blk_dev_write(int dev, uint64_t sector, const void *buf);

#endif /* VIRTIO_BLK_H */
