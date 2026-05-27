# Chapter 67 — DOM construction

[Chapter 66](066-html-tokenizer.md) gave us a flat stream of
HTML tokens. By the end of that chapter, `/bin/htmltok` could walk
[`assets/osfs/test.html`](../../../assets/osfs/test.html)
and print one `[DOCTYPE]`, `[START]`, `[END]`, `[CHARS]`,
or `[COMMENT]` line per token, with entities decoded and
`<script>` / `<style>` correctly handled as raw text. Those
tokens are the *input* to a tree builder; they are not yet
the tree.

This chapter folds the stream into a Document Object Model.
By the end of it, `/bin/htmldom /mnt/test.html` walks the
same fixture and prints an indented tree of nodes:

```
[DOC]
  [DOCTYPE] "html"
  [ELEM] "html" lang="en"
    [ELEM] "head"
      [ELEM] "meta" charset="utf-8"
      [ELEM] "title"
        [TEXT] "tokenizer fixture"
      [ELEM] "style"
        [TEXT] "\n  body { color: red; }\n"
    [ELEM] "body"
      [COMMENT] " a comment in the middle "
      [ELEM] "h1"
        [TEXT] "Hello, & goodbye"
      [ELEM] "p" class="intro" id="lead"
        [TEXT] "The quick brown fox <jumps> over the lazy dog."
      [ELEM] "ul"
        [ELEM] "li"
          [TEXT] "one"
        [ELEM] "li"
          [TEXT] "two"
        [ELEM] "li" data-x="42" disabled=""
          [TEXT] "three"
      [ELEM] "img" src="/icon.png" alt="x"
      [ELEM] "script"
        [TEXT] "\nvar x = 1 < 2 && 3 > 0; // not html, won't be re-tokenized\n"
      [ELEM] "p"
        [TEXT] "price: $ & done."
[TOTAL] 43 nodes, 538 bytes input
```

That tree is the input to the next chapter, which hangs CSS
on it and computes a layout box for every node.

The chapter goal is, again, deliberately narrow:

- One header file: [`userspace/libc/dom.h`](../../../userspace/libc/dom.h).
- One driver binary: [`/bin/htmldom`](../../../userspace/htmldom/htmldom.c).
- The same fixture: [`assets/osfs/test.html`](../../../assets/osfs/test.html).
- One test harness: [`scripts/test_html_dom.py`](../../../scripts/test_html_dom.py).

No CSS, no layout, no rendering. Each of those gets its own
chapter.

## Why a DOM at all

It's tempting to skip the tree and let the layout engine
read tokens directly. Browsers don't do that, and after
about ten minutes you can see why:

1. **Layout needs random access.** Once you've placed the
   `<h1>`, you sometimes have to revisit it (e.g. if a
   later `<style>` tag changes its computed style, or if a
   `position: absolute` descendant needs to know its
   nearest positioned ancestor). A flat token stream
   doesn't give you that.

2. **CSS selectors are tree queries.** `p.intro > a` only
   makes sense against parent / child / sibling pointers.
   You cannot evaluate it without a tree.

3. **The HTML parser itself wants a tree.** Some of the
   trickier HTML rules ("if you see a `<li>` and the
   nearest open `<li>` ancestor isn't yet closed, close it
   first") are easier to express as "look at the open
   element stack" than as "remember the last few tokens".
   Even our tiny builder ends up using an open-element
   stack.

The DOM is the seam between two very different concerns:
the parser's "who's open right now" world, and the layout
engine's "give me every node matching `div.foo` whose
parent is a flex container" world.

## The model

`userspace/libc/dom.h` defines five node types:

```c
enum dom_node_type {
    DOM_NODE_DOCUMENT = 1,
    DOM_NODE_ELEMENT  = 2,
    DOM_NODE_TEXT     = 3,
    DOM_NODE_COMMENT  = 4,
    DOM_NODE_DOCTYPE  = 5,
};
```

Every node is one struct:

```c
struct dom_node {
    int                type;
    char              *tag;            /* ELEMENT only */
    char              *text;           /* TEXT/COMMENT/DOCTYPE; not NUL-term */
    size_t             text_len;
    struct dom_attr   *attrs;          /* ELEMENT only, source order */
    struct dom_node   *parent;
    struct dom_node   *first_child;
    struct dom_node   *last_child;
    struct dom_node   *next_sibling;
};

struct dom_attr {
    char              *name;
    char              *value;
    struct dom_attr   *next;
};
```

A few choices to flag explicitly, because they are
load-bearing for everything that follows:

- **`first_child` + `last_child` + `next_sibling`** (no
  `prev_sibling`). Append is the dominant operation during
  parsing, so we keep it `O(1)` by tracking `last_child`.
  We don't need fast prepend or fast remove yet; CSS
  selector matching only walks forward / down. If the
  layout engine later needs `prev_sibling` we'll add it
  then.

- **No `id` field on `dom_node`.** `id` is just one of
  many attributes. We look it up via `dom_node_attr(n,
  "id")` rather than caching it. The CSS engine
  will probably add a hash from `id` to node, but that's a
  layer-up concern.

- **`text` is not NUL-terminated.** It's `(char *, size_t)`
  for the same reason `html_token::data_len` is: HTML text
  contains literal NUL bytes very rarely but legally, and
  pretending otherwise creates lurking bugs the moment we
  try to render a binary attachment.

- **`malloc` per node, not arena.** A full document is
  hundreds to thousands of nodes; the user heap we set up
  earlier gives us 32 MiB and our test pages don't come close. Arena
  packing is a worthwhile optimisation for a real browser
  but it isn't the lesson of *this* chapter, and arenas
  badly complicate `dom_node_free` for the case where a
  partially-built tree has to be torn down on OOM.

The `struct dom` itself caches the canonical ancestors:

```c
struct dom {
    struct dom_node *root;             /* always DOCUMENT */
    struct dom_node *html;             /* root's <html> child */
    struct dom_node *head;             /* <html>'s <head>   */
    struct dom_node *body;             /* <html>'s <body>   */

    size_t           n_nodes;

    struct dom_node *open_stack[64];   /* the parser's open-element stack */
    int              open_top;
};
```

`open_stack` is the stack of currently-open elements, the
HTML5 spec's central data structure. We size it at 64
entries, which is comfortably more than any realistic page
nesting depth (the real spec has no fixed limit but real
pages don't usefully nest 64 deep). On overflow we return
`-1` and the parser bails. We treat that as a fatal parse
error rather than papering over it; if a real page hits the
limit we'd rather know.

## The implicit-skeleton problem

Token streams don't always announce their containers.
Real pages routinely look like:

```html
<!DOCTYPE html>
<title>hi</title>
<p>hello
```

There's no `<html>`, no `<head>`, no `<body>`, no closing
tags. Every browser still produces a tree like:

```
DOCUMENT
  DOCTYPE html
  html
    head
      title — "hi"
    body
      p — "hello"
```

The HTML5 spec describes this as a state machine over
twenty-odd insertion modes ("initial", "before html",
"before head", "in head", "after head", "in body", "in
table", "in caption", ...). Each mode has its own table
of "what to do with this token". It is conceptually clean
and it is also more code than this chapter needs.

We use a simplified rule that fits in a page of C. There
are three lazy-initialised slots — `d->html`, `d->head`,
`d->body` — and three `dom_ensure_*` helpers that create
them if absent. Then the `start` handler routes:

```c
if (tag == "html") ensure_html(); merge_attrs;
if (tag == "head") ensure_head(); merge_attrs;
if (tag == "body") ensure_body(); merge_attrs;
if (tag is in HEAD_ONLY)        ensure_head();
else if insertion point at root/html/head: ensure_body();
```

`HEAD_ONLY` is a static set of six tags — `meta`, `title`,
`link`, `style`, `base`, `script` — that the spec says
belong in `<head>`. Anything not in that set, encountered
while we're still at document/html/head level, forces a
body switch. Most real pages do have an explicit `<body>`,
but the rule keeps us correct on the ones that don't.

The `merge_attrs` step is what makes `<html lang="en">`
work even though the auto-skeleton already created the
`<html>` element. The merge keeps existing attributes (so
the page can't override an attribute we somehow added
ourselves) and appends any new attributes from the token in
source order. This matches the spec's behaviour for
"in body, html start tag" and the analogous rules for head
/ body.

## Void elements and self-closing

A subset of HTML elements never have content. The spec
calls them "void":

```
area base br col embed hr img input link meta param
source track wbr
```

`dom.h` keeps this list in `dom_is_void_tag`. When the
parser sees `<img src="...">` it creates an `img` element,
attaches it as a child of the current open element, and
then **does not push it onto the open stack**. There is no
matching `</img>` to ever pop it. This is a one-line check
in `dom_handle_start`:

```c
if (dom_is_void_tag(t->tag_name) || t->self_closing) {
    return 0;   /* attached, but not pushed */
}
```

The `t->self_closing` part covers the `<br/>` syntax. HTML5
treats the trailing slash as a no-op on most elements but
honours it on void and foreign-namespace elements. We're
permissive — any tag with a trailing slash is treated as
self-closing — because in practice the only place
`<.../>` shows up in real-world HTML is on void elements
or on SVG / MathML content we aren't parsing yet.

## End tags: pop down to match

The end-tag handler is the simpler of the two:

```c
static int dom_handle_end(struct dom *d, const struct html_token *t)
{
    int idx = dom_find_open(d, t->tag_name);
    if (idx < 0) return 0;        /* stray end tag */
    while (d->open_top > idx) (void)dom_pop(d);
    return 0;
}
```

Find the nearest matching open element on the stack. If
there isn't one, treat the tag as a stray and drop it (this
is what real browsers do; there is no notion of a
"mismatched close tag" error). If there is one, pop down to
*and including* it. This also implicitly closes any
intervening misnested elements — `<b><i></b>` will close
both `<i>` and `<b>` on the `</b>`. That's wrong by the
spec's "Adoption Agency Algorithm" rules (which would
re-open the `<i>`) but it's correct enough for hand-written
HTML and for most generated HTML, and the misbehaviour is
visible in the printed tree if anyone needs it.

## The whitespace-at-root rule

Token streams are noisy with whitespace. In our fixture
every line break between top-level tags becomes a `[CHARS]`
token containing `"\n"`. If we attached those naively, the
DOCUMENT node would have ten or twenty whitespace TEXT
children interleaved with `<html>` and `<!DOCTYPE>`. That's
not a wrong tree per se — the whitespace really is there —
but it's noise: layout will collapse it later anyway.

The compromise: at insertion points equal to `d->root` or
`d->html`, **drop pure-whitespace text runs**. Inside
`<head>` and `<body>` we keep them, because inline layout
needs them ("the quick brown fox" and "the quick brown fox
" are two different things to a renderer). The check is
literally:

```c
if (cur == d->root || cur == d->html) {
    int all_ws = 1;
    for (int i = 0; i < t->data_len; i++)
        if (t->data[i] != ' ' && t->data[i] != '\n' &&
            t->data[i] != '\r' && t->data[i] != '\t') {
            all_ws = 0; break;
        }
    if (all_ws) return 0;
}
```

A non-whitespace text run at root/html level *also* forces
a body switch — that handles the `Hello world` case for a
page that opens with bare text.

## What we omit on purpose

The grown-up HTML5 parser does several things we don't:

- **Foster parenting.** If a stray `<p>` appears inside a
  `<table>`, the spec moves it to *before* the table. We
  attach it where it lands. Tables are a chapter of their
  own and we don't render any yet.

- **The Adoption Agency Algorithm.** Misnested formatting
  elements (`<b><i></b></i>`) get reopened in the spec.
  We just pop. Good enough for our fixtures.

- **Quirks mode and DOCTYPE-driven box-model switching.**
  We always behave as if the page is in standards mode.
  The DOCTYPE is parsed and stored, but doesn't change
  any later behaviour.

- **Template, fragment parsing, foreign content (SVG /
  MathML).** None of these are needed for the text-heavy
  pages we'll render later.

- **Speculative parsing while a stylesheet loads.** We're
  not async; everything is one straight-line `dom_build`
  call.

If any of these later become necessary they're additive —
the parsing model isn't disturbed by adding a foster-parent
check, for example. We deferred them, we didn't bake the
codebase into a corner.

## The driver

[`/bin/htmldom`](../../../userspace/htmldom/htmldom.c) is
the test harness's puppet. It mirrors `/bin/htmltok` from
the previous chapter:

1. `slurp(path)` reads the file into a heap buffer.
2. `html_tok_init(&tz, buf, n)` initialises the tokenizer.
3. `malloc(sizeof(struct html_token))` heap-allocates the
   ~12 KiB token scratch — same stack-discipline rule as
   the tokenizer chapter. The 16 KiB user stack can't hold a 12 KiB
   automatic.
4. `dom_init(&dom)` builds the bare DOCUMENT root.
5. `dom_build(&dom, &tz, scratch)` runs the tokenizer to
   completion, calling the appropriate handler for each
   token.
6. `print_node(dom_root(&dom), 0)` walks the tree
   depth-first, printing one indented `[TYPE] ...` line
   per node. Control bytes in text are escaped (`\n`, `\t`,
   `\r`, `\\`, `\"`) so each node fits on one line.
7. `dom_destroy(&dom)` recursively frees the tree.

That's the whole binary. About 180 lines of C, no shared
state, deterministic output.

## The test harness

[`scripts/test_html_dom.py`](../../../scripts/test_html_dom.py)
boots a clean kernel under HVF, waits for the shell prompt,
runs `htmldom /mnt/test.html`, and greps the output for
landmark substrings:

- `[DOC]` and `[DOCTYPE] "html"` at the top.
- `[ELEM] "html" lang="en"` — confirms the merge_attrs
  step wired the page's `<html>` attribute onto the
  auto-created element.
- `[ELEM] "meta" charset="utf-8"` and
  `[ELEM] "title"` with the title text — confirms the
  HEAD_ONLY routing.
- `[TEXT] "Hello, & goodbye"` — confirms the tokenizer's
  entity decoding survived tree construction.
- `[ELEM] "p" class="intro" id="lead"` — confirms
  multi-attribute elements, including single-quoted
  values, made it into the tree.
- `[ELEM] "li" data-x="42" disabled=""` — confirms
  unquoted attribute values and boolean attributes are
  represented as empty-string values (which is what CSS
  expects).
- `[ELEM] "img" src="/icon.png" alt="x"` — confirms void
  elements are attached without children and without
  pushing onto the stack.
- `[ELEM] "script"` followed by a single `[TEXT]` child
  containing `var x = 1 < 2`, with no inner `[ELEM]` —
  confirms `<script>` rawtext from the tokenizer round-
  trips into the tree.
- `[TOTAL] 43 nodes, 538 bytes input` at the end.

The first `make build/disk.img` rebuilds the kernel and
the disk image including `/bin/htmldom`; the test harness
then drives a fresh kernel boot in headless QEMU. A green
run looks like:

```
$ python3 scripts/test_html_dom.py
--- captured 8047 bytes -> /tmp/m60.log ---
PASS: DOM construction — all checks green
```

The full transcript is dropped at `/tmp/m60.log` for the
chapter and for any future debugging.

## A capacity bump along the way

While wiring `/bin/htmldom` into the disk image we hit a
prosaic limit: the OSFS-1 directory is two sectors, and
two sectors / 32 bytes-per-entry = 32 file slots. The previous
chapter already filled 32, so this chapter hit:

```
too many files (max 32)
make: *** [build/disk.img] Error 1
```

Fix: bump the OSFS directory from two sectors to four.
That moves `OSFS_MAX_FILES` to 64 and shifts
`OSFS_FIRST_DATA_SECTOR` from 3 to 5 (one superblock plus
four directory sectors). Both the kernel reader
(`kernel/core/osfs.h`) and the image builder
(`scripts/mkosfs.py`) had to agree.

This is the kind of layout decision you want to be able to
change cheaply once. Because the OSFS-1 superblock has
never advertised the directory size — the kernel hard-codes
it from `OSFS_DIR_SECTORS` — bumping it required no
on-disk-format change and no migration: the next image
build just produces the new layout. Worth flagging as a
lesson: hard-coding a layout constant *in two places that
both reference the same header* is exactly the symmetry
that makes such bumps trivial. Hard-coding it in two
*different* headers, or worse, in code that doesn't read
the header at all, would have made this one a slog.

## What we built

| File | Lines | Purpose |
|---|---|---|
| [`userspace/libc/dom.h`](../../../userspace/libc/dom.h) | ~520 | Header-only DOM model + tree builder. |
| [`userspace/htmldom/htmldom.c`](../../../userspace/htmldom/htmldom.c) | ~180 | `/bin/htmldom` test driver. |
| [`scripts/test_html_dom.py`](../../../scripts/test_html_dom.py) | ~180 | Headless test harness with landmark assertions. |
| [`kernel/core/osfs.h`](../../../kernel/core/osfs.h) (edit) | +6 | Bump `OSFS_MAX_FILES` 32 → 64. |
| [`scripts/mkosfs.py`](../../../scripts/mkosfs.py) (edit) | +3 | Match the new on-disk layout. |
| [`Makefile`](../../../Makefile) (edit) | +18 | Build / strip / OSFS-pack `htmldom`. |

A real browser's DOM is enormously more capable than what
we just built — events, mutation observers, shadow trees,
custom elements, the Range API. None of those are in our
hot path. What we built is just enough to feed a CSS
engine and a layout box generator. That's the next two
chapters.

## What this enables

- **CSS parser.** With a tree of elements carrying
  `class`, `id`, and `style` attributes, we can write a
  rule-based selector matcher.

- **Box layout.** With every element in a tree, we
  can compute width / height / position by walking the
  tree top-down (block flow) and bottom-up (intrinsic
  sizing).

- **`/bin/browser`.** The end-to-end pipeline:
  fetch URL → tokenize → DOM → CSS → layout → paint into
  a virtio-gpu window.

[whatwg]: https://html.spec.whatwg.org/multipage/parsing.html
