# Chapter 34 — Shell quoting

The shell from chapter 33 expanded `$VAR` inside any input. This
made `export GREETING="hello world"` set GREETING to `"hello`
(everything past the first space was lost), and there was no way
to embed a literal `$` in a value. POSIX shells solve both with
quoting; this chapter ships single quotes, double quotes, and a
backslash escape.

## What the chapter ships

```
/$ export GREETING="hello world"
/$ echo greeting is $GREETING
greeting is hello world

/$ export LITERAL='$NOPE'             # single-quotes preserve $
/$ echo literal is $LITERAL
literal is $NOPE

/$ echo "double has $HOME inside"     # double-quotes expand
double has / inside

/$ echo "back\\slash and \\$dollar"   # backslash escape
backslash and $dollar

/$ echo 'single skips $HOME'          # single doesn't expand
single skips $HOME
```

Three quoting modes:

- `'literal'` — no expansion, no metacharacter handling. The
  surrounding quotes are dropped from the output. Single quotes
  cannot themselves be embedded inside single quotes (POSIX).
- `"expanded"` — `$VAR` and `${VAR}` are still expanded; the
  surrounding quotes are dropped.
- `\\x` — copy `x` literally. Useful for embedding `"` in a
  double-quoted run, or `$` anywhere.

## Implementation

The whole change is inside `expand_vars` in `userspace/sh/sh.c`.
The state machine grows one new flag (`in_dq` for "currently
inside a double-quoted run") and three new branches at the top
of the loop:

```c
if (c == '\'' && !in_dq) {
    src++;                                  /* drop opening quote */
    while (*src && *src != '\'' && pos < cap - 1)
        dst[pos++] = *src++;                /* literal copy */
    if (*src == '\'') src++;                /* drop closing quote */
    continue;
}
if (c == '"') { in_dq = !in_dq; src++; continue; }
if (c == '\\' && src[1]) { dst[pos++] = src[1]; src += 2; continue; }
```

Then the existing `$` branches run unchanged. Notice we don't
need a separate "inside double quote" path for variable
expansion — the existing logic Just Works because we don't
suppress expansion in double quotes. Nice when an extension
falls out for free.

### Why no nested single quotes

POSIX explicitly says single quotes don't nest: `'a'b'c'` is
three runs (`a`, `b`, `c`) where the second is unquoted. Bash
matches POSIX. We do too, but only because that's the easiest
implementation — the loop drops the outer single quote, copies
until the next `'`, and the rest of the string starts a fresh
parse round.

### Why the backslash is so simple

Real bash distinguishes:
- inside single quotes: `\\` is a literal backslash
- inside double quotes: `\\$`, `\\\\`, `\\"`, `` \\` `` are escapes,
  others are literal `\\` + char
- unquoted: `\\x` always escapes `x`

We compress all of that to "outside single quotes, `\\x` always
escapes `x`". That's enough to embed any character that would
otherwise be metaplexed (`$`, `"`, `\\`, space). A future, more
faithful implementation can grow the cases.

### What stays unchanged

- The two-buffer model (raw vs expanded) from chapter 33.
- Word splitting: still none. `cmd "$args"` and `cmd $args`
  produce the same single args-string today. When we add
  IFS-driven splitting on the *expanded* result, the
  distinction starts to matter.
- `$VAR` expansion logic itself.

## What's still missing

- **Nested expansion.** `${${KEY}}`, `${VAR-default}`,
  `${VAR:-default}`, `${VAR%suffix}` — POSIX parameter
  expansion is enormous; we have only the trivial `$VAR` and
  `${VAR}` forms.
- **Backtick / `$()`.** Both require running a child shell on
  the captured text and stitching its stdout back into the
  current command.
- **Heredocs (`<<EOF`).** Need multiline input; the read loop
  is line-at-a-time today.
- **Word splitting.** When we get pipes, this becomes
  important — `cat $files` should expand into multiple argv
  elements if `$files` contained spaces. Today it's one.
- **History (`!!`, `!$`).** Different feature, also deferred.

## What changed

```
userspace/sh/sh.c        +30 lines inside expand_vars:
                          single-quote run, double-quote toggle,
                          backslash escape; in_dq state flag
```

A 30-line change closes a usability gap that anyone scripting
the shell would hit immediately. POSIX-shaped enough that
people's muscle memory works.
