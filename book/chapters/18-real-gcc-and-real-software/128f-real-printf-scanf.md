# Chapter 128f — Real `printf` and `scanf`

> **Status:** complete. `scripts/test_libc_printf2.py` green.
> **Prereq:** chapters 19 / 116b (existing printf + stdio FILE
> infrastructure), chapter 128e (stdlib, for shared
> `_scn_src`-style abstractions).
> **Unlocks:** GCC's diagnostics (which lean on `%*s` for
> source-line markers and `%n` for column accounting), tar's
> `printf("%-10s %8d %s\n", mode, size, name)` listing format,
> any upstream code that calls `sscanf("%d %s", ...)` on a
> config-file line.

---

## What you'll do in this chapter

1. Extend `userspace/libc/printf.h` with octal (`%o`),
   precision (`.N`), the `+`/space/`#` flags, variable
   width/precision (`%*`), and `%n`.
2. Ship a new `userspace/libc/scanf.h` that exposes a tiny
   `_scn_src` abstraction plus `sscanf` / `vsscanf` and the
   full conversion-spec table.
3. Add `fscanf` / `vfscanf` / `scanf` / `vscanf` shims to
   `userspace/libc/stdio.h` so FILE-based input scanning
   reuses the same formatter.
4. Write a `printftest2` binary that exercises every new
   case (printf and scanf), then run
   `scripts/test_libc_printf2.py` and watch it land green.

Float specifiers (`%f` / `%e` / `%g` on the printf side,
`%f` on the scanf side) are deferred to chapter 129 — they
take or return `double`, which `-mgeneral-regs-only` forbids.

## Why now

Two gaps between what ships today and what real upstream
expects:

1. **Output side.** Chapter 19's `printf.h` (extended through
   116b) already covers `%d`, `%i`, `%u`, `%x`, `%X`, `%p`,
   `%s`, `%c`, `%%`, plus zero-pad / left-justify / width and
   the `l`/`ll`/`z` length modifiers. It doesn't yet cover
   octal (`%o`), precision (`.N`), the `+`/space/`#` flags,
   variable-width (`%*d`), or `%n`. All five appear constantly
   in real code. GCC's diagnostic format strings use `%-*s`,
   `%+d` for delta amounts, `%#x` for tag dumps, and `%n` to
   feed its self-positioning underliner. Without precision and
   alt-form, GCC's output looks nothing like the upstream
   reference and several internal asserts fire.
2. **Input side.** No `scanf` at all. tar's `--checkpoint=N`
   option parses `N` with `sscanf("%d", ...)`. GCC's
   `gcov.c` reads `.gcda` files via a `sscanf("%[^=]=%d",
   ...)` loop. Even Doom's `R_TextureNumForName` uses
   `sscanf` to interpret `-warp 1 3` as a (1, 3) tuple.

This chapter closes both gaps. No new linker dependencies; the
existing `printf.h` grows in place, and a sibling header
`scanf.h` handles the input formatter.

---

## What ships

| Surface | What |
|---|---|
| `printf.h` additions | `%o` octal, `%n` count-storing, `+`/space/`#` flags, `.N` precision, `*` width/precision, `j`/`t` length modifiers |
| `scanf.h` (new) | `_scn_src` source-callback abstraction; `sscanf`, `vsscanf`; conversion specs `%d`/`%i`/`%u`/`%o`/`%x`/`%X`/`%s`/`%c`/`%n`/`%[set]`/`%%` plus `*` suppression, width, length modifiers |
| `stdio.h` additions | `fscanf`, `vfscanf`, `scanf`, `vscanf` — thin shims that plug `fgetc`/`ungetc` into `_scn_src` and dispatch to the scanf formatter |

`%f` / `%e` / `%g` (printf side) and `%f` (scanf side) are
**not** in this chapter — they take/return `float`/`double`
which the `-mgeneral-regs-only` build forbids. They land in
chapter 129 alongside the FP-at-EL0 work.

Length modifier `L` (long double) is not implemented. Long
double on AArch64 LP64 is 128-bit IEEE754 quad — unsupported
elsewhere in the libc, and no in-tree consumer needs it.

---

## printf changes, in order

### Octal — `%o`

Trivial: drop the existing `%x` switch case, change base from
16 to 8, suppress upper-case (octal has no concept of it).
Five lines of code in `_fmt_vformat`. The `_fmt_render_unsigned`
helper was already base-parameterised; the renaming of "10 or
16" to "8, 10 or 16" in its comment is the only signal that
anything changed.

### Precision — `.N`

This is the biggest visible change. Parser-side: after the
width digits, optionally consume `.`, then digits or `*`.
Empty after the dot means precision 0 per C99 (which is
distinct from "no precision specified", which we encode as
`-1`).

Behaviour side, two distinct semantics depending on what the
specifier is:

- **Integer (`%d`/`%i`/`%u`/`%o`/`%x`/`%X`)**: minimum digit
  count. Pad with leading zeros if the rendered body is
  shorter than `prec`. Explicit precision overrides the `0`
  flag — `"%08.3d", 42` ⇒ `"     042"` (5 spaces + 3-digit
  zero-padded body), not `"00000042"`.
- **String (`%s`)**: maximum output length. `"%.3s",
  "hello"` ⇒ `"hel"`.

There's also a C99 corner case the test pins down:

> `"%.0d", 0` ⇒ empty string.

A precision of zero with a value of zero means "no digits at
all". Real glibc does this. Important because GCC's
array-bounds diagnostic uses `"[%.0d%s]"` to elide the
empty-index case.

The pad-vs-precision-vs-sign-vs-prefix interaction is
complicated enough that `_fmt_emit_padded` grew two new
parameters (`prefix` / `prefix_len` for the alt-form `0x`/`0`
prefixes, and `prec` for the minimum-digit count). Caller-side
the rule is:

```
natural = sign + prefix + precision_pad + body
pad     = max(0, width - natural)
```

When zero-padding, layout is
`sign prefix [pad of zeros] [precision pad of zeros] body`.
When space-padding (the default), layout is
`[pad of spaces] sign prefix [precision pad of zeros] body`.

That's what real libc does. Two specific cases the test
catches if you get the layout wrong:

- Put the prefix *after* the spaces in the non-zero-pad case
  and `"%6#x", 0xff` comes out `"  0xff"` (fine); put the
  precision pad *before* the prefix in the zero-pad case and
  you get `"0x00ff"` (correct). Inconsistency isn't tested,
  but the symmetric layout above keeps both right.
- The "explicit precision overrides `0` flag" rule is easy
  to miss. `"%08.3d", 42` must come out `"     042"` (5
  spaces + 3-digit zero-padded body), not `"00000042"`.

### Sign flags `+` and space

Reading `'+' in flags ⇒ has_sign=1, sign_char='+'` and the
analogous space case. The trap is that `+` overrides space
(spec is explicit), and that for *negative* values the `-`
always wins regardless of which flag was set. Encoded as
"if v < 0, has_sign='-'; else if force_sign, '+'; else if
space_sign, ' '".

### Alt form `#`

For `%x` and `%X`: prepend `"0x"` / `"0X"` unless the value
is zero (C99 §7.19.6.1: "alternate form has no effect if the
value is zero"). For `%o`: prepend `"0"` unless the body
already starts with `0`. Implemented by setting the
`prefix`/`prefix_len` pair before calling `_fmt_emit_padded`.

### `*` width and `.*` precision

`"%*d", 6, 42` ⇒ width=6 consumed from the va_list. Negative
value means "left-justify with the absolute width", per spec.

`.*` is the same for precision. Both are common in GCC's
diagnostic.cc where the width comes from the terminal width
the runtime detected.

### `%n`

`printf("abc%nXYZ", &n)` stores `3` into `n`. Doesn't count
toward the return value. Cheap because the formatter already
tracks `s->total`. Mostly a GCC thing; tar uses it once.

---

## scanf, end to end

### Why a new header instead of bolting it onto stdio.h

Two reasons:

1. `sscanf` doesn't need a `FILE *`. Putting it in stdio.h
   means anyone calling `sscanf` pulls in the entire FILE
   machinery — buffers, the open-FILE list, flush-on-exit.
   The new file `userspace/libc/scanf.h` exposes `sscanf` and
   `vsscanf` standalone.
2. Mirrors how `printf.h` and `stdio.h` already split. The
   core formatter is in `printf.h`; `fprintf` / `vfprintf`
   are in `stdio.h` and just plumb a sink into the formatter.

`scanf.h` defines a `_scn_src` struct with two function
pointers (`get` and `unget`) and a cookie. The core formatter
`_scn_vformat` takes only this struct, never sees a string or
a FILE.

`sscanf` lives entirely in `scanf.h` because its source is a
string pointer + a one-byte pushback slot — no dependencies
beyond `<stddef.h>` / `<stdint.h>` / `<stdarg.h>`.

`fscanf`/`vfscanf`/`scanf`/`vscanf` live in `stdio.h` because
their source plumbs through `fgetc` and `ungetc`. The FILE
struct already has a one-byte `ungot` slot (chapter 116b), so
the source's pushback depth-of-one is enough.

### Conversion specifiers

| Spec | Behaviour |
|---|---|
| `%d` | Skip ws, optional sign, decimal digits |
| `%i` | Skip ws, optional sign, base auto-detect (`0x` → 16, `0` → 8, else 10) |
| `%u` | Skip ws, optional sign, decimal digits → unsigned |
| `%o` | Skip ws, optional sign, octal digits |
| `%x` / `%X` | Skip ws, optional sign, optional `0x` prefix, hex digits |
| `%s` | Skip ws, consume non-ws chars, NUL terminate |
| `%c` | NO skip-ws; consume exactly `width` (default 1) chars |
| `%[set]` | Match against the character set; `%[^set]` inverts; consume run, NUL terminate |
| `%n` | Store running input-char count into `(int *)`; doesn't count as a conversion |
| `%%` | Literal `%` in input |
| `*` flag | Suppress assignment: conversion runs, va_arg not consumed, doesn't count toward return value |
| Width | Cap on chars consumed by the conversion (and on chars stored for `%s`/`%[]`) |
| `l`/`ll`/`z`/`j`/`t` | Widen the integer destination pointer; LP64 means any of these maps to `long *`/`unsigned long *` |

Whitespace in the format string matches zero-or-more
whitespace chars in the input. A literal char in the format
must match exactly; a mismatch terminates the scan.

### Return-value subtleties

Pinned down by the test:

- `sscanf("", "%d", &a)` → `EOF`. No conversions and no
  input ever consumed.
- `sscanf("42", "%d %d", &x, &y)` → `1`. First conversion
  succeeded, second hit EOF.
- `sscanf("foo", "%d", &a)` → `0`. Input present but never
  matched the first conversion.
- `sscanf("100 200", "%*d %d", &a)` → `1`. The suppressed
  conversion ran but doesn't increment the count.

The implementation tracks a `got_any` flag (did it ever read
a byte off the source?). If a conversion fails *and*
`got_any` is false, it returns `EOF`. Otherwise it returns
`matched`.

### Scansets

`%[abc]` accepts a run of bytes that are 'a', 'b', or 'c'.
`%[^abc]` is the inverse (any byte EXCEPT 'a'/'b'/'c').
The implementation builds a 256-bit acceptance table on the
stack (32 bytes) by walking the format string between `[` and
`]`. Special case: `]` immediately after `[` (or `^`) is a
literal close-bracket member of the set, not a terminator —
this is the POSIX rule.

We do **NOT** support ranges like `%[a-z]` — they're a GNU
extension, not POSIX, and binutils + GCC + Doom all enumerate
their accepted-character sets explicitly.

---

## Pitfalls

### Pitfall — `INT32_MAX` isn't guaranteed in freestanding `<stdint.h>`

**Symptom.** Build fails with `error: 'INT32_MAX' undeclared`.

**Cause.** Freestanding `<stdint.h>` is only required to define
`int32_t`/`int64_t`/etc; the limit macros (`INT32_MAX`,
`UINT_LEAST16_MAX`, etc) are conditionally exposed and many
toolchains hide them unless `__STDC_LIMIT_MACROS` is defined.
The `aarch64-elf-gcc` used here is one of those toolchains for
the freestanding header set.

**Fix.** Two options:

- Define `__STDC_LIMIT_MACROS` before including `<stdint.h>`.
- Use the literal `0x7fffffff` instead.

The literal wins here — one line, no need to touch every TU
that includes `<stdint.h>` later.

### Pitfall — sign goes between the zero pads

**Symptom.** `printf("%+06d", 42)` produces `"0+0042"`
instead of `"+00042"`.

**Cause.** Emitting the sign character *after* (or interleaved
with) the zero pad slots.

**Fix.** The only emission order that satisfies every line in
the printf matrix is:

```
if (has_sign) emit(sign_char);
emit_prefix();
for (i = 0; i < pad; i++) emit('0');
for (i = 0; i < precision_pad; i++) emit('0');
emit_str(body);
```

Copy that sequence verbatim into any future printf
implementation in this repo.

### Pitfall — `INT64_MIN`-unsafe negation in `%d`

**Symptom.** `printf("%ld", INT64_MIN)` produces garbage on
some compilers and triggers UBSan on others.

**Cause.** Naive `mag = (uint64_t)(-v)` is undefined for
`v == INT64_MIN`.

**Fix.** Use the identity that avoids the overflow:

```c
mag = (uint64_t)(-(v + 1)) + 1;
```

A pre-existing bug from chapter 19's printf, cleaned up at
the same time as the new sign-flag code because the lines
live side-by-side. The chapter-19 printftest doesn't exercise
`INT64_MIN` and the new tests don't either (would need a
`printf("%ld", INT64_MIN)` check), but the code is now
correct regardless.

---

## Test coverage — `scripts/test_libc_printf2.py`

Single in-guest binary `printftest2`. Pattern matches
`stdlibtest` (chapter 128e): `CHECK()` macro increments
`g_fail`, success marker only fires when `g_fail == 0`.
`CHECK_STR(buf, expected)` does exact-string equality and
prints `got=... want=...` on mismatch.

What's covered (~60 lines of CHECK):

- **Octal** — 0, 8, 0o777, `%#o` prefix, `%#o` on zero.
- **Integer precision** — `.5d`, `.0d`-on-zero (empty),
  `.3x`, `.5d` + width=8, left-justify + precision, the
  precision-overrides-zero-flag corner.
- **Plus / space** — positive, negative, mutual override,
  combined with zero-pad and width.
- **Alt form** — `%#x` lowercase + uppercase + zero + with
  zero-pad width.
- **String precision** — short cap, left-justified short cap
  with explicit width.
- **Variable width / precision** — `%*d` positive, `%.*s`,
  negative `*` width (auto-converts to left-justify).
- **`%n`** — store running output count.
- **Pre-existing behaviour** — `%s %d`, `%05d` negative,
  `%-5d` left-justify (regression-checks the existing surface).
- **sscanf** — `%d` single, `%d %d` whitespace-delimited,
  `%u`/`%x`/`%o`/`%i` base auto-detect, `%s` to a buffer with
  ws skip, `%s` with width cap, `%c` reads literal space,
  `%*d` suppression returns count-minus-one, scansets
  (positive + inverted), `%n` for input position,
  literal-char interleave (`%d:%d:%d`), EOF return on empty
  input, partial-match return on truncated input.

Total wall time ~6 seconds, dominated by boot.

---

## What this unlocks

This is a libc-API chapter; the value is in what upstream code
now compiles unchanged. In-tree consumers that will pick the
new surface up:

- **`userspace/sh/sh.c`** could use `sscanf("%d", ...)` for
  numeric variable expansion. Deferred — `atoi` already works.
- **`userspace/ls/ls.c`** could use `printf("%-10s %8d %s\n",
  ...)` instead of hand-padded columns. Deferred to the
  section-18 cleanup pass.
- **`userspace/date/date.c`** doesn't actually need `printf`
  changes; `strftime` from 128d already produces its output.

New in-tree binary:

- **`userspace/printftest2/printftest2.c`** — the chapter's
  regression target. Listed in `OSFS_BIN_FILES`, baked into
  `build/disk.img` as `/bin/printftest2`.

External code unblocked:

- **GCC `diagnostic.cc`** — `%*s` source-line markers, `%#x`
  tag dumps, `%n` column tracking all work.
- **GCC `gcov.c`** — `sscanf("%[^=]=%d", ...)` for `.gcda`
  parsing.
- **tar `list.c`** — `printf("%-10s %8lld %s\n", ...)` for
  the verbose listing.
- **Doom `i_main.c`** — `sscanf` of `-warp N M`.

---

## What's deferred

- **`%f` / `%e` / `%g` in printf** — float specifiers. Blocked
  on chapter 129 lifting `-mgeneral-regs-only`. The formatter
  has no slot for them; when 129 ships, add a
  `_fmt_render_double` helper and three new cases in the
  switch.
- **`%f` in scanf** — same blocker.
- **`L` length modifier (long double)** — not on any roadmap.
  AArch64 LP64 long double is 128-bit IEEE quad and nothing
  in the toolchain emits it.
- **Positional arguments (`%1$d`, `%2$s`)** — POSIX extension
  used by gettext / i18n. None of the Part XVIII targets are
  localised.
- **`scanf` `%a`** (allocating string variant) — glibc-only
  extension. Not portable.
- **`asprintf` / `vasprintf`** — heap-allocating sprintf
  variants. Easy to add when needed; no in-tree caller.

---

## Things to remember

1. **The pad-vs-precision-vs-sign-vs-prefix order is the only
   tricky thing about printf.** Once the
   sign-first-then-prefix-then-pad-then-precision-then-body
   sequence in `_fmt_emit_padded` is right and the test matrix
   lines up, you're done. Spending time on the digit renderer
   is a waste; spend it on the layout sequencer.

2. **`<stdint.h>` limit macros are conditional in freestanding
   builds.** Use `0x7fffffff` (or define `__STDC_LIMIT_MACROS`
   centrally if you want the macros everywhere). Save the
   `__STDC_LIMIT_MACROS` dance for the chapter that ports
   C++ — until then, literal integer constants are simpler.

3. **Splitting scanf from stdio is the same trick as splitting
   printf from stdio.** The core formatter takes a tiny
   abstract source/sink. String variants are 10 lines (just
   define the cookie + the two callbacks). FILE * variants
   live in stdio.h next to the FILE machinery they need.
   Generalise this pattern when adding the next I/O formatter
   (e.g. a `%`-style strftime, or a regex matcher).

4. **POSIX `%[...]` is more powerful than people remember.**
   It's effectively a one-shot tokenizer. GCC and binutils
   use it for option-parsing in places where you'd reach for
   strtok. Worth knowing — it dropped some ad-hoc tokenizer
   code from the eventual binutils port.

---

## What's next

Chapter 129 — FP/SIMD at EL0. Kernel change: set
`CPACR_EL1.FPEN = 0b11` at boot; extend `trap_frame` with
`q0..q31` + `fpsr` + `fpcr`; save/restore on context switch
(lazy strategy — only do it if the outgoing task touched FP).
Drop `-mgeneral-regs-only` from `USER_CFLAGS`. Re-enable the
deferred surface in `setjmp.S` (save `d8..d15`), `time.h`
(`difftime` actually works), `stdlib.h` (`strtod` / `strtof`
re-enabled), `printf.h` (`%f` / `%e` / `%g`), `scanf.h`
(`%f`). Add a FP regression test.
