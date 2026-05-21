# Chapter 127 — Notepad gets a Build button: an in-OS dev loop

**Status:** Stub. Tracking the developer-loop
milestone, part 2. See [Chapter 115](115-c-compiler-strategy.md).

## Why this chapter exists

Native gcc plus make gives the OS a working "edit,
compile, run" loop — but only at the shell. The
desktop apps that already exist (notepad, gui_term,
launcher) sit alongside that loop without
participating in it. This chapter folds the loop into
the GUI: notepad gets a "Build" button on its
toolbar; clicking it shells out to the right
compiler invocation; output streams into a
gui_term-style pane next to the editor. The reader
ends the section with a workflow that looks like a
small IDE, built from primitives all of which were
written by hand earlier in the book.

The chapter is intentionally not ambitious about
*being* an IDE. It is the obvious next step the
moment the toolchain runs in-guest, and stopping
short of it would leave the section feeling
unfinished.

## What this chapter adds

- A "Build" toolbar button in notepad. Visible
  when the current file's extension is `.c` or
  `.cc` or when a `Makefile` exists in the
  current directory.
- The build orchestration is dumb-on-purpose:
  - If `Makefile` exists in `cwd`: run `make`.
  - Else if file is `*.c`: run
    `cc -O2 -o /tmp/notepad_build <file>`.
  - Else: button is disabled.
- A second pane on the right half of the notepad
  window — a stripped-down gui_term embedded as a
  view, not a separate process. Streams the
  build's stdout/stderr in real time.
- A "Run" button next to "Build" that, after a
  successful build, runs the produced binary
  with its stdout/stderr piped back into the
  build pane.
- `scripts/test_notepad_build_button.py`: opens
  notepad on a fixture `.c`, clicks Build, waits
  for "build OK", clicks Run, asserts the
  binary's stdout appears in the pane.

## Prerequisites

- Chapter 51 — notepad itself.
- Chapter 79b — fork/exec via gui_term-style
  spawn (we reuse the same pattern).
- Chapter 124 — native gcc.
- Chapter 126 — make (so the Makefile-detection
  branch has somewhere to call).
- Chapter 108d / 108e — userspace WM + decoration
  (we resize and recompose notepad's window to
  fit the build pane).

## Plan

1. Refactor notepad's main window to support a
   right-hand side panel that can be hidden or
   shown. Default hidden; "Build" makes it
   appear.
2. Embed a tiny terminal renderer in that panel.
   This is *not* a new gui_term process — it's
   the same scrollback ring + glyph blitter,
   imported from gui_term's source as a small
   library.
3. Add the toolbar buttons. The icons are
   text-only ("Build", "Run") to avoid yet
   another asset.
4. Spawn the build process with stdout+stderr on
   a pipe; an event-loop branch drains the pipe
   and appends to the terminal panel's ring.
5. On exit, parse the child's status; mark the
   pane with a colored bar (green = success,
   red = failure) and enable/disable the Run
   button accordingly.

## What you'll learn

- How small the leap from "shell pipeline" to
  "IDE-shaped GUI" actually is, once the
  underlying primitives (fork, pipe, exec,
  waitpid, a terminal renderer, a GUI toolkit)
  are in place.
- Why every notepad-shaped editor in history
  ends up here: once you have a build button,
  every other thing about an "IDE" (project
  navigation, multi-file tabs, debugger
  integration) is incremental on top of it.

## What this unlocks

- A reader who follows the section to this point
  can boot the OS, open notepad on a `.c` file
  in `/data/src/`, edit it, click Build, click
  Run, and see the result — without ever
  leaving the desktop or touching the host.
- The book has a satisfying "we built the
  system, and the system can build itself, and
  the GUI knows about it" closing.

## Applied to

- **Existing apps:** **notepad** — gains the
  Build/Run buttons and the embedded terminal
  pane. About 400 lines of new code, almost all
  in notepad itself; the terminal renderer is
  ~50 lines extracted from gui_term and shared.
- **New apps:** none — this chapter is
  emphatically *not* a new IDE app, it's notepad
  growing one feature.
- **New tests:** `scripts/test_notepad_build_button.py`,
  `scripts/test_notepad_build_makefile.py`
  (asserts the Makefile branch is taken when a
  Makefile exists in `cwd`),
  `scripts/test_notepad_build_error.py` (asserts
  the red bar shows on a build with a deliberate
  syntax error in the source).

## Closing the section

This is the final chapter of Part XVII. The OS
now has:

- A native compiler (`gcc`, also `tcc`).
- A native build driver (`make`).
- A native assembler and linker (`as`, `ld`,
  `ar`).
- A POSIX-shaped libc rich enough to host them.
- A GUI editor with a Build button.

The development loop that took the first 124
chapters to design — "edit on host, cross-build,
ship to OS, run" — has been replaced by the same
loop running entirely inside the OS. The book has
done what it set out to do.
