# Chapter 99 — Extending the PNG decoder: palette, grayscale, and content-type sniffing

Chapter 98 shipped a PNG decoder that could read the assets *we*
baked: 8-bit RGB and 8-bit RGBA, 16×16 pixels, perfectly
non-interlaced, dimensions ≤ 4096. Every test image we hand-crafted
in `scripts/make_test_png.py` fell inside that subset, so every
test passed.

The first time the user navigated the GUI browser to a real PNG on
the open web — `https://test-images.github.io/png/202105/cs-black-000.png`
— two failures landed simultaneously:

1. The image never reached the decoder. The browser fed the raw
   PNG bytes through the HTML tokenizer and rendered them as a
   page of garbled symbols ("PNG IHDR PLTE IDAT …").
2. Even if the decoder *had* received the bytes, it would have
   refused them: the image is colour type 3 (palette), and chapter
   97's decoder accepted only types 2 and 6.

This chapter fixes both. The decoder grows from a two-format
toy into something that handles every PNG colour type real-world
sites serve, and the browser learns to recognise an image
response and synthesise an HTML wrapper around it instead of
parsing it as markup.

## What this chapter adds

- **Decoder support** in [userspace/libc/png.h](../../../userspace/libc/png.h)
  for:
  - colour type **0** (grayscale) at bit depths 1, 2, 4, 8
  - colour type **3** (palette / indexed) at bit depths 1, 2, 4, 8
  - colour type **4** (grayscale + alpha) at 8-bit
  - the **`tRNS`** chunk (single-key transparency for types 0 and 2,
    per-entry alpha for type 3)
  - **sub-byte sample unpacking** for the bit depths < 8 above
- **Two new test images** baked at build time:
  - `assets/osfs/icon_palette.png` — a four-quadrant palette PNG
  - `assets/osfs/icon_gray.png` — an 8-bit grayscale gradient
- **Browser content-type sniffing** in
  [userspace/browser/browser.c](../../../userspace/browser/browser.c):
  - `br_looks_like_png()` — eight-byte signature check on the
    response.
  - `br_img_cache_install_bytes()` — pre-decode the response and
    install in the image cache under the page URL.
  - `br_synthesize_image_page()` — emit a tiny `<html><body>…
    <img …></body></html>` wrapper that references the image at
    its own URL.
  - Hooked into `load_page()` between fetch and parse.
- **Smoke tests:**
  - [scripts/test_png_palette.py](../../../scripts/test_png_palette.py) —
    standalone decoder verification for both new colour types.
  - [scripts/test_browser_direct_png.py](../../../scripts/test_browser_direct_png.py)
    — end-to-end framebuffer check that direct PNG navigation
    actually paints the picture.

The full sweep grows from 51 / 51 tests to **53 / 53 PASS**, with
no regressions in any of the chapter-97 image tests.

## Prerequisites

- Chapter 98 — the original decoder, image cache, layout
  `LAY_PAINT_IMAGE` paint command, and the alpha-compositing
  blitter. This chapter extends every layer, not replaces them.
- Chapter 62 — the browser's fetch / parse / layout / paint
  pipeline. The sniff hook lives in the seam between fetch and
  parse.

## The bug, exactly

When the user typed `http://10.0.2.2:8080/test-images.github.io/png/202105/cs-black-000.png`
into the address bar (our HTTP proxy rewrites this onto the real
URL), the browser called `fetch()`, got back 1 916 bytes of PNG,
and handed it straight to `build_page_from_html()`. The HTML
tokenizer's job is to tokenise *bytes*, not to validate them; it
happily produced tag-looking tokens out of the binary garbage. The
DOM was a single text node holding the file's printable bytes.
Layout rendered that as a wall of monospace text.

You can see the result in the bug report screenshot: the address
bar shows the PNG URL, and the page body shows the literal IHDR /
PLTE / IDAT / IEND chunk-tag bytes interleaved with random
deflate-stream output rendered as Latin-1.

Two things had to change:

1. **Recognise PNG bytes as not HTML.** The fix is the simplest
   possible content-type sniff: look at the first eight bytes of
   the response. Real browsers do roughly the same thing on every
   load (the WHATWG sniffing spec); the Content-Type header is
   advisory and frequently wrong on the open web.
2. **Decode what we recognised.** The PNG in the bug report is a
   400×400 paletted image — a colour format chapter 98 did not
   support. Even if the sniff had fired, `png_decode()` would have
   returned `-1`. The user would have seen our error page instead
   of garbled text — better, but still not the picture.

## The decoder, extended

### What "colour type" means in PNG

Every PNG header (IHDR chunk, 13 bytes) carries a `colour type`
byte that picks one of five interpretations for the IDAT pixel
stream:

| Type | Meaning            | Channels | Allowed bit depths | Real-world use                                      |
| :--: | :----------------- | :------: | :----------------- | :-------------------------------------------------- |
| 0    | Grayscale          | 1        | 1, 2, 4, 8, 16     | Old screenshots, some logos, masks                  |
| 2    | Truecolour (RGB)   | 3        | 8, 16              | Photos, modern screenshots                          |
| 3    | Indexed (palette)  | 1        | 1, 2, 4, 8         | Logos, icons, screenshots optimised for size        |
| 4    | Grey + alpha       | 2        | 8, 16              | Anti-aliased text masks, alpha-only matte channels  |
| 6    | RGBA               | 4        | 8, 16              | Modern photos with transparency, modern app icons   |

Chapter 98 supported only the rightmost two (8-bit RGB and 8-bit
RGBA). That covers most newly-baked photo content but misses the
giant pile of legacy screenshots, paletted icons, and anti-aliased
font sprites that already exist on the web.

We add support for types 0, 3, and 4 — and the `tRNS` chunk that
carries transparency information for the colour types that don't
have an alpha channel built in. We continue to refuse 16-bit
depths and interlaced streams; both are rare enough on the open
web that we will revisit them only if a future chapter motivates
it.

### Unpacking sub-byte samples

PNG's three small bit depths (1, 2, 4) pack multiple samples per
byte, MSB-first. A 1-bit row of width 16 occupies *2* bytes, not
16 — the leftmost pixel sits in bit 7 of byte 0, the second in
bit 6, and so on:

```
bit:    7 6 5 4 3 2 1 0   7 6 5 4 3 2 1 0
sample: 0 1 2 3 4 5 6 7   8 9 ...
```

`png_unpack_sample()` extracts one sample at a given x coordinate
without ever materialising the whole expanded row:

```c
static inline uint8_t png_unpack_sample(const uint8_t *row,
                                        uint32_t x, int bit_depth)
{
    if (bit_depth == 8) return row[x];
    int spb       = 8 / bit_depth;        /* samples per byte */
    uint32_t byte = x / (uint32_t)spb;
    int sub       = (int)(x - byte * (uint32_t)spb);
    int shift     = 8 - bit_depth * (sub + 1);
    int mask      = (1 << bit_depth) - 1;
    return (uint8_t)((row[byte] >> shift) & mask);
}
```

For grayscale at sub-8-bit depth we then need to map the raw
sample onto the full 0..255 display range. The PNG spec is exact
about the multipliers (§13.13) — they are chosen so that the
full-on raw sample maps to 0xFF without rounding error:

| Bit depth | Multiplier |
| --------- | ---------- |
| 1         | 0xFF       |
| 2         | 0x55       |
| 4         | 0x11       |
| 8         | 1 (identity) |

```c
static inline uint8_t png_scale_gray(uint8_t sample, int bit_depth)
{
    if (bit_depth == 8) return sample;
    if (bit_depth == 4) return (uint8_t)(sample * 0x11);
    if (bit_depth == 2) return (uint8_t)(sample * 0x55);
    if (bit_depth == 1) return sample ? 0xFF : 0x00;
    return sample;
}
```

For palette images the raw sample is an *index* into the PLTE
chunk; we don't scale it at all, we just look up `(R, G, B)` from
the palette and apply the per-index alpha from `tRNS` if present.

### The `bpp` filter trap

PNG row filters (Sub, Up, Average, Paeth) all reference "the
pixel `bpp` bytes to the left", where `bpp` is defined by the
spec as **bits per pixel rounded up to the nearest whole byte**
(§9.2). For sub-byte depths every depth — 1, 2, 4 — collapses to
a `bpp_filter` of exactly **1**. The Sub filter on a 1-bit row
subtracts `byte[x-1]`, not `bit[x-1]`, even though that "previous
byte" contains 8 unrelated samples.

This took some thought the first time we saw it. The natural
generalisation of "bpp" to sub-byte depths would be "previous
sample", but that's *not* what PNG does. The decoder's old code
computed:

```c
int bpp_in = (color_type == 6) ? 4 : 3;        // RGBA or RGB
```

…which was right for the two formats supported so far and silently
wrong for any sub-byte depth. The new code:

```c
int bits_per_pixel = channels * bit_depth;
int bpp_filter = (bits_per_pixel + 7) / 8;
if (bpp_filter < 1) bpp_filter = 1;
int row_data = (int)(((uint64_t)w * (uint32_t)bits_per_pixel + 7) / 8);
```

…matches §9.2 exactly. Both `bpp_filter` and `row_data` are
parameterised on `bit_depth × channels`, so adding a new
combination is one switch arm in the channel count and zero
arithmetic changes.

### `tRNS` — single-key and palette-alpha transparency

For colour types without a built-in alpha channel (0, 2, and 3),
PNG carries transparency via an optional `tRNS` chunk:

| Source colour type | `tRNS` payload                    | Decoded interpretation                                         |
| ------------------ | --------------------------------- | -------------------------------------------------------------- |
| 0 (grey)           | 2 bytes (16-bit grey value)       | Pixels matching this exact grey value get α = 0; others α = FF |
| 2 (RGB)            | 6 bytes (16-bit R, G, B)          | Pixels matching this exact RGB triple get α = 0                |
| 3 (palette)        | up to 256 bytes of α              | `tRNS[i]` is the alpha for palette index `i`; missing → α = FF |

We capture the raw chunk during the IDAT-walk first pass and
consult it during the row → BGRA conversion. The grey/RGB
single-key matches use the bottom 8 bits of the 16-bit chunk
value (we don't support 16-bit images yet, so the high byte is
always zero).

### What still isn't supported

- 16-bit-per-channel formats (any colour type at depth 16).
- Adam7 interlacing (any colour type with the interlace byte set
  to 1).
- Ancillary chunks beyond `PLTE` and `tRNS`. We ignore `gAMA`,
  `sRGB`, `iCCP`, `pHYs`, `tEXt`, `iTXt`, `bKGD`, `cHRM`. The
  rendered image is whatever raw RGB the IDAT decodes to,
  uncalibrated. This is fine for icons, screenshots, and most
  web art; it will look subtly wrong on professional photography
  with embedded ICC profiles. Accepting these chunks (without
  acting on them) requires nothing — we already skip unknown
  chunks. Acting on them is a future chapter.
- The PNG spec's chunk-ordering rules (PLTE before IDAT, tRNS
  before IDAT). We are tolerant of any order; this matters only
  for streaming decoders, which we are not.

## The browser, sniffing

`load_page()` is the single function in the browser that turns a
URL into a renderable page. After this chapter it has a new
guard between fetch and parse:

```c
p->html_buf = fetch(url, &p->html_len, &p->origin_url);
/* …error fallback… */

if (br_looks_like_png(p->html_buf, p->html_len)) {
    struct br_img_cache_entry *e = br_img_cache_install_bytes(
        p, p->url,
        (const uint8_t *)p->html_buf, p->html_len);
    if (e) {
        size_t new_len = 0;
        char *wrap = br_synthesize_image_page(p->url, e->w, e->h,
                                               &new_len);
        if (wrap) {
            free(p->html_buf);
            p->html_buf = wrap;
            p->html_len = new_len;
        }
    } else {
        /* decode failed -> show an error page */
        free(p->html_buf);
        p->html_buf = make_error_html(url,
            "Image fetched but decoder rejected it (unsupported "
            "PNG variant — interlaced or 16-bit? Try another image).",
            &p->html_len);
    }
}
```

Three things happen here:

1. **Sniff.** `br_looks_like_png()` checks the eight-byte PNG
   signature. We chose pure signature sniffing rather than
   honouring a `Content-Type` header for the same reasons real
   browsers did — server-supplied content types are wrong often
   enough on the open web that "trust but verify" looks the same
   as "ignore and verify". The sniff is one byte cheaper than the
   PNG decoder's own signature check, so a positive match is
   essentially free.
2. **Pre-decode and cache.** `br_img_cache_install_bytes()` is a
   peer of the existing `br_img_cache_load()` from chapter 98,
   sharing the cap policy (16 entries / 4 MiB) but skipping the
   fetch step — the bytes are right there. We install under
   `p->url`, the page's own URL, so when the synthesised
   `<img src="…">` later goes through `br_resolve_img_src()` and
   `br_img_cache_lookup()`, it finds the entry instantly.
3. **Synthesise wrapper HTML.** A 256-byte HTML page with one
   `<p>` describing the image and one `<img>` pointing at the
   page URL. We embed the intrinsic dimensions in the `width=""`
   and `height=""` attributes so the layout pass reserves a
   correctly-sized REPLACED box on its first pass — there is no
   intrinsic-size feedback loop into layout yet.

   ```c
   snprintf(buf, cap,
       "<html><body><p>Image: %s (%dx%d)</p>"
       "<img src=\"%s\" width=\"%d\" height=\"%d\" alt=\"image\" />"
       "</body></html>",
       img_url, img_w, img_h, img_url, img_w, img_h);
   ```

After this the existing pipeline runs unchanged. The parser sees
HTML, builds a DOM, lays out the box tree, and
`br_attach_images()` looks up the synthetic `<img>`'s src in the
cache — instant hit, since we installed it ourselves. The
`LAY_PAINT_IMAGE` blitter does its alpha-compositing job, and the
picture appears on the framebuffer.

### Why a wrapper HTML page rather than a special render path?

We considered three options and picked the one with the smallest
diff:

| Option | Cost | What it would look like |
| :----- | :--- | :---------------------- |
| **Synthesise an HTML wrapper** (chosen) | +30 lines in load_page; reuses every layer below it. | The browser's parse / layout / paint pipeline runs unchanged. The user sees a small "Image: URL (WxH)" header above the picture, which doubles as a useful piece of information. |
| Carve out a separate "image viewer" mode | New top-level state, new event dispatch, duplicated window-management. | A cleaner aesthetic but at the cost of an entirely parallel render loop. We don't have an image viewer (yet) for which this would otherwise pay off. |
| Make the parser content-type aware | Tokenizer learns to recognise binary content. | Slowest to implement, hardest to test. The HTML tokenizer is byte-oriented; bolting "is this binary?" into the state machine touches every dispatch. |

The wrapper-HTML path also gives us a free upgrade target: when
we add other binary content types later (JPEG, GIF, WebP), each
one fits the same mould. Sniff. Decode-and-cache. Wrap. Reparse.
Nothing in the pipeline below `load_page()` cares.

## Test corpus

`scripts/make_test_png.py` grew a `--kind=…` flag and two new
recipes:

```sh
python3 scripts/make_test_png.py --kind=palette assets/osfs/icon_palette.png
python3 scripts/make_test_png.py --kind=gray    assets/osfs/icon_gray.png
```

**Palette test image** (`icon_palette.png`):

A 16×16 picture split into four 8×8 quadrants, one solid colour
each:

| Quadrant     | RGB           | Decoded BGRA bytes      | Bytes / pixel |
| :----------- | :------------ | :---------------------- | :------------ |
| Top-left     | red           | (0, 0, 255, 255)        | sum 510       |
| Top-right    | green         | (0, 255, 0, 255)        | sum 510       |
| Bottom-left  | blue          | (255, 0, 0, 255)        | sum 510       |
| Bottom-right | white         | (255, 255, 255, 255)    | sum 1020      |

Total decoded BGRA byte sum: 64 × (510 + 510 + 510 + 1020) =
**163 200**. All 256 pixels are fully opaque (no `tRNS`).

**Grayscale test image** (`icon_gray.png`):

A 16×16 picture with `pixel(x, y) = min((x + y) × 8, 255)`, an 8-
bit black-to-white diagonal gradient. Each pixel decodes to BGRA
`(grey, grey, grey, 255)`, contributing `3 × grey + 255` to the
byte sum. Computed at bake time: **157 440**.

Both images bake from the same script, with the byte-sums echoed
to stdout so the test scripts can hard-code them. The bake-don't-
commit policy from chapter 98 stays in force: zlib output isn't
reproducible across host versions, but pixel arrays are.

### `scripts/test_png_palette.py`

A direct copy of `test_png.py` parameterised over a list of
`(path, w, h, byte_sum, opaque_count, label)` tuples. Boots the
kernel, drops to a shell, runs `pngdec /mnt/icon_palette.png` and
`pngdec /mnt/icon_gray.png`, asserts on every field. If a future
edit breaks the palette branch the test fails with the exact
mismatch:

```
FAIL [palette (type 3)]: BGRA sum got 65280, want 163200 —
decoder produced different pixels than Pillow
```

### `scripts/test_browser_direct_png.py`

End-to-end framebuffer check. Boots graphically, runs
`browser --gui /mnt/icon_palette.png 800 &` (note: the page
*itself* is the PNG — no wrapper HTML on disk), waits 6 seconds
for the browser to spawn / fetch / sniff / synthesise / decode /
render, and screendumps the framebuffer. Then it counts pure-
red, pure-green, and pure-blue pixels:

```
on-screen pixel counts: red=64 green=64 blue=64
PASS: direct PNG navigation rendered the image (red=64 green=64 blue=64)
```

The pixel counts are *exact*: the palette image has exactly 64
pixels of each pure colour by construction, with no anti-aliasing
to fuzz the count, so the screendump should report exactly 64 of
each. This is the cleanest pixel-perfect signal we have anywhere
in the test suite — if any downstream step starts dithering or
re-quantising the image, the counts will move and the test will
catch it immediately.

## Files added / modified

**Added:**

- [scripts/test_png_palette.py](../../../scripts/test_png_palette.py)
- [scripts/test_browser_direct_png.py](../../../scripts/test_browser_direct_png.py)
- [assets/osfs/icon_palette.png](../../../assets/osfs/icon_palette.png) (built)
- [assets/osfs/icon_gray.png](../../../assets/osfs/icon_gray.png) (built)

**Modified:**

- [userspace/libc/png.h](../../../userspace/libc/png.h) — colour types
  0/3/4, sub-byte sample unpacking, gray-scale scaling, `tRNS`
  handling, fixed `bpp` semantics.
- [userspace/browser/browser.c](../../../userspace/browser/browser.c) —
  `br_looks_like_png()`, `br_img_cache_install_bytes()`,
  `br_synthesize_image_page()`, sniff hook in `load_page()`.
- [scripts/make_test_png.py](../../../scripts/make_test_png.py) — `--kind`
  flag for palette and grayscale variants; bake-time BGRA sum
  reporting for each.
- [Makefile](../../../Makefile) — bake rules for the two new images;
  added to `OSFS_FILES` and `mkosfs.py` invocation.

## Build & run

```sh
make -j8
python3 scripts/test_png_palette.py
python3 scripts/test_browser_direct_png.py
```

Both should print `PASS`. Then sweep:

```sh
bash -c 'for f in scripts/test_*.py; do
  echo -n "Running $(basename $f)... "
  if timeout 240 python3 "$f" > /tmp/sweep.log 2>&1; then
    echo "PASS"
  else
    echo "FAIL"
  fi
done'
```

The full suite is now **53 / 53 PASS**. The two new tests slot
between `test_browser_direct_png` and `test_png_palette` in the
alphabetical run order.

## Looking ahead

We can now navigate the GUI browser to a paletted PNG and see
the picture. The next ladders — JPEG (chapter 101 candidate), GIF,
WebP — all hang off the same sniff hook. Each new format is a new
`br_looks_like_X()` predicate and a new decoder; the cache, the
URL resolver, and the LAY_PAINT_IMAGE blitter never have to know.

The chapter-97 → chapter-98 progression is, for me, one of the
most satisfying micro-arcs in the project. Chapter 98 *built* the
infrastructure — cache, layout seam, blitter, tests — out of
nothing, and chapter 99 *spent* it: every new format is now a
small, isolated decoder that drops into the existing slot. The
shape of the seam is the chapter; everything else is just paint.
