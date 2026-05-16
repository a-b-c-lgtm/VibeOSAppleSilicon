# Chapter 46 — virtio-gpu: a framebuffer at native resolution

Up to chapter 45 the kernel only knew how to paint to a serial
console.  This chapter and the next two unlock the entire GUI
stack: a framebuffer, a keyboard, a window manager, and a demo
GUI app.

We start with **virtio-gpu** in 2D mode under QEMU's `virt`
machine.  The goal is modest but motivating: bring up a contiguous
pixel buffer, paint a boot banner with three colour swatches, and
prove the pipeline by running `make run-graphical` and watching a
Cocoa window light up at 1280×800 (or 1920×1080, your choice).

We touch four new files:

- `kernel/device/virtio_gpu.{h,c}` — the device driver.
- `kernel/device/fb.{h,c}` — a framebuffer abstraction layered on
  top of the GPU.
- `kernel/device/font.{h,c}` and `kernel/device/font_8x16.h` —
  an 8×16 ASCII bitmap font, copied verbatim from the x86 edition.
- `kernel/device/text.{h,c}` — text rendering on top of the font
  and framebuffer.

Plus one helper added to `kernel/core/pmem.c`:
`pmem_alloc_contig(npages)`, which we need because virtio-gpu
expects a single physically-contiguous backing region.

## The virtio-gpu handshake

`virtio-gpu` lives on the same virtio-mmio bus we already know
from `virtio-blk` (chapter 20).  The probe loop in
`virtio_gpu_init` walks slots 0..31 at base `0x0A000000`, looking
for `device_id == 16`.  Once found, the handshake is the textbook
modern-virtio sequence:

1. write `0` to `STATUS` (reset);
2. set `ACKNOWLEDGE`, then `DRIVER`;
3. read 64 feature bits, accept only `VIRTIO_F_VERSION_1` (we don't
   want any of the optional GPU features yet — no EDID, no resource
   blob, no virgl), then write the accepted bits;
4. set `FEATURES_OK`, read it back, abort if the device cleared it;
5. allocate one 4 KiB page for the controlq descriptor table,
   avail ring, and used ring (the same shared-page layout we used
   for `virtio-blk`);
6. write the queue address, set `QUEUE_READY`, set `DRIVER_OK`.

The control queue is a simple request/response channel.  Each
request is a `struct virtio_gpu_ctrl_hdr` followed by command
arguments; each response is another header (sometimes followed by
data, e.g. `RESP_OK_DISPLAY_INFO`).  We use four commands:

| command                            | purpose                              |
|------------------------------------|--------------------------------------|
| `GET_DISPLAY_INFO` (0x0100)        | discover screen geometry             |
| `RESOURCE_CREATE_2D` (0x0101)      | allocate a host-side 2D resource     |
| `RESOURCE_ATTACH_BACKING` (0x0106) | attach guest pages as backing store  |
| `SET_SCANOUT` (0x0103)             | bind the resource to the display     |
| `TRANSFER_TO_HOST_2D` (0x0105)     | copy guest pixels into the host res  |
| `RESOURCE_FLUSH` (0x0104)          | tell the host to repaint that region |

Bring-up issues `GET_DISPLAY_INFO`, `CREATE_2D` (with
`VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` = 2), `ATTACH_BACKING` to a
contiguous block of low RAM, and `SET_SCANOUT`.  Per-frame we
`TRANSFER_TO_HOST_2D` the dirty rectangle and `RESOURCE_FLUSH`
the same region.

## The framebuffer abstraction

`fb.c` owns the contiguous pixel buffer and exposes a tiny API:

```c
int  fb_init(void);
void fb_clear(struct fb_color c);
void fb_draw_pixel(uint32_t x, uint32_t y, struct fb_color c);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  struct fb_color c);
void fb_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
```

`fb_init` asks `pmem_alloc_contig(ceil(w*h*4 / PAGE_SIZE))` for
the backing pages, then walks them through the GPU handshake
above.  It also paints a default dark-blue background and sets
the global "ready" flag.

`fb_present(0, 0, 0, 0)` is overloaded to mean "the whole
screen", which is what every caller in the WM uses today.

### B8G8R8X8 packing on aarch64-LE

`virtio-gpu` format 2 wants pixels in memory as `B, G, R, X`.
We store them as a single little-endian `uint32_t`:

```c
static inline uint32_t pack_color(struct fb_color c)
{
    return ((uint32_t)c.r << 16)
         | ((uint32_t)c.g <<  8)
         | ((uint32_t)c.b      );
}
```

On a little-endian host this lays down `B, G, R, 0` in memory
order — exactly what `B8G8R8X8_UNORM` expects.  The same shift
pattern shows up later in the `GUI_BGRA(R, G, B)` macro that
userspace uses (chapter 48), and the smoke test in
[scripts/test_wm.py](scripts/test_wm.py) parses the resulting PPM
on this assumption.

## The font and text renderer

`font_8x16.h` is the classic VGA 8×16 bitmap font: 256 glyphs,
each 16 bytes, one bit per pixel.  `font.c` is a thin lookup:

```c
const struct font *font_get_default(void);
const uint8_t     *font_glyph(const struct font *f, char ch);
```

`text.c` walks a string and pokes pixels into the framebuffer:

```c
void text_draw_string(uint32_t x, uint32_t y, const char *s,
                      struct fb_color fg, struct fb_color bg,
                      int transparent_bg);
```

For each glyph it walks 16 rows of 8 bits, calling
`fb_draw_pixel(x + bit, y + row, fg)` when the bit is set, and
optionally painting the background colour for unset bits.

## Boot banner

Right after `fb_init` succeeds we paint a four-line banner plus
three colour swatches:

```c
text_draw_string(40, 40, "osdev / aarch64", WHITE, BLACK, 1);
text_draw_string(40, 60, "milestone 38: virtio-gpu",
                 LIGHT_BLUE, BLACK, 1);
fb_fill_rect( 40, 100, 64, 32, FB_COLOR(0xC0, 0x40, 0x40));
fb_fill_rect(112, 100, 64, 32, FB_COLOR(0x40, 0xC0, 0x40));
fb_fill_rect(184, 100, 64, 32, FB_COLOR(0x40, 0x40, 0xC0));
fb_present(0, 0, 0, 0);
```

The R/G/B swatches catch any byte-order mistake immediately: if
they came out as B/G/R you packed colour wrong, and if they came
out as a single solid block you forgot the per-rect coordinates.

## Three gotchas

1. **`memset` from struct initialisers.**  `virtio_gpu.c` builds
   `struct virtio_gpu_resource_create_2d req = { .hdr = ... };` —
   GCC at `-O2` lowered the zero-init to a `memset` call.  In a
   freestanding kernel that symbol doesn't exist.  Fixed by adding
   a one-line `memset` to the same translation unit.  See
   [/memories/freestanding-c-memset-trap.md](/memories/freestanding-c-memset-trap.md).

2. **Allocation order in `main.c` matters.**  The kernel heap, the
   `virtio_blk` queue, the `virtio_gpu` queue, and the framebuffer
   all want contiguous physical pages.  Pmem hands them out in
   monotonically decreasing order, so as long as each allocator
   runs on a fresh-pmem map you get contiguous regions for free.
   We deliberately initialise them in that order in
   `kernel_main`.

3. **HVF + cacheable framebuffer.**  Because QEMU reads the guest
   framebuffer in software when handling `TRANSFER_TO_HOST_2D`, a
   single `dsb sy` before the queue notify is enough — no
   `DC CVAC` dance is needed.  This held up under stress testing
   at 60 fps.

## Smoke test

`make run-graphical` brings up Cocoa.  For headless CI we use the
same QMP `screendump` trick that chapters 47 and 48 build on:

```bash
qemu-system-aarch64 ... -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait
echo '{"execute":"qmp_capabilities"}'              | nc -U /tmp/qmp.sock
echo '{"execute":"screendump","arguments":{"filename":"/tmp/fb.ppm","format":"ppm"}}' \
                                                   | nc -U /tmp/qmp.sock
```

The resulting PPM should have the dark-blue background plus three
non-background colours from the swatches.

## Files changed

- `kernel/device/virtio_gpu.{h,c}` — new.
- `kernel/device/fb.{h,c}` — new.
- `kernel/device/font.{h,c}`, `kernel/device/font_8x16.h` — new
  (font copied verbatim from VibeOS).
- `kernel/device/text.{h,c}` — new (one rename:
  `framebuffer.h` → `fb.h`).
- `kernel/core/pmem.{h,c}` — added `pmem_alloc_contig(npages)`.
- `kernel/core/main.c` — calls `fb_init()` and the banner painter
  after `virtio_blk_init()`.
- `Makefile` — `make run-graphical` now adds `-vga none` and
  `-device virtio-gpu-device,xres=...,yres=...`.

## What's deferred

- **Multi-scanout / multi-monitor** — we hard-code scanout 0.
- **Display hot-plug** — we don't subscribe to virtio-gpu config
  change interrupts; resolution is whatever QEMU started with.
- **Cursor sprite via `RESOURCE_CREATE_2D` + `MOVE_CURSOR`** —
  saved for milestone 41 when we add a mouse.
- **Hardware acceleration** — we never enable virgl; all
  composition is CPU-side in chapter 48's WM.

These are all incremental; the contiguous-pixel-buffer model in
this chapter is the foundation everything else layers on top of.
