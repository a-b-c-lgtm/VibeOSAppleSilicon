# Chapter 124 — The first native compile from disk

> "Disk-resident source, in-guest compiler, in-guest binary."

By chapter 123 `/bin/cc` could compile a string we typed at
the shell prompt. That is not what a workstation feels like.
What a workstation feels like is: a `.c` file exists *on the
disk*, you type its name, and the compiler reads it. This
chapter is the smallest possible version of that loop.

## The loop

1. Host: seed `/data/hello.c` into `build/data.img` with
   `scripts/mkosfs2.py hello.c=/path/on/host`. (This is the
   exact mechanism we documented in chapter 122 — it is the
   only way to put a file on disk before the guest boots.)
2. Boot the OS.
3. `cat /data/hello.c` — proves the file content survived
   the disk image round trip.
4. `/bin/cc /data/hello.c -o /tmp/hello` — the in-guest
   compiler reads from `/data/` and writes to `/tmp/`.
5. `/tmp/hello` — runs the binary, asserts its marker and
   exit code.
6. Repeat step 4 and 5 — proves the compiler did not mutate
   the disk source on read.

## The program

[scripts/test_first_native_compile.py](../../../scripts/test_first_native_compile.py)
embeds the source as a Python literal, writes it to a host
temp file, then mkosfs2's it onto `build/data.img`. The
program itself is twelve lines of chapter-123-grade C:

```c
int main(void) {
    int a = 100;
    int b = 23;
    int c = a + b;
    printf("M124-COMPILED-OK\n");
    return c;
}
```

Four locals, one `+`, one `printf`, one `return EXPR`. Every
construct is something `/bin/cc` first learned in chapter
123. The marker `M124-COMPILED-OK` is intentionally unique
to this chapter so a grep over a boot log can attribute the
success line to this specific test.

Expected exit code: `100 + 23 = 123 = 0x7b`.

## Why this is a milestone

Up to and including chapter 123, every `.c` source the OS
ever compiled was either typed by the user or staged
through a `here-doc` from a Python test. The compiler was
real, but the "source code" lived in a Python literal that
was sent down the serial line and into `/tmp` by the shell.

Chapter 124 cuts that dependency. The source lives on a
real disk. The shell opens it via the VFS. The compiler
opens it via the VFS. The binary lands in a real RAM-backed
filesystem. The only host involvement is the original
`mkosfs2` seed *before* boot — there is no host write into
the live guest.

That is the minimum viable "I can write a program and run
it on my computer" loop. Every later chapter — `/bin/make`,
notepad's Build button, a hand-typed program saved to
`/data/src/` — is a UX improvement on top of this loop.

## What the test verifies

| Assertion | What it proves |
|---|---|
| `/data/hello.c` is listed by `ls /data` | mkosfs2 seeded the file into OSFS-2 successfully |
| `int a = 100;` is in the file | Source byte content survived the disk image |
| `M124-COMPILED-OK` is in the file | The string literal survived unescaped |
| `return c;` is in the file | The trailing lines survived (no truncation) |
| `/bin/cc /data/hello.c -o /tmp/hello` reports `cc: wrote` | The pipeline (cc → as → ld) ran end-to-end |
| `/tmp/hello` printed `M124-COMPILED-OK` | The compiled binary's data segment is correct |
| `/tmp/hello` exited with `0x7b` | The compiled binary's arithmetic is correct |
| Recompile succeeded with the same output | `/bin/cc` does not mutate its source files |
| Re-run exits with `0x7b` again | The disk-resident source is the canonical source |

Nine assertions, all in one boot, all passing on the first
run after this chapter lands.

## Gotchas that bit us, kept here so they don't bite again

### 1. The startup-race prompt

Booting an OS that has a desktop and a window manager
running prints a lot of stuff. Some of those lines contain
`/$ ` substrings — most notably the window-manager noise
`[wmclient] DAMAGE failed status=-5` which appears just
*after* the shell prompt, but a naive `wait_for(b"/$ ")`
will satisfy *before* the shell is ready. The fix (same as
chapter 122):

```python
wait_for(s, PROMPT, timeout=20.0)
time.sleep(1.5)
drain(s, time.time() + 0.5)
```

### 2. mkosfs2 is flat — no subdirectories

We would have liked to put the source at `/data/src/hello.c`
to match a real /src layout. `mkosfs2.py` only supports
top-level `name=path` seeds — it has no `mkdir`. So we
ship the source as `/data/hello.c` and accept the
flatness. A later chapter will give the guest a real
`mkdir /data/src` capability and then `/data/src/hello.c`
becomes the canonical layout. Until then: flat.

### 3. Idempotence is a property of the *read* path

The compiler does not literally write to its input file,
but it does open it for read. On OSFS-2 that goes through
the block cache, which (correctly) caches read pages. If
the chapter-123 cleanup logic in `/bin/cc` ever drifted to
truncating the *input* by mistake — `O_RDWR | O_TRUNC`
instead of `O_RDONLY` — the second compile would find an
empty file. The "idempotent compile" assertion catches
this regression class.

## Applied to existing apps

Per the user directive ("OS features get used by apps"):

- `/bin/cc` itself is the app this chapter exercises. No
  code change to the compiler — chapter 123 already taught
  it everything it needed.
- `/data` mount is now exercised for a *production* file
  (a buildable C source), not just the chapter-122 contract
  smoke (which used `/etc/hosts` for size).
- `/bin/ls` and `/bin/cat` get exercised against an OSFS-2
  file that was placed there by a Python test rather than
  by an interactive shell session — small change, but it
  proves the seed → boot → tool flow is end-to-end.

## What stays missing

| Limitation | Will be fixed in |
|---|---|
| Source must live at `/data/` top level | A later chapter when mkdir reaches /data |
| No `#include` — every source must be standalone | A future preprocessor chapter |
| Compiler can only handle one `.c` per invocation | Chapter 126 (/bin/make) batches them |
| Test harness still pushes the source from host | Chapter 127 (notepad Build button) lets the *user* type the source in the guest |

## Lessons

1. The smallest "write a program and run it" loop is
   four moving parts: a file on disk, a compiler that
   reads it, a linker that writes a binary, a loader that
   runs the binary. We had all four by chapter 121. What
   chapter 124 adds is making the *first* of those four
   parts a real disk file, not a hand-fed string.
2. Recompile-idempotence is cheap to test and catches a
   whole class of real bugs (input clobbered by output,
   block-cache aliasing).
3. mkosfs2 being flat is a real limitation, but the
   workaround (top-level file) is one line of code, not a
   week of FS work. Defer the proper fix; ship the chapter.

Next: chapter 125, the self-hosting bootstrap concept — how
a real compiler bootstraps from a smaller version of itself,
and what we'd need to add to `/bin/cc` to make that real.
