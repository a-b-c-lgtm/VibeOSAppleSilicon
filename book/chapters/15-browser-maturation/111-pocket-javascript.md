# Chapter 111 — A pocket JavaScript: expression evaluator for onclick

**Status:** Stub. Tracking milestone 98.

We will not implement a full JavaScript engine — that's
years of work. But even a tiny expression evaluator that
can handle `onclick="this.style.display='none'"` opens up
a surprising amount of interactive web content. This
chapter builds exactly that: a recursive-descent parser +
tree-walking interpreter for a JS subset.

## What this chapter adds

- `userspace/libc/pocketjs.h` — a header-only interpreter
  for:
  - Number, string, bool, null, undefined.
  - Variable declaration (`var`, `let` treated alike).
  - Property access on a small set of host objects:
    `document`, `this`, the DOM node passed in.
  - Method calls limited to a tiny built-in table.
  - Operators: arithmetic, comparison, logical, ternary.
- A small DOM bridge: `node.style.display = 'none'` flips
  a flag the layout engine respects.
- Browser hooks: `<script>` collects bodies; `onclick`
  attribute fires on click; `onload` fires after parse.

## Prerequisites

- Chapter 71 — browser
- Chapter 68 — DOM (we will mutate it from JS)

## Plan

- Tokenizer + Pratt-style parser, ~600 lines for the
  subset.
- Interpreter is tree-walking; no bytecode.
- Hard caps: 1000 statements per script, 100 deep call
  stack, 1 MiB heap. A misbehaving page does not lock the
  browser.
- All eval() / Function() / setTimeout-with-string forms
  are explicitly unsupported.

## What you'll learn

- Why even tiny JS engines start with a tokenizer + a
  parser + an interpreter — same shape as our HTML and
  CSS code.
- The "host objects" idea: the engine is small; the
  interesting part is what you bind to it.
- Why production engines are not interpreters anymore
  (and why ours is).

## What this unlocks

- Many "static-with-a-bit-of-JS" sites become navigable.
- A bridge for the (much later) "DOM events" arc.
