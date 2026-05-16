/*
 * kernel/device/fb.h — framebuffer abstraction (milestone 38).
 *
 * The framebuffer is a contiguous, physically-allocated B8G8R8X8 pixel
 * buffer that the virtio-gpu driver has bound as the active scanout.
 * After fb_init() succeeds, all drawing primitives write into RAM and
 * fb_present() pushes the dirty rectangle to the host display via
 * virtio-gpu's TRANSFER_TO_HOST_2D + RESOURCE_FLUSH commands.
 *
 * Pixel format is fixed: 32 bits per pixel, byte order B G R X in
 * memory.  A `struct fb_color` is stored 8-bit per channel and packed
 * into the on-wire format internally; callers never see raw pixels.
 *
 * The aarch64 port does not use a Multiboot2 framebuffer tag — there
 * is no firmware that hands us one.  Instead the kernel allocates a
 * physically-contiguous run of pages out of pmem (the only pmem trick
 * we rely on is its monotonic-decreasing allocation order during boot,
 * exposed via pmem_alloc_contig) and tells the GPU "this is now your
 * framebuffer".
 */

#ifndef KERNEL_DEVICE_FB_H
#define KERNEL_DEVICE_FB_H

#include <stdint.h>

struct fb_color {
    uint8_t r, g, b, a;
};

#define FB_COLOR(R, G, B)        ((struct fb_color){ (R), (G), (B), 0xFF })
#define FB_COLOR_BLACK           FB_COLOR(0x00, 0x00, 0x00)
#define FB_COLOR_WHITE           FB_COLOR(0xFF, 0xFF, 0xFF)
#define FB_COLOR_RED             FB_COLOR(0xFF, 0x00, 0x00)
#define FB_COLOR_GREEN           FB_COLOR(0x00, 0xFF, 0x00)
#define FB_COLOR_BLUE            FB_COLOR(0x00, 0x00, 0xFF)
#define FB_COLOR_CYAN            FB_COLOR(0x00, 0xFF, 0xFF)
#define FB_COLOR_MAGENTA         FB_COLOR(0xFF, 0x00, 0xFF)
#define FB_COLOR_YELLOW          FB_COLOR(0xFF, 0xFF, 0x00)
#define FB_COLOR_GRAY            FB_COLOR(0x80, 0x80, 0x80)

/* Discovered geometry of the active scanout.  Read-only after fb_init. */
struct fb_info {
    uint8_t  *base;       /* virtual = physical (identity-mapped low RAM) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;      /* bytes per scanline = width * 4 */
    uint32_t  size_bytes; /* pitch * height, rounded up to a page */
    uint64_t  phys;       /* same as base in our identity map */
};

/* Allocate a contiguous framebuffer sized to the GPU's reported
 * scanout, attach it as the GPU's backing buffer, clear it to a dark
 * blue, and push one initial frame.  Returns 0 on success.
 *
 * Pre-conditions:
 *   - virtio_gpu_init() has succeeded.
 *   - pmem has enough contiguous pages left to cover width*height*4
 *     bytes.  In practice this requires fb_init to be called BEFORE
 *     any other contiguous allocation that could leave gaps; see
 *     kernel/core/main.c for the canonical order.
 */
int fb_init(void);

/* 1 if fb_init succeeded, 0 otherwise. */
int fb_is_ready(void);

/* Read-only access to the framebuffer geometry. */
const struct fb_info *fb_get_info(void);

/* Drawing primitives (all clip to screen bounds). */
void fb_clear(struct fb_color c);
void fb_draw_pixel(uint32_t x, uint32_t y, struct fb_color c);
void fb_fill_rect(uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h,
                  struct fb_color c);
void fb_draw_rect(uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h,
                  struct fb_color c);

/* Blit a tightly-packed BGRX source bitmap (B8G8R8X8 little-endian)
 * into the framebuffer at (dst_x, dst_y).  Source rows are stride
 * = src_w * 4 bytes.  Clipped to screen bounds.  No scaling. */
void fb_blit_bgra(uint32_t dst_x, uint32_t dst_y,
                  uint32_t src_w, uint32_t src_h,
                  const uint8_t *src);

/* Push the rectangle [(x,y), (x+w, y+h)) of guest-side framebuffer
 * memory to the host display via virtio-gpu.  Out-of-bounds is
 * silently clipped.  Pass (0, 0, 0, 0) to flush the whole screen. */
void fb_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif /* KERNEL_DEVICE_FB_H */
