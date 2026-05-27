# Chapter 68 — A tiny CSS parser

[Chapter 67](067-dom-construction.md) gave us a tree of HTML
elements with attributes attached. By the end of that chapter,
`/bin/htmldom /mnt/test.html` walked our fixture and printed a
clean parent/child/sibling structure. The tree is the *what*;
this chapter adds the *how it should look*.

This chapter ships a CSS parser and a selector matcher. By the
end of it, `/bin/cssparse /mnt/test.css /mnt/test.html` parses
both sides and prints which rules apply to which DOM elements:

```
[STYLESHEET] 11 rules, 17 decls, 1573 bytes input
[RULE 0] selectors=1 specificity=0
  [SEL] *
  [DECL] box-sizing: "border-box"
[RULE 1] selectors=1 specificity=1
  [SEL] body
  [DECL] background: "white"
  [DECL] color: "black"
  ...
[RULE 7] selectors=1 specificity=10101
  [SEL] p.intro#lead
  [DECL] font-style: "italic"
[RULE 8] selectors=1 specificity=10000
  [SEL] #lead
  [DECL] text-decoration: "underline"
[RULE 9] selectors=2 specificity=100
  [SEL] .note
  [SEL] .warn
  [DECL] border: "1px solid red"
  [DECL] padding: "4px"
[RULE 10] selectors=1 specificity=1
  [SEL] script
  [DECL] display: "none"

[MATCH] <html lang="en"> -> rules 0
[MATCH] <head> -> rules 0
[MATCH] <body> -> rules 0 1
[MATCH] <h1> -> rules 0 2
[MATCH] <p class="intro" id="lead"> -> rules 0 3 6 7 8
[MATCH] <ul> -> rules 0
[MATCH] <li> -> rules 0 4 5
[MATCH] <script> -> rules 0 10
[MATCH] <p> -> rules 0 3
```

The output is the input to the next chapter, which converts
"this rule applies to this element" into a computed style
dictionary on every node and from there into a layout box.

The chapter goal is, again, narrow:

- One header file: [`userspace/libc/css.h`](../../../userspace/libc/css.h).
- One driver binary: [`/bin/cssparse`](../../../userspace/cssparse/cssparse.c).
- One fixture: [`assets/osfs/test.css`](../../../assets/osfs/test.css).
- One test harness: [`scripts/test_css.py`](../../../scripts/test_css.py).

No cascade resolution, no inheritance, no computed-style table,
no layout. Each of those gets its own chapter.

## What we parse — and what we don't

Real CSS is enormous. The [CSS Selectors module][selectors]
alone is hundreds of pages, before we even get to the syntax
spec, the value definitions, or the cascade. We are aiming at a
hobby browser that can render text-heavy pages, not a CSS
conformance test suite.

What `css.h` *does*:

- **Type selectors** — `body`, `h1`, `p`, ...
- **Class selectors** — `.intro`, `.warn`
- **ID selectors** — `#lead`
- **Universal** — `*`
- **Compound selectors** — `p.intro#lead` (multiple simples
  with no whitespace)
- **Descendant combinator** — `body p`
- **Child combinator** — `ul > li`
- **Comma-separated lists** — `.note, .warn { ... }` becomes
  one rule with two selector chains
- **Block comments** — `/* ... */` anywhere whitespace fits
- **Declaration blocks** — `prop: value;` repeated
- **`@`-rule recovery** — anything starting with `@` (think
  `@media`, `@import`, `@keyframes`) is skipped to the next
  matching `}`. The contents do **not** leak rules into the
  outer stylesheet.
- **Bad-rule recovery** — selectors with unknown syntax are
  dropped along with their declaration block; parsing
  continues at the next rule. This is what the spec calls
  "rule that doesn't parse, drop", and is what makes real-world
  CSS not catastrophically blow up when one new feature lands.

What `css.h` *doesn't* do:

- **Pseudo-classes** (`:hover`, `:nth-child`) — out of scope;
  we have no input model for hover and no positional bookkeeping
  for nth-child yet.
- **Pseudo-elements** (`::before`, `::first-line`) — would
  need extra synthetic DOM nodes, which is more layout
  concern than parser concern.
- **Attribute selectors** (`[type="text"]`) — would fit the
  model trivially but no fixture page needs them yet.
- **Sibling combinators** (`~`, `+`) — same story; would
  require `prev_sibling` on `dom_node` (we deferred that in
  Ch68).
- **Media queries / @media nesting** — we skip the whole at-rule.
- **`calc()`, `var()`, nested function values** — we keep
  values as raw strings.
- **Shorthand expansion** — `margin: 1px 2px 3px 4px` stays as
  one declaration with one value string. Layout will tokenize
  it later.
- **`!important`** — accepted syntactically (it stays in the
  value string), but no special precedence in the cascade.

If any of these later become necessary they are additive — the
parsing model is modular enough that adding pseudo-class checks
to the matcher, or sibling combinators to the chain walker, is
local work.

## The model

`userspace/libc/css.h` defines five types:

```c
enum css_simple_kind {
    CSS_SIMPLE_UNIVERSAL = 1,   /* *      */
    CSS_SIMPLE_TYPE      = 2,   /* p, h1  */
    CSS_SIMPLE_CLASS     = 3,   /* .intro */
    CSS_SIMPLE_ID        = 4,   /* #lead  */
};

struct css_simple {
    int kind;
    char *name;          /* NULL for universal */
    struct css_simple *next;
};

struct css_compound {
    struct css_simple   *simples;     /* p.intro#lead = three simples */
    int                  combinator;  /* ' ' or '>' (see below) */
    struct css_compound *next;
};

struct css_selector {
    struct css_compound *chain;       /* one chain = one comma alternative */
    struct css_selector *next;
};

struct css_decl {
    char *property;
    char *value;
    struct css_decl *next;
};

struct css_rule {
    struct css_selector *selectors;   /* >1 = comma-separated */
    struct css_decl     *decls;
    int                  source_order;
    struct css_rule     *next;
};
```

A `struct css_stylesheet` holds the linked list of rules plus a
tail pointer for `O(1)` append during parsing.

A few choices to flag, because they are load-bearing:

- **Values are raw strings.** Every declaration value is the
  source bytes between `:` and `;` (or `}`), trimmed of leading
  and trailing whitespace, with comments stripped. We do not
  tokenize "10px" into number + unit, and we do not split
  shorthands. Layout is the right layer for that — until we
  know whether `10px` is a `border-width` or a `font-size` we
  can't even pick a default unit conversion. Keeping the value
  as a string lets the parser stay simple and the layout engine
  pick its own value tokenizer.

- **`malloc` per node.** A typical stylesheet is a few hundred
  rules at most. Malloc cost is invisible compared to the cost
  of one render. The user heap gives us 32 MiB and we are not
  near it. Arena packing is a worthwhile optimisation for a
  real browser but not for *this* chapter.

- **Comma lists become one rule with multiple chains, not
  multiple rules sharing declarations.** This costs one extra
  level of indirection but matches how CSS authors think
  ("this group of selectors styles these properties") and
  halves the rule-count on the sort of repetitive utility
  CSS that real-world frameworks emit.

- **`source_order` is captured per rule.** The cascade in real
  CSS is `(specificity desc, source_order asc)`. We don't
  apply it yet — that's the layout chapter's job — but we record the
  information now so we don't have to walk the list a second
  time later.

## Selector storage: right-to-left

The single non-obvious data-structure choice is *which way the
chain points*.

Source order is left-to-right. `A > B C` reads as: an `A`
that has a `B` child, where the `B` has a `C` descendant. To
match a candidate node, however, you start from the rightmost
compound (`C`, the one that must match the candidate) and
climb ancestors. So the matcher always walks right-to-left.

We could store the chain left-to-right and reverse during
matching, or we could store it right-to-left and walk it
naturally. We picked the latter.

For `A > B C`, the storage is:

```
chain head ──► { simples: C, combinator: ' ', next ─┐
                                                    │
                ┌───────────────────────────────────┘
                ▼
                { simples: B, combinator: '>', next ─┐
                                                     │
                ┌────────────────────────────────────┘
                ▼
                { simples: A, combinator: 0,   next: NULL }
```

Each compound's `combinator` describes the relationship to
its `next` (which is the source-LEFT neighbor, which is an
ancestor in the DOM). So `C.combinator = ' '` means "to find a
node matching `C.next` (= `B`), look at any ancestor"; and
`B.combinator = '>'` means "to find a node matching `B.next`
(= `A`), look only at the direct parent".

Parsing builds the chain by *prepending*: we parse `A`, then
on each combinator we see we attach the new compound to the
front of the list with the just-parsed combinator stamped onto
the new compound. By the time we've consumed the whole chain,
the head is the rightmost compound, exactly as the matcher
wants.

The matcher itself is six lines:

```c
static inline int css_match_chain(const struct css_compound *cmp,
                                  const struct dom_node *n)
{
    if (!cmp) return 1;
    if (!css_match_compound(cmp, n)) return 0;
    if (!cmp->next) return 1;

    if (cmp->combinator == CSS_COMB_CHILD) {
        struct dom_node *parent = n->parent;
        if (!parent) return 0;
        return css_match_chain(cmp->next, parent);
    }
    /* descendant: walk all ancestors */
    for (struct dom_node *anc = n->parent; anc; anc = anc->parent)
        if (css_match_chain(cmp->next, anc)) return 1;
    return 0;
}
```

That's the whole thing. The recursion is bounded by the chain
depth (rarely more than four or five compounds) so a recursive
implementation is fine — we don't need an explicit stack.

## Specificity

CSS resolves competing rules by specificity, computed as a
triple `(a, b, c)` where:

- `a` = number of ID selectors,
- `b` = number of class (and attribute and pseudo-class)
  selectors,
- `c` = number of type (and pseudo-element) selectors.

Universal contributes nothing. `p.intro#lead` is `(1, 1, 1)`;
plain `body` is `(0, 0, 1)`; plain `*` is `(0, 0, 0)`.

We pack the triple as `a * 10000 + b * 100 + c` for a single
integer comparison. The base-100 packing assumes no chain has
more than 99 selectors of any one kind — which is true of
hand-written CSS and almost all generated CSS.

Per-chain specificity matters because a single rule with a
comma list has *several* chains, and the cascade applies the
specificity of the *matching* chain, not the rule. The layout
chapter will need this when it walks the rules in cascade order. For now
the driver prints the maximum specificity across a rule's
chains, which is what you'd intuitively expect when eyeballing
the parse tree.

## At-rules and bad-rule recovery

Real CSS is forwards-compatible: any feature you don't
recognise is supposed to be skipped without breaking the rest
of the stylesheet. The two recovery rules:

1. **An at-rule** (anything starting with `@`) skips its
   prelude up to the first `;` or `{`. If a `{` was found,
   we then skip-balance to the matching `}` and discard the
   contents.

2. **A bad rule** — a selector that fails to parse — skips
   forward until the next `}` (balancing braces) and then
   resumes.

Skipping balanced braces requires honouring CSS's string
escapes: the literal sequence `}` inside a `"..."` string is
not a brace. Our `css_skip_block` accounts for both `"..."`
and `'...'` strings, plus block comments, while counting brace
depth.

The fixture exercises both:

```css
/* this @-rule should be skipped entirely */
@media (min-width: 800px) {
  body { background: pink; }
  h1   { font-size: 48px; }
}

/* trailing rule after the at-rule must still parse */
script { display: none; }
```

The test harness asserts that no `[DECL] background: "pink"`
appears in the output, and that the trailing `script` rule
shows up at index 10. Both held on the first run, which means
the brace-balanced skipper works the way the spec needs.

## Parsing without a tokenizer

We didn't write a separate CSS tokenizer. The parser reads the
source byte-by-byte, with a couple of helpers (`take_ident`,
`skip_ws_and_comments`, `skip_string`, `skip_block`). This is a
deliberate departure from the HTML pipeline (Ch67 had a token
stream that fed Ch68 a tree builder).

The reason: CSS doesn't have HTML's "tokens are reusable for
multiple parsers" property. The grammar of CSS *is* the
parser; selectors and declarations can't even share a single
token type because identifiers in selector position have
different acceptable syntax than identifiers in property-name
position. Splitting the parser into a tokenize/parse pair
would mostly produce duplication.

If we ever need it (e.g. for a CSS-in-JS-style live editor),
we can add a tokenizer later. Today we don't.

## The driver

[`/bin/cssparse`](../../../userspace/cssparse/cssparse.c) is the
test harness's puppet. It mirrors the tokenizer and DOM drivers:

1. `slurp(path)` reads the CSS file into a heap buffer.
2. `css_init(&ss)` zeroes the stylesheet header.
3. `css_parse(&ss, buf, len)` runs the parser to completion.
4. `print_rule()` walks each rule, printing `[RULE N]`,
   `[SEL]` lines, and `[DECL]` lines with values quoted +
   control-byte escaped.
5. **If a second argument is given**, it slurps the HTML
   file, runs the tokenize+DOM pipeline (`html_tok_init` →
   `dom_init` → `dom_build`), and walks the DOM depth-first.
   For each element node it loops over the rules and prints
   the indices of any that match. This is the matcher's contribution
   to the eventual "computed style per node" of the layout chapter.
6. `css_destroy(&ss)` frees the rule list. `dom_destroy(&dom)`
   frees the tree. `free()` the buffers.

About 250 lines. Heap-allocates the 12 KiB `html_token` scratch
(same stack-discipline rule from earlier). No shared state,
deterministic output.

## The test harness

[`scripts/test_css.py`](../../../scripts/test_css.py) boots a
clean kernel under HVF, waits for the shell prompt, runs both
phases of `cssparse`, and greps the output for landmarks.

Phase 1 (parse-only) checks:

- `[STYLESHEET] 11 rules, 17 decls, 1573 bytes input` — the
  fixture has eleven non-`@media` rules and the parser sees
  exactly eleven; the `@media` block must not contribute.
- `[RULE 0]` is the universal selector with `specificity=0`.
- `[SEL] body p`, `[SEL] ul > li`, `[SEL] p.intro#lead`,
  `[SEL] .note` + `[SEL] .warn` — confirms descendant, child,
  compound, and comma-list parsing.
- `specificity=10000` for `#lead` and `specificity=10101` for
  `p.intro#lead` — confirms the packing.
- `[DECL] background: "pink"` does *not* appear — confirms the
  `@media` skipper.
- `[SEL] script` `[DECL] display: "none"` — confirms parsing
  resumes correctly after the `@media`.

Phase 2 (parse + match) checks:

- `<body>` matches rules `0` (`*`) and `1` (`body`).
- `<p class="intro" id="lead">` matches `0`, `3` (`body p`),
  `6` (`p.intro`), `7` (`p.intro#lead`), `8` (`#lead`).
- Both `<li>`s match `4` (`ul li`) and `5` (`ul > li`) — the
  descendant variant *and* the child variant should hit, and
  do.
- `<script>` matches `10` — confirms the post-`@media` rule
  found its target.

A green run looks like:

```
$ python3 scripts/test_css.py
--- captured 9870 bytes -> /tmp/m61.log ---
PASS: CSS parser + matcher — all checks green
```

The full transcript is dropped at `/tmp/m61.log` for debugging.

## What we built

| File | Lines | Purpose |
|---|---|---|
| [`userspace/libc/css.h`](../../../userspace/libc/css.h) | ~700 | Header-only parser + matcher. |
| [`userspace/cssparse/cssparse.c`](../../../userspace/cssparse/cssparse.c) | ~230 | `/bin/cssparse` driver. |
| [`scripts/test_css.py`](../../../scripts/test_css.py) | ~200 | Headless test harness with rule + match landmarks. |
| [`assets/osfs/test.css`](../../../assets/osfs/test.css) | ~50 | Fixture exercising every supported selector form. |
| [`Makefile`](../../../Makefile) (edit) | +20 | Build / strip / OSFS-pack `cssparse` + `test.css`. |

A real browser's CSS engine is enormously more capable —
inheritance, the cascade with `!important`, computed values,
font fallback chains, transitions, animations, `:has()`. None
of those are in our hot path. What we built is just enough to
feed the layout box generator a list of "here are the
declarations that apply to this element".

## What this enables

- **Block and inline layout.** Now that we know which
  rules apply to which nodes, the layout chapter can resolve the cascade
  (specificity desc, source_order asc), pull the
  computed-style values out of the resulting per-node
  declaration set, and assign each element a box position.

- **`/bin/browser`.** End-to-end pipeline: fetch URL →
  HTML tokenize → DOM → CSS parse → matcher → layout → paint.

[selectors]: https://www.w3.org/TR/selectors-3/
