# Chapter 28 — A `printf` for the user libc

Every chapter so far has ended with at least one user program
hand-formatting integers with a `putn` helper that does the
divide-by-10 dance, then concatenating fragments with `write(1,
str, strlen(str))`. The pattern is always the same:

```c
write(1, "cat: cannot open ", 17);
write(1, target, strlen(target));
write(1, ": errno=", 8);
putn(-fd);
write(1, "\n", 1);
```

Five lines to print one error message. Multiply that by every
program with non-trivial output and the cost of not having
`printf` becomes obvious. Time to retire the `putn`s.

## A header-only formatter

`userspace/libc/printf.h` is a single-file `printf` family. Same
shape as `malloc.h` from chapter 26: every `.c` that includes it
gets its own private inline copy. Fine because each in-tree
binary is one source file, and inlining lets the compiler
constant-fold away most of the format-string parsing for typical
calls.

The supported subset:

| Specifier | Meaning |
|-----------|---------|
| `%d` `%i` | signed decimal int (or `%ld`/`%lld`/`%zd` for 64-bit) |
| `%u`      | unsigned decimal int |
| `%x` `%X` | unsigned hex, lower / upper case |
| `%p`      | pointer (always `0x%016lx`) |
| `%c`      | single character |
| `%s`      | NUL-terminated string (`(null)` if NULL) |
| `%%`      | literal `%` |

Flags: `-` (left-justify), `0` (zero-pad). Width: any decimal
number after the flags. Length modifiers `l`, `ll`, `z`, `h`,
`hh` are accepted; on AArch64 LP64 we treat all of them the same
way internally (everything is 64 bits) but parsing them lets
existing format strings work unchanged.

What's deliberately missing: floating-point (no FPU support
configured, no use case yet), precision (`%.5d`, `%.5s`),
positional args (`%2$s`), `+` flag, `#` flag. Each is
mechanical to add when the first program needs it.

## The single dispatcher

The whole design is one inner function plus three public
wrappers:

```
                ┌─────────────────────┐
                │  _fmt_vformat()     │  parses fmt string
                │  with a sink struct │  emits into sink
                └──────────┬──────────┘
                           │
            ┌──────────────┴──────────────┐
            │                             │
            ▼                             ▼
        sink.buf == NULL          sink.buf != NULL
        ──────────────────         ──────────────────
        batch up to 128 chars,    write into buf up to
        then SYS_WRITE on fd 1    cap-1 bytes, NUL-term
            (printf path)            (snprintf path)
```

A `struct _fmt_sink` carries either a destination buffer
(snprintf mode) or an internal 128-byte batch buffer (printf
mode). `_fmt_emit(c)` increments the running total and either
appends to the buffer or flushes the batch when full. Splitting
the "what's a character?" question from "where does it go?" means
both `printf` and `snprintf` share the entire formatter.

The return value follows C99: total number of characters that
*would have been written* with infinite buffer. So `snprintf`
truncating doesn't change the return — useful for "how big a
buffer would I need?" probes.

## A small pitfall: implicit `memset`

The first attempt at the wrappers looked like this:

```c
struct _fmt_sink s = { 0 };
```

GCC saw a 152-byte struct (the 128-byte batch buffer is the bulk)
being zero-initialized and emitted a call to `memset`. We don't
have a `memset` symbol — we're freestanding. The fix was just to
initialize the scalar fields explicitly:

```c
struct _fmt_sink s;
s.buf = NULL; s.cap = 0; s.used = 0; s.total = 0; s.batch_len = 0;
```

The `batch[128]` array is left uninitialized — we never read from
it before writing, so its initial contents don't matter. The
generated code drops the `memset` call entirely. This is the same
freestanding-C trap that bites every kernel project the first
time it tries to write a non-trivial library.

## `printftest` — every specifier in one screen

```
$ /bin/printftest extra arg here
[printftest] starting
  hello world, you are 7 years old
  unsigned 43981, hex dead, HEX BEEF
  ptr 0x0000000000123456, char 'Q', literal %
  long  -1234567890, ulong 1122334455667788
  size  4096
  '|   42|' '|42   |' '|00042|'
  '|-0007|' (negative)
  argc=4
    argv[0] = "/bin/printftest"
    argv[1] = "extra"
    argv[2] = "arg"
    argv[3] = "here"
  snprintf -> n=15 buf="pi=3.14, x=cafe"
  trunc    -> n=15 buf="abcdefg"
[printftest] all checks passed
```

Things to read out of that output:

- `%u 0xABCD` → `43981` (correct decimal conversion of 0xABCD)
- `%X 0xBEEF` → `BEEF` (uppercase hex)
- `%p 0x123456` → `0x0000000000123456` (full 64-bit zero-pad)
- `%lx 0x1122...88` → `1122334455667788` (full long, no truncation)
- `%5d 42` → `"   42"` (right-justified, width 5)
- `%-5d 42` → `"42   "` (left-justified)
- `%05d 42` → `"00042"` (zero-padded)
- `%05d -7` → `"-0007"` (sign before zero-pad, not after)
- snprintf into 8-byte buf truncates body at 7 chars + NUL,
  but returns 15 (the would-have-been length)

The argv echo also exercises chapter 27's stack layout — `argc=4`
because `/bin/printftest` itself is `argv[0]` plus the three
shell tokens.

## Migrations

Three programs got the cleanup treatment:

**`hello.c` — before:**
```c
char buf[32];
char *p = buf;
*p++ = 'p'; *p++ = 'i'; *p++ = 'd'; *p++ = '='; *p++ = '0'; *p++ = 'x';
for (int shift = 28; shift >= 0; shift -= 4) {
    unsigned nib = (unsigned)((pid >> shift) & 0xF);
    *p++ = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
}
*p++ = '\n';
write(1, buf, (size_t)(p - buf));
```

**after:**
```c
printf("pid=0x%08x\n", getpid());
```

**`cat.c`** dropped its hand-rolled `putn` and the five-write
error path. **`echo.c`** dropped its `write(1, " ", 1)` and
`write(1, s, strlen(s))` dance. Net source line reduction across
the three programs: ~50 lines.

The `puts()` and `write()` wrappers in `syscall.h` are kept for
the cases (early boot, panic paths) where you really do want a
single syscall with no formatting overhead.

## Binary size cost

Every program that includes `printf.h` gets its own copy of the
formatter. The cost:

| program     | before (bytes) | after (bytes) | delta |
|-------------|----------------|---------------|-------|
| `hello`     | 4592           | 6488          | +1896 |
| `cat`       | 4960           | 6704          | +1744 |
| `echo`      | 4480           | 6496          | +2016 |

About 1.7-2.0 KiB per binary. For an OS that fits in a 1 MiB disk
image, this is fine — we have ~900 KiB of slack. If it ever
matters we can pull the formatter into a shared `.a`/`.so` (which
needs a real linker setup we don't have yet) or ship a single
`libc.o` that all binaries share via static link from the
Makefile.

For now: the cost is one-page per binary, and the readability
gain is enormous.

## What this unlocks

- Every future user program can use `printf` instead of
  hand-formatting. Reduces the activation energy for writing
  small utilities (`ls`, `wc`, `head`, `cp`).
- Debug output from in-progress code is now one line instead
  of five.
- `snprintf` is the foundation for any string-building code:
  composing paths, building HTTP headers, formatting log lines.
- A future kernel-side `printk` can borrow the same `_fmt_vformat`
  with a different sink (serial port instead of batch+SYS_WRITE).

## What's still missing

- Floating-point. Easy to add (binary-to-decimal conversion via
  Grisu/Ryu, or just a naive `%f` for now), but useless until
  we have an FPU configured for user code.
- `precision` (`%.5s` to truncate, `%.5d` to zero-pad to a
  fixed digit count distinct from width). Mechanical addition.
- A `vfprintf(fd, fmt, ap)` for writing to fds other than 1.
  Right now we hardcode SYS_WRITE on stdout.
- A kernel-side `printk` that reuses the formatter. The kernel
  currently uses its own `serial_puts` / `serial_puthex` family
  for everything; would be nice to unify.

## What changed

```
userspace/libc/printf.h            NEW \u2014 header-only printf family
userspace/printftest/printftest.c  NEW \u2014 exercises every specifier
userspace/hello/hello.c            migrated to printf
userspace/cat/cat.c                migrated to printf
userspace/echo/echo.c              migrated to printf
kernel/core/main.c                 banner -> milestone 19
Makefile                           wires printftest into disk image
```

One header, one test, three migrations. The user libc is now
~270 lines of code (`syscall.h` 175 + `malloc.h` ~120 +
`printf.h` ~270 = 565) and starting to look like a real C
library.
