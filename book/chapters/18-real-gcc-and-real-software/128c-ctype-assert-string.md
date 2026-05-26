# Chapter 128c — ctype.h, assert.h, and the str* family

> **Status:** complete. `scripts/test_libc_string.py` green.
> **Prereq:** chapter 128b (raise/abort, expanded SIG table).
> **Unlocks:** every upstream C program needs these three
> headers before it will even parse. With 128c in place the
> libc surface is wide enough that BearSSL is no longer the
> only third-party code we can build.

---

## What you'll do in this chapter

1. Add `userspace/libc/ctype.h` (header-only, every function
   `static inline`, C-locale ASCII).
2. Add `userspace/libc/assert.h` plus the extern
   `__assert_fail` it calls (implementation in
   `userspace/libc/cstring.c`).
3. Add `userspace/libc/string.h` covering the `str*` family
   as `static inline` and re-declaring the existing `mem*`
   externs.
4. Write two regression binaries: `strtest` (positive and
   negative cases for every function) and `assertfail`
   (proves `assert(0)` dies with the right diagnostic and
   exit status 134).
5. Run `scripts/test_libc_string.py` and watch it land green.

## Why now

`<ctype.h>`, `<assert.h>`, and the `str*` family of `<string.h>`
are the bedrock of every C program written since 1990. Doom's
`d_main.c` opens with `#include <ctype.h>` on line 3 and uses
`toupper`/`tolower` a dozen times in its argv parsing. The GNU
build system's `configure` scripts auto-detect *exactly* these
three headers before doing anything else — if any of them is
missing the script aborts with `error: missing required header`.

Up to this chapter the userspace has gotten away with no
ctype, no assert, and a sliver of string (`strlen` only, as a
single static inline shim in `userspace/libc/syscall.h`). Every
in-tree binary that needed more rolled its own. That's been
fine for thirty hand-written apps; it would be ridiculous for a
GCC port.

What you'll ship in this chapter:

1. **`<ctype.h>`** — fully header-only, every function `static
   inline`. C-locale-only.
2. **`<assert.h>`** — the macro plus the extern
   `__assert_fail` it calls. Implementation lives in
   `userspace/libc/cstring.c` (which gets linked into every
   binary that uses `assert()`).
3. **`<string.h>`** — header-only `static inline` definitions
   for the str* family. The `mem*` family already lives as
   extern symbols in `cstring.c`; `string.h` just re-declares
   them so callers can include one header.
4. Two regression binaries: `strtest` (exercises every
   function with positive and negative cases) and `assertfail`
   (calls `assert(0)` and dies with the expected diagnostic and
   exit status 134).

---

## The "static inline vs extern" question

There are two patterns for libc symbols in this codebase, and
chapter 128c uses both deliberately.

| Pattern | When | Examples |
|---|---|---|
| `static inline` in a header | Short, leaf, one-or-two-instruction body | `isdigit`, `strlen` (in `syscall.h`), `tolower` |
| Extern in `cstring.c` | Either called by external `.a` archives or large enough to be worth de-duplicating | `memcpy`, `memset`, `__assert_fail` |

The `string.h` header re-declares the extern `mem*` symbols
from `cstring.c` so callers can `#include <string.h>` and get
the full surface. The `str*` functions are short enough to be
static inline; the `mem*` ones are extern because BearSSL's
already-compiled archive references them by name from outside
your header reach.

`__assert_fail` is extern because the diagnostic-printing code
(file:line:func: Assertion `expr' failed.\n) is ~30 lines and
inlining it into every `assert(0)` call site would bloat every
binary. One copy in `cstring.o` is right.

---

## Headers and what they provide

### `userspace/libc/ctype.h`

C99 7.4. All ASCII, all `static inline`:

```text
isascii  isdigit  isxdigit  isupper  islower  isalpha
isalnum  isspace  isblank   iscntrl  isprint  isgraph
ispunct  tolower  toupper
```

C99 says the argument "shall be representable as an unsigned
char or shall be the value EOF". The implementations don't
crash on out-of-range inputs (they use range comparisons, not
table lookups), but the header docstring warns callers to
cast to `(unsigned char)` for portability.

### `userspace/libc/assert.h`

Two macros:

- `assert(expr)` — if `NDEBUG` is defined at include time,
  expands to `((void)0)`. Otherwise: if `!expr`, calls
  `__assert_fail("#expr", __FILE__, __LINE__, __func__)`,
  which never returns.
- `static_assert(cond, msg)` — wrapper around C11
  `_Static_assert`.

`__assert_fail` is forward-declared `extern noreturn`. Real
implementation in `cstring.c`.

### `userspace/libc/string.h`

Three groups:

1. **Re-declarations of cstring.o externs**: `memcpy`,
   `memmove`, `memset`, `memcmp`, `strlen`. Repeated
   `extern` of the same signature is legal in C.

2. **Header-only static inline str***:

   ```text
   memchr   strchr    strrchr  strcmp   strncmp  strcpy
   strncpy  strcat    strncat  strspn   strcspn  strpbrk
   strstr   strlcpy   strlcat  atoi
   ```

   `strncpy` matches the broken-by-spec POSIX semantics
   (zero-pads if source shorter than `n`, no NUL appended
   if longer). Use `strlcpy` if you want sane behaviour.

3. **`atoi`** — accepts leading whitespace and optional sign.
   No error reporting; that's `strtol`'s job (chapter 128e).

---

## The Makefile pattern

`strtest` and `assertfail` both need `cstring.o` for their
externs. The pattern:

```make
STRTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/strtest/strtest.o \
                $(BUILD)/userspace/libc/cstring.o
```

### Pitfall — `:=` resolves immediately

**Symptom.** The link fails with `undefined reference to
`__assert_fail`` even though `cstring.o` is named in the
OBJS list.

**Cause.** Make's `:=` is *immediate-expansion*. If you write
`STRTEST_OBJS := ... $(CSTRING_OBJ)` and `CSTRING_OBJ` is
defined further down the Makefile, the variable is empty at
the point of assignment and the link line ends up with no
`cstring.o`.

**Fix.** Reference the literal path
`$(BUILD)/userspace/libc/cstring.o` in any OBJS block defined
above the `CSTRING_OBJ := …` line. (Alternative: use `=` for
lazy expansion. Existing convention here is `:=` everywhere;
literal path keeps it consistent.)

---

## Test design

### `strtest`: positive + negative for everything

Every assertion is wrapped in a `CHECK(expr)` macro that
increments a fail counter and prints `FAIL file:line: expr`
on failure. Success path prints `[strtest] all checks passed`
exactly once at the end. Failure path prints the count and
exits non-zero.

The harness asserts both:

1. `all checks passed` appears in the log.
2. No `FAIL` lines appear *before* `all checks passed`.

(2) catches the case where the test binary prints individual
FAILs but then also reaches the success line through a path
that doesn't check `g_fail` — a bug pattern worth defending
against in every harness.

### `assertfail`: separate binary

`assert(0)` terminates the process. You can't put it at the
end of `strtest` because then earlier tests get to run but the
final "success" marker never prints, and the harness gets the
wrong signal. Separate binary keeps the semantics clean:

- Run `assertfail`. Wait for `Assertion` in the output.
- Wait for shell prompt (the process has died, shell is back).
- `echo assertfail_status=$?`, parse the LAST occurrence of the
  marker (shell echoes typed bytes first; the expansion comes
  second).
- Assert status == 134 == 128 + SIGABRT.

### The TTY-echo gotcha (already documented in 128b)

Same trap as `test_signal_raise.py`. If you grep for the marker
text, you'll find the *typed command's echo* before you find
the *shell's expansion*. Always take the LAST occurrence after
the prompt that follows the command's completion. Two test
scripts now encode this pattern; before adding a third,
extract it to a helper.

---

## Pitfalls

Three more to know about beyond the `:=` one above:

### Pitfall — `extern long _svc1(...)` doesn't link

**Symptom.** `cstring.c` adds `extern long _svc1(...)` and
`extern long _svc3(...)` and the build fails at link with
`undefined reference to _svc1`.

**Cause.** `_svc1` and `_svc3` are `static inline` in
`syscall.h`, instantiated per-TU. `cstring.c` doesn't include
`syscall.h` (it's a leaf TU that BearSSL pulls in), so
referencing those names as externs catches nothing at compile
time but blows up at link time.

**Fix.** Re-emit the svc asm directly in `cstring.c` as
`__cstring_svc1` / `__cstring_svc3` static inlines.

### Pitfall — wrong default syscall numbers

**Symptom.** `assertfail` exits, but the harness sees the
wrong status code (e.g. write fails silently and the process
limps out with a stale exit code).

**Cause.** Fallback `#define`s in `cstring.c` had `SYS_EXIT 1`,
`SYS_WRITE 5` — guessed, not looked up. Real values are
`SYS_WRITE 1`, `SYS_EXIT 2`.

**Fix.** Look up `userspace/libc/syscall.h` lines 21–22 before
writing fallback constants. Better: include the header and
let it own the numbers.

---

## Surface count

After this chapter the libc surface looks like:

| Header | Status | Chapter |
|---|---|---|
| `<stddef.h>`, `<stdint.h>` | toolchain | — |
| `<errno.h>` | done | 116a |
| `<stdio.h>` (FILE*) | done | 116b |
| `<env.h>` (getenv) | done | 116c |
| `<sys/stat.h>`, `<fcntl.h>`, `<dirent.h>` | done | 117 |
| `<signal.h>` | done | 77 + 128b |
| `<setjmp.h>` | done | 128a |
| `<ctype.h>` | done | **128c** |
| `<assert.h>` | done | **128c** |
| `<string.h>` (str*) | done | **128c** |
| `<string.h>` (mem*) | done | 112a (in `cstring.c`) |
| `<time.h>` | pending | 128d |
| `<stdlib.h>` qsort/bsearch/strtol | pending | 128e |
| `<stdlib.h>` getopt | pending | 128e |
| `<stdio.h>` %f/scanf | pending | 128f |

Once 128f lands, the surface is wide enough to start building
binutils (chapter 131).

---

## What this unlocks

This chapter adds new binaries; nothing existing was rewritten
because the targets — Doom, GCC, binutils — aren't ported yet.
The applied-to wave will hit during chapters 130–133 when the
real third-party builds start consuming these headers.

- **New test binaries**:
  - `userspace/strtest/strtest.c`
  - `userspace/assertfail/assertfail.c`
- **New scripts**:
  - `scripts/test_libc_string.py`
- **New headers**:
  - `userspace/libc/ctype.h`
  - `userspace/libc/assert.h`
  - `userspace/libc/string.h`
- **Extended**:
  - `userspace/libc/cstring.c` (+ `__assert_fail` and its
    private svc trampolines)

---

## Things to remember

- **Make's `:=` resolves immediately.** When adding new OBJS
  blocks, check the line number of every variable you
  reference. Lazy `=` or literal paths are safer for
  cross-chapter refs.
- **`static inline` isn't extern.** Adding `extern long
  _svc1(...)` to a TU that doesn't include `syscall.h`
  compiles fine and fails at link. Either include the header,
  or re-emit the asm.
- **Look up the constants.** `SYS_WRITE` and `SYS_EXIT` are
  literally on lines 21–22 of `syscall.h`. Don't guess.
- **Separate binary for assertion failures.** Anything that
  calls `assert(0)` (or `abort()`, or `_exit()`) needs its own
  `userspace/<name>/` directory. Don't bury
  terminate-the-process tests inside larger sweeps.
