/*
 * kernel/device/fb.c — framebuffer module.
 *
 * Owns the contiguous pixel buffer that backs the active virtio-gpu
 * scanout.  All drawing primitives are pure stores into normal
 * cacheable RAM; nothing is sent to the device until fb_present()
 * runs the TRANSFER_TO_HOST_2D + RESOURCE_FLUSH pair.
 *
 * Pixel format is fixed at B8G8R8X8 (== virtio-gpu's
 * VIRTIO_GPU_FMT_B8G8R8X8_UNORM).  In aarch64 little-endian memory,
 * a uint32 store of `(R<<16) | (G<<8) | B` lays down B G R 0 — which
 * is exactly the byte order the format expects.
 *
 * Caching: the framebuffer lives in normal-cacheable inner-shareable
 * RAM (the L1[2..N] block descriptors installed at boot).  QEMU's
 * virtio-gpu emulation reads guest physical memory in software when
 * processing TRANSFER_TO_HOST_2D, so as long as we issue a `dsb sy`
 * before submitting the command (the virtio_gpu submit path already
 * does an explicit `dmb sy` before publishing the avail index, and
 * the response polling forces another `dmb`), the host sees the
 * latest pixel writes.  No DC CVAC needed.
 */

#include "fb.h"
#include "virtio_gpu.h"
#include "../core/serial.h"
#include "../core/pmem.h"

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096u

static struct fb_info g_fb;
static int            g_fb_ready;

/* Pack an 8-bit-per-channel colour into the B8G8R8X8 pixel layout.
 * Inline so the compiler can specialise the per-pixel loops. */
static inline uint32_t pack_color(struct fb_color c)
{
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

static inline void put_pixel_unchecked(uint32_t x, uint32_t y, uint32_t pixel)
{
    uint32_t *row = (uint32_t *)(g_fb.base + (uint64_t)y * g_fb.pitch);
    row[x] = pixel;
}

int fb_init(void)
{
    g_fb_ready = 0;

    if (!virtio_gpu_present()) {
        serial_puts("[fb] virtio-gpu not present — graphics disabled\n");
        return -1;
    }

    uint32_t w = virtio_gpu_width();
    uint32_t h = virtio_gpu_height();
    if (w == 0 || h == 0) {
        serial_puts("[fb] degenerate scanout geometry\n");
        return -2;
    }

    /* width * height * 4 bytes per pixel, rounded up to a page. */
    uint64_t bytes = (uint64_t)w * h * 4ULL;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    /* Reserve a tiny safety margin (one extra page) so the device's
     * RESOURCE_ATTACH_BACKING length is always strictly inside the
     * allocated region even if we round geometry oddly later. */
    pages += 1;

    uint64_t phys = pmem_alloc_contig((size_t)pages);
    if (!phys) {
        serial_puts("[fb] FATAL — pmem_alloc_contig(");
        serial_puthex(pages);
        serial_puts(" pages) returned 0\n");
        return -3;
    }

    g_fb.base       = (uint8_t *)(uintptr_t)phys;
    g_fb.phys       = phys;
    g_fb.width      = w;
    g_fb.height     = h;
    g_fb.pitch      = w * 4u;
    g_fb.size_bytes = (uint32_t)(pages * PAGE_SIZE);

    serial_puts("[fb] backing = ");
    serial_puthex(phys);
    serial_puts(" .. ");
    serial_puthex(phys + g_fb.size_bytes);
    serial_puts(" (");
    serial_puthex(g_fb.size_bytes);
    serial_puts(" bytes)\n");

    g_fb_ready = 1;

    /* Bind the buffer to the scanout. */
    if (virtio_gpu_set_framebuffer(phys, g_fb.size_bytes, w, h) < 0) {
        serial_puts("[fb] virtio_gpu_set_framebuffer failed\n");
        g_fb_ready = 0;
        return -4;
    }

    /* Clear to a pleasant dark blue so a successful init is visually
     * obvious (distinguishable from "framebuffer never written" which
     * QEMU shows as black, and from "wrong colour packing" which
     * usually shows as garbage or inverted colours). */
    fb_clear(FB_COLOR(0x10, 0x20, 0x40));
    fb_present(0, 0, 0, 0);

    serial_puts("[fb] ready: ");
    serial_puthex(w);
    serial_puts(" x ");
    serial_puthex(h);
    serial_puts(" B8G8R8X8\n");
    return 0;
}

int fb_is_ready(void)
{
    return g_fb_ready;
}

const struct fb_info *fb_get_info(void)
{
    return g_fb_ready ? &g_fb : NULL;
}

void fb_clear(struct fb_color c)
{
    if (!g_fb_ready) return;
    fb_fill_rect(0, 0, g_fb.width, g_fb.height, c);
}

void fb_draw_pixel(uint32_t x, uint32_t y, struct fb_color c)
{
    if (!g_fb_ready || x >= g_fb.width || y >= g_fb.height) return;
    put_pixel_unchecked(x, y, pack_color(c));
}

void fb_fill_rect(uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h,
                  struct fb_color c)
{
    if (!g_fb_ready) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (x + w > g_fb.width)  w = g_fb.width  - x;
    if (y + h > g_fb.height) h = g_fb.height - y;

    uint32_t pixel = pack_color(c);
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *dst = (uint32_t *)(g_fb.base +
                                     (uint64_t)(y + row) * g_fb.pitch) + x;
        for (uint32_t i = 0; i < w; i++) dst[i] = pixel;
    }
}

void fb_draw_rect(uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h,
                  struct fb_color c)
{
    if (!g_fb_ready || w == 0 || h == 0) return;
    fb_fill_rect(x,         y,         w, 1, c);
    fb_fill_rect(x,         y + h - 1, w, 1, c);
    fb_fill_rect(x,         y,         1, h, c);
    fb_fill_rect(x + w - 1, y,         1, h, c);
}

/* Blit `src_w * src_h` BGRX pixels from `src` into the framebuffer
 * at (dst_x, dst_y).  Source rows are tightly packed; destination
 * rows are spaced by g_fb.pitch.  The source pixel format must
 * already match the framebuffer (B8G8R8X8 little-endian).
 *
 * Clips to screen bounds.  Performs no scaling — caller pre-resizes
 * (today the wallpaper.bgra blob is already the framebuffer's
 * exact dimensions). */
void fb_blit_bgra(uint32_t dst_x, uint32_t dst_y,
                  uint32_t src_w, uint32_t src_h,
                  const uint8_t *src)
{
    if (!g_fb_ready || src == 0) return;
    if (dst_x >= g_fb.width || dst_y >= g_fb.height) return;

    uint32_t w = src_w, h = src_h;
    if (dst_x + w > g_fb.width)  w = g_fb.width  - dst_x;
    if (dst_y + h > g_fb.height) h = g_fb.height - dst_y;

    /* Copy whole rows as 4-byte words.  Both source and destination
     * are 4-byte aligned (FB starts on a page; BGRA blob starts at
     * a section boundary which we forced to 8-byte align). */
    for (uint32_t row = 0; row < h; row++) {
        uint32_t       *dst = (uint32_t *)(g_fb.base +
                              (uint64_t)(dst_y + row) * g_fb.pitch) + dst_x;
        const uint32_t *sp  = (const uint32_t *)(src +
                              (uint64_t)row * src_w * 4ULL);
        for (uint32_t i = 0; i < w; i++) dst[i] = sp[i];
    }
}

void fb_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g_fb_ready) return;
    if (w == 0 && h == 0) {
        x = 0; y = 0;
        w = g_fb.width;
        h = g_fb.height;
    }
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (x + w > g_fb.width)  w = g_fb.width  - x;
    if (y + h > g_fb.height) h = g_fb.height - y;

    /* offset = byte offset of (x, y) inside the backing buffer. */
    uint64_t off = (uint64_t)y * g_fb.pitch + (uint64_t)x * 4ULL;

    /* `dsb sy` ensures all the pixel stores have completed before the
     * device-side TRANSFER reads them.  virtio_gpu_flush_rect's submit
     * path also does a `dmb sy` between filling the buffer and
     * publishing the avail index, but a barrier here is cheap and
     * documents intent at the framebuffer boundary. */
    __asm__ volatile("dsb sy" ::: "memory");
    virtio_gpu_flush_rect(x, y, w, h, off);
}
