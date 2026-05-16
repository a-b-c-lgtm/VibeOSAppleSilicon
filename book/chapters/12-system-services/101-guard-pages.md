# Chapter 101 — Guard pages and a friendlier stack overflow

**Status:** Stub. Tracking milestone 91.

Chapter 27's postscript described the user-stack-overflow
forensic dance: notice ESR EC=0x24, FAR just below the
mapped stack base, ELR in a function prologue. This
chapter converts that diagnosis from a forensic exercise
into a one-line message at fault time.

## What this chapter adds

- A `PROT_NONE` guard page mapped immediately below the
  user stack (no physical backing; the L3 entry is
  invalid).
- The data-abort handler distinguishes "fault in guard
  region" from "regular bad pointer" using FAR.
- On a stack-overflow fault, the message is:
  `[svc] user stack overflow at depth N (FAR=0x..., function=...)`
  where N is computed from `USER_STACK_TOP - FAR`.
- Optional: bump-on-overflow mode for development —
  treats the guard fault as "extend the stack by one
  page" instead of killing the process.

## Prerequisites

- Chapter 5 — Exception vectors, ESR, FAR
- Chapter 24 — Per-process address spaces
- Chapter 27 (postscript) — the original forensics pass

## Plan

- One extra page reserved per AS at AS-create time, mapped
  invalid.
- Guard page does not consume physical RAM; only an L3 entry.
- The "function name" decoding looks up a tiny per-binary
  symbol stub baked in by the linker (a future appendix).

## What you'll learn

- A guard page costs (almost) nothing and saves hours of
  diagnostic time.
- The FAR-based disambiguation pattern, reusable for
  heap-overflow detection later.

## What this unlocks

- Anyone reading this book gets a clear error instead of
  a binary fault dump.
- Foundation for richer per-region error reporting (heap,
  shared libs, etc).
