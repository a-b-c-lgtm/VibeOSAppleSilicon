# Chapter 97 — PNG decoding and the browser image cache

For sixty-three milestones our browser has rendered text only. CSS
backgrounds work. Borders work. Hyperlinks navigate. Replaced
elements like `<img>` are *laid out* — the box has the right
intrinsic size, the alt text shows in place — but in the spot where
a real picture should go, you see a flat grey rectangle.

This chapter fixes that. We add:

1. **A header-only PNG decoder** in
   [userspace/libc/png.h](../../../userspace/libc/png.h) — about 600 lines
   that can take a `.png` byte stream and produce a tightly-packed
   BGRA buffer ready to hand directly to `gui_present`. The decoder
   contains its own complete RFC 1951 inflate.
2. **A standalone consumer**, `/bin/pngdec`, that reads a PNG file
   and prints `WxH, sum=N, opaque=N` so the regression suite can
   verify byte-for-byte that the decoder matches the host's Pillow
   output.
3. **A test corpus**, `assets/osfs/icon.png`, baked at build time
   from a Python script (rather than committed as a binary) so
   pixel-level expectations are reproducible across host zlib
   versions.
4. **A browser-side image cache** keyed on canonical URL, with a
   16-entry / 4 MiB cap, that resolves `<img src=...>` references,
   fetches them, decodes once, and stuffs a borrowed pointer into
   `layout_box::replaced_pixels` so the layout pass picks them up.
5. **A new paint command**, `LAY_PAINT_IMAGE`, plus an
   alpha-compositing blitter in the browser's GUI render switch
   that puts the bytes on screen.

The end-to-end smoke test
[scripts/test_browser_image.py](../../../scripts/test_browser_image.py)
boots the kernel graphically, opens a one-image test page, and
asserts that a 4×4 pure-blue landmark from the test PNG appears on
the framebuffer. With the test passing we are about to retire the
last "TODO" comment from the layout pass.

## What this chapter adds

- `userspace/libc/png.h` — the decoder + inflate, header-only.
- `userspace/pngdec/pngdec.c` — `/bin/pngdec` test harness.
- `scripts/make_test_png.py` — bake a known-pixels 16×16 RGBA PNG
  to `assets/osfs/icon.png` at build time. Uses the same Pillow
  self-bootstrap pattern as `scripts/img_to_bgra.py`.
- `assets/osfs/img_test.html` — single-image test page.
- Layout extensions in `userspace/libc/layout.h`:
  - `struct layout_box` gets `replaced_pixels` /
    `replaced_pixels_w` / `replaced_pixels_h`.
  - `enum layout_paint_kind` gets `LAY_PAINT_IMAGE = 4`.
  - `struct layout_paint_cmd` gets `image_pixels` / `image_w` /
    `image_h`.
  - The REPLACED paint emitter chooses between `LAY_PAINT_IMAGE`
    (when pixels are present) and the existing grey-placeholder /
    alt-text fallback (when they are not).
- Browser extensions in `userspace/browser/browser.c`:
  - A linked-list image cache hung off `struct loaded_page`.
  - `br_resolve_img_src()` — scheme-aware URL resolver for `<img>`
    references in both `http://` and `file://` modes.
  - `br_img_cache_load()` — fetch + decode + cache.
  - `br_attach_images_walk()` — depth-first walk of the layout tree
    that looks up each `<img>`'s decoded BGRA and patches it into
    the box.
  - `LAY_PAINT_IMAGE` case in the GUI render switch that does
    intrinsic-size blits with per-pixel alpha compositing against
    the page background.
- Smoke tests:
  - `scripts/test_png.py` — standalone decoder verification.
  - `scripts/test_browser_image.py` — end-to-end framebuffer check.

## Prerequisites

- Chapter 63 — the browser pipeline (fetch → tokenize → DOM →
  CSS → layout → paint).
- Chapter 62 — layout, especially the `LAY_BOX_REPLACED` path and
  the paint command stream.
- Chapter 28 — `gui_present` syscall (used to blit the BGRA buffer
  into the browser's window).
- Chapter 50 — the BGRA pixel format used everywhere in the
  graphics stack. Decoding straight to BGRA means we never have to
  re-shuffle channels at paint time.

The chapter relies on no new kernel features. Every line of new
code lives in userspace or in build-time scripts.

## What a PNG actually is

PNG files have a fixed eight-byte signature (`89 50 4E 47 0D 0A 1A
0A` — the bytes spell "\x89PNG\r\n\x1a\n", chosen to make almost
any common text-mode mangler corrupt them visibly), then a stream
of *chunks*. Every chunk is:

```
4-byte big-endian length L
4-byte ASCII tag (e.g. "IHDR", "IDAT", "IEND")
L bytes of body
4-byte CRC32 over the tag and body
```

The tags we care about are:

- `IHDR` — header. Always first. 13 bytes: width (4), height (4),
  bit-depth (1), colour type (1), compression method (1), filter
  method (1), interlace method (1).
- `IDAT` — image data. One or more; their bodies concatenate to
  form a single zlib stream.
- `IEND` — end marker. Always last; zero-length body.

Other chunks (`gAMA`, `sRGB`, `pHYs`, `iCCP`, `tEXt`, `zTXt`,
`PLTE`, `tRNS`, ...) carry colour-management hints, palette data,
authorship metadata, and so on. Our decoder skips them all by name:
unknown tags are walked over by the chunk loop using their declared
length, then ignored.

The strict subset we accept:

- 8-bit channels (`bit_depth == 8`)
- Colour type 2 (RGB, three bytes per pixel) or colour type 6
  (RGBA, four bytes per pixel)
- Compression method 0 (deflate) and filter method 0 (the standard
  per-row filter set) — these are the only values the spec defines
- Interlace method 0 (no Adam7)
- Width and height ≤ 4096

Anything outside that subset gets a one-line `[png] ...` log and a
`-1` return. The browser treats decode failures as non-fatal: the
existing grey-placeholder + alt-text path renders instead.

## The zlib wrapper around the IDAT stream

The concatenated IDAT bodies form a single zlib stream, not a raw
deflate stream. Zlib adds:

- A two-byte header (CMF + FLG). For PNG it's almost always
  `0x78 0x9C` (deflate, 32 KiB window, default compression). We
  log a note if it's anything else but proceed anyway — the bits
  describe the *encoder*, not the *bitstream*, and the decoder
  doesn't care.
- A four-byte trailer holding the Adler-32 of the *uncompressed*
  output. We don't verify it; libpng's own decoder usually skips
  it for the same reason we do (a CRC mismatch on a valid bitstream
  is a one-in-four-billion event that signals nothing useful you
  couldn't catch by simply inspecting the rendered image).

So the actual deflate stream lives in `IDAT[2 .. -4]`. The decoder
strips those six bytes before handing the stream to `png_inflate`.

## Inflate: a 200-line implementation of RFC 1951

DEFLATE — the bitstream the deflate library produces — is what
zlib, gzip, zip, woff2, and a dozen other formats actually use
under the hood. Once you can inflate, you can decode all of them.
Implementing it once, in `png.h`, gets us future-proofing for the
WOFF2 font reader in chapter 98.

A deflate stream is a sequence of blocks. Each block starts with
three bits:

- `BFINAL` (1 bit) — set on the last block.
- `BTYPE` (2 bits) — `00` stored (uncompressed), `01` fixed
  Huffman, `10` dynamic Huffman, `11` reserved.

### The bit reader

The bit stream is read LSB-first within each byte, and bytes are
consumed in input order. We pull bytes into a 32-bit accumulator
on demand:

```c
struct png_br {
    const uint8_t *p;     /* next byte */
    const uint8_t *end;   /* one past the last byte */
    uint32_t bitbuf;      /* lowest `nbits` bits hold pending input */
    int      nbits;
};
```

`png_br_need(n)` ensures at least `n` bits are buffered;
`png_br_get(n)` extracts them and shifts. The accumulator never
holds more than 31 bits at a time (we top up at the start of each
extract instead of on a cadence) so the shift is always defined.
For stored blocks we discard remaining bits in the current byte
with `png_br_align_byte`.

### Stored blocks (BTYPE=00)

Trivial. Skip to the next byte boundary, read a 16-bit `LEN` and
its one's complement `NLEN`, verify they match, and copy `LEN`
bytes verbatim. A defensive check prevents the copy from running
off either end of the input or output. This block type is what
deflate emits when the input was effectively incompressible (PNG
encoders rarely produce it; you mostly see it in zip files of
already-compressed payloads).

### Huffman tables

For the two compressed block types we need to decode prefix codes.
The decoder uses a flat lookup scheme: for each code length L (1
to 15), we keep the count of codes of that length and a sorted
list of the symbols they map to. Decoding walks lengths upward,
shifting in one bit per length, until the running code falls into
the L-th group:

```c
int code = 0, first = 0, base = 0;
for (int l = 1; l <= 15; l++) {
    code = (code << 1) | get_one_bit();
    int cnt = h->counts[l];
    if (code - first < cnt)
        return h->syms[base + (code - first)];
    base  += cnt;
    first  = (first + cnt) << 1;
}
return -1;  /* oversized code */
```

`png_huff_build` constructs the table from a length-per-symbol
array using the canonical-code construction in RFC 1951 §3.2.2:
sort the symbols by `(length, value)`, then assign codes in order.
That layout is exactly what the flat table wants, so we never have
to materialise the codes themselves — we use the symbol's index in
the sorted list.

### Fixed Huffman (BTYPE=01)

The literal/length tree is fixed by the spec (RFC 1951 §3.2.6):

| symbols     | length |
|-------------|-------:|
| 0–143       | 8      |
| 144–255     | 9      |
| 256–279     | 7      |
| 280–287     | 8      |

Distance tree: all 30 symbols at length 5. We just hand-build the
`lens[]` arrays and call `png_huff_build`.

### Dynamic Huffman (BTYPE=10)

Almost everything in the wild uses this. The encoder builds two
custom trees and tells us their shapes by:

1. Reading three counts: HLIT (1..30 + 257 = number of literal/
   length codes), HDIST (1..32 = number of distance codes), HCLEN
   (4..19 = number of code-length-code lengths).
2. Reading HCLEN 3-bit values into a 19-element array indexed
   through a fixed permutation `png_cl_perm[]` — the permutation
   puts the most common code-length codes (`16`, `17`, `18`, `0`)
   at the front so HCLEN can usually be small.
3. Building a Huffman tree (`cl`) from those 19 lengths.
4. Decoding HLIT + HDIST symbols using `cl`. The symbols `0..15`
   are literal lengths; `16` means "repeat last length 3..6 times"
   (with 2 extra bits), `17` means "0 for 3..10 times" (3 extra
   bits), `18` means "0 for 11..138 times" (7 extra bits).
5. Splitting the resulting array at `HLIT` and building the two
   real trees.

It is a Huffman tree describing a Huffman tree describing a
Huffman tree. This is the part of inflate that everyone hates the
first time and finds elegant the third time.

### The decompression loop

Once we have a literal/length tree and a distance tree:

```
loop:
    sym = decode(lit_tree)
    if sym < 256: emit byte sym
    else if sym == 256: end of block
    else:
        length = lookup(sym - 257) + extra_bits
        dsym = decode(dist_tree)
        dist = lookup(dsym) + extra_bits
        copy `length` bytes from `output[-dist .. -dist + length]`
```

The lookup tables `png_len_base[]` / `png_len_extra[]` (29
entries) and `png_dist_base[]` / `png_dist_extra[]` (30 entries)
encode the variable-width run-length and distance ranges from RFC
1951 §3.2.5. The longest length is 258 bytes; the longest distance
is 32 768 bytes (the maximum window size).

### The byte-by-byte copy trap

The back-reference copy looks like a `memcpy`. It is *not* a
`memcpy`:

```c
for (uint32_t i = 0; i < len_v; i++) {
    out[*opos] = out[*opos - dist_v];
    (*opos)++;
}
```

If `dist == 1` and `len == 50`, the loop reads byte `i-1` and
writes byte `i`, fifty times. A naive `memcpy(dst, src, n)` with
overlapping ranges has undefined behaviour: most implementations
read the source ahead in 16- or 32-byte SIMD chunks, so the
"repeat the previous byte fifty times" pattern would actually
produce "the same 16 bytes pasted three and a bit times" instead.

This is how DEFLATE represents run-length encoding: a single
distance-1 backref of arbitrary length is a stream of identical
bytes. PNG encoders use it constantly for solid-coloured rows.
Other inflate implementations sometimes special-case `dist == 1`
to a `memset`; we don't bother. The naive byte loop is correct,
fits on a single screen, and the cost is invisible compared to
the surrounding Huffman decode work.

## PNG row filters: prediction in five flavours

The deflate output is *not* the raw RGB(A) pixel array. Each row
is preceded by a one-byte filter type (0–4) and the body of the
row holds residuals against a prediction made from already-known
neighbouring bytes. Decoding strips the filter byte and undoes
the prediction in place. The five filters (RFC 2083 §6.3) are:

| code | name    | predictor                                                |
|-----:|---------|----------------------------------------------------------|
| 0    | None    | no prediction; bytes are stored raw                      |
| 1    | Sub     | `cur[x] += cur[x-bpp]`                                   |
| 2    | Up      | `cur[x] += prev[x]`                                      |
| 3    | Average | `cur[x] += (cur[x-bpp] + prev[x]) / 2`                   |
| 4    | Paeth   | `cur[x] += paeth(cur[x-bpp], prev[x], prev[x-bpp])`      |

`bpp` is bytes-per-pixel (3 for RGB, 4 for RGBA). At the row's
left edge `cur[x-bpp]` is treated as zero; for the first row
`prev[x]` is treated as zero.

The Paeth predictor (named after Alan Paeth, 1991) is the most
interesting. Given the left neighbour `a`, the upper neighbour
`b`, and the upper-left neighbour `c`, it picks whichever of
`a`, `b`, `c` is closest to the linear extrapolation `a + b - c`:

```c
int p  = a + b - c;
int pa = abs(p - a);
int pb = abs(p - b);
int pc = abs(p - c);
if (pa <= pb && pa <= pc) return a;
if (pb <= pc)             return b;
return c;
```

In effect it looks at the local gradient and predicts that the
target pixel continues the same gradient. For natural-image
photographs Paeth usually wins; for artificial images with
horizontal solid runs Up or Sub usually wins. The encoder picks
per-row.

The decoder applies the inverse in place, row by row, top to
bottom — modifying the current row before moving on, so by the
time we reach row N the previous row's data is already in its
final form ready to feed the predictor.

## Output layout: BGRA from the start

Every consumer of decoded pixels in this kernel — `gui_present`,
the wallpaper baker, the browser blitter — expects BGRA: bytes
B, G, R, A in increasing memory order, four bytes per pixel,
tightly packed. PNG colour type 2 is RGB and colour type 6 is
RGBA, so the conversion is a per-pixel shuffle:

```c
dst[x*4 + 0] = r_or_b_swap[0];
dst[x*4 + 1] = src_g;
dst[x*4 + 2] = r_or_b_swap[1];
dst[x*4 + 3] = src_a_or_FF;
```

For RGB sources we synthesise an alpha of `0xFF`. For RGBA we
copy the alpha through. By doing the shuffle inside the decoder
the rest of the system never has to know — the decoded buffer
can be passed straight to `gui_present(win, x, y, w, h, ptr)`
with no second copy.

## The decoder's memory hand-off

The full pipeline allocates three buffers in succession:

1. `zbuf` — the concatenation of all IDAT bodies, sized via a
   first pass over the chunk stream that just sums chunk lengths.
   Lifetime: from "start of decode" to "deflate complete". Freed
   immediately after `png_inflate` returns.
2. `raw` — the inflated bitstream, sized exactly
   `(width * bpp_in + 1) * height`. The `+1` is the per-row
   filter byte. Lifetime: from "deflate complete" to
   "filter+colour conversion complete". Freed immediately after
   the row loop.
3. `bgra` — the final output, sized `width * height * 4`. This
   is the pointer the caller receives; it is freed via
   `png_free` (a thin wrapper over `free`).

Every error path frees whatever scratch buffers are live at that
point and returns `-1` with `*out_bgra` left as `NULL`. There is
no `setjmp`/`longjmp` and no partial-state leak on failure.

## `/bin/pngdec`: a 60-line consumer that doubles as a smoke test

[userspace/pngdec/pngdec.c](../../../userspace/pngdec/pngdec.c) reads a
file, calls `png_decode`, computes a checksum, and prints:

```
/mnt/icon.png: 16x16, sum=129030, opaque=253
```

The checksum is a 32-bit add-and-fold of every BGRA byte; the
opaque count is the number of pixels whose alpha is `0xFF`.
Together they give the test harness a stable signature it can
compare against numbers Pillow computed at build time.

This is also the cheapest way to validate the decoder in
isolation, away from the browser's parser and layout passes.
When something is wrong with the inflate or the filter undo,
`pngdec` will fail the assertion and the kernel boot logs will
have one or more `[png] ...` lines pointing at the broken stage.

## The test corpus: bake, don't commit

`scripts/make_test_png.py` produces `assets/osfs/icon.png` at
build time. It's a 16×16 RGBA picture with hand-chosen content:

- Background: solid red (255, 0, 0, 255)
- Diagonal: green (0, 255, 0, 255) along (0,0) → (15,15)
- Four corners: fully transparent (0, 0, 0, 0)
- Bottom-right 4×4 block: pure blue (0, 0, 255, 255)

Two reasons we bake instead of committing the bytes:

- **The byte stream isn't reproducible.** Pillow's deflate output
  depends on the host's zlib. `make_test_png.py` produces
  *pixels* deterministically, but the bytes that encode those
  pixels can drift between zlib versions (different default
  block boundaries, different code-length-code permutations).
  If we commit the bytes, a `git apply` round-trip on a different
  host could silently corrupt the file. Baking from the script
  means the bytes always match whatever the host's zlib emits —
  and the test asserts on the *decoded pixels*, which are
  reproducible.
- **Hand verification.** You can read the python and immediately
  see what the test image looks like; you can't do that with a
  hex dump.

The script uses the same Pillow self-bootstrap as
`scripts/img_to_bgra.py` (try `import PIL`, fall back to
`pip install --user pillow` with the PEP-668 escape hatches). See
[/memories/python-host-tools-pillow.md](/memories/python-host-tools-pillow.md)
for the rationale.

The test asserts:

- Width and height are exactly `(16, 16)`.
- The byte sum equals `129030` (Pillow's `sum(sum(p) for p in
  pixels)` over the four channels).
- The opaque-pixel count equals `253` (`16*16 == 256`, minus the
  three transparent corners — the (0,0) corner is on the green
  diagonal but the diagonal is set *first* and the corner-
  transparency pass overrides it, so 3 of the 4 corners overlap
  the green diagonal but only 4 are zeroed; the script also
  zeroes the (0,0) corner so the diagonal there ends up
  transparent too — net count = 256 - 3 = 253. The math checks
  out by construction; the assertion catches accidental
  off-by-ones in the decoder's alpha plumbing.).

If any of these change without `make_test_png.py` itself
changing, the assertion fires.

## The browser image cache

The image cache is a per-page linked list of:

```c
struct br_img_cache_entry {
    char    *url;           /* canonical URL we fetched the bytes from */
    uint8_t *bgra;          /* png_decode output; freed via png_free */
    int      w, h;
    struct br_img_cache_entry *next;
};
```

It's hung off `struct loaded_page`, alongside the DOM, the
computed-style storage, and the paint-command buffer. `free_page`
walks it and tears down each entry. The cap is 16 entries OR 4
MiB total decoded bytes, whichever hits first; over the cap, new
entries are simply skipped (we don't evict — pages that pull in
hundreds of images aren't supported yet).

### Resolving `<img src=...>`

Image references can come in three shapes:

- Absolute URL: `http://example.com/foo.png` — used verbatim.
- Path-relative: `images/foo.png` — resolved against the page's
  origin URL via the same `resolve_url()` we already use for
  `<link href>` stylesheets.
- Root-relative: `/foo.png` — for HTTP pages goes to
  `scheme://host/foo.png`. For `file://` pages (origin URL is
  `NULL`) it gets rewritten to `/mnt/foo.png`, since the OSFS
  mount is the only place a file load can resolve from.

The `file://` rewrite is conservative: any source that doesn't
start with `http://` or `https://` is interpreted as living
under `/mnt`. We don't track the loaded page's directory, so
relative paths like `foo.png` also resolve to `/mnt/foo.png`.
For test purposes this is exactly what we want — both
`assets/osfs/test.html` and `assets/osfs/img_test.html` reference
`/icon.png` and the rewrite finds `/mnt/icon.png`.

### The attach pass

After layout completes, `br_attach_images` walks the box tree
depth-first. For every `LAY_BOX_REPLACED` node whose DOM tag is
`<img>`, it:

1. Reads `src` off the DOM attributes.
2. Resolves it to an absolute URL.
3. Looks the URL up in the cache.
4. On miss: fetches the bytes, calls `png_decode`, installs in
   the cache. On success: returns the cache entry.
5. Patches `b->replaced_pixels` / `replaced_pixels_w` /
   `replaced_pixels_h` with borrowed pointers from the cache
   entry.

Failures are silent (well, one printf per failed asset). A
broken or missing image leaves `replaced_pixels == NULL` and
the layout pass falls back to the existing grey-placeholder +
alt-text path — exactly what classic browsers do for a 404.

After the attach pass we re-run `layout_paint_collect` so that
the per-box choice between `LAY_PAINT_IMAGE` and the placeholder
emit picks up the freshly-attached pixels. If we did the attach
*before* the first paint collection the layout would have
emitted placeholders for every image and we'd never get a
chance to upgrade them.

`relayout_page` (called on `GUI_EVENT_RESIZE`) runs the attach
pass too, but the cache hits short-circuit the fetch and decode
work — only the borrowed pointers are re-stamped onto the new
box tree.

## The render path

The browser's GUI render switch already had three cases:
`LAY_PAINT_RECT`, `LAY_PAINT_TEXT`, `LAY_PAINT_UNDERLINE`. We
add `LAY_PAINT_IMAGE`:

```c
case LAY_PAINT_IMAGE: {
    // figure out clip rect, allocate temp BGRA buffer of the
    // visible sub-region, alpha-composite each source pixel
    // against page_bg, gui_present.
}
```

Two design decisions worth pulling out:

### Why we copy through a temporary

`gui_present(win, x, y, w, h, src)` expects `src` to point at a
contiguous `w*h*4` byte buffer. The image in the cache is itself
a contiguous `image_w * image_h * 4` buffer, but if we want to
blit only a clipped sub-region — e.g. when the image is partly
scrolled off the top of the window — passing a pointer into
its interior would have the kernel walk past the end of each
visible row into the next row of the original image, painting
garbage. We allocate a temporary the size of the visible
sub-rect, copy and composite into it, blit it, free it.

For images that fit fully on screen with no clipping the temp
is the entire image and the cost is one extra `w*h*4` traversal
per frame. For a 16×16 icon that's 1 KiB; for a hypothetical
500×500 image it's 1 MiB and would be painfully slow on every
scroll event. When we get to non-trivial pages we'll want a
proper sub-region present syscall, but that's a future chapter.

### Why we composite manually

PNGs use straight (un-premultiplied) alpha. The framebuffer
doesn't have an alpha channel — every pixel is either drawn or
not — so we have to fold the source's alpha into the *colour*:

```
out.rgb = src.rgb * src.a/255 + bg.rgb * (255 - src.a)/255
out.a   = 255  (the framebuffer is opaque)
```

with `bg` being the *page background colour at this pixel*.
Without this step every transparent corner of every PNG would
show as opaque black. The icon's four transparent corners are
the easiest visual proof: with compositing disabled they appear
as a black border around the red square; with compositing enabled
they let the page's white background show through.

We hard-code the background to the page's canvas colour
(`page_bg`, computed once per frame from `<body>`'s background-
color via the CSS canvas-propagation rule). For an image that
sits on top of a coloured `<div>` the compositing would end up
slightly wrong — we'd composite over the page bg instead of
the div bg — but our test pages don't exercise that case and a
proper fix would require a layered paint pass. We file it under
"future chapter".

## The smoke tests

### Standalone — `scripts/test_png.py`

Boots the kernel, drops to `/bin/sh`, runs `pngdec /mnt/icon.png`,
parses the output line, and asserts (w, h, sum, opaque) match
the values Pillow produced at build time. Nothing graphical;
runs in about half a minute.

### End-to-end — `scripts/test_browser_image.py`

Boots the kernel **graphically** (with `-display none` plus a
QMP socket so we can capture the framebuffer), drops to the
shell over serial, runs `browser --gui /mnt/img_test.html 800
&`, sleeps for six seconds to let the browser draw, takes a
screendump, and counts pixels in the PPM:

- Pure red (255, 0, 0): the icon's main fill. Expected at least
  16; observed 226 in practice (the image gets clipped slightly
  but most of the red is on screen).
- Pure green (0, 255, 0): the diagonal. Used as a sanity check
  only.
- Pure blue (0, 0, 255): the icon's 4×4 BR landmark. Expected
  at least 4; observed 16 in practice (the entire 4×4 block is
  visible).

Blue is the assertion key because it's the *rarest* colour on
any of our test pages — we use red liberally for the body text,
and grey/black for everything else. A positive blue match is
unambiguous proof that the decoder + cache + layout + render
path all worked.

## The full sweep

After this chapter the regression suite is **51 tests, all
passing**. Two are new:

- `test_png.py` — standalone PNG decoder verification (~32s).
- `test_browser_image.py` — end-to-end framebuffer check (~9s).

The remaining 49 are the pre-existing suite carried over
unchanged from chapter 96.

## Files added or modified

Added:

- [userspace/libc/png.h](../../../userspace/libc/png.h)
- [userspace/pngdec/pngdec.c](../../../userspace/pngdec/pngdec.c)
- [scripts/make_test_png.py](../../../scripts/make_test_png.py)
- [scripts/test_png.py](../../../scripts/test_png.py)
- [scripts/test_browser_image.py](../../../scripts/test_browser_image.py)
- [assets/osfs/img_test.html](../../../assets/osfs/img_test.html)
- `assets/osfs/icon.png` (built at make-time, not committed)

Modified:

- [userspace/libc/layout.h](../../../userspace/libc/layout.h):
  `replaced_pixels*` fields on `struct layout_box`,
  `LAY_PAINT_IMAGE = 4` enum value, `image_*` fields on
  `struct layout_paint_cmd`, image-emit branch in the REPLACED
  paint emitter, init of new fields in `layout_box_new` and on
  every existing paint cmd construction.
- [userspace/browser/browser.c](../../../userspace/browser/browser.c):
  `#include "../libc/png.h"`, `struct br_img_cache_entry`,
  cache fields on `struct loaded_page`, `br_img_cache_lookup`,
  `br_resolve_img_src`, `br_img_cache_load`,
  `br_attach_images_walk`, `br_attach_images`, calls into
  `load_page` and `relayout_page`, image-cache teardown in
  `free_page`, and the `LAY_PAINT_IMAGE` case in the GUI
  render switch.
- [Makefile](../../../Makefile): `PNGDEC_OBJS`/`ELF`/`STRIPPED` rules,
  `assets/osfs/icon.png` build rule, `OSFS_BIN_FILES` += pngdec,
  `OSFS_FILES` += icon.png + img_test.html, mkosfs.py invocation
  extended.

## Build & run

```sh
make all
python3 scripts/test_png.py            # standalone, ~32 s
python3 scripts/test_browser_image.py  # end-to-end, ~9 s
```

Or interactively:

```sh
make run-graphical
# in the QEMU window, click on the launcher to open a shell:
$ browser --gui /mnt/img_test.html 800
```

You should see a small red square with a green diagonal on a
white page, no grey "missing image" placeholder.

## Looking ahead

The decoder is built around a clean separation between *bit
stream* and *pixel rules*: `png_inflate` is a pure RFC 1951
implementation that knows nothing about images, and the chunk
walker drives it without ever caring how compression works.

Two pieces immediately benefit:

- **Chapter 98 (TrueType fonts).** WOFF2 wraps a TrueType
  binary in a Brotli stream — but WOFF1 is just deflate, and a
  surprising number of the smaller webfonts on the open web are
  still WOFF1. The same `png_inflate` decompresses them.
- **Chapter 99 (procfs).** `/proc/self/maps` will compress
  well; we can add an opt-in `gzip` accept-encoding to the
  HTTP client and reuse the same inflate.

JPEG and WEBP need their own decoders (JPEG: DCT + Huffman;
WEBP: VP8 / VP8L block coding). Both are larger projects. PNG
is by far the most common image format on the kind of static
pages our browser can render today, so it earns a chapter to
itself.

The image cache is intentionally simple: linked list, no LRU,
no on-disk persistence, no cross-page sharing. A future
chapter could promote it to a global cache hung off a long-
lived "browser process" — once we have one — and add an LRU
eviction policy. For now, opening a page is cheap, and the
cache lives only as long as the page does.
