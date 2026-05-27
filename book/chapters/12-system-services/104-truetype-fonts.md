# Chapter 104 -- TrueType fonts in the kernel

For 41 chapters the screen has been painted with the same VGA
8x16 bitmap font from chapter 45. It was a great call: a 256-byte
header file rendered every UI surface in the system from boot
banner to browser. It also meant that everything on screen looked
like 1985 -- monospace, blocky, no AA. This chapter swaps it for
a real TrueType rasteriser and a real proportional font (DejaVu
Sans at 16 px). The integration is invasive in the way most font
changes are: not because the API moves, but because *the
assumption that every character is 8 pixels wide* turns out to be
hiding in surprisingly many places, and the only way to find them
all is to break the assumption and then chase the consequences.

The whole thing is about 880 lines of self-contained kernel C
(`kernel/device/ttf.c`), a 757 KB font blob embedded in the
kernel ELF, and one new syscall (`gui_measure_text`) that exists
specifically to let userspace ask "how wide *would* this string
be?" now that the answer is no longer a multiplication.

## Prerequisites

* [Chapter 45 -- virtio-gpu and the framebuffer](../05-devices/045-virtio-gpu-framebuffer.md)
  introduced `struct bitmap_font`, `text_draw_string`, and the
  8x16 VGA glyph table. Most of this chapter generalises that
  API; the original surface still works as a fallback.
* [Chapter 47 -- the window manager](../06-gui/047-window-manager-and-gui-syscalls.md)
  for the `wm_draw_text` syscall path that this chapter rewrites
  internally. The syscall ABI doesn't change; the implementation
  switches from "blit 16 rows of 8 bits, set/clear" to "look up
  glyph, alpha-blend per-pixel".
* [Chapter 26 -- argc/argv](../05-devices/026-argc-argv.md) (which
  also surfaces in [chapter 103](103-guard-pages.md)'s
  postscript). This chapter rediscovers the same "16 KiB of
  kernel stack is not enormous" lesson the hard way -- so the
  diagnostic trick from chapter 103 applies here too.

## What TrueType actually is, at the byte level

A `.ttf` file is a flat archive of tables. The first 12 bytes
say "SFNT version 1.0 (or 'true'), N tables follow". Each table
entry is a 4-byte tag, a checksum, an offset, and a length. The
seven tables this chapter parses are:

| tag    | purpose                                                |
|--------|--------------------------------------------------------|
| `head` | units-per-em, the `loca` format flag                   |
| `maxp` | number of glyphs                                       |
| `cmap` | codepoint -> glyph-index lookup tables                 |
| `hhea` | ascender / descender / line-gap, count of long hmetrics|
| `hmtx` | per-glyph advance widths and left side bearings        |
| `loca` | glyph index -> byte offset into `glyf`                 |
| `glyf` | actual glyph outlines                                  |

Every multi-byte integer in the file is big-endian. We read them
through three helpers (`rb_u16`, `rb_s16`, `rb_u32`) and never
read them any other way -- the rest of the file is just walking
table layouts described in the OpenType spec. The "look up the
table by tag" loop is ten lines:

```c
static const uint8_t *find_table(const uint8_t *blob, uint32_t blob_size,
                                 const char tag4[4])
{
    uint16_t num_tables = rb_u16(blob + 4);
    uint32_t want = tag(tag4);
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = blob + 12 + i * 16;
        if (rb_u32(rec) == want) {
            uint32_t off = rb_u32(rec + 8);
            return blob + off;
        }
    }
    return NULL;
}
```

### A glyph is a list of contours of Bezier curves

In the `glyf` table each glyph is either *simple* (a list of
contours, each a closed loop of points alternating "on-curve"
and "off-curve") or *compound* (composed of references to other
glyphs with affine transforms). This chapter handles simple
glyphs only; compounds fall through to the `.notdef` placeholder.
That's not a real loss for our font -- DejaVu Sans encodes the
ASCII range with simple glyphs.

A point is `(x, y, on_curve_bit)`. Three consecutive points
`(P0, C, P2)` where `C` is off-curve form a quadratic Bezier
segment with `C` as the control point. Two consecutive off-curve
points imply an on-curve point at their midpoint (the "implicit
midpoint" rule). The point list is delta-encoded with a small bag
of flag bits to make short deltas cheap; parsing that takes about
60 lines of `parse_simple_glyph` and isn't conceptually hard, it
just has six branches for `(short|long) x (positive|negative|zero)
x (same as previous)`.

### Coordinates: FUnits, Q16.16, and 26.6

Three coordinate systems show up in the same file. To stop them
running together in the code, this chapter is strict about names
and units:

* **FUnits** -- the integer units stored in the file. The em-box
  is `head.unitsPerEm` FUnits wide and tall (typically 1000 or
  2048).
* **Q16.16 fixed point** -- the scale factor `pixel_size *
  65536 / units_per_em`. Stored as a single `int32_t` per face.
* **26.6 subpixel pixels** -- once we multiply a FUnit by the
  Q16.16 scale, we shift right by 10 to land in 26.6 (i.e. 1
  pixel = 64). Everything downstream of `parse_simple_glyph`
  works in 26.6. The Bezier flattener emits 26.6 segments; the
  rasteriser scans in 26.6; only the final per-pixel alpha is in
  Q0.

Why 26.6? Because the AA supersampler is 4x4 -- 16 subpixel
samples per output pixel -- and 64 / 4 = 16 subpixel rows per
output row, which is a power of two and fits naturally into the
fixed-point math. (FreeType uses the same convention for the same
reason.)

## The pipeline, end to end

The whole rasteriser is a single 5-stage funnel. Each stage is a
file-static function in `kernel/device/ttf.c`:

```
   codepoint
      |  cmap_lookup    (format 4 subtable: codepoint -> glyph-index)
      v
   gid
      |  glyph_offset / glyph_size      (loca -> byte range in glyf)
      v
   raw glyf bytes
      |  parse_simple_glyph             (decode point list, flip Y)
      v
   struct outline (in 26.6 subpixel pixels)
      |  flatten_outline                (quad Beziers -> line segments)
      v
   struct seg_list (line segments in 26.6)
      |  rasterise                      (4x4 supersample, even-odd fill)
      v
   alpha bitmap (one byte per output pixel)
      |  cache + return
      v
   struct glyph_info { pixels, bitmap_w, bitmap_h,
                       left_bearing, top_bearing, advance }
```

Two stages deserve closer attention.

### Bezier flattening

A quadratic Bezier `B(t) = (1-t)^2 P0 + 2(1-t)t C + t^2 P2` for
`t` in `[0, 1]`. There are two standard ways to turn one into a
list of straight lines: adaptive subdivision (stop when the
control point is close enough to the chord) and fixed-step
sampling. We do the second:

```c
static void flatten_quad(struct seg_list *L, ...)
{
    const int steps = 16;          /* 16 sub-segments per Bezier  */
    int32_t px = x0, py = y0;
    for (int i = 1; i <= steps; i++) {
        int32_t t  = (i * 1024) / steps;
        int32_t it = 1024 - t;
        int64_t a = (int64_t)it * it;
        int64_t b = (int64_t)2 * it * t;
        int64_t c = (int64_t)t * t;
        int64_t qx = (a*x0 + b*cx + c*x2) / ((int64_t)1024 * 1024);
        int64_t qy = (a*y0 + b*cy + c*y2) / ((int64_t)1024 * 1024);
        seg_add(L, px, py, (int32_t)qx, (int32_t)qy);
        px = (int32_t)qx; py = (int32_t)qy;
    }
}
```

Sixteen steps is wasteful for the straight-ish curves in an
uppercase 'M' and tight for the bowls in an 'O'. At 16 px the
difference between adaptive and 16-step uniform is invisible:
sub-pixel deviation on the worst curve, well below the AA
resolution. The cost of being lazy here is a few hundred extra
line segments per page of text, which the rasteriser walks once
per glyph and then forgets.

The implicit-midpoint walk lives one function up
(`flatten_outline`). It builds a synthetic point array twice the
size of the original, inserting a midpoint between every
off-off pair, then traverses (on, off, on) triples emitting one
Bezier each, and (on, on) pairs emitting one line each. If the
contour happens to start on an off-curve point, we rotate the
array so the first point is on-curve -- that's where the
file-scope `g_flat_rot` scratch buffer comes from.

### Scanline rasterisation with 4x4 supersampling

For each output pixel row, we walk SS=4 subpixel rows inside it.
For each subpixel row, intersect every flattened segment with the
horizontal line at that y, collect the x crossings, sort them,
and walk them in pairs -- pair `(x0, x1)` brackets an "inside"
span by the even-odd fill rule. For every subpixel column inside
the span, increment a per-output-pixel sample counter.

At the end of the row, each pixel's sample count is in `[0, 16]`,
and the alpha is `count * 255 / 16`. That's grayscale anti-
aliasing in 30 lines of code. Even-odd works here because
TrueType outlines are non-overlapping for the BMP glyph set; the
non-zero winding rule would also work and gives the same answer.

```c
for (int py = 0; py < bmp_h; py++) {
    for (int i = 0; i < bmp_w; i++) row_count[i] = 0;
    for (int sy = 0; sy < SS; sy++) {
        int32_t y = org_y + py*64 + sy*(64/SS) + (64/SS/2);
        /* collect crossings, sort, walk pairs, bump row_count */
    }
    for (int x = 0; x < bmp_w; x++) {
        uint8_t c = row_count[x];
        alpha[py*bmp_w + x] = (uint8_t)(((uint16_t)c * 255u) / SS2);
    }
}
```

A single horizontal `row_count` buffer is enough because we
finish each row before moving on. The crossings array is a tiny
stack buffer of 64 int32s -- at 16 px a glyph has at most ~8
crossings per scanline.

## Wiring the font into the kernel

The kernel doesn't have a filesystem at this point in boot, so
the font ships as part of the kernel ELF. We use the same
`objcopy -I binary` trick the ramfs uses (chapter 12):

```make
FONT_BLOB_SRC := assets/fonts/DejaVuSans.ttf
FONT_BLOB_OBJ := build/font/DejaVuSans.ttf.o

$(FONT_BLOB_OBJ): $(FONT_BLOB_SRC)
	mkdir -p $(@D)
	cd $(<D) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	    $(<F) $(abspath $@)
```

`objcopy` synthesises three symbols around the blob:
`_binary_DejaVuSans_ttf_start`, `_binary_DejaVuSans_ttf_end`, and
`_binary_DejaVuSans_ttf_size`. `ttf.c` declares the first two as
`extern const uint8_t[]` and treats the byte range between them
as the source font.

`font.c` learns one new entry point:

```c
int font_init_ttf(void);
```

…which calls `ttf_init_default(&g_ttf_font)` and, on success,
points `font_get_default()` at the TTF face instead of the
bitmap. If TTF init fails (corrupt blob, missing table, malloc
failure) the bitmap stays the default. That fallback is why the
kernel always has *some* font available even on a stripped boot
where the TTF blob isn't linked in.

`main.c` calls `font_init_ttf()` after `fb_init()` succeeds:

```c
if (fb_init() == 0) {
    font_init_ttf();     /* Chapter 104: upgrade default font to TTF */
    paint_boot_banner();
    ...
}
```

The order matters only insofar as the banner is the first
visible text on screen; with `font_init_ttf` ahead of it, the
banner renders in DejaVu Sans rather than the 8x16 bitmap. The
banner itself didn't change.

## The 16-KiB-stack story

The first integration crashed at boot before anything was drawn
on screen. The serial console showed:

```
[svc] FATAL: non-SVC sync exception from EL1
        ESR_EL1 = 0x0000000096000045
        FAR_EL1 = 0x0020202000202044
        ELR_EL1 = 0x00000000401046a8
```

`FAR_EL1 = 0x0020202000202044`. Stare at the hex for a moment:
`0x20` is ASCII for space. The kernel was trying to read or
write through a "pointer" whose every byte was ASCII space --
i.e. a buffer that should have contained a `space` glyph's
rasterised pixels, being interpreted as a pointer. The
`addr2line` for `ELR_EL1` pointed at `blit_glyph_alpha` in
`text.c`. So we were past parsing, past flattening, past
rasterisation, and into the *blit* -- and at that point the
`struct glyph_info` we were reading was clearly garbage.

The cause was on the kernel stack, not in the font data:

```c
static int rasterise_glyph(...)
{
    struct outline_pt  buf[MAX_POINTS * 2];   /* 512 * 2 * 12 = 12288 B */
    struct seg_list    segs;                  /* 2048 * 16    = 32768 B */
    ...
}
```

`struct outline_pt` is 12 bytes per point, 512 points, doubled
for the implicit-midpoint scratch -- 12 KB. `struct seg_list` is
16 bytes per segment, 2048 segments -- 32 KB. Plus the parse
scratch, plus saved registers. The kernel stack is 16 KiB total
(one page). The first call to `rasterise_glyph` blew clean
through the stack into whatever was beneath, including (in this
build) the `struct glyph_info` that the caller had just
constructed. The subsequent blit read it back as a garbage
pointer, and `FAR_EL1` told us we'd tried to dereference the
glyph's rasterised pixel pad -- the source bytes for a `space`
glyph -- as if it were the address of a `struct glyph_info`.

The lesson is the one from
[chapter 103](103-guard-pages.md) only inverted:
a guard page would have produced a `[kernel stack overflow]`
banner instead of the address-shaped ASCII garbage. We don't yet
have guards on the *kernel* stacks (chapter 103 added them only
to user stacks), so the diagnostic trick is the next best thing:
**when FAR_EL1 looks like printable ASCII when split into bytes,
suspect that something rasterising or formatting strings has
overrun a buffer.**

The fix was to demote the offending arrays from automatics to
file-scope statics:

```c
/* Singletons -- see comment at top of file. */
static struct outline    g_outline;
static struct outline    g_outline_flipped;
static struct outline_pt g_flat_buf[MAX_POINTS * 2];
static struct outline_pt g_flat_rot[MAX_POINTS * 2];
static struct seg_list   g_segs;
```

This is exactly what `stb_truetype.h` does for the same reason.
It costs ~70 KB of permanent .bss in exchange for never blowing
the stack again. The trade-off is that rasterisation is no
longer reentrant: the kernel must rasterise one glyph at a time.
That's fine for us -- all drawing is on a single GUI cooperative
path, no IRQ rasterises -- but it's the kind of thing future
work needs to remember (see "what's deferred", below).

The header comment at the top of `ttf.c` calls this out so it
isn't easy to miss:

```c
/* The TTF rasteriser uses several large working buffers ...
 * Together they total ~64 KB -- far too much for the kernel
 * stack (16 KB). All rasterisation happens from a single
 * drawing path (no nested calls, no interrupts that rasterise),
 * so we promote the buffers to file-scope statics. ... */
```

## The "8" was hiding everywhere

The TTF integration above was the easy half. The kernel renders
glyphs at proportional widths now, but ~40 chapters of userspace
code was written when `font.cell_width == 8` was a load-bearing
constant. So every place a userspace app multiplied "the length
of this string in characters" by 8 to find a pixel position was
suddenly off by however much DejaVu Sans differed from 8 px per
glyph at that particular string.

The pattern showed up in three flavours:

1. **Centring** -- `tx = x + (w - n * 8) / 2;` where `n` is the
   character count. The right edge of the label drifted left of
   centre by the amount of "slack" lost to the now-narrower 'i's
   and 'l's, and a 'm'-heavy label crept off the right side.
2. **Caret position** -- `caret_x = field_x + cursor * 8;` for
   the URL field, notepad's insertion point, and the gui_term
   prompt. The caret no longer landed under the letter it was
   "between".
3. **Truncation** -- `if (label_chars * 8 > max_w) truncate;` in
   the taskbar and notify popup. Some labels were truncated when
   they didn't need to be (proportional widths fit), others
   weren't (proportional widths overflowed).

The clean fix is the same in all three places: ask the kernel
how wide the string *actually* is. There was no syscall that did
that, so we added one. Numbered 52 in `enum syscall_nr`:

```c
SYS_GUI_MEASURE_TEXT = 52,   /* Chapter 104 */
```

The kernel side is a 20-line handler in `wm.c` that mirrors the
loop in `wm_draw_text` exactly:

```c
long wm_measure_text(const char *s_user)
{
    char buf[256];
    long got = copy_string_from_user(buf, ..., sizeof(buf));
    if (got < 0) return -EFAULT;
    const struct bitmap_font *font = font_get_default();
    uint32_t w = 0;
    for (size_t i = 0; buf[i]; i++) {
        if (buf[i] == '\n') break;
        struct glyph_info gi;
        if (font_get_glyph(font, (uint32_t)(uint8_t)buf[i], &gi) != 0) continue;
        w += gi.advance ? gi.advance : font->cell_width;
    }
    return (long)w;
}
```

Mirroring the loop is important: if `wm_measure_text` and
`wm_draw_text` ever disagreed about the advance of a character,
centring would be subtly off forever. By sharing the same path
through `font_get_glyph` they can't.

Userspace gets an inline wrapper in `userspace/libc/syscall.h`:

```c
static inline int gui_measure_text(const char *s)
{
    long r = _svc1(SYS_GUI_MEASURE_TEXT, (long)(uintptr_t)s);
    return r < 0 ? 0 : (int)r;
}
```

Then the app sweep replaces every `n * 8` with a call. The
launcher's centring becomes:

```c
int pix_w = gui_measure_text(g_buttons[i].label);
int tx    = BTN_X + (BTN_W - pix_w) / 2;
```

The browser's caret becomes:

```c
char saved = url[cursor]; url[cursor] = '\0';
int caret_x = field_x + gui_measure_text(url);
url[cursor] = saved;
```

The taskbar truncates by probing one more character at a time
until it would overflow rather than computing
`label_chars * 8 > max_w`:

```c
for (int n = 1; n <= len; n++) {
    char saved = label[n]; label[n] = '\0';
    if (gui_measure_text(label) > max_w) { label[n-1] = '\0'; break; }
    label[n] = saved;
}
```

All three of these patterns are now in the codebase and tested.
The set of files touched in the sweep:

* [userspace/browser/browser.c](../../../userspace/browser/browser.c)
  -- URL field, scroll indicator, toolbar button labels.
* [userspace/notepad/notepad.c](../../../userspace/notepad/notepad.c)
  -- caret position and width.
* [userspace/gui_term/gui_term.c](../../../userspace/gui_term/gui_term.c)
  -- prompt caret.
* [userspace/launcher/launcher.c](../../../userspace/launcher/launcher.c)
  -- button label centring.
* [userspace/taskbar/taskbar.c](../../../userspace/taskbar/taskbar.c)
  -- label truncation, clock centring.
* [userspace/notify/notify.c](../../../userspace/notify/notify.c)
  -- toast text truncation.
* [userspace/libgui/save_dialog.c](../../../userspace/libgui/save_dialog.c)
  -- file-list right-alignment, field positioning, caret.
* [userspace/hellogui/hellogui.c](../../../userspace/hellogui/hellogui.c)
  -- demo label width.

The browser's *layout pass* (`userspace/layout/`) still uses
`LAYOUT_BASE_GLYPH_W = 8` for the box tree -- a faithful layout
that consults real font metrics is still future work and is
called out in the
[chapter 69 sidebar](../08-browser/069-block-and-inline-layout.md).
What the chapter-102 changes guarantee is that text *positioning*
inside an already-laid-out box is correct.

## ASCII-only, for now

There's one more trap that fell out of the migration, severe
enough to deserve its own section. Pre-chapter-102 the bitmap
font silently rendered any byte outside the printable ASCII
range as a blank cell -- the 8x16 glyph table only had entries
for 0x20..0x7E. The TTF font has a `.notdef` glyph (a small empty
rectangle) for every codepoint not in the cmap, and the
rasteriser draws it. So bytes that used to disappear quietly
now show up as visible boxes.

This bit us in three places on day one:

* The "Save As" dialog title: `"Save As -- New Folder"` was
  written with a U+2014 em-dash. The em-dash is three bytes in
  UTF-8 (`0xE2 0x80 0x94`), and our cmap lookup is byte-by-byte
  -- so each of the three bytes individually fell through to
  `.notdef`, producing three little boxes where the dash should
  have been.
* `top`'s output: `printf("frame %lu -- refresh /1s\n", ...)`
  -- another em-dash, three boxes.
* `heaptest`'s and `badpoke`'s failure messages -- same em-dash
  pattern.

All of these came from comments-style writing leaking into
rendered strings. The em-dash is fine inside `/* ... */` because
the preprocessor strips comments before the linker ever sees
them; it only causes trouble in string literals that flow into
`gui_draw_text` or `printf` to a GUI terminal.

The fix is mechanical: use `--` or `-` instead of `\u2014`,
straight `"..."` instead of curly `"..."`. The rule: keep
rendered strings ASCII-only until real Unicode support lands.

If/when we want real Unicode in rendered text the work is fairly
contained: decode UTF-8 in `cmap_lookup`'s caller, walk all
cmap segments rather than just the BMP-low one, and advance by
codepoint width rather than by byte. The rasteriser doesn't care
what codepoint it's rasterising. That's deferred to a future
chapter; for now, ASCII-only.

## Files changed

* [userspace/fontd/ttf.h](../../../userspace/fontd/ttf.h) -- public
  `ttf_init_default`, `ttf_get_glyph` declarations.
* [userspace/fontd/ttf.c](../../../userspace/fontd/ttf.c) -- the
  parser, flattener, rasteriser, cache; 888 lines.
* [kernel/device/font.h](../../../kernel/device/font.h) --
  `enum bitmap_font_kind`, `struct glyph_info`, the kind-aware
  `font_get_glyph` and `font_init_ttf` declarations.
* [kernel/device/font.c](../../../kernel/device/font.c) -- lazy
  `font_init_ttf`, bitmap-kind synthesis of `struct glyph_info`.
* [kernel/device/text.c](../../../kernel/device/text.c) --
  `text_alpha_blend`, `blit_glyph_alpha`, the per-glyph advance
  loop in `text_draw_string` / `text_measure`.
* [kernel/core/wm.c](../../../kernel/core/wm.c) -- `wm_blend_pixel`,
  the per-pixel alpha blend in `wm_draw_text`, the new
  `wm_measure_text` handler.
* [kernel/core/wm.h](../../../kernel/core/wm.h) --
  `wm_measure_text` declaration.
* [kernel/core/syscall.h](../../../kernel/core/syscall.h) --
  `SYS_GUI_MEASURE_TEXT = 52`.
* [kernel/core/syscall.c](../../../kernel/core/syscall.c) --
  dispatcher case and `sys_gui_measure_text` thunk.
* [kernel/core/main.c](../../../kernel/core/main.c) --
  `font_init_ttf()` call inside the GPU init block.
* [Makefile](../../../Makefile) -- `kernel/device/ttf.c` in
  C_SRCS, `FONT_BLOB_SRC`/`FONT_BLOB_OBJ` rule, blob object on
  the link line.
* [userspace/libc/syscall.h](../../../userspace/libc/syscall.h) --
  `SYS_GUI_MEASURE_TEXT` enum entry and `gui_measure_text` inline
  wrapper.
* The eight userspace apps listed in the sweep above.
* [assets/fonts/DejaVuSans.ttf](../../../assets/fonts/DejaVuSans.ttf)
  -- the font (757 KB) and its BSD-style licence file.
* [scripts/test_truetype.py](../../../scripts/test_truetype.py)
  -- new regression test (see next section).

Total: ~880 lines of new kernel C, ~30 lines of new syscall
surface, ~80 lines of cumulative app changes, ~140 lines of test
harness, one 757 KB font asset.

## How we test it

The regression suite gets one new entry,
[scripts/test_truetype.py](../../../scripts/test_truetype.py),
that boots the system to desktop, screendumps the launcher area,
and checks two properties:

1. **Grayscale anti-aliasing is happening.** Sample the row of
   pixels across the centre of a launcher button label. Count
   pixels whose colour is neither the launcher background
   (BG_BGRA) nor the launcher text colour (TEXT_BGRA), within a
   small tolerance. With the old bitmap font there are zero such
   pixels (it's a 1-bit font, every pixel is exactly fg or bg).
   With TTF there are dozens -- the partial-coverage edge pixels
   of every glyph. We require at least 4 of them on the row.
2. **Glyphs land off the 8-pixel grid.** Scan the same row for
   transitions from background to foreground, and confirm at
   least one transition is at an x-coordinate that isn't a
   multiple of 8. With a monospace bitmap font every transition
   is at a multiple of 8 by definition; with a proportional font
   most aren't. This is the cheapest possible "the layout is
   actually proportional" assertion.

Together these two checks fail loudly if anyone ever wires the
bitmap font back in as the default by mistake, or if the
proportional-advance plumbing regresses.

The harness follows the same shape as
[scripts/test_clock.py](../../../scripts/test_clock.py): boot
QEMU with QMP+serial sockets, wait for the shell prompt,
screendump, sample pixels. The clock test is the closest model.

## What's deferred

The deliberate "no" list, in priority order for follow-up work:

* **Userspace, not kernel.** The rasteriser is a self-contained
  ~880 lines that has no reason to be in ring 0. The right
  long-term home is a userspace font server that publishes
  pre-rasterised glyph atlases to shared memory; the WM looks
  up `(face, size, codepoint)` and blits from the atlas. That's
  [chapter 115](../14-userspace-services/115-userspace-font-server.md)
  in the planned chapters.
* **Multiple sizes.** We ship one face, one size. The cache key
  is `codepoint` only. Generalising to `(font, codepoint, size)`
  is mostly bookkeeping; the rasteriser already takes
  `pixel_size` as a constant that we could lift to a parameter.
* **Subpixel positioning.** For an LCD layout we'd store glyphs
  at three horizontal offsets (1/3, 2/3, 0) and pick the closest
  at render time. That's the next visual upgrade after AA.
* **Hinting.** TrueType ships a stack machine (the "bytecode
  interpreter") that runs per-glyph instructions to nudge points
  onto the pixel grid at small sizes. At 16 px DejaVu Sans
  without hinting is acceptable; at 11 px it would be muddy.
  We don't have a bytecode interpreter and we don't plan to add
  one -- the modern alternative is to ship a font that was
  designed without hinting in mind (DejaVu Sans qualifies).
* **Compound glyphs.** A few accented Latin glyphs and most CJK
  ideographs are compound -- references to other glyphs with
  affine transforms. We render them as `.notdef`. ASCII gets
  through with no compounds; once Unicode arrives, this will
  matter.
* **Real Unicode.** The cmap lookup is byte-by-byte; multi-byte
  UTF-8 sequences each fall through to `.notdef`. The fix has
  three pieces (UTF-8 decode, all-segments cmap walk, codepoint-
  advance in `wm_draw_text` / `wm_measure_text`) and is the
  obvious next step once anyone wants emoji or non-Latin scripts.

What this chapter does ship is the load-bearing piece: a font in
the kernel that draws non-monospace, anti-aliased glyphs from
real outline data, with a small new syscall to keep every
userspace consumer correctly aligned. That was enough to retire
the 8x16 grid from the system UI without retiring any of the
chapters that built on top of it.
