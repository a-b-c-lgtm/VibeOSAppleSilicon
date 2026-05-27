/*
 * kernel/device/virtio_gpu.h — virtio-gpu driver (2D).
 *
 * Smallest-possible path that puts pixels onto a real display through
 * QEMU's virtio-gpu emulation:
 *
 *   1. virtio_gpu_init() walks the virtio-mmio bus looking for device
 *      id 16, runs the modern feature-negotiation handshake, sets up
 *      the control queue, and asks the host for the geometry of
 *      scanout 0.
 *
 *   2. virtio_gpu_present(...) is called once per frame after the
 *      framebuffer module (kernel/device/fb.c) has written pixels
 *      into the contiguous RAM block that backs the resource.  It
 *      issues TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for the dirty
 *      rectangle, which causes QEMU to push the bytes to the host
 *      window.
 *
 * The cursor queue (virtqueue 1) is required to exist by the spec but
 * QEMU does not enforce it for the commands we issue.  We omit it.
 *
 * Design choices for the initial bring-up:
 *   - One scanout (output 0).  QEMU defaults to a single output unless
 *     `-device virtio-gpu-device,max_outputs=...` is set.
 *   - One resource (id = 1) sized to the scanout's reported geometry.
 *   - Pixel format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM (numeric value 2).
 *     In aarch64 little-endian memory, byte order is B, G, R, X — i.e.
 *     a uint32 store of 0xAARRGGBB lays down BB GG RR AA, which is
 *     exactly what the format expects.
 *   - Synchronous polled completion via the same dmb/dsb-and-spin
 *     pattern as virtio-blk.  Fine for a 60-FPS framebuffer where the
 *     guest is the only thread driving the device.
 */

#ifndef KERNEL_DEVICE_VIRTIO_GPU_H
#define KERNEL_DEVICE_VIRTIO_GPU_H

#include <stdint.h>

/* Pixel format constant from virtio-gpu spec section 5.7.6.4. */
#define VIRTIO_GPU_FMT_B8G8R8X8_UNORM   2u

/* Driver-owned resource id.  0 is reserved by the spec; we only ever
 * allocate one resource for the scanout. */
#define VIRTIO_GPU_FB_RESOURCE_ID       1u

/* Probe the bus and bring the GPU up through DRIVER_OK.  Returns 0
 * on success, negative on any failure (no device found, handshake
 * rejection, queue init failure, OOM).  After success the geometry
 * accessors below return non-zero values. */
int virtio_gpu_init(void);

/* 1 if the device exists and has been brought up successfully. */
int virtio_gpu_present(void);

/* Display geometry reported by GET_DISPLAY_INFO for scanout 0.
 * Both return 0 if the driver has not been initialised yet. */
uint32_t virtio_gpu_width(void);
uint32_t virtio_gpu_height(void);

/* Bind a guest-physical pixel buffer to the scanout.  `phys` must
 * point to a contiguous run of `length` bytes covering at least
 * width*height*4 pixels in B8G8R8X8 layout.  Issues, in order:
 *   - RESOURCE_CREATE_2D (resource id = VIRTIO_GPU_FB_RESOURCE_ID)
 *   - RESOURCE_ATTACH_BACKING (single mem-entry pointing at phys)
 *   - SET_SCANOUT (output 0)
 * Returns 0 on success.  Safe to call once after virtio_gpu_init. */
int virtio_gpu_set_framebuffer(uint64_t phys, uint32_t length,
                               uint32_t width, uint32_t height);

/* Push the rectangle [(x,y), (x+w, y+h)) of the framebuffer to the
 * host display.  Issues TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 * `offset` is the byte offset of the rectangle's top-left pixel
 * inside the backing buffer (callers usually pass 0 and let the
 * rectangle bounds carry the location).
 * Returns 0 on success. */
int virtio_gpu_flush_rect(uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h,
                          uint64_t offset);

#endif /* KERNEL_DEVICE_VIRTIO_GPU_H */
