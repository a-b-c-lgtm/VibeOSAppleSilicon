# Chapter 100 — Intrinsic image sizing and the resize race

Chapter 99 made the browser render real-world PNGs. The first
time we pointed it at a *page* of real-world PNGs —
`https://test-images.github.io/png/202105/202105.html`, a gallery
of 400×400 colour swatches — every image rendered as a tiny
square in the top-left corner of its slot. Sometimes only a
single quadrant of colour was visible.

```
+--------------------------------------------------+
| Color Swatch: color-hex                          |
| Programmatically created as SVGs. ...            |
|                                                  |
| cs-black-000.png                                 |
| [██]    <-- this should be 400x400                |
|                                                  |
| cs-blue-00f.png                                  |
| [██]    <-- this should be 400x400                |
|                                                  |
| ...                                              |
+--------------------------------------------------+
```

The bytes were fetched. The decoder accepted them. The cache had
the right pixels. The pixels were just being painted into a
16×16 destination rectangle.

The chapter-97 layout engine reserved 16×16 for *every* `<img>`
that didn't carry explicit `width=""` and `height=""` attributes.
Our own test HTML always included both, so the bug had been
hiding in plain sight since the image cache was first wired up.
The real web omits them constantly: an `<img>` tag's intrinsic
size has come from the image itself, not from markup, ever since
HTML2.

This chapter fixes that — and in the process reveals a second
bug that only fires on window resize, plus a performance trap
where the obvious fix doubles the cost of every resize.

## What this chapter adds

- **An intrinsic-size hook in layout** ([userspace/libc/layout.h](../../../userspace/libc/layout.h)):
  the layout engine now calls a function pointer the browser
  installs before each pass, asking "do you already have the
  pixels for this `src`?" When the answer is yes, the `<img>`
  box is sized to the image's real dimensions; only on cache
  miss does it fall back to the 16×16 placeholder.
- **A two-pass load in the browser** ([userspace/browser/browser.c](../../../userspace/browser/browser.c)):
  initial load runs layout once *without* the hook (cache is
  empty), decodes the images, then runs layout a second time
  *with* the hook (cache is hot). The second pass picks up the
  real dimensions for every `<img>` that omitted explicit width
  / height.
- **The same hook on the resize path:** the chapter-94 parser
  thread also installs the hook around its layout call, so a
  resized window keeps every image at its real intrinsic size
  instead of reverting to 16×16 placeholders.
- **Cache-lookup-only image attach on the parser thread:** so the
  parser can wire `<img>` box → BGRA pointers before collecting
  paint, without mutating the cache (which only the GUI thread
  owns). This keeps the resize path to a single layout pass and
  a single paint collection.
- **A larger test image** baked at build time:
  `assets/osfs/icon_large.png` — a 64×64 four-quadrant palette
  PNG. Big enough that a 16×16 placeholder would show only the
  top-left quadrant, making "did the hook fire?" a binary
  pixel-count check.
- **A new regression test** [scripts/test_browser_intrinsic_size.py](../../../scripts/test_browser_intrinsic_size.py):
  loads a tiny HTML page whose `<img>` tag has *no* width or
  height attribute, then asserts the framebuffer contains the
  full 32×32 = 1024 pixels of each pure colour (red, green,
  blue) — anything less would mean the image had been clipped
  to the 16×16 placeholder.

After this chapter the regression sweep is **54 / 54 PASS**.

## Prerequisites

- [Chapter 69 — block and inline layout](../08-browser/069-block-and-inline-layout.md)
  defined the `LAY_BOX_REPLACED` box kind and the
  `replaced_pixels` / `replaced_pixels_w` / `replaced_pixels_h`
  fields.
- [Chapter 95 — browser parser thread](../11-smp-and-memory/095-browser-parser-thread.md)
  introduced the second CPU's parser-and-layout thread and the
  `parser_state` request / response handshake that the resize
  path uses.
- [Chapter 98 — PNG and image cache](098-png-and-image-cache.md)
  built the per-page image cache that the intrinsic-size hook
  reads from.
- [Chapter 99 — extending the PNG decoder](099-png-extended.md)
  was the immediate predecessor and the chapter under which the
  bug was discovered.

## The bug: 16×16 placeholders for every `<img>`

The layout code at the heart of the bug was a couple of lines in
[userspace/libc/layout.h](../../../userspace/libc/layout.h)'s `<img>`
handler:

```c
/* If the HTML omitted width="" / height="" attrs, we'd
 * land here with replaced_w / replaced_h == 0.  Pre-100:
 * just use a placeholder rectangle. */
if (b->replaced_w <= 0) b->replaced_w = 16;
if (b->replaced_h <= 0) b->replaced_h = 16;
```

The placeholder existed because layout runs *before* image
decoding — when the engine sizes the box, the image hasn't been
fetched yet. The browser's load pipeline is:

```
fetch HTML → parse → build CSS cascade → layout → decode images → paint
                                          ↑                       ↑
                                          | reserves 16x16 here   | paints
                                          |                       | the
                                          | (no image bytes yet)  | actual
                                                                  | pixels
                                                                  | into
                                                                  | that
                                                                  | 16x16
                                                                  | rectangle
```

The blitter ([userspace/browser/browser.c](../../../userspace/browser/browser.c)'s
`LAY_PAINT_IMAGE` case) faithfully crops the source pixels to
the destination box. A 400×400 image painted into a 16×16 box
shows the top-left 16×16 corner. For a four-quadrant test
image, that's a single pure-coloured square.

This had been broken since chapter 98 shipped. It was invisible
because every chapter-97 / chapter-98 test image was wrapped in
HTML we wrote ourselves, and we always wrote `width=""` and
`height=""` attributes. The real web doesn't.

## Why the obvious fix is wrong

The obvious fix is to delay layout until after image decode:

```
fetch → parse → decode all images → layout → paint
                                     ↑
                                     | now `<img>` boxes know
                                     | their intrinsic size
```

This is wrong for three reasons:

1. **Image fetch is slow.** A page with a dozen images would
   not lay out at all until every fetch completed. The user
   would stare at a blank window for tens of seconds — much
   worse than the current "page renders instantly, then images
   pop in."
2. **Image attach is a per-page-instance step.** The pixels
   live in the per-page image cache, which the parser thread
   (chapter 95) can't touch. Moving image attach into the
   parser would require either a lock on the cache or a copy
   of every BGRA buffer — neither acceptable.
3. **The user-visible failure mode without a placeholder is
   worse.** Without a sized box, the surrounding text would
   reflow every time an image arrived, jumping around the page
   like an early-2000s dial-up site.

So we keep the two-stage pipeline. We just need the second
layout pass to know the intrinsic sizes after the images have
arrived.

## The fix, part 1: a function-pointer hook in layout

`layout.h` is a single-TU header-only library — the browser is
the only consumer per binary. So we can add a process-global
function pointer to the header without any name-collision
risk:

```c
typedef int (*lay_img_size_fn)(const char *src, int *out_w, int *out_h,
                               void *ud);

static lay_img_size_fn lay__img_size_fn = 0;
static void          *lay__img_size_ud = 0;

static inline void layout_set_img_size_lookup(lay_img_size_fn fn, void *ud)
{
    lay__img_size_fn = fn;
    lay__img_size_ud = ud;
}
```

The contract is small: pass in the raw `src` attribute string;
the callback either writes the intrinsic w/h and returns 0
(cache hit) or returns -1 (cache miss → use the 16×16
fallback). The browser sets the hook before calling
`layout_build_and_run` and clears it afterwards, so the layout
engine itself has no permanent reference to anything in the
browser.

The `<img>` handler in `layout.h` changes from this:

```c
if (b->replaced_w <= 0) b->replaced_w = 16;
if (b->replaced_h <= 0) b->replaced_h = 16;
```

…to this:

```c
if (b->replaced_w <= 0 || b->replaced_h <= 0) {
    int iw = 0, ih = 0;
    if (lay__img_size_fn &&
        lay__img_size_fn(src, &iw, &ih, lay__img_size_ud) == 0 &&
        iw > 0 && ih > 0) {
        if (b->replaced_w <= 0) b->replaced_w = iw;
        if (b->replaced_h <= 0) b->replaced_h = ih;
    }
}
if (b->replaced_w <= 0) b->replaced_w = 16;  /* unchanged fallback */
if (b->replaced_h <= 0) b->replaced_h = 16;
```

Explicit `width=""` / `height=""` attributes still win — `iw`
and `ih` are only consulted when the corresponding `replaced_w`
/ `replaced_h` is still zero. Sites that DO carry HTML5 sizing
attributes (Hacker News with its 18×18 favicons, for example)
are unaffected.

## The fix, part 2: two-pass layout on initial load

The browser's callback walks the per-page image cache:

```c
static int br_layout_img_size_cb(const char *src, int *out_w, int *out_h,
                                  void *ud)
{
    struct loaded_page *p = (struct loaded_page *)ud;
    char abs[1024];
    if (br_resolve_img_src(p->url, p->origin_url, src,
                            abs, sizeof(abs)) != 0)
        return -1;
    struct br_img_cache_entry *e = br_img_cache_lookup(p, abs);
    if (!e) return -1;
    *out_w = e->w;
    *out_h = e->h;
    return 0;
}
```

`load_page()` already had the structure for a second layout
pass — `relayout_page()` was added in chapter 95 for window
resize. We just install the hook around it:

```c
/* First pass: hook is unset, every <img> gets 16x16 placeholder.
 * That's fine — we're only running layout here to know where to
 * place text and which <img> srcs to fetch.  We'll redo it. */
build_page_from_html(p, viewport);

/* Fetch and decode every <img> the page references.  Populates
 * the per-page image cache. */
br_attach_images(p);

/* Second pass: hook is live, every cached <img> contributes
 * its real dimensions to layout. */
if (p->img_cache_count > 0) {
    relayout_page(p, viewport);
}
```

`relayout_page()` itself installs and clears the hook, so the
two-pass logic lives in one place rather than being scattered
across every layout call site.

## The fix, part 3: the resize path's parser thread

This is where the chapter took a left turn into much more
interesting territory.

After parts 1 and 2 the initial load is correct. But on the
first resize, every image collapses back to 16x16.

The cause was the **parser thread** from chapter 95. It runs
layout on CPU 1 in response to resize events:

```c
static void parser_thread_main(void *arg)
{
    /* ... */
    layout_build_and_run(&local_ldoc, dom_root(dom),
                          css, css_len, viewport);
    layout_paint_collect(&local_ldoc, &local_pb);
    /* publish new_ldoc + new_pb under the mutex */
}
```

There's no hook installed around its `layout_build_and_run`.
Every resize re-laid out every `<img>` at the 16×16 fallback,
because the layout engine had no way to ask anyone for the
intrinsic sizes.

The fix is straightforward — pass the page pointer through the
parser request and bracket the layout call with hook set /
clear:

```c
struct parser_state {
    /* ... */
    struct loaded_page *req_page;   /* new in 100 */
};

/* In parser_request_relayout: */
ps->req_page = p;

/* In parser_thread_main: */
layout_set_img_size_lookup(br_layout_img_size_cb, page);
int rc = layout_build_and_run(&local_ldoc, dom_root(dom),
                               css, css_len, viewport);
layout_set_img_size_lookup(0, 0);
```

The hook is a process-global function pointer, but only one
layout pass is ever in flight at a time — the GUI thread calls
`parser_wait_idle()` before any path that itself runs layout —
so the set/clear is race-free.

This made layout correct on resize. But it surfaced a third
bug.

## Third bug: empty boxes after resize

After the parser-thread fix, resized images had the right size
but no pixels. Just empty rectangles with placeholder borders.

The fresh box tree the parser publishes has `replaced_pixels =
NULL` on every `<img>` — image attachment is a separate step
that walks the box tree and patches in BGRA pointers from the
cache. The parser doesn't run that step (image cache mutation
must stay on the GUI thread), so the published paint buffer
encoded every `<img>` as a placeholder rectangle.

The first fix attempted was to re-run attach + paint-collect
on the GUI thread after each parser swap:

```c
/* in parser_absorb_completion, after the swap: */
if (p->img_cache_count > 0) {
    br_attach_images(p);
    layout_paint_buf_destroy(&p->pb);
    layout_paint_collect(&p->ldoc, &p->pb);
}
```

It worked. The images came back. And resize was *very slow*.

Image attach is a tree walk. Paint collect is another tree walk.
Doing both on the GUI thread after every resize meant we were
walking the box tree four times per resize (parser: layout +
paint-collect; GUI: attach + paint-collect) when the right
answer is two.

The real fix is to do the cache-lookup-only attach on the
parser thread, BEFORE it collects paint. The parser doesn't
need to mutate the cache; every image on a resize is a cache
hit because the GUI's initial load already populated it. So
we split `br_attach_images_walk` to take a `lookup_only` flag:

```c
static void br_attach_images_walk(struct loaded_page *p,
                                  struct layout_box *b,
                                  int lookup_only)
{
    /* ... walk to each LAY_BOX_REPLACED <img> ... */
    struct br_img_cache_entry *e = br_img_cache_lookup(p, abs_url);
    if (!e && !lookup_only)
        e = br_img_cache_load(p, abs_url);  /* GUI thread only */
    if (e) {
        b->replaced_pixels   = e->bgra;
        b->replaced_pixels_w = e->w;
        b->replaced_pixels_h = e->h;
    }
}

/* Old entry point — GUI thread, may mutate cache. */
static void br_attach_images(struct loaded_page *p) { ... lookup_only = 0 ... }

/* New entry point — parser thread, lookup only. */
static void br_attach_images_root(struct loaded_page *p,
                                  struct layout_box *root)
{
    br_attach_images_walk(p, root, /*lookup_only=*/1);
}
```

The parser now does:

```c
layout_set_img_size_lookup(br_layout_img_size_cb, page);
layout_build_and_run(&local_ldoc, dom_root(dom), css, css_len, viewport);
layout_set_img_size_lookup(0, 0);

br_attach_images_root(page, local_ldoc.root_box);
layout_paint_collect(&local_ldoc, &local_pb);
```

…and the GUI thread's absorb step is back to a clean
no-extra-work swap. One layout pass, one attach, one paint
collection — all on the parser thread (CPU 1) — and the GUI
thread (CPU 0) just swaps pointers under the mutex.

## Why this is safe

The chapter-94 contract was that the parser thread reads `dom`
and `author_css` read-only, and the GUI thread guarantees both
outlive the parser pass via `parser_wait_idle()` before any
`free_page` / `load_page`. We're extending the parser's read-only
window to include the per-page image cache. Three things make
that safe:

1. **Lookup-only.** The parser never calls `br_img_cache_load`,
   `br_img_cache_install`, or any other cache mutator. It only
   reads pointers out of an already-populated `br_img_cache_entry`.
2. **No eviction during a parser pass.** The cache only grows
   during `load_page` (initial fetch) or during explicit user
   navigation, both of which already drain the parser via
   `parser_wait_idle()`. Resize never evicts.
3. **BGRA buffers outlive both threads.** Once an entry is
   in the cache its `bgra` pointer is stable until the page
   is freed; `free_page` already drains the parser before any
   `free()` call.

So the parser reading `e->bgra` while the GUI is doing anything
*other* than `free_page` cannot race. And `free_page` is, by
chapter-94 contract, preceded by `parser_wait_idle`.

## The test

Existing 16×16 test images couldn't distinguish "fully rendered
at intrinsic size" from "clipped to the 16×16 placeholder" —
they'd look identical either way. So we bake a new 64×64
four-quadrant palette PNG via `scripts/make_test_png.py
--kind=large_palette`:

```
+---------+---------+
|         |         |
|   red   |  green  |     each quadrant 32x32 = 1024 px
|  32x32  |  32x32  |
|         |         |
+---------+---------+
|         |         |
|  blue   |  white  |
|  32x32  |  32x32  |
|         |         |
+---------+---------+
```

And a tiny HTML wrapper with **no width/height attrs**:

```html
<html><body>
  <p>Image intrinsic-size test</p>
  <img src="/icon_large.png" alt="missing big icon" />
</body></html>
```

Pre-fix: this page renders the image as 16×16, showing only
the top-left quadrant. The framebuffer contains ~256 pure-red
pixels and 0 pure-green / pure-blue / pure-white pixels.

Post-fix: the image renders at its true 64×64. Each quadrant
contributes 1024 pixels of pure colour.

The test [scripts/test_browser_intrinsic_size.py](../../../scripts/test_browser_intrinsic_size.py)
boots graphically, navigates to the page, screendumps the
framebuffer, counts pure pixels of each colour, and asserts
each count is at least 600:

```
on-screen pixel counts: red=1024 green=1024 blue=1024
PASS: 64x64 intrinsic-size image fully rendered (red=1024 green=1024 blue=1024)
```

The 600 threshold is enough slack for any text or chrome a
future browser polish change might add above the image, but
far below what a clipped 16×16 placeholder could produce.

## File-name lesson: 19-byte OSFS limit

A small detour: the test page was originally named
`img_intrinsic_test.html`. `make` failed:

```
name too long (> 19 bytes): img_intrinsic_test.html
```

The osfs filename field is 19 bytes (the rest of the 24-byte
slot holds size and flags). The limit was internalised for
binaries but not for HTML assets. Renamed to `intrinsic.html`
(14 bytes) and moved on.

Worth recording because every future test asset will hit the
same wall: chapter 11's mkosfs design is locked in at this
length, and there's no friendlier error than a one-line `name
too long`.

## Performance picture

For a page with 12 images, single resize event:

| Path                             | Layout passes | Tree walks   | Where        |
|----------------------------------|---------------|--------------|--------------|
| Pre-100                          | 1             | 1 (layout)   | parser (CPU1)|
| 100, naïve absorb fix            | 2             | 4 (lay+pc on parser, attach+pc on GUI) | both |
| 100, parser-side attach (final) | 1             | 2 (lay, attach, pc on parser) | parser (CPU1) |

The naïve fix didn't just double the cost — it doubled the
*GUI-thread* cost, which is exactly the cost chapter 95 spent
six months getting off the GUI thread in the first place. The
"feels slow" report from the user was the right signal at the
right time: chapter 95's whole architectural promise had been
silently undone by a four-line fix.

## Looking ahead

The intrinsic-size hook is the smallest mechanism that
solves the immediate bug. Several near-term chapters can lean
on it:

- **`<picture>` and `srcset`** would let the hook return one of
  several candidate sources based on viewport width.
- **Lazy loading** (`loading="lazy"`) would have the hook
  return -1 (no size known yet) while the image is below the
  fold, deferring its fetch.
- **CSS `aspect-ratio`** would let the hook fall back to a
  ratio-only answer when one dimension is unknown.

All would slot into the same hook signature; only
`br_layout_img_size_cb`'s body would change.

The wider lesson is the one chapter 95 was about: as soon as you
have a parser thread, every new mechanism added on the GUI side
has to ask itself "does the parser also need this?" — and any
answer that involves the GUI doing extra work after the parser
publishes is almost certainly the wrong answer. This is the
chapter-94 architecture quietly enforcing its own discipline.
