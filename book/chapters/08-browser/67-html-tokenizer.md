# Chapter 67 — The HTML tokenizer

[Chapter 66](66-url-and-http-parser.md) finished the
networking story: by the end of M58 we could fetch a URL,
parse the response, follow a redirect, and print a
structured summary. The bytes-on-the-wire problem is solved.
What we don't yet have is any way to *read* those bytes as
HTML.

This chapter ports the first brick of that wall: an HTML5
tokenizer in `userspace/libc/html.h`. By the end of it,
`/bin/htmltok /mnt/test.html` walks our test fixture and
emits a clean stream of `[DOCTYPE]`, `[START]`, `[END]`,
`[CHARS]`, `[COMMENT]` tokens — entity-decoded, attribute-
parsed, with `<script>` and `<style>` correctly handled as
raw text. That stream is the input to the DOM construction
chapter that follows.

The chapter goal is deliberately narrow:

- One header file: [`userspace/libc/html.h`](../../../userspace/libc/html.h).
- One driver binary: [`/bin/htmltok`](../../../userspace/htmltok/htmltok.c).
- One fixture: [`assets/osfs/test.html`](../../../assets/osfs/test.html).
- One test harness:
  [`scripts/test_html_tokenizer.py`](../../../scripts/test_html_tokenizer.py).

No DOM, no CSS, no layout, no rendering. Each of those gets
its own chapter.

## Why a tokenizer first

HTML is a strict-precedence-free format compared to most
programming languages — there are no parens to balance, no
semicolons to terminate, and the syntax is forgiving by
design. That sounds like an excuse to write a single
combined parser that produces the DOM in one pass. **We do
the opposite, deliberately.**

A tokenizer is the right scope for an OS-development
chapter for three reasons:

1. **It is a finite state machine you can trace by hand.**
   The WHATWG specification names every state explicitly
   and the transitions are local. You can run the machine
   on paper for a small input and predict the output. That
   makes it easy to write tests that are tight enough to
   catch regressions but loose enough to survive future
   tweaks (we test for landmark tokens, not exact byte
   counts).

2. **The output is a flat stream, not a tree.** Trees are
   the next chapter's problem. By keeping the tokenizer's
   output flat, we can drive it from a tiny test program
   and grep its output with a Python harness. We do not
   need a DOM yet to validate the tokenizer; we need only
   to print every token on its own line.

3. **It cleanly separates the "weird HTML rules" from the
   "weird DOM rules".** Things like `<script>` not being
   re-tokenized are a tokenizer concern; things like
   `<table>` foster-parenting are a tree-construction
   concern. Conflating them turns a 600-line tokenizer plus
   a 600-line tree builder into a 3000-line ball of mud.

## Scope: what we do and don't tokenize

Real HTML is huge — the [WHATWG spec][whatwg] runs to
thousands of pages, almost all of it about pathological
edge cases that exist because two different browsers
happened to disagree in 1997. We are aiming at a hobby
browser that can render real-world articles, not a
conformance test runner.

What `html.h` *does*:

- The data state, tag-open, tag-name, end-tag-open
  states.
- Before / in attribute name, before / in attribute value
  (double-quoted, single-quoted, and unquoted forms).
- Self-closing start tags (`<br/>`, `<img ... />`).
- Bogus comments (`<!foo>`) and proper comments
  (`<!-- ... -->`).
- DOCTYPE token (we keep just the name slug, e.g. "html").
- The two raw-text elements that *every* page on the open
  web uses: `<script>` and `<style>`. Inside one of these
  the tokenizer suspends tag recognition until it sees
  the matching end tag.
- Character references: the named set
  `{ amp, lt, gt, quot, apos, nbsp }` plus numeric
  `&#NN;` and hex `&#xHH;` forms.

What `html.h` *deliberately doesn't*:

- Parse errors. The spec's "parse error" objects exist
  for browser conformance testing. We silently recover.
- `<![CDATA[ ... ]]>` sections. These only occur in
  foreign-content (SVG/MathML), which we don't support.
- Foreign-content parsing for SVG and MathML.
- The full HTML5 named-entity table (≈ 2000 entries). The
  six we keep cover essentially every real-world page;
  unknown references pass through verbatim, which is
  exactly how browsers cope with mistakes today.

We are also explicitly not building an XML or XHTML
tokenizer. XHTML's well-formedness rules are stricter,
which sounds easier but means more states (no implicit
self-closing for `<br>`, no unquoted attributes, etc.).
The web is HTML5; we follow the web.

[whatwg]: https://html.spec.whatwg.org/

## File and module layout

The tokenizer lives in
[`userspace/libc/html.h`](../../../userspace/libc/html.h)
as a header-only library, following the same
single-translation-unit convention we have used since
[`printf.h`](../../../userspace/libc/printf.h):

> Include from one `.c` per binary. No allocation. No
> external symbols beyond what the caller already pulls in.

This convention has paid off repeatedly. Every userspace
binary inlines exactly the parser fragments it actually
uses, dead code is dropped by the linker, and we do not
have a libc archive to keep up to date. The cost is that a
program that uses two parsers (say, the URL parser and the
HTML parser) carries two copies of the small `tolower` /
`isspace` helpers — the bytes are negligible.

## The token model

Every call to `html_tok_next()` fills a caller-owned
`struct html_token`:

```c
enum html_tok_type {
    HTML_TOK_NONE     = 0,
    HTML_TOK_CHARS    = 1,    /* text run, entity-decoded */
    HTML_TOK_START    = 2,    /* <tag attr=...>  (or self-closing) */
    HTML_TOK_END      = 3,    /* </tag> */
    HTML_TOK_COMMENT  = 4,
    HTML_TOK_DOCTYPE  = 5,
    HTML_TOK_EOF      = 6,
};

struct html_attr {
    char  name[HTML_ATTR_NAME_MAX];   size_t name_len;
    char  value[HTML_ATTR_VALUE_MAX]; size_t value_len;
};

struct html_token {
    int    type;
    char   tag_name[HTML_TAG_NAME_MAX];   size_t tag_name_len;
    int    self_closing;
    int    n_attrs;
    struct html_attr attrs[HTML_MAX_ATTRS];
    char   data[HTML_DATA_MAX];           size_t data_len;
};
```

Three observations are worth pulling out:

- **One struct, all token shapes.** Instead of a tagged
  union, every field exists on every token, and `type`
  picks which fields are meaningful. This makes the
  caller's switch cleaner (`t->tag_name` for `START`/`END`,
  `t->data` for `CHARS`/`COMMENT`/`DOCTYPE`) and lets us
  reuse one heap allocation across millions of calls.
  Bytes are not the bottleneck; we'll see in a moment why
  layout is.

- **Names are lower-cased in place.** `<P CLASS="X">`
  yields `tag_name = "p"`, attribute name `"class"`. The
  HTML spec is case-insensitive for ASCII tag and attr
  names; lowering once at tokenize time means the DOM and
  CSS layers can compare with a cheap byte equality.

- **The token is ~12 KiB.** That is the reason we malloc
  it: see [Chapter 66](66-url-and-http-parser.md)'s
  "user-stack discipline" section. A 16 KiB user thread
  stack with one SVC frame and one IRQ frame in flight has
  no room for a single 12 KiB local. The token belongs on
  the heap.

The fixed array sizes are deliberately generous and small
at the same time:

| Constant | Value | Why |
|----------|-------|-----|
| `HTML_TAG_NAME_MAX`    | 64   | Real tags top out at `blockquote` |
| `HTML_ATTR_NAME_MAX`   | 64   | `data-controller-something` style |
| `HTML_ATTR_VALUE_MAX`  | 512  | URLs in `href`/`src` |
| `HTML_DATA_MAX`        | 4096 | One paragraph of text per CHARS |
| `HTML_MAX_ATTRS`       | 16   | Real elements have < 8; 16 has slack |

Everything is treated as silent truncation rather than a
hard error. This matches browser practice: a giant
attribute or a paragraph longer than 4 KiB does not break
the page; it just gets cut off in the tokenizer's output.
The DOM layer never sees the truncation as a structural
event.

## The state machine

The tokenizer keeps a tiny state struct:

```c
struct html_tokenizer {
    const char *src;
    size_t      src_len;
    size_t      pos;
    int         state;          /* HTML_S_DATA / HTML_S_RAWTEXT / HTML_S_DONE */
    char        ends_with[16];  /* for raw text: lowercase end-tag name */
    size_t      ends_len;
};
```

Three states are enough because we collapse the WHATWG
state graph wherever the substates are local. For example,
the spec splits "tag open state", "tag name state",
"before attribute name state", "attribute name state", etc.
We fold all those into one inline `htm_parse_tag()` routine
that returns when it has hit either `>` or `/>`. The result
is more imperative, less prose-spec-faithful, but easier to
read and easier to test.

The pull-style loop in `html_tok_next()` is small enough to
quote in full:

```c
static int html_tok_next(struct html_tokenizer *tz, struct html_token *out)
{
    htm_token_reset(out);

    if (tz->pos >= tz->src_len || tz->state == HTML_S_DONE) {
        out->type = HTML_TOK_EOF;
        tz->state = HTML_S_DONE;
        return 0;
    }

    if (tz->state == HTML_S_RAWTEXT) {
        /* Slurp text until "</ends_with" (case-insensitive). */
        out->type = HTML_TOK_CHARS;
        size_t p = tz->pos;
        while (p < tz->src_len) {
            if (tz->src[p] == '<' && htm_rawtext_end_at(tz, p)) break;
            htm_putc(out->data, &out->data_len, HTML_DATA_MAX, tz->src[p]);
            p++;
        }
        tz->pos = p;
        tz->state = HTML_S_DATA;
        tz->ends_len = 0;
        if (out->data_len == 0) return html_tok_next(tz, out);
        return 1;
    }

    /* Data state. */
    if (tz->src[tz->pos] == '<') return htm_parse_tag(tz, out);

    /* Coalesce a run of plain text, decoding entities. */
    out->type = HTML_TOK_CHARS;
    size_t p = tz->pos;
    while (p < tz->src_len && tz->src[p] != '<') {
        if (out->data_len >= HTML_DATA_MAX - 1) break;
        if (tz->src[p] == '&')
            htm_decode_entity(tz->src, tz->src_len, &p,
                              out->data, &out->data_len, HTML_DATA_MAX);
        else
            out->data[out->data_len++] = tz->src[p++];
    }
    tz->pos = p;
    return 1;
}
```

`htm_parse_tag()` is the bulky one — about a hundred lines —
because it has to deal with all the in-tag substates: the
tag name, optional attributes, optional values in three
quote styles, optional self-closing slash, and the special
case for comments and DOCTYPE. The shape is:

```c
if (src[p+1] == '!') {
    /* "<!--" -> comment, "<!doctype" -> doctype, else bogus comment */
} else if (src[p+1] == '/') {
    /* "</name>" end tag */
} else if (htm_is_alpha(src[p+1])) {
    /* "<name attr=val ...>" or "<name .../>" start tag */
    /* ... attribute loop, name/value parsing ... */
    /* if name in {script,style}: arm rawtext mode */
} else {
    /* "<" not followed by tag-start char: emit literal '<' */
}
```

The crucial detail is at the very end of the start-tag
branch: if the tag we just emitted is `<script>` or
`<style>`, we set the tokenizer's state to `HTML_S_RAWTEXT`
and copy the lowercase tag name into `ends_with`. The next
call to `html_tok_next()` then emits all the bytes between
here and the matching close tag as one big `CHARS` token,
without trying to recognise any inner `<` as a tag opener.
That is the entire mechanism by which JavaScript code
containing `1 < 2` doesn't break the parser.

## Entity decoding

We support the named set
`{ amp, lt, gt, quot, apos, nbsp }` plus numeric forms
`&#NN;` and `&#xHH;`. Anything else is passed through as
the literal `&` followed by the surrounding bytes — exactly
how every real browser handles unknown references that are
missing their semicolon.

```c
static void htm_decode_entity(const char *src, size_t src_len, size_t *pos,
                              char *buf, size_t *len, size_t cap)
{
    /* Numeric? */
    if (... starts with "&#" ...) {
        /* parse decimal or hex digits, swallow optional ';' */
        if (v < 0x80)         emit (char)v;
        else if (v == 0xA0)   emit ' ';   /* nbsp */
        else                  emit '?';   /* high-bit -> placeholder */
        return;
    }
    /* Named: case-insensitive match against the small set */
}
```

The "ASCII or `?`" choice is the most arbitrary one in the
file. The real fix is UTF-8 handling in the font, which is
its own milestone — when we get there, this branch becomes
"emit the bytes of the codepoint" instead of "emit `?`".

The semicolon-optional matching is intentional. If we
required the `;`, then `&amp text` (a real pattern produced
by some static-site generators) would render literally as
`&amp text`. Browsers gave up on requiring the trailing
`;` more than a decade ago, and so do we.

## Coalescing character data

`html_tok_next()` keeps consuming non-`<` bytes into one
`CHARS` token until it hits `HTML_DATA_MAX - 1` bytes or a
real `<`. This is more aggressive than the spec — the spec
emits one character token per byte — but it is more
useful for the consumer: the DOM will create one text node
per character run, which means one node per several KiB of
text instead of one per glyph. We are an order of magnitude
ahead before the next layer even gets the data.

The reason we leave `HTML_DATA_MAX - 1` is that
`htm_decode_entity()` may emit more than one byte (today it
doesn't, but the structure leaves room for `<` from `&lt;`
followed by another character). Keeping a one-byte slack
means a malformed entity at the very end of a buffer does
not over-run.

## The driver: `/bin/htmltok`

The driver does almost nothing on its own:

```c
int main(int argc, char **argv)
{
    const char *path = "/mnt/test.html";
    if (argc >= 2 && argv[1] && argv[1][0]) path = argv[1];

    size_t n = 0;
    char  *buf = slurp(path, &n);
    if (!buf) return 1;

    struct html_tokenizer tz;
    html_tok_init(&tz, buf, n);

    struct html_token *t = malloc(sizeof(*t));   /* ~12 KiB */
    if (!t) { ...; return 1; }

    int total = 0;
    for (;;) {
        int r = html_tok_next(&tz, t);
        if (r == 0) { printf("[EOF]\n"); break; }
        total++;
        switch (t->type) {
        case HTML_TOK_DOCTYPE:  printf("[DOCTYPE] "); print_quoted(t->data, t->data_len); ...
        case HTML_TOK_START:    printf("[START]   "); print_quoted(t->tag_name, t->tag_name_len);
                                for (int i = 0; i < t->n_attrs; i++) print_attr(&t->attrs[i]);
                                if (t->self_closing) write(1, " /", 2);
                                ...
        case HTML_TOK_END:      printf("[END]     "); print_quoted(t->tag_name, t->tag_name_len); ...
        case HTML_TOK_CHARS:    printf("[CHARS]   "); print_quoted(t->data, t->data_len); ...
        case HTML_TOK_COMMENT:  printf("[COMMENT] "); print_quoted(t->data, t->data_len); ...
        }
    }
    printf("[TOTAL] %d tokens, %lu bytes input\n", total, (unsigned long)n);
}
```

`print_quoted()` escapes newlines as `\n`, tabs as `\t`,
backslashes as `\\`, and quotes as `\"`, so each token is
exactly one line. That is the single thing the test
harness depends on.

## The fixture

[`assets/osfs/test.html`](../../../assets/osfs/test.html)
exercises every feature the tokenizer claims to handle:

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>M59 tokenizer fixture</title>
<style>
  body { color: red; }
</style>
</head>
<body>
<!-- a comment in the middle -->
<h1>Hello, &amp; goodbye</h1>
<p class="intro" id='lead'>The quick brown fox &lt;jumps&gt; over the lazy dog.</p>
<ul>
  <li>one</li>
  <li>two</li>
  <li data-x=42 disabled>three</li>
</ul>
<img src="/icon.png" alt="x" />
<script>
var x = 1 < 2 && 3 > 0; // not html, won't be re-tokenized
</script>
<p>price: &#36; &#x26; done.</p>
</body>
</html>
```

The interesting corners are:

- `lang="en"` (double-quoted attribute) and `id='lead'`
  (single-quoted attribute) — both parsed.
- `data-x=42 disabled` — unquoted value followed by a
  bare boolean attribute, both parsed; `disabled` ends up
  with `value=""` which the DOM layer will treat as a
  present-but-empty boolean attribute.
- `<img ... />` — self-closing slash, retained as a flag.
- `<style>...</style>` and `<script>...</script>` — both
  end up as a single `CHARS` token whose body would
  otherwise contain markup-like characters.
- `&amp;`, `&lt;`, `&gt;`, `&#36;`, `&#x26;` — five
  different entity forms, all decoded.

## Reference output

Run the driver against the fixture and you get this stream:

```text
[DOCTYPE] "html"
[CHARS]   "\n"
[START]   "html" lang="en"
[CHARS]   "\n"
[START]   "head"
[CHARS]   "\n"
[START]   "meta" charset="utf-8"
[CHARS]   "\n"
[START]   "title"
[CHARS]   "M59 tokenizer fixture"
[END]     "title"
[CHARS]   "\n"
[START]   "style"
[CHARS]   "\n  body { color: red; }\n"
[END]     "style"
[CHARS]   "\n"
[END]     "head"
[CHARS]   "\n"
[START]   "body"
[CHARS]   "\n"
[COMMENT] " a comment in the middle "
[CHARS]   "\n"
[START]   "h1"
[CHARS]   "Hello, & goodbye"
[END]     "h1"
[CHARS]   "\n"
[START]   "p" class="intro" id="lead"
[CHARS]   "The quick brown fox <jumps> over the lazy dog."
[END]     "p"
[CHARS]   "\n"
[START]   "ul"
[CHARS]   "\n  "
[START]   "li"
[CHARS]   "one"
[END]     "li"
[CHARS]   "\n  "
[START]   "li"
[CHARS]   "two"
[END]     "li"
[CHARS]   "\n  "
[START]   "li" data-x="42" disabled=""
[CHARS]   "three"
[END]     "li"
[CHARS]   "\n"
[END]     "ul"
[CHARS]   "\n"
[START]   "img" src="/icon.png" alt="x" /
[CHARS]   "\n"
[START]   "script"
[CHARS]   "\nvar x = 1 < 2 && 3 > 0; // not html, won't be re-tokenized\n"
[END]     "script"
[CHARS]   "\n"
[START]   "p"
[CHARS]   "price: $ & done."
[END]     "p"
[CHARS]   "\n"
[END]     "body"
[CHARS]   "\n"
[END]     "html"
[CHARS]   "\n"
[EOF]
[TOTAL] 60 tokens, 538 bytes input
```

Every column lines up because `printf("[CHARS]   ")` uses
the wider literal — the harness doesn't care about column
alignment, but a human reading the output does.

## The harness

[`scripts/test_html_tokenizer.py`](../../../scripts/test_html_tokenizer.py)
deliberately does not diff against a fixed-byte expected
output. We have learned the hard way that tests which
demand exact equality with a generated transcript become
churn-magnets the first time you change the test fixture
or the printf format. Instead it asserts a list of
landmarks that any correct tokenizer would produce:

| Check | Predicate |
|-------|-----------|
| `doctype`          | `[DOCTYPE] "html"` line present |
| `html-start`       | `[START]   "html"` line present |
| `p-intro-attr`     | `[START]   "p" class="intro"` present |
| `img-selfclose`    | `[START]   "img"` with `src="/icon.png"` |
| `amp-decoded`      | `Hello, & goodbye` literal in transcript |
| `ltgt-decoded`     | `<jumps>` literal in transcript |
| `num-dollar`       | `price: $` (from `&#36;`) literal |
| `script-rawtext`   | `var x = 1 < 2` literal |
| `script-no-inner`  | no spurious `[START]` between `<script>` and `</script>` |
| `eof` + `total`    | both terminator lines present |

Pre-flight steps in the harness mirror the ones we built
for [`test_m58_repeat.py`](../../../scripts/test_m58_repeat.py):
boot QEMU with serial routed to a Unix socket, wait for
the shell prompt, send `htmltok /mnt/test.html\n`, drain
until `[TOTAL]` shows up, drain a little more so the next
prompt makes it into the captured log, then assert.

The full transcript is dropped at `/tmp/m59.log` for
reference and so the book has a stable file to point at.

## A bug surfaced by `-Werror`

The first build of `html.h` shipped a tiny helper —
`htm_is_digit()` — that the tokenizer never called.
GCC's `-Wunused-function` (which we promote to an error)
caught it on the first compile of `htmltok.c`:

```text
userspace/htmltok/../libc/html.h:120:12: error:
  'htm_is_digit' defined but not used [-Werror=unused-function]
```

This is the *good* kind of build failure. It catches the
header-only-libc anti-pattern of accumulating helpers that
no caller actually wants, before the cruft has a chance to
spread. The fix was to delete the function. (Numeric
entity parsing inlines its own `'0'..'9'` check inside the
hex/decimal loop because the conditions there also need to
know whether the digit slot is a-f or 0-9.)

Take-away: **header-only libraries with `-Werror=unused-function`
turn dead code into a build break the moment someone
includes the header**. That is the single best reason to
run `-Werror=unused-function` in a freestanding project.
A normal libc would let `htm_is_digit` rot for a decade.

## Stack discipline (revisited)

The first instinct when writing `htmltok.c` was the obvious
one:

```c
struct html_token t;                 /* on the stack */
html_tok_init(&tz, buf, n);
while (html_tok_next(&tz, &t)) { ... }
```

That would have worked, but `sizeof(struct html_token)` is
about 12 KiB and our user thread stack is 16 KiB. The
moment any signal handler or syscall preemption stuck
another frame on top, we'd have been blown off the bottom
of the stack and into the kernel's data abort handler.

The same rule from M58 applies, even more strongly:
*anything bigger than ~1 KiB lives on the heap*. The
tokenizer state struct (~32 bytes) is fine on the stack;
the token (~12 KiB) is not. Every browser-component
chapter from here on will follow the same convention:
DOM nodes, style rule sets, layout boxes — all heap.

## Performance shape

We have not optimised for speed. The fixture is 538 bytes
and tokenizes to 60 tokens in unmeasurably-fast time on
the Apple Silicon host. Three things are worth flagging
for a future optimisation pass:

- **Lower-casing on the fly.** Tag and attribute names
  are lowered byte-by-byte during parsing. A SIMD lowercase
  would help, but we have `-mgeneral-regs-only` set in
  CFLAGS, so that's a non-starter without enabling the FP
  unit (which is its own milestone we have parked).
- **One token per call.** A streaming consumer pays a
  function-call cost per token. We could expose a
  callback API instead, but that complicates the consumer
  for a benefit we cannot measure.
- **Memcpy-by-byte loops everywhere.** `htm_putc` is a
  single-byte append; `htm_puts` is a loop over a string.
  We will leave both alone until profiling says otherwise;
  the freestanding-C `memset`/`memcpy` trap (see the
  user-memory note) is not worth invoking for this.

## What this enables

The DOM-construction chapter ([Chapter 68](68-dom.md), to
be written) consumes this token stream and builds an actual
tree. It will need:

- The HTML5 implicit-tag-insertion rules (inserting
  `<html>`/`<head>`/`<body>` when missing, foster-parenting
  text out of `<table>` contexts, etc.). Most of these we
  will simplify or skip — the goal is *most pages render*,
  not *every conformance test passes*.
- The "void elements" list (`<br>`, `<img>`, `<meta>`,
  `<link>`, `<input>` ...) so that an `END` token is not
  required to close them.
- A small set of "content model" rules that decide when an
  open `<li>` is implicitly closed by another `<li>`.

After the DOM lands we get CSS in [Chapter 69](69-css.md),
layout in [Chapter 70](70-layout.md), and finally the GUI
binding in [Chapter 71](71-browser.md). At the very end we
have `/bin/browser http://example.com/` in our shell, with
a real `<h1>` rendering as a real `<h1>` inside our own
window manager, drawn through our own font, served over
our own TCP/IP stack.

That entire stack starts with this 500-line header.
