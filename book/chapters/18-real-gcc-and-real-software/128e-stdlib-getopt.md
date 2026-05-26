# Chapter 128e — `<stdlib.h>`: `qsort`, `bsearch`, `strtol`, `getopt`

> **Milestone in this chapter:** ship the C99 / POSIX surface
> missing from libc — the `strto*` family, `qsort` / `bsearch`,
> and `getopt` — so Doom's command-line parser, binutils'
> symbol-table sort, and GCC's option dispatch all compile
> unchanged.
> **Code referenced:**
> - [userspace/libc/stdlib.h](../../../userspace/libc/stdlib.h)
>   (the umbrella header)
> - [scripts/test_libc_stdlib.py](../../../scripts/test_libc_stdlib.py)
>
> **At the end of this chapter** you will have `strtol` /
> `strtoul` / `strtoll` / `strtoull` with base-0 auto-detection
> and per-digit overflow handling, `qsort` (median-of-three
> quicksort + insertion sort for small n) and `bsearch`, and a
> POSIX `getopt` with `optarg` / `optind` / `optopt` / `opterr`.
> Prerequisites: chapter 128c (string / ctype / assert),
> chapter 116a (errno).

---

## What you'll do in this chapter

1. Ship `userspace/libc/stdlib.h` as an umbrella header that
   pulls in the existing `env.h`, `malloc.h`, `atexit.h`,
   `string.h` and adds the C99 surface that's still missing.
2. Add the `strtol` / `strtoul` / `strtoll` / `strtoull`
   family with full base-0 auto-detection and per-digit
   overflow handling.
3. Add `qsort` (median-of-the-middle quicksort,
   insertion-sort for n < 12) and `bsearch`.
4. Add POSIX `getopt` with `optarg` / `optind` / `optopt` /
   `opterr`.
5. Write a `stdlibtest` binary that exercises every entry
   point with positive and negative cases, then run
   `scripts/test_libc_stdlib.py` and watch it land green.

Float conversion (`strtod` / `strtof` / `strtold`) is deferred
to chapter 129 — they return `double`, which the
`-mgeneral-regs-only` build forbids today.

## Why now

`<stdlib.h>` is the C standard library's grab-bag header. It
covers process control (`exit`, `atexit`), memory (`malloc`,
`free`), conversion (`strtol`, `strtod`), and the two generic
algorithms (`qsort`, `bsearch`). The codebase already ships
most of the process-control and memory pieces, but spread
across `atexit.h`, `malloc.h`, and `env.h`. Upstream code
expects a single `#include <stdlib.h>` to give all of it.

Beyond bundling, this chapter actually adds new functionality:

1. **`qsort` and `bsearch`** — binutils' symbol-table code
   sorts elf symbols by name before binary-searching for
   relocations. GCC's `tree.cc` sorts type vectors before doing
   the same. Without these two, both ports stall at "ld: error:
   undefined reference to `qsort`" the moment you link them.
2. **`strtol`, `strtoul`, `strtoll`, `strtoull`** — Doom's
   `-warp 1 3` argv parsing needs `strtol`; binutils' linker
   script parser needs `strtoul` for hex addresses; GCC's
   `cpp_token_pos` parser needs the long-long variants. `atoi`
   (chapter 128c) is too lossy — it can't report invalid input
   and can't return values larger than `INT_MAX`.
3. **`getopt`** — Doom calls `getopt(argc, argv, "iwm:n")` to
   tease apart `-iwad`, `-warp`, `-monsters N`. tar uses it for
   `-x`, `-v`, `-f FILE`. `getopt` is the smallest piece of
   POSIX that lets command-line tools feel like Unix tools.

The float-conversion siblings `strtod` / `strtof` / `strtold`
are NOT in this chapter — they return `double`, which the
`-mgeneral-regs-only` build forbids. They land in chapter 129
once FP at EL0 turns on. Doom doesn't call them; binutils and
GCC don't call them until the post-FP chapters anyway, so the
deferral is free.

---

## What ships

| Surface | What |
|---|---|
| Macros | `EXIT_SUCCESS`, `EXIT_FAILURE`, `MB_CUR_MAX`, `RAND_MAX`, the `INT_*`/`LONG_*`/`LLONG_*`/`ULONG_*`/`ULLONG_*` limit suite |
| Types | `div_t`, `ldiv_t`, `lldiv_t` |
| Conversion | `strtol`, `strtoul`, `strtoll`, `strtoull` (bases 0, 2..36; leading whitespace + sign + `0x`/`0` auto-detect; overflow → `errno = ERANGE`, returns `*_MAX`/`*_MIN`) |
| Convenience | `atol`, `atoll` (decimal-only shortcuts onto `strtoll`); `atoi` is re-exported from `string.h` |
| Arithmetic | `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv` |
| Algorithms | `qsort` (median-of-the-middle quicksort, insertion-sort for n < 12), `bsearch` |
| argv parsing | `getopt` + `optarg` / `optind` / `optopt` / `opterr` |
| Re-exports | Pulls in `env.h` (`getenv`/`setenv`/`unsetenv`/`putenv`/`clearenv`), `malloc.h` (`malloc`/`free`/`realloc`/`calloc`), `atexit.h` (`atexit`/`__cxa_finalize`), and `string.h` (`atoi`) so callers get the full C99 surface from one `#include` |

Header-only. No new `.c` file, no new linker dependency. The
chapter adds one new test binary (`stdlibtest`) and edits one
existing header (`atexit.h`, see below).

---

## Algorithm choices

### `strtol` family: one unsigned core, four signed wrappers

Real libc factors `strtol` and friends into:

```text
__strtoull_raw(nptr, &endptr, base, &overflow) -> unsigned long long
        |
        | (called by)
        v
strtoull(nptr, &endptr, base)      -> clamp to ULLONG_MAX
strtoul (nptr, &endptr, base)      -> clamp to ULONG_MAX
strtoll (nptr, &endptr, base)      -> re-apply sign, clamp to LLONG_*
strtol  (nptr, &endptr, base)      -> via strtoll, clamp to LONG_*
```

Three traps fall out of building it this way:

1. **Base 0 has to look ahead at `s[2]`.** "Base 0 means
   auto-detect" — `0x`/`0X` ⇒ base 16, leading `0` ⇒ base 8,
   anything else ⇒ base 10. But `s[1]` being `'x'` isn't
   enough: if `s[2]` is not a hex digit, you *must* treat the
   "0" as a decimal zero and leave the `x` as the terminator
   (so `strtol("0x", &ep, 0) == 0`, `*ep == 'x'`). Real libc
   does this exact lookahead. The test `strtol("0xFF", ...)
   == 255` catches the bug; `strtol("0x", ...) == 0` would
   catch the inverse.
2. **Per-digit overflow is checked against `ULLONG_MAX / base`
   and `% base`, NOT against the final accumulator.** Checking
   only at the end would itself overflow the accumulator. The
   loop computes `cutoff = ULLONG_MAX / base` and `cutlim =
   ULLONG_MAX % base` once; then `acc > cutoff` (any further
   digit would overflow) OR `acc == cutoff && d > cutlim` (the
   exact-boundary case where one specific next-digit value
   tips it over) marks `*overflow = 1`. This is the same
   algorithm as glibc's `strtol_l_internal`.
3. **Negative overflow has a one-magnitude-larger limit than
   positive.** `LLONG_MIN == -LLONG_MAX - 1`. The signed
   wrapper has to special-case `mag == (LLONG_MAX + 1)` and
   return `LLONG_MIN` without a sign flip, because
   `-(long long)mag` would itself overflow for that value.
### `qsort`: simple quicksort, byte-at-a-time swap

Simple quicksort with insertion-sort-for-small-n beats a
fancier introsort or pattern-defeating quicksort for this
codebase, because:

- There are no benchmarks pushing toward asymptotically
  pathological inputs. Doom sorts <100 elements at a time;
  binutils' symbol tables are larger (~10k) but already nearly
  sorted (the assembler emits them in source order). Both
  cases are fine for naive quicksort.
- Pivot is the middle element. This avoids the worst case for
  the most common adversarial input you'll see: already-sorted
  arrays. A median-of-three would be marginally better but
  adds a swap + branch per recursion frame.
- Element swap is **byte-at-a-time**. The obvious 64-bit-word
  swap loop runs faster, but breaks for callers passing
  3-byte structs (binutils' `relent[]` with a 3-byte
  reloc-type field) because the last word goes past the end
  of the array. The goal is "runs correctly on any size" not
  "fastest possible". One day someone will profile and notice
  qsort is hot; that's the time to grow a fast path. Not
  today.

Edge cases that the test pins down:

- `nmemb == 0` → return without touching `base`. Some callers
  pass `NULL` here as well; the implementation tolerates that.
- `nmemb == 1` → single insertion-sort pass over zero
  comparisons, no-op.
- `size == 0` or `cmp == NULL` → return without crashing.
  Real libc isn't required to handle these and most implement
  UB; the implementation here degrades gracefully because the
  cost is one branch and the alternative is a hard-to-debug
  guest fault.

### `bsearch`: textbook half-interval search

Five lines of actual logic. The midpoint computation uses
`lo + (hi - lo) / 2` rather than `(lo + hi) / 2` because the
latter can overflow on huge arrays — academic here (no
`bsearch` over a 2-billion-entry table is on the horizon) but
the safer form is the same number of instructions.

### `getopt`: POSIX, no GNU long-options

Two pieces of per-process state:

- The four spec'd externs `optarg`, `optind`, `optopt`,
  `opterr`. Declared `static` inside the header so each TU
  gets its own copy. This is fine because every in-tree
  binary is a single TU; the moment a binary spans multiple
  `.c` files these have to move to a real `.c`. (Doom and
  binutils each cross that line — when they get ported,
  `getopt` is lifted out of the header into a
  `userspace/libc/getopt.c`.)
- A `static int subindex` cursor *inside* `getopt`. Lives
  across calls so clustered short-options work: `-abc` is
  three calls returning `'a'`, `'b'`, `'c'` from one argv
  token.

Three call shapes the test pins down:

1. `-iwad DOOM1.WAD` — argument is the next argv token.
2. `-m4` — argument is the rest of the current token.
3. `-abc hello` plus `"abc:"` — first two are arg-less
   flags, third (`c`) wants an argument which it gets from
   the next token.

Two error shapes:

1. `-X` with optstring `"ab"` → return `'?'`, set `optopt
   = 'X'`.
2. `-b` with optstring `":b:"` and no following token →
   return `':'` (the leading-colon "silent" mode) instead
   of `'?'`, set `optopt = 'b'`.

GNU long options (`--name=value`) are NOT supported. Doom
doesn't use them. binutils does (`--strip-all`), but its
own driver already vendors a copy of `getopt_long`, so the
port can keep using that. GCC ships `libiberty/getopt.c`
and `libiberty/getopt1.c` from a vintage GNU snapshot —
again, self-contained.

---

## Pitfalls

### Pitfall — `<stdlib.h>` can't forward-declare `static inline` symbols

**Symptom.** Build fails with
`error: static declaration of 'getenv' follows non-static declaration`.

**Cause.** A classic-libc-shape opener like:

```c
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
void *malloc(size_t n);
void  exit(int status);
int   atexit(void (*fn)(void));
int   atoi(const char *s);
```

looks fine, but `env.h` already defines `static inline char
*getenv(...)`. C lets you forward-declare and then define,
but the two declarations have to agree on storage class.

**Fix.** Don't forward-declare. Make `<stdlib.h>` `#include`
the four homes of those symbols (`env.h`, `malloc.h`,
`atexit.h`, `string.h`). One include of `<stdlib.h>` cascades
into the full C99-shaped surface. Each of the included
headers has its own multi-include guard, so this is safe even
in TUs that already pulled them in directly.

### Pitfall — `static int atexit(...)` triggers `-Wunused-function`

**Symptom.** After fixing the linkage issue above, the next
compile fails with
`error: 'atexit' defined but not used [-Werror=unused-function]`.

**Cause.** `atexit.h` was using bare `static` (not `static
inline`) for its definition. That's fine in a TU that calls
`atexit()`, but `stdlibtest` doesn't — it includes
`<stdlib.h>` (which includes `atexit.h`) and only uses
qsort/strtol/getopt. GCC quite reasonably flags an unused
static function.

**Fix.** Convert `atexit.h`'s `static int atexit(...)` to
`static inline int atexit(...)`. GCC explicitly suppresses
the unused warning for `inline` functions, on the assumption
that they're library helpers that may or may not be called
from a given TU. Behaviour for callers is unchanged at the
`-Os` setting — inline already gets eaten or kept based on
size.

**Lesson.** Any `static` definition in a header that's part
of the core libc surface should be `static inline`. Bare
`static` is fine in app-private headers; in libc, it breaks
the moment anyone wires the header into a header-only
"umbrella" include like `<stdlib.h>`.

### Pitfall — a `*/` inside a block comment eats your `#include`

**Symptom.** GCC says
`error: extra tokens at end of #include directive [-Werror]`.

**Cause.** A header containing:

```c
#include "string.h"     /* atoi (and the rest of mem*/str*) */
```

The `*/` inside the comment terminates the comment at the
first occurrence, leaving the trailing `str*) */` as stray
tokens after the include.

**Fix.** Paraphrase the comment so it doesn't contain `*/`.
For example: `/* atoi and the mem/str surface */`.

---

## Test coverage — `scripts/test_libc_stdlib.py`

One in-guest binary, `stdlibtest`, exercises every public
entry point with at least one happy-path and one edge-case
input. Same `CHECK()` + `g_fail` counter + `"all checks
passed"` marker pattern as `strtest` (chapter 128c) and
`timetest` (128d). The script boots the kernel, waits for
`$ `, runs the binary, fails on any `FAIL`/`PANIC` line, and
prints `stdlibtest: PASS` on success.

What `stdlibtest` covers:

- **`abs`/`labs`/`llabs`** — positive, negative, zero.
- **`div`/`ldiv`** — positive quotient + remainder, and the
  C99-specified truncation-toward-zero behaviour for
  negative dividends.
- **`strtol`** — base 0 with `0x` prefix, base 10 with
  trailing junk (verifies `*endptr` lands on the first
  non-digit), base 8 detection (`0` prefix), base 36 (last
  legal base), invalid input (returns 0, `*endptr == nptr`),
  positive overflow (`LONG_MAX` + `errno = ERANGE`), negative
  overflow (`LONG_MIN` + `errno = ERANGE`).
- **`strtoul`** — explicit `0x` prefix, base 2 binary.
- **`strtoll` / `strtoull`** — the wide-magnitude edges that
  `strtol` can't represent (`-9223372036854775807`,
  `18446744073709551615`).
- **`atol` / `atoll`** — that they're real shortcuts onto
  the strtoll family, not separate parsers.
- **`qsort`** — a 14-element int array, a 8-element string
  array (binutils symbol-table use case in miniature), and
  the `nmemb=0`/`nmemb=1` edge cases.
- **`bsearch`** — hit and miss, on both int and string
  arrays.
- **`getopt`** — full Doom-style invocation: argument-taking
  flags with both `-i FILE` and `-m4` styles, an arg-less
  flag, a trailing positional, optind advancing correctly
  to point at the positional after the option loop.
- **`getopt`** clustered short options (`-abc`).
- **`getopt`** unknown-option diagnostic (returns `'?'`,
  sets `optopt`).
- **`getopt`** silent-mode missing-arg (returns `':'`).

Total: ~60 CHECK lines. Single run takes ~6 seconds, mostly
boot.

---

## What this unlocks

This is a libc-API-only chapter; the value is in what
future ports can do unchanged. In existing in-tree code, the
chapter's surface gets picked up incrementally:

- **`userspace/sh/sh.c`** could swap its hand-rolled
  `parse_number` for `strtol(_, _, 0)` to gain hex literal
  support (`echo $(sleep 0x10)`). Deferred — not on the
  critical path for section 18.
- **`userspace/sleep/sleep.c`**, **`head`**, **`tail`** all
  call `atoi(argv[1])` today. They'll move to
  `strtol(argv[1], &ep, 0)` with `*ep != '\0' → "invalid
  argument"` diagnostics when the section-18 cleanup pass
  happens after Doom builds.

New in-tree binaries this chapter added:

- **`userspace/stdlibtest/stdlibtest.c`** — the chapter's
  regression target. Listed in OSFS_BIN_FILES, baked into
  `build/disk.img` as `/bin/stdlibtest`.

External code this chapter unblocks:

- **DoomGeneric** (chapter 130) — its `M_CheckParm` /
  `M_GetParm` argv parser stops failing at link time.
- **binutils `gas`** (chapter 131) — its symbol-table sort
  and search no longer need a manual `qsort` paste.
- **GCC's libiberty** (chapter 132) — `xstrtol` builds
  unmodified.

---

## What's deferred

- **`strtod` / `strtof` / `strtold`** — return `double` or
  `float`, blocked by `-mgeneral-regs-only`. Ship in
  chapter 129 alongside the FP-at-EL0 work.
- **`rand` / `srand`** — `RAND_MAX` is defined but the
  functions aren't. No in-tree caller; Doom carries its
  own LCG. Add a Park–Miller LCG when something actually
  needs it.
- **`mblen` / `mbtowc` / `wctomb` / `mbstowcs` /
  `wcstombs`** — locale-aware functions. The OS is
  C-locale, ASCII-only. Add stubs that just memcpy if a
  port ever needs them.
- **`bsearch` and `qsort` with `_r` (reentrant) variants**
  — GNU extensions. Not portable, not required by any
  upstream Part XVIII plans to build.
- **`getopt_long`** — GNU extension. binutils and GCC both
  ship their own copies inside libiberty; the libc doesn't
  need one.
- **A multi-TU `userspace/libc/getopt.c`** — the moment a
  binary ports that spans more than one `.c` file (Doom
  will), `optarg`/`optind`/etc. have to move out of the
  header. Tracked in chapter 130a.

---

## Things to remember

1. **Static inline is the libc default, not bare static.**
   Bare `static` in a header works as long as every include
   site uses every symbol. The moment one include site uses
   only some, `-Wunused-function` fires. Start with `static
   inline` everywhere when adding the next libc header.

2. **C99 `<stdlib.h>` is an umbrella header.** Real glibc and
   musl ship `<stdlib.h>` as a one-stop include that pulls
   in everything you'd reach for. Trying to make it a thin
   extern-declaration layer fights the existing `static
   inline` headers (env.h, malloc.h, atexit.h). The right
   structure: split the implementation across topic-specific
   headers, then make `<stdlib.h>` a thin include cascade.
   Future C99-shaped umbrellas (`<stdio.h>` past chapter
   116b additions, `<unistd.h>` if one ever ships) should
   follow the same pattern.

3. **Per-digit overflow check, not end-of-loop overflow
   check.** Every libc author makes this mistake once.
   End-of-loop check overflows the accumulator itself; only
   per-digit cutoff/cutlim reasoning is sound. Copy the
   `cutoff = MAX/base; cutlim = MAX%base; acc > cutoff || (acc
   == cutoff && d > cutlim)` snippet straight from `strtol`
   when writing the next numeric parser.

4. **`*/` inside `/* */` ends the comment.** Trivial but
   `-Werror` makes it un-ignorable. When paraphrasing a
   real symbol like `mem*/str*`, write "the mem/str
   surface" instead.

---

## What's next

Chapter 128f — replace the chapter-19 minimal `printf` with
a real one. `%d`, `%u`, `%x`, `%o`, `%s`, `%c`, `%p`, `%lld`
plus width / precision / flag modifiers (`-`, `+`, ` `, `0`,
`#`). `%f`/`%e`/`%g` deferred to chapter 129. Adds `scanf`
for the `%d` and `%s` subset. Then chapter 129 turns FP on,
re-enables `strtod` here, and re-enables `%f` in 128f's
`printf`.
