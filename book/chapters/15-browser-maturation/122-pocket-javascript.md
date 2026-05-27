# Chapter 122 — A pocket JavaScript: expression evaluator for onclick

Chapter 121 closed the cookie/SOP loop and announced — at
the end — that the policy was designed to survive whatever
JavaScript shipped next. This chapter ships that JavaScript.
Not a real engine. A *pocket* engine: a tokenizer, a
precedence-climbing parser, and a tree-walking interpreter,
all small enough to live in a single header. It runs the one
form of JavaScript that actually matters for navigating the
mostly-static pages this OS browses: the right-hand side of
`onclick="..."`.

The hard rule going in was: the engine does not live in
`browser.c`. That file already carries the URL bar, the
form submitter, the cookie jar, the GUI repaint loop, and
the HTTPS proxy bridge. Bolting an interpreter onto it would
have pushed it past 5000 lines. So the engine ships as a
header-only userspace library and the browser carries only
a thin click hook plus a debug subcommand for the regression
test.

## What ships in this slice

### 1. The engine at `userspace/libc/pocketjs.h`

One header, around 750 lines, zero external dependencies
beyond what the freestanding libc already provides
(`syscall.h`, `printf.h`, `malloc.h`, the `freestanding.h`
mem-shims). It exposes a tiny public API:

```c
struct pj;          /* engine state */
struct pj_arena;    /* caller-provided bump allocator */
struct pj_value;    /* tagged union */

void          pj_arena_init(struct pj_arena *a, void *buf, size_t n);
void          pj_init(struct pj *p, struct pj_arena *arena);

void          pj_set_global(struct pj *p, const char *name,
                            struct pj_value v);
struct pj_value pj_eval(struct pj *p, const char *source);

/* value constructors */
struct pj_value pj_undef(void);
struct pj_value pj_num(int64_t);
struct pj_value pj_bool(int);
struct pj_value pj_str(struct pj *p, const char *);
struct pj_value pj_host(void *self, const struct pj_host_class *cls);
```

The language subset it handles:

- Literals: integers (signed 64-bit), single- and
  double-quoted strings (with `\n \t \r \\ \' \"` escapes),
  `true`, `false`, `null`, `undefined`.
- Identifiers, member access (`a.b`), method calls
  (`a.b(x, y)`), function calls (`f(x)`), parentheses.
- Operators (with precedence): unary `! -`, `* /`, `+ -`,
  `<`, `>`, `<=`, `>=`, `==`, `!=`, `&&`, `||`, `=`.
- Statement sequence via `;`.
- Globals: any assignment to a bare identifier creates or
  updates a global.

The language subset it explicitly does **not** handle:

- `var`, `let`, `const` (no locals; no scopes).
- `function`, `=>`, `return` (no user-defined functions).
- `if`, `for`, `while`, `switch`, `try`/`catch` (no
  control flow beyond `&&`/`||` short-circuit).
- `new`, prototypes, classes, `this` rebinding.
- Floating-point arithmetic. Numbers are `int64_t`.
- Regular expressions, template literals, destructuring.
- `eval`, `Function`, `setTimeout(string, ...)`, anything
  string-to-code.
- `Promise`, `async`, `await`. The engine is synchronous
  and runs to completion (or hits its node-pool cap) on
  the click thread.

That subset is enough for `this.style.display='none'`,
`alert('hi')`, `document.getElementById('x').value`, and a
surprising amount of mid-2000s web copy-paste.

### 2. The host-object vtable

Identifiers like `document` and `alert` are not built into
the engine. They are arena-allocated `HOSTOBJ` values bound
to a vtable:

```c
struct pj_host_class {
    const char *name;
    struct pj_value (*get)   (struct pj *p, void *self, const char *prop);
    int             (*set)   (struct pj *p, void *self, const char *prop,
                              struct pj_value v);
    struct pj_value (*method)(struct pj *p, void *self, const char *name,
                              int argc, struct pj_value *argv);
    struct pj_value (*call)  (struct pj *p, void *self,
                              int argc, struct pj_value *argv);
};
```

Any field may be NULL. `get`/`set` handle property reads
and writes; `method` handles `obj.foo(args)`; `call`
handles `obj(args)` (so `alert(...)` is just a HOSTOBJ
whose `call` is bound).

This is the chapter's structural decision. The engine is
small *because* it knows nothing about the DOM, the
browser, or the page. All the interesting behaviour lives
on the other side of that vtable. Which brings us to:

### 3. The bridge at `userspace/browser/jsdom.h`

The bridge is the only file that knows about both pocketjs
*and* the browser's DOM. It exports one entry point that
the browser calls right before each `pj_eval`:

```c
int jsdom_install(struct pj *p, struct jsdom_ctx *ctx,
                  struct dom_node *this_node);
```

`jsdom_install` binds five host objects as globals on the
engine: `document`, `console`, `alert`, plus `this` (the
clicked element, possibly NULL), and arena-allocates the
element/style/document wrappers they share.

Element bindings expose:

| read                | source                                      |
| ------------------- | ------------------------------------------- |
| `id`, `tagName`     | direct from `struct dom_node`               |
| `value`             | the `"value"` attribute                     |
| `innerText`         | first text child's `text` field             |
| `style`             | a freshly-allocated style binding (see below) |
| any other name      | `dom_node_attr(n, name)` as a string        |

Element writes:

| write              | effect                                          |
| ------------------ | ----------------------------------------------- |
| `value`            | `dom_node_set_attr(n, "value", v)`              |
| `innerText`        | drops children, attaches a single text node     |
| any other name     | `dom_node_set_attr(n, name, v)`                 |

Element methods: `getAttribute`, `setAttribute`,
`hasAttribute`.

`element.style` returns a binding that parses the
element's inline `style="..."` attribute, lets the script
read or rewrite individual properties, and marks the page
for relayout/repaint when any write succeeds. So
`this.style.display = 'none'` walks: dispatch to element
get → returns style binding → dispatch to style set →
strips the existing `display:` declaration from the
attribute string, appends `display:none`, sets the
attribute back, raises `needs_relayout`.

`document.getElementById(id)` walks the DOM tree from
the root looking for a node whose `id` attribute matches,
and wraps the hit in an element binding (or returns
`undefined`).

`console.log(...)` prints `[browser] console.log: ...` to
the serial log and bumps a per-context counter — useful
for the regression test, and free observability while
developing onclick handlers.

`alert(msg)` is a callable HOSTOBJ. The pocketjs engine
sees `alert('hi')` as: lookup `alert` → HOSTOBJ → invoke
its `call` slot with one argument. The handler latches
the message into the page's alert buffer and mirrors it
to the serial log. There is no modal UI yet; that's
chapter-115-or-later territory once we have a proper
dialog primitive.

### 4. The click hook in `browser.c`

The browser side is intentionally small. Two static
functions plus one MOUSE_DOWN branch:

```c
static const char *onclick_at(struct dom_node *root,
                              int px, int py,
                              struct dom_node **out_target);

static int onclick_dispatch(struct loaded_page *page,
                             struct dom_node *target,
                             const char *source);
```

`onclick_at` walks the box tree to find the node under
the cursor, then climbs the DOM ancestry looking for the
first ancestor that carries an `onclick` attribute. That
matches every other browser's behaviour: clicks bubble.

`onclick_dispatch` is the only place in `browser.c` that
even mentions the engine:

```c
struct pj      *pj    = malloc(sizeof(*pj));
char           *abuf  = malloc(8192);
...
pj_init(pj, &arena);
jsdom_ctx_init(&ctx, page->dom, page);
jsdom_install(pj, &ctx, target);
pj_eval(pj, source);
if (ctx.needs_relayout || ctx.needs_repaint) return 1;
```

The MOUSE_DOWN handler runs the click hook *after* the
URL-bar and *before* the form submitter, so a clicked
`<button>` with an `onclick` fires the script (and only
the script — the form submit is skipped). If the script
mutated the DOM or the inline style, `relayout_page()`
runs and the screen is marked dirty.

### 5. The `--check-js` debug subcommand

The browser's GUI path is heavyweight to test. So
`browser.c` also carries a headless mode that mirrors the
`--check-sop` pattern from chapter 121:

```
browser --check-js <expression>
```

…initialises the engine with the same host bindings as a
real click (minus the DOM — `this` is NULL,
`document.getElementById` returns undefined), evaluates the
expression, and prints one deterministic line of the form
`JS: <type>=<value>`. If `alert()` fired it adds a second
line `JS: alert=...`. If `console.log()` fired it adds
`JS: logs=N`. On parse or eval error: `JS: error <msg>`
and a non-zero exit code.

That's the whole user-facing surface added to
`browser.c`: one struct, one click hook, one debug flag.
The 750-line engine and the 510-line bridge live in their
own headers.

## What this slice does not do yet

- **Scripts inside `<script>` tags.** The HTML parser
  swallows them and the engine never sees them. Pages that
  rely on top-level side effects (analytics ping, onload
  setup) get nothing. The plumbing is straightforward —
  collect the body during parse, run it once after the
  DOM is built — but it touches the parser, so it ships
  on its own.
- **DOM events beyond `onclick`.** No `onload`, `onchange`,
  `onsubmit`, no `addEventListener`. The MOUSE_DOWN branch
  is the only dispatcher.
- **Floating-point.** Numbers are 64-bit signed integers.
  A page that does `0.1 + 0.2` gets a parse error on the
  `.` after `0`. That's a future chapter's problem.
- **`setTimeout`, `requestAnimationFrame`.** No event loop
  reentry: every script runs to completion inside the
  MOUSE_DOWN handler.
- **Closures.** No function definitions, so the question
  doesn't come up.
- **A debugger.** Errors set a string field; you read it
  from the calling code. No backtrace, no source location.

These exclusions are deliberate. Every feature included
is reachable from `onclick="..."`. Every feature left
out would have required at least one of: a re-entry into
the parser, a heap object with non-trivial lifetime, or
floating-point arithmetic. All three are big enough to
deserve their own chapter.

## Key implementation points

### Why the engine state cannot live on the userspace stack

The first version had `pj_eval` allocate its token array
and AST node pool inline in `struct pj` and the call site
put `struct pj` on the stack. That works on paper and
crashes every invocation in QEMU.

The user stack on this OS is 16 pages — 64 KiB — with a
guard page below (chapter 72). The first cut of `struct
pj` had 4096 tokens × 24 bytes = 96 KiB of tokens and
2048 nodes × 112 bytes = 229 KiB of AST nodes, for a
total of ~326 KiB. The very first frame to allocate one
of those drove SP through the guard page into unmapped
memory, the MMU raised a translation fault, ESR_EL1 read
`0x92000047` (data abort, write, level 3 translation
fault), and FAR_EL1 was 340 KiB below `USER_STACK_TOP`.

Two fixes, applied together:

1. **Slim the engine.** Tokens went 4096→512, nodes
   2048→256, globals 32→16, error buffer 128→96. The
   AST node lost its inline `args[8]` array — it now
   carries a `first_arg` head pointer and each node a
   `next_arg` so call arguments chain through the same
   node pool. That alone dropped the node size from
   ~112 bytes to ~64 bytes. After both diet steps the
   whole struct is around 30 KiB.

2. **Heap-allocate at the call site.** Even at 30 KiB
   it was uncomfortable to put on the stack next to
   the browser's own locals. `onclick_dispatch` now
   does `malloc(sizeof(struct pj))` and a separate
   `malloc(8192)` for the arena buffer, with `goto
   done` cleanup. The `--check-js` path does the
   same with a 16 KiB arena.

The lesson is the same one chapters 72 and 79 taught
about kernel stacks: userspace stacks are tight, sized
for ordinary call frames, not workspace buffers. If you
need 100+ KiB of scratch, malloc it.

### Why arguments live in a linked list, not an array

The slim AST node holds two extra pointers: `first_arg`
and `next_arg`. A function call node's children are
chained through `next_arg` rather than living in an
inline array. That saves ~48 bytes per node (the cost
of an `args[8]` array) at the cost of two pointer
walks per argument.

That trade-off is correct here for two reasons. First,
most nodes are not call nodes — they're binary operators
with two children, or leaves. Paying for a max-args array
on every node would burn memory on the common case to
serve the rare one. Second, the argument count of a
call is bounded by parsing: `pj_parse_args` reads until
it hits `)`, so there's no need to pre-size the
argument vector. A linked list whose nodes already exist
in the pool is exactly the right shape.

### Why the test driver avoids `<`, `>`, `|`

The regression test drives `browser --check-js "EXPR"`
through `/bin/sh`. Our shell does single- and
double-quote expansion before scanning for redirect /
pipe operators (see [userspace/sh/sh.c](../../../userspace/sh/sh.c)).
The consequence is that `<`, `>`, and `|` *inside*
quoted strings are still interpreted as redirect / pipe.
A test that wrote `browser --check-js "2 < 3"` would
have the shell try to open a file named `3` for read,
fail, and never spawn the browser.

The engine itself fully supports `<`, `>`, `<=`, `>=`,
`||` — they're tested via the `onclick` fixture page in
[assets/osfs/onclick.html](../../../assets/osfs/onclick.html)
where the shell never sees them. The headless regression
exercises equality, inequality, logical AND, unary not,
sequence, assignment, alert, console, getElementById, and
loose type coercion — all written with no shell
metacharacters.

A second shell quirk: the kernel's `sys_spawn` splits
the args string on whitespace into argv tokens and
caps at 16 entries (`MAX_SPAWN_ARGV`). So an expression
like `"1 + 2 * 3"` arrives at `main` as four argv tokens,
and the `--check-js` handler re-joins them with single
spaces before passing the result to `pj_eval`. Tests
that need to stay under the argv cap (e.g. the long
arithmetic chain) write the expression with no spaces
so the kernel sees one token.

### Why string concat coerces, but addition doesn't

The `+` operator on two numbers adds. On any combination
involving at least one string, it concatenates with the
other operand coerced to a string. Real JavaScript does
the same. No other operator coerces between types:
`'7' * 2` is a parse-time number for `'7'` (which we
*don't* do — we just error out), and `'7' == 7` *does*
coerce because loose equality is the one place in the
language coercion is genuinely useful.

This is a tiny semantic surface — number addition, string
concatenation, loose equality coercion, and the boolean
truthiness rule (everything truthy except `false`, `null`,
`undefined`, `0`, `''`). It's enough to make the test
cases behave the way someone writing JS would expect, and
small enough to fit in the 750-line budget.

## Regression test

[scripts/test_browser_js.py](../../../scripts/test_browser_js.py)
walks 20 expectations across 16 expressions in one boot:

1. Integer literal: `42`.
2. Arithmetic precedence: `1 + 2 * 3 == 7`.
3. Parens + integer division: `(10 - 4) / 2 == 3`.
4. String concat: `'foo' + 'bar' == 'foobar'`.
5. Mixed string + number coerces: `'x=' + 7 == 'x=7'`.
6. Equality: `5 == 6`, `7 != 8`.
7. Logical AND: `true && 42 == 42`, `false && 42 == false`.
8. Unary not: `!false == true`.
9. Sequence: `1; 2; 3` evaluates to `3`.
10. Assignment + read-back: `x = 7; x + 1 == 8`.
11. `alert('hello')` returns undefined; alert buffer
    carries `hello`.
12. `console.log('a'); console.log('b')` bumps the log
    counter to 2.
13. `nope.thing` on an undefined global returns undefined
    without crashing.
14. Loose equality: `7 == '7'` is true.
15. `document.getElementById('x')` on the empty DOM
    returns undefined.
16. Long arithmetic chain: `1+2+3+4+5+6+7+8+9+10 == 55`.
17. Unknown method on a known host object:
    `document.bogus('x')` returns undefined.

All 20 expectations pass in one boot.

## Manual testing

Boot the OS, type:

```
browser --gui /mnt/onclick.html
```

…and click the four buttons. The first hides itself via
`this.style.display='none'`; the second pops `alert('hello
from pocketjs')` to the serial log; the third concatenates
the input's value with a greeting; the fourth hides a
sibling paragraph by id.

Or, headlessly from the shell:

```
$ browser --check-js "1 + 2 * 3"
JS: num=7

$ browser --check-js "alert('hi')"
JS: undefined=
JS: alert=hi
```

## Applied to

- Existing apps modified:
  - [userspace/browser/browser.c](../../../userspace/browser/browser.c) —
    `onclick_at` walks DOM ancestors, `onclick_dispatch`
    runs the engine, MOUSE_DOWN dispatches before
    form_submit, `--check-js` headless debug flag.
- New libc headers:
  - [userspace/libc/pocketjs.h](../../../userspace/libc/pocketjs.h)
    — engine.
  - [userspace/libc/dom.h](../../../userspace/libc/dom.h) — gained
    `dom_node_set_attr` for JS-driven attribute writes.
- New browser-private header:
  - [userspace/browser/jsdom.h](../../../userspace/browser/jsdom.h)
    — DOM/style/console/alert bindings.
- New asset:
  - [assets/osfs/onclick.html](../../../assets/osfs/onclick.html)
    — interactive demo page (4 buttons).
- Tests added:
  - [scripts/test_browser_js.py](../../../scripts/test_browser_js.py)
    — 20 expectations, all green.
- Tests still green (verified post-change):
  - [scripts/test_browser_sop.py](../../../scripts/test_browser_sop.py)
    — cookie/SOP chapter 120/121 regression.
  - [scripts/test_browser_self.py](../../../scripts/test_browser_self.py)
    — in-guest browser ↔ httpd loopback.
  - [scripts/test_browser_image.py](../../../scripts/test_browser_image.py)
    — chapter 98 PNG render still works.

## What this unlocks

- Pages with simple `onclick` interactivity (show/hide,
  toggle, copy-into-input) navigate cleanly. The
  mid-2000s "press the button to reveal the answer"
  style of page now works.
- The chapter 121 SOP gate is no longer theoretical:
  a `form.action = "http://evil.com/"` mutation from
  inline JS would hit the cross-origin blocker on
  submit. (We don't yet expose `form.action` as a JS
  property — that's the next slice — but the path is
  in place.)
- The host-object vtable is the obvious place to bolt
  the next batches of bindings: `window.location`,
  `cookie`, `XMLHttpRequest`, all of them are "another
  HOSTOBJ with a vtable." None of that requires
  re-touching the parser or the interpreter.
- The "engine in a header, bindings in a sibling
  header, app keeps a thin call site" pattern is now
  proven for a 750-line interpreter. Future chapters
  that ship interpreters (a regex engine? a JSON
  parser?) can follow the same shape.
