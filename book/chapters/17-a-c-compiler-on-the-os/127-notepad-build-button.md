# Chapter 127 — Notepad's Build button: closing the loop inside the guest

> *Type code in the GUI. Build it from the GUI. Run it from the
> shell — all inside osdev.*

This is the chapter that earns Part XVII its title.  Up
to now, every program that ran on osdev was either
written on the host and shipped on `disk.img`, or written
into a `/data/` file at the serial console and then
manually compiled with `/bin/cc` (chapter 124).  Chapter
127 ties together everything Part XVII has built so that
the whole loop — *write, build, run* — lives behind a
single key chord in the editor.

## What this chapter ships

| Component | File | Status |
|---|---|---|
| Notepad Build handler | [userspace/notepad/notepad.c](../../../userspace/notepad/notepad.c) (+~60 lines) | Wired |
| Regression test | [scripts/test_notepad_build.py](../../../scripts/test_notepad_build.py) (10/10 PASS) | Green |

The key chord is **Ctrl-B**.  When the user presses it,
notepad:

1. Writes the live editor buffer to `/tmp/np_src.c`.
   Crucially, this does NOT touch `g_path` or `g_dirty`
   — the user's actual file on disk is untouched and the
   modified-marker in the status bar stays accurate.
2. Writes a one-rule Makefile to `/tmp/np_build.mk`:
   ```
   all:
       /bin/cc /tmp/np_src.c -o /tmp/np_out
   ```
3. Spawns `/bin/make -f /tmp/np_build.mk` and `waitpid`s
   for it.
4. Prints `[notepad] build code=N` to its serial stdout
   (a marker so headless regression tests can grep the
   boot log for the outcome).
5. Updates the status bar: `"built /tmp/np_out"` on
   success, `"build failed"` otherwise.

That's it.  60 lines of C — most of it just careful
ordering of `write/fsync/close` and `spawn/waitpid`.

## The end-to-end pipeline triggered by one keystroke

```
notepad     write_buffer_to("/tmp/np_src.c")
   │
   ├──spawn(/bin/make, "-f /tmp/np_build.mk")
   │
   │      make parses /tmp/np_build.mk
   │      make's mk_build("all") runs the recipe
   │      mk_run_recipe spawns "/bin/cc /tmp/np_src.c -o /tmp/np_out"
   │            │
   │            │   /bin/cc reads /tmp/np_src.c (chapter 124)
   │            │   /bin/cc emits AT&T-ish text asm to /tmp/_cc.s
   │            │   /bin/cc spawns /bin/as /tmp/_cc.s -o /tmp/_cc.o
   │            │         /bin/as parses, assembles, writes ELF
   │            │   /bin/cc spawns /bin/ld /tmp/_cc.o -o /tmp/np_out
   │            │         /bin/ld merges sections, applies relocs
   │            ▼
   │      make returns exit code
   ▼
notepad waitpid → printf("[notepad] build code=0")
notepad set_status("built /tmp/np_out")
```

Six processes participate in a single Ctrl-B.  None of
them is a host helper, none of them is a stub.  This is
the pipeline.

## Design choices and why

### Why a Makefile and not direct `spawn("/bin/cc", ...)`?

Notepad could call `/bin/cc` itself.  It would be three
lines fewer.  Two reasons we route through `/bin/make`
instead:

1. **Apps must use the OS features we built.**  Chapter
   126 shipped `/bin/make`.  If chapter 127 bypassed it,
   the make port would be a dead artefact in the disk
   image rather than a real piece of the user-visible
   toolchain.  This is the user directive
   [apps-must-use-features.md](../../../memories/apps-must-use-features.md)
   in action: every kernel/userspace feature has to be
   exercised by at least one real application.
2. **Future-proofing for multi-file projects.**  The
   moment a user wants to build *two* source files, or
   link against a precompiled object on `/data`, the
   single-`/bin/cc`-spawn approach breaks but the
   make-driven approach scales.  Today the Makefile is
   one rule; tomorrow it can be ten.

### Why a fixed scratch path?

`/tmp/np_src.c`, `/tmp/np_build.mk`, `/tmp/np_out`.
Three fixed paths, regardless of what file the user is
editing.  This means:

- The Build button works even when notepad was
  bare-launched (no `g_path` set) — Save vs Build are
  decoupled.
- The test harness knows exactly where to look for the
  artefact.
- The user can't accidentally overwrite the file they're
  editing by pressing Build (the buffer goes to the
  scratch path, not to `g_path`).

The cost is that if the user wants to build the file at
its real path, they have to copy it manually.  That's a
fair trade for v1.

### Why a separate `write_buffer_to()`?

Compare:

```c
static int save_file(const char *path)
{
    /* ... write the buffer ... */
    g_dirty = 0;          // <-- this is the difference
    return 0;
}

static int write_buffer_to(const char *path)
{
    /* ... write the buffer ... */
    return 0;
}
```

`save_file` is what Ctrl-S uses — it clears the
modified-marker because the user explicitly saved.
`write_buffer_to` is what Build uses — the user's *real*
file on disk wasn't touched, so the modified-marker must
stay correct.  Sharing the function would be DRYer but
wrong.

## Test coverage

[scripts/test_notepad_build.py](../../../scripts/test_notepad_build.py)
asserts ten things in one boot:

| # | Assertion |
|---|---|
| 1 | `/data/hello127.c` seeds onto the OSFS-2 partition |
| 2 | Shell prompt is reached after boot |
| 3 | `notepad /data/hello127.c` opens a window |
| 4 | Notepad prints `[notepad] build code=` on serial after Ctrl-B |
| 5 | The reported build code is 0 |
| 6 | `/bin/make` echoed the cc recipe (proves the build went through make, not directly through cc) |
| 7 | Notepad exits cleanly on Ctrl-Q |
| 8 | `/tmp/np_out` (the compiled binary) prints its marker `M127-BUILD-OK` |
| 9 | The shell prompt returns after the binary exits |
| 10 | `/tmp/np_src.c` on disk contains the buffer the user was editing (confirms the write actually landed) |

All ten pass on the first deterministic run.

The test deliberately splits "wait for marker" and "wait
for prompt" into a single combined drain, because
sequential `wait_for` calls would drop bytes that landed
in the gap between them — a chapter-104 lesson about
how `wait_for` consumes its socket.

## Limitations (and the chapters that will lift them)

| Missing | When it lands |
|---|---|
| Capturing make/cc stdout into a popup | Needs `spawn_pipe` integration in libgui — future chapter |
| Build button on the actual file (not /tmp scratch) | Needs notepad to do "Save first then build the saved path" with a confirm |
| Run-after-build (single key chord = compile + execute) | Trivial once `/tmp/np_out`'s exit code can be displayed |
| Build errors highlighted in the editor | Needs `/bin/cc` to emit machine-parseable diagnostics |
| `.PHONY`, automatic variables, multi-file projects | Future make extensions (chapter 128+) |

Every entry above is honest — it's a feature deferred,
not abandoned.  The smallest meaningful unit ships in
this chapter.

## Applied to apps

Per the standing user directive — *the OS features we
build need to be incorporated into the existing
applications* — this chapter does exactly that for the
entirety of Part XVII:

- **`/bin/cc`** (chapters 121, 123, 124) — invoked by the
  notepad Build button via `/bin/make`.  First time a GUI
  app drives the compiler.
- **`/bin/as` and `/bin/ld`** (chapters 118-119) —
  invoked transitively through `/bin/cc`.  Exercised by
  every Ctrl-B press.
- **`/bin/make`** (chapter 126) — invoked by notepad as
  the orchestrator.  Without this chapter, make would be
  a tool with no user-visible application that runs it.
- **`spawn` + `waitpid`** (chapters 73-78) — used by
  notepad to drive the build.  Old code paths, new use
  case.
- **`fsync`** (chapter 82) — used by both `write_buffer_to`
  and the Makefile-write path.  Without it the kernel
  could buffer the source file in cache, the spawned
  `/bin/cc` would open the file via the same cache and
  see the new bytes, but the on-disk state would lag
  behind what Build "committed."  fsync makes Build
  durable.
- **Notepad** (chapter 32, then chapters 47/84/108e) —
  grows a new keystroke.  Closes the loop:
  *editor → compiler → linker → executable*, all in the
  guest.

## Section XVII close

This is the last chapter of Part XVII.  What this part
*actually* delivered:

- A POSIX-ish libc rich enough to host a small compiler
  pipeline (chapters 116, 117).
- An in-guest assembler, linker, and `ar` (chapters
  118, 119).
- A crt0 + libgcc-style runtime stubs (chapter 120).
- A from-scratch ~1000-line `/bin/cc` accepting a tiny
  subset of C \u2014 literals, locals, `int` arithmetic, a
  fixed-shape `printf`, `return`/`exit` (chapters 121,
  123).
- A regression-gated cross-toolchain contract that any
  future backend has to satisfy (chapter 122).
- The first on-disk native compile (chapter 124).
- An honest write-up of the self-hosting gap that
  Part XVII does *not* close (chapter 125).
- `/bin/make` (chapter 126) and notepad's Build button
  (this chapter).

What Part XVII *did not* deliver, and what could
plausibly land in Part XVIII:

- **A real GCC port.** The cross-toolchain contract in
  chapter 122 is the spec a `aarch64-none-osdev-gcc`
  would build against; chapter 125 catalogues the
  language work `/bin/cc` would need before a GCC
  bootstrap would even be meaningful. Either path
  (grow `/bin/cc` to self-host, or stand up GCC) is a
  multi-chapter undertaking on its own.
- **A real `printf` formatter in `/bin/cc`** (today it
  only takes a single string literal).
- **A real type system** (today everything is `int`).
- **Function calls** (today there's only `main`).
- **Build-error capture in notepad** \u2014 needs
  `spawn_pipe` in libgui + a parseable diagnostic
  format from `/bin/cc`.

For now: type a program in notepad, press Ctrl-B, watch
`[notepad] build code=0` scroll past on serial, switch
to the terminal, run `/tmp/np_out`, see your marker
appear.  That is osdev as a real machine.
