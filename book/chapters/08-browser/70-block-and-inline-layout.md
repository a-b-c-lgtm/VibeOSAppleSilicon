# Chapter 70 — Block and inline layout

[Chapter 69](69-css-parser.md) ended with `cssparse` printing
which CSS rules apply to which DOM elements. That output is a
list of *matches*, not a *layout*: every element knew its
applicable rules, but no one had asked yet "where on the page
does this thing actually go, and what colour does it end up?"

This chapter answers both questions. By the end of it,
`/bin/layout /mnt/test_layout.html 800` parses HTML, parses CSS
(both the user-agent stylesheet and any author `<style>` blocks),
runs the cascade, builds a tree of layout boxes, performs block
and inline formatting at a viewport width of 800 px, and emits
two streams:

- a **box tree** — every layout box with its computed
  position, size, and tag.
- a **paint command stream** — back-to-front list of `RECT`,
  `TEXT`, and `UNDERLINE` ops that a renderer (M63 or any
  framebuffer drawer) executes to put pixels on screen.

Sample output (truncated; the full fixture produces 231 boxes
and 224 paint commands):

```
[DOC] viewport=800 height=954 boxes=231 paints=224
[BOX#0]   kind=BLOCK x=0   y=0   w=800 h=954 tag=#doc
[BOX#1]   kind=BLOCK x=0   y=0   w=800 h=954 tag=html
[BOX#2]   kind=BLOCK x=16  y=16  w=768 h=922 tag=body
[BOX#3]   kind=BLOCK x=16  y=16  w=768 h=22  tag=h1
[BOX#4]   kind=TEXT  x=16  y=16  w=48  h=22  text="M62"
[BOX#5]   kind=TEXT  x=80  y=16  w=96  h=22  text="Layout"
...
[BOX#88]  kind=TEXT  x=584 y=150 w=40  h=22  text="word."
[BOX#89]  kind=BLOCK x=16  y=196 w=768 h=22  tag=h2
[BOX#90]  kind=TEXT  x=16  y=196 w=60  h=22  text="Lists"
[BOX#92]  kind=BLOCK x=48  y=230 w=736 h=22  tag=li
[BOX#93]  kind=TEXT  x=48  y=230 w=32  h=22  text="• "

[PAINT#0]   RECT       x=16  y=16  w=768 h=922 color=#FFFFFFFF
[PAINT#1]   TEXT       x=16  y=16  w=48  h=22  color=#FF000080 fs=32 fw=700 fst=0 "M62"
[PAINT#36]  UNDERLINE  x=224 y=120 w=48  h=1   color=#FF0050C0
[PAINT#146] RECT       x=16  y=482 w=768 h=64  color=#FFFFFFC0
[PAINT#147] RECT       x=16  y=482 w=768 h=2   color=#FFC0A040
...
```

The chapter goal is again narrow:

- One header file: [`userspace/libc/layout.h`](../../../userspace/libc/layout.h).
- One driver binary: [`/bin/layout`](../../../userspace/layout/layout.c).
- One fixture: [`assets/osfs/test_layout.html`](../../../assets/osfs/test_layout.html).
- One test harness: [`scripts/test_layout.py`](../../../scripts/test_layout.py).

Everything that the renderer in M63 will need is now decided in
this layer. M63 just consumes the paint stream.

## The model: cascade, box tree, two-pass layout, paint stream

A real browser splits the work between several engines. Our
hobby version follows the same outline, just with each piece
shrunk to fit:

| Stage | Input | Output |
|-------|-------|--------|
| **1. Cascade** | DOM + parsed stylesheets | `layout_computed` per element |
| **2. Box tree** | DOM + computed styles | tree of `layout_box` nodes |
| **3. Block layout** | box tree + viewport width | every block box has `x/y/w/h` |
| **4. Inline layout** | inline children of each block | line boxes + per-word `BOX_TEXT` |
| **5. Paint** | sized box tree | back-to-front paint command stream |

The cascade is the only stage that needs help from M61's matcher.
The other four are self-contained inside `layout.h`. There is no
separate "render tree" — boxes ARE the render tree, with paint
done by walking them in document order.

## Three origins, sorted by `(origin, specificity, source_order)`

CSS lets multiple stylesheets contribute declarations to the
same property. The cascade picks the one with the highest
*priority tuple*. We support three origins:

```c
enum layout_origin {
    LAYOUT_ORIGIN_UA     = 0,   /* user-agent default sheet  */
    LAYOUT_ORIGIN_AUTHOR = 1,   /* author <style> blocks     */
    LAYOUT_ORIGIN_INLINE = 2,   /* style="..." attributes    */
};
```

The ordering is `INLINE > AUTHOR > UA`, matching the CSS spec
modulo the `!important` flag (which we don't yet parse). Within
the same origin, the rule with the higher specificity wins; ties
go to the rule that appeared *later* in the source — the
"source-order" tiebreak. We collect all matches, sort ascending
on the tuple, then apply each declaration in turn so the highest
priority match writes last.

The user-agent stylesheet is hard-coded at the top of `layout.h`
and applies before anything else. It establishes the baseline:
which elements are blocks, which are inline, what `<h1>` looks
like out of the box, that `<head>` and `<script>` are
`display: none`, etc. Without it our pages would render as one
long inline run.

```c
static const char *layout_ua_stylesheet =
    "html, body, div, p, h1, h2, h3, h4, h5, h6, "
    "ul, ol, li, blockquote, pre, hr, table, "
    "thead, tbody, tfoot, tr, header, footer, "
    "section, article, nav, aside, main { display: block; }\n"
    "head, script, style, title, meta, link { display: none; }\n"
    "h1 { font-size: 32px; font-weight: bold; ... }\n"
    "b, strong { font-weight: bold; }\n"
    "i, em     { font-style: italic; }\n"
    "u         { text-decoration: underline; }\n"
    "li { display: list-item; }\n"
    "ul { list-style: disc; padding-left: 32px; }\n"
    "ol { list-style: decimal; padding-left: 32px; }\n"
    /* ... */
    ;
```

Three origins, one cascade pass, no recursion, no `@media`, no
`@supports`. Plenty of room to grow.

## Inheritance — yes for `color` and `font-size`, no for `background`

Once a property is decided for an element, the children should
ask "did *I* get this property explicitly? No? Then take my
parent's value." That is *inheritance* in CSS, and it is not
universal — backgrounds, borders and the box-model dimensions do
NOT inherit, because doing so would tile the parent's background
on every nested element. `color`, `font-*`, `text-align`,
`line-height`, `white-space`, `list-style`: those do inherit.

`layout_computed_inherit()` copies the inheriting subset from
parent to child before any matching declaration is applied. The
child can then override what it cares about.

```c
static inline void layout_computed_inherit(struct layout_computed *child,
                                            const struct layout_computed *parent)
{
    child->color           = parent->color;
    child->font_size_px    = parent->font_size_px;
    child->font_weight     = parent->font_weight;
    child->font_style      = parent->font_style;
    child->text_align      = parent->text_align;
    child->text_decoration = parent->text_decoration;
    child->white_space     = parent->white_space;
    child->list_style      = parent->list_style;
    child->line_height_px  = parent->line_height_px;
    /* background, margin, padding, border, width, height: NOT inherited */
}
```

Subtle bit: `font-size` must be resolved at the moment the
declaration is applied, because `em` and `%` units depend on
*the parent's* font size. We can't defer that to a later pass —
once we see `font-size: 1.2em` we look at the parent (already
resolved) and write `font_size_px = parent->font_size_px * 1.2`
right away.

## The box tree

A layout box is a single rectangle in document order. Every box
has a kind, a style pointer back into the per-element cascade
result, and the four sides of its content rectangle (`x`, `y`,
`w`, `h`).

```c
enum layout_box_kind {
    LAY_BOX_BLOCK,       /* generates a block formatting context  */
    LAY_BOX_INLINE,      /* generates inline-level content        */
    LAY_BOX_TEXT,        /* anonymous text run                    */
    LAY_BOX_ANON_BLOCK,  /* wraps inline siblings of a block      */
    LAY_BOX_REPLACED,    /* <img>, with intrinsic size            */
    LAY_BOX_BR,          /* hard line break                       */
    LAY_BOX_BULLET,      /* generated marker for list-item        */
};
```

Three of those — `ANON_BLOCK`, `BR` and `BULLET` — are
*generated content*. They have no DOM node behind them; the
layout engine inserts them as needed. The DOM is read-only from
the layout engine's perspective.

### Anonymous-block wrapping for mixed children

Consider this fragment, common in real pages:

```html
<div>
  Some text
  <p>A paragraph</p>
  more text
</div>
```

The `<div>` is a block. Its children are: a text node, a `<p>`
(also a block), and another text node. CSS solves this by
saying: when a block has a mix of block and inline children, the
inline runs are wrapped in *anonymous block boxes* so that the
container becomes "all blocks" and each inline run can do its
own line-breaking inside its anonymous wrapper.

`layout_emit_children()` does exactly this: scan the immediate
children, look for the mix, wrap consecutive inline children in
a freshly-allocated `LAY_BOX_ANON_BLOCK`. Pure-whitespace text
nodes between two blocks are dropped (no anonymous wrapper is
created for "just spaces"), which avoids ugly empty rows.

## Block formatting and margin collapsing

`layout_block()` lays out a single block box once its parent has
told it where its top-left content corner is and how wide its
container is. It resolves the four padding values, the four
border widths, and the four margin values; figures out its own
content width (a `width: 50%` becomes `0.5 * parent_avail`,
clamped to non-negative); then recurses to lay out its children.

The interesting moment is *margin collapsing*. Two adjacent
sibling blocks are NOT separated by `margin_a + margin_b`; they
are separated by a value that depends on whether the margins are
positive or negative:

```c
static inline int layout_collapse_margins(int a, int b)
{
    int pos = (a > b ? a : b);  /* max of positives */
    int neg = (a < b ? a : b);  /* min of negatives */
    if (a >= 0 && b >= 0) return pos;
    if (a <= 0 && b <= 0) return neg;
    return pos + neg;           /* mixed: max-of-pos + min-of-neg */
}
```

This is THE classic CSS gotcha and the single weirdest rule in
the cascade. It exists so that a sequence of `<p>`s with
`margin-top: 1em` AND `margin-bottom: 1em` doesn't produce
double-spaced text — instead the bottom of one paragraph and the
top of the next collapse into a single 1em gap. Without this,
typography would be broken on every page.

We collapse ONLY between adjacent block siblings. The first
block's top margin is NOT collapsed with the parent (a full
implementation does this in some cases, but our pages look fine
without it).

## Inline formatting: line boxes, soft breaks, alignment

Inside an anonymous block (or a block whose children are all
inline), `layout_inline_format()` does the line-break work.

It walks the inline children and tokenises each `LAY_BOX_TEXT`
into a stream of *items*:

```c
enum layout_item_kind {
    LAY_ITEM_WORD,      /* a single non-breakable word run      */
    LAY_ITEM_SPACE,     /* a soft break opportunity             */
    LAY_ITEM_BR,        /* a hard break (force new line)        */
    LAY_ITEM_REPLACED,  /* <img> placeholder with intrinsic w   */
};
```

Each `WORD` knows its rendered width based on the current font
size (we use the kernel's 8x16 cell with `glyph_width = fs/2`).
SPACE items are soft break opportunities.

The wrap loop is greedy: append items until the next item would
overflow the right edge, then break at the *most recent* SPACE
item, emit a line, reset. We track `last_break` as we go.
Trailing SPACE items at the end of a wrapped line are dropped
(otherwise centred / right-aligned text gets a phantom gap).

### Alignment

Once a line's items are decided, alignment runs second:

- `left`   — items keep the position they got from the wrap.
- `center` — pen advances by `(line_width - content_width) / 2`
  before emitting items.
- `right`  — pen advances by `line_width - content_width`.
- `justify` — extra space is distributed across the SPACE items
  on this line. Formula:

  ```
  slack = line_width - content_width
  per_space   = slack / num_spaces
  leftover    = slack % num_spaces      /* given to the first
                                           `leftover` spaces  */
  ```

  We deliberately skip justification on the *last* line of a run
  (`line_end == buf.n`). A real implementation would also skip
  the last line ending with `<br>`, but our test fixture
  doesn't exercise that.

### Why we don't measure font metrics

Real browsers consult per-glyph advance metrics from the font
file. Our kernel ships a single 8x16 fixed-cell font, and we
make every other size by simple proportion: the advance of one
glyph at `font_size_px` is `font_size_px / 2`. That's not what
real text would look like, but it gives stable, predictable
layouts that are easy to test and easy to reason about. When M63
gets a real GUI framebuffer we can swap in proper metrics.

## Paint command stream

Layout produces a tree of sized boxes. The renderer needs an
ordered list of "draw this rectangle, then that text, then that
underline". `layout_paint_collect()` walks the box tree
back-to-front and emits one of three command kinds:

```c
enum layout_paint_kind {
    LAY_PAINT_RECT,       /* solid rectangle                  */
    LAY_PAINT_TEXT,       /* a text run with font + colour    */
    LAY_PAINT_UNDERLINE,  /* 1-px line under text-decoration  */
};
```

For each box we emit (in this order):

1. **Background** rectangle (if `background` is non-transparent
   and the kind is `BLOCK` / `ANON_BLOCK` / `REPLACED`).
2. **Four border rectangles** (top, right, bottom, left), one
   per side that has non-zero width AND a visible colour.
3. **Recurse into children** — so children paint OVER the
   parent's background and inside its border.
4. **Own text** (for `BOX_TEXT` and `BOX_BULLET`).
5. **Underline rectangle** if `text-decoration: underline` is
   set on the style at this point.

That ordering gives us the painter's algorithm in one pass with
no z-buffer. The renderer just executes the list top-to-bottom.

The current engine deliberately does NOT paint backgrounds for
*inline*-level elements. Real CSS does (an inline `<span>` with
a background paints a colored rectangle behind its glyphs), but
implementing that correctly requires per-line-fragment painting
and is left for a follow-up. The test harness asserts only that
the inline element's `color` cascades through.

## Replaced boxes and `<img>`

`<img>` becomes a `LAY_BOX_REPLACED`. We give it a placeholder
intrinsic size (default 32x32) and paint a light-grey RECT plus
the alt-text string inside it. There's no image decoding in the
hobby OS yet (no PNG/JPEG support), so the placeholder is the
right answer.

## What we do NOT do

Because the layout engine is the largest single piece of code in
the browser stack, it is also where it is easiest to silently
overscope. Things we explicitly skip:

- **Floats and `clear`**. Floats were the foundation of layout
  in CSS 2.x and remain the right answer for "wrap text around
  this image", but they require a side stack for the float and
  a separate measurement pass for line shortening.
- **`position: absolute / fixed / relative` (other than
  `static`)**. These create a separate containing-block stack
  that interacts non-trivially with overflow.
- **Flexbox and Grid**. Modern CSS, modern complexity.
- **Real tables**. We treat `<table>` as a generic block, which
  means cells stack vertically. Cell-grid layout is its own
  weekend project.
- **Box-sizing: border-box**. We always behave like
  `content-box`, so a `width: 50%` block with padding gets
  *wider* than 50%. The test loosens its assertion to absorb
  that.
- **Vertical-align and baselines**. All inline content sits on
  the same baseline; line height is uniform per line.
- **Generated content beyond bullets**. No `::before`,
  `::after`, no `content: "..."`.
- **CSS variables, calc(), media queries, animations**.

These omissions keep the engine under ~2.5k lines of header.
M63's `/bin/browser` will tolerate them: most static content on
the open web renders well enough at this fidelity to read.

## Defensive engineering: freestanding `mem*`

A trap that bit us partway through M62: GCC's optimiser silently
emits `memcpy` / `memset` calls for struct copies and zero-init
when the struct grows past ~64 bytes. Our userspace libc has no
`mem*` in scope, so the linker fails with
`undefined reference to 'memcpy'` — even though *our* C code
never calls it.

`layout_paint_cmd` (~80 bytes) and `layout_item` are both above
the threshold, so the failure showed up immediately. The fix is
to define minimal `static __attribute__((used))` mem-functions
inside `layout.h` itself:

```c
static __attribute__((used)) void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    for (size_t i = 0; i < n; i++) p[i] = q[i];
    return d;
}
static __attribute__((used)) void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d, v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) p[i] = v;
    return d;
}
static __attribute__((used)) void *memmove(void *d, const void *s, size_t n)
{
    /* overlap-safe two-direction copy */
    ...
}
```

`__attribute__((used))` keeps the linker happy when GCC chooses
NOT to emit a direct call (and would otherwise warn that the
function is unused under `-Werror=unused-function`). The
`static` keeps them invisible to the rest of the libc, which
will get its own real `mem*` later.

## The use-after-free that nearly shipped

While writing the inline pass we hit a bug that printed garbled
text: words like `paragraph` came out as `paragraph`, but
shorter words turned into mojibake — `swatches` became `atches`,
`within` became `ithin`, `fixture` became `ixture`. Always the
*first* character was missing.

The cause was a use-after-free. `layout_collect_inline()` was
slicing words by storing pointers into the source `BOX_TEXT`'s
`text` buffer. Then `layout_inline_format()` was *freeing* the
source `BOX_TEXT` via `layout_box_free_recursive(old)` BEFORE it
read back from those slices to allocate fresh per-word boxes.
Whatever heap recycler reused the buffer happened to overwrite
the first byte of each freed slot, hence the missing first
characters.

Fix: `layout_collect_inline()` now `malloc`s a copy of each
word's bytes and stores that in the item; the item carries an
`owns_text` flag; `layout_inline_format()` frees those copies
after the wrap loop. Cleaner and correct.

The lesson — already noted in repo memory but worth restating —
is that whenever you build temporary structures over a tree and
then mutate the tree, you HAVE to copy what you read or sequence
the operations so that no read crosses a free.

## Driver and fixture

`/bin/layout` is a tiny driver that wires it all together:

```c
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/mnt/test_layout.html";
    int viewport = argc > 2 ? my_atoi(argv[2]) : 800;

    char *src = slurp(path);

    struct html_tokenizer tok; html_tok_init(&tok, src, strlen(src));
    struct dom_doc doc; dom_init(&doc); dom_build(&doc, &tok);

    char *inline_css = layout_collect_inline_styles(&doc);

    struct layout_doc ldoc;
    layout_build_and_run(&ldoc, &doc, inline_css, viewport);

    struct layout_paint_buf pbuf = {0};
    layout_paint_collect(&ldoc.root, &pbuf);

    print_doc(&ldoc, viewport, pbuf.n);
    print_box_tree(&ldoc.root, 0);
    print_paints(&pbuf);
    return 0;
}
```

The fixture exercises every feature we just listed: an `h1`, a
bold-italic lead paragraph, a paragraph long enough to wrap, a
`<ul>` with three list items, a div with a 1px border and 12px
padding, a `.note` with a thick gold border and a pale-yellow
background, a `width: 50%` block, three text-align variants
(`left` / `center` / `right`), a small grey footer-style
paragraph, and an inline-swatch paragraph.

The harness `scripts/test_layout.py` boots the kernel, runs
`/bin/layout /mnt/test_layout.html 800`, parses the
`[DOC]` / `[BOX]` / `[PAINT]` lines, and asserts ~25 landmarks
(font sizes, colours, margins, alignment offsets, border counts,
underline emission, bullet emission, …). Run it with
`python3 scripts/test_layout.py`.

## What's next

`/bin/layout` produces a paint stream. M63's `/bin/browser`
plugs that paint stream into a real renderer:

- text-mode renderer first (so it works without a GPU and
  finally lets the browser run on the ASCII serial console),
- then a GUI window-server backend that paints actual pixels.

`/bin/browser` will also wire `httpget`'s URL handling on the
front, so we can render real pages off the network. The layout
engine doesn't know about any of that — it just hands out a list
of paint commands.

[selectors]: https://www.w3.org/TR/selectors-4/
