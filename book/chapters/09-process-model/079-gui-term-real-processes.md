# Chapter 79 — gui_term gets real processes, signals, and Ctrl-C

> **Milestone in this chapter:** 79 — pty + fork+exec inside `gui_term`.
> **Code referenced:**
> - [kernel/core/pty.c](../../../kernel/core/pty.c),
>   [kernel/core/pty.h](../../../kernel/core/pty.h)
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_OPENPTY`, `sys_set_fg_pid` auto-routing)
> - [userspace/gui_term/gui_term.c](../../../userspace/gui_term/gui_term.c)
>
> **At the end of this chapter** you will have a `gui_term` whose
> inner `/bin/sh` runs through a real pty, supports background jobs
> (`&`), forwards Ctrl-C as `SIGINT`, and reaps children correctly
> via `waitpid` — the same shell binary that the serial console
> runs, in the same window manager, talking to the same kernel
> signal plumbing.

This chapter is the first of a recurring pattern: every time the
kernel grows a real Unix-shaped feature, the existing apps that were
faking it until then get rewritten to use the real thing. Without
that discipline the chapters past 73 are mathematically true but the
user-visible system on screen stays frozen at the chapter-50
desktop.

The biggest such app is `gui_term`. Chapter 49 introduced it as "a
terminal in a window," but the spawn pattern shown there is the
**synchronous-pipe** model: when you type `ls\n`, `gui_term` forks a
*thread* that runs `ls` to completion against an in-memory pipe,
then dumps the captured output into the window. That model was a
stand-in for the real thing because at chapter 49 the kernel did
not yet have

- `fork()` of an address space (chapter 72),
- `exec()` of an arbitrary binary (chapter 73),
- `SIGINT` delivery (chapter 75),
- `waitpid` semantics that distinguish exit from signalled
  (chapter 77),
- a foreground-process notion that survives across `exec`
  (chapter 78).

Chapters 72–78 added every one of those, but only the serial shell
(`/bin/sh` as `init`'s child) actually used them. This chapter is
where `gui_term` finally catches up.

## What this chapter ships

- A `pty` kernel object — two pipes plus a `fg_pid`. See
  [kernel/core/pty.c](../../../kernel/core/pty.c) and
  [kernel/core/pty.h](../../../kernel/core/pty.h).
- `SYS_OPENPTY` (=34) returns two new fds — `FD_PTY_MASTER` and
  `FD_PTY_SLAVE` — both bidirectional. Wrapper at
  [userspace/libc/syscall.h](../../../userspace/libc/syscall.h).
- A line discipline on the master side: any `0x03` written to
  master is consumed and translated into
  `thread_signal_pid(pty->fg_pid, SIGINT)` — see the master
  scanner in [kernel/core/pty.c](../../../kernel/core/pty.c).
- `sys_set_fg_pid` now auto-routes: if the caller's fd 0 is a
  `FD_PTY_SLAVE`, the new fg pid is stored on *that pty*; else
  on the global serial `g_fg_pid`. The same `/bin/sh` binary
  works in both contexts unmodified — see
  [kernel/core/syscall.c](../../../kernel/core/syscall.c).
- `gui_term` is rewritten end-to-end. ~150 lines of in-process
  builtin/pipeline code is gone, replaced by a `fork +
  execv("/bin/sh")` through the pty plus a tiny terminal
  emulator (`\n`, `\r`, `\b`, `\t`, hard-wrap at 88 cols, last
  N rows of history rendered with a block cursor). See
  [userspace/gui_term/gui_term.c](../../../userspace/gui_term/gui_term.c).

## The bug we didn't expect: sys_spawn skipped fd inheritance

The chapter started easily — pty object, `openpty`, master
scanner, gui_term rewrite — and the window opened, the
prompt rendered, typing got echoed, and pressing Enter…
produced output **on the serial console, not in the window.**

That was startling. The shell was clearly running through the
pty (it was reading our keystrokes). But everything it ran via
`uptime`, `ls`, `cat` was writing to fd 1 = serial. Why?

Because `sys_spawn` had never inherited the parent's fd table.
It allocated a fresh one with `FD_CONSOLE` on 0/1/2. The
serial shell never noticed because in that world `FD_CONSOLE`
*is* the right thing — it routes to the same UART the shell is
reading from. Inside a pty, `FD_CONSOLE` was a footgun: every
spawned binary bypassed the pty entirely.

The fix is the new helper
[`thread_inherit_fds`](../../../kernel/core/thread.c) — for every
in-use, non-socket fd in the parent, copy the slot into the
child after dropping any pre-existing pipe/pty refs in the
destination, then bump the appropriate ref counters
(`pipe r_refs/w_refs`, `pty refs + s2m + m2s`). Wired into
**three** spawn paths so the override semantics still work:

1. `sys_spawn` → inherit, then no further changes.
2. `sys_spawn_redir` → inherit first, then `vfs_open_into`
   the redirect target on top of (the now-inherited) fd 0.
3. `sys_spawn_pipe` → inherit first, then
   `dup_parent_fd_into_child` for the explicit pipe ends.

Both `vfs_open_into` and `dup_parent_fd_into_child` were
updated to **release-before-overwrite**: if the slot already
holds a pipe or pty (from inheritance), drop the ref before
clobbering. Without that, every redirect or pipe override
leaks a refcount on the inherited slot.

`thread_fork_user` was also refactored to call the same
helper (it was duplicating ~30 lines of fd-copy logic
inline). Both code paths are now the same: a child thread
starts life with the same fd table as its parent. POSIX
`fork+exec` semantics, finally.

The lesson worth highlighting: **a syscall named `spawn` had
silently been doing something `fork+exec` would never do
(reset fds), and nothing noticed for ~30 chapters because in
the only world that existed, the wrong default and the right
default happened to coincide.** Pty was the first place they
diverged.

## Ctrl-C during `sleep` was a second bug

After fd inheritance was fixed, the prompt worked, pipelines
worked, builtins worked — but pressing Ctrl-C during
`sleep 30` did nothing. The signal was *delivered* (the bit
was set on `sig_pending`) but the sleeping thread didn't
wake up to notice.

Two fixes, both in [kernel/core/thread.c](../../../kernel/core/thread.c):

1. `thread_sleep_ms` was a yield loop on a wall-clock
   deadline with no signal check. Now it checks
   `g_current->sig_pending` after each yield and breaks
   early if any bit is set.
2. `thread_signal_pid` only requeued targets in
   `THREAD_BLOCKED`. A sleeping shell waiting on
   `thread_sleep_ms` is in `THREAD_SLEEPING`, which the
   timer tick eventually wakes — but not for ~tens of ms,
   far slower than user-perceptible Ctrl-C. Extended to
   wake `THREAD_SLEEPING` targets too.

Combined effect: Ctrl-C in `sleep 30` returns the prompt in
~2 ms (measured by the test).

For pipe-blocked reads (the more common case),
[kernel/core/pipe.c](../../../kernel/core/pipe.c)'s `pipe_read` already
returned `-EINTR=4` on signal wake from earlier work, so no
additional change was needed there.

## The test-design lesson

The pre-existing `scripts/test_gui_term.py` was meant to
type `gui_term\n` via the QMP-keyboard endpoint and then
drive the resulting window. After the launcher chapter added the
auto-focusing launcher window, *every* keystroke from the
QMP endpoint got eaten by the launcher (mouse-only), so the
test couldn't even spawn gui_term anymore. It had been quietly
broken for many chapters.

The fix in [scripts/test_gui_term.py](../../../scripts/test_gui_term.py):
spawn `gui_term` via the **serial socket** (sh's actual
stdin), wait for `[wm] window created`, then drive the
resulting in-window inner sh via QMP keyboard — gui_term
auto-focuses on creation, so its window receives the
keystrokes correctly. From there the test runs:

- `uptime` (executable + pty inheritance),
- `cd /mnt ; pwd` (sh builtins inside the pty),
- `cat hello.txt | wc -l` (pipeline through pty),
- `sleep 30` then **Ctrl-C** then `echo back` (signal
  delivery to the foreground pid + shell recovery).

It validates each step by counting body pixels in
`screendump`s — every passing step bumps the foreground
pixel count because new text is rendered to the window.

The same launcher-autofocus issue affected
`test_launcher.py`, `test_notepad.py`, and `test_tablet.py`,
which had been quietly broken since the launcher landed for the same reason
— they all typed `<app>\n` via QMP and assumed it reached
sh.  All three are fixed in this chapter using the same
two techniques:

- Route shell-targeted spawn commands through the serial
  socket (`ser.sendall(b"<cmd>\n")`) rather than QMP
  keyboard.  GUI-targeted keystrokes (typing into notepad's
  text area, Ctrl-S/Ctrl-Q) keep using QMP because the
  GUI app does have focus at that point.
- Drop the manual `launcher\n` spawn from `test_launcher.py`
  entirely — since init auto-spawns the launcher, the
  window is already there by the time the prompt appears.
- Bump `WIN_X`/`WIN_Y` in `test_tablet.py` from `(80, 60)`
  to `(112, 92)`.  Paint used to be the first auto-cascaded
  window; now the launcher takes the (80, 60) cascade slot
  first, and paint cascades 32px south-east of that.
  The drag click happened to land in paint's body either
  way, but the close-button click at `(WIN_X+WIN_W-10,
  WIN_Y+10)` missed the title bar entirely until the
  coordinates moved.

Lesson: **adding an auto-spawned window at boot shifts the
WM cascade for every test that hard-codes window positions.**
Future tests should either use mouse-driven coordinates
relative to the WM's known cascade rules, or query the WM
for a window's actual position.

## Files touched

Kernel:

- [kernel/core/pty.c](../../../kernel/core/pty.c) — new
- [kernel/core/pty.h](../../../kernel/core/pty.h) — new
- [kernel/core/syscall.c](../../../kernel/core/syscall.c) —
  `sys_openpty`, `sys_spawn` / `sys_spawn_redir` /
  `sys_spawn_pipe` inherit-fds wiring,
  `dup_parent_fd_into_child` release-before-overwrite,
  `sys_set_fg_pid` auto-route
- [kernel/core/syscall.h](../../../kernel/core/syscall.h) —
  `SYS_OPENPTY=34`
- [kernel/core/thread.c](../../../kernel/core/thread.c) —
  `thread_inherit_fds` (new), `thread_fork_user` refactored
  to use it, `thread_sleep_ms` sig_pending check,
  `thread_signal_pid` wakes SLEEPING
- [kernel/core/thread.h](../../../kernel/core/thread.h) —
  `thread_inherit_fds` declaration
- [kernel/core/vfs.c](../../../kernel/core/vfs.c) —
  `vfs_open_into` release-before-overwrite for pipe/pty;
  `vfs_read` dispatches `FD_PTY_MASTER` / `FD_PTY_SLAVE`
- [kernel/core/vfs.h](../../../kernel/core/vfs.h) —
  `vfs_open_into` declaration
- [kernel/core/pipe.c](../../../kernel/core/pipe.c) — already
  returned `-EINTR` on signal wake; no further change
- [Makefile](../../../Makefile) — added `kernel/core/pty.c` to
  `C_SRCS`

Userspace:

- [userspace/gui_term/gui_term.c](../../../userspace/gui_term/gui_term.c)
  — full rewrite (~205 lines, debug-print-free)
- [userspace/libc/syscall.h](../../../userspace/libc/syscall.h) —
  `openpty` libc wrapper

Tests:

- [scripts/test_gui_term.py](../../../scripts/test_gui_term.py) —
  rewritten for pty flow with builtins, pipeline, Ctrl-C
- [scripts/test_launcher.py](../../../scripts/test_launcher.py) —
  drop manual `launcher\n` spawn (init auto-spawns it now)
- [scripts/test_notepad.py](../../../scripts/test_notepad.py) —
  route notepad spawn + post-quit `cat` through serial
- [scripts/test_tablet.py](../../../scripts/test_tablet.py) —
  route paint spawn through serial; bump `WIN_X`/`WIN_Y`
  from (80, 60) to (112, 92) for paint's post-launcher
  cascade slot

## What you'll learn

- A pty isn't a kernel device — it's a kernel object: two
  pipes plus the privileged byte scanner (line discipline)
  on one end. The simplest possible version is ~200 lines.
- `fork+exec` semantics imply fd inheritance. If your
  "spawn" syscall doesn't inherit fds, you'll get away with
  it until something further upstack (here, a pty) actually
  cares which fd is open at index 1.
- A signal handler is no use if the target thread can't
  wake up to notice it. Every blocking primitive in the
  kernel needs a sig_pending check, and every delivery path
  needs to requeue every blocked-or-sleeping target.
- Tests written against "type at the boot prompt" silently
  break the moment the boot screen grows a focused window.
  The fix is to drive the actual stdin of the shell that
  was supposed to receive the keystrokes — in our case, the
  serial socket.

## What this unlocks

- Every shell builtin works inside a gui_term window for
  free, because the shell *is* `/bin/sh`.
- `cat | wc -l`, redirects, `cd`, `pwd`, env, history — all
  of it works in the window without gui_term containing one
  line of shell logic.
- Ctrl-C interrupts arbitrary running children
  (`sleep`, `cat` of a long file, future `vi`/`less`/etc.)
  and returns the prompt promptly.
- The pty is reusable: any future "thing that needs a TTY"
  (`stty`, `less`, `top`) plugs into the same fd type.

## A note on the recurring pattern

Past this chapter, every Part-IX–and-beyond feature that the
user can plausibly *see* should end with a short "applied
to" section listing which existing app(s) now use it and
which new app (if any) was added to demonstrate it.
Examples that are coming up:

- Chapter 84 (journal): `notepad` saves are now durable —
  power-cycle the VM mid-save and the file survives.
- Chapter 90 (mmap): `cat` of a 100 MiB file uses zero
  copy on the userspace side; the browser's image cache
  stops doing redundant memcpys.
- Chapter 96 (procfs): `ps`, `top`, and a new `taskman`
  GUI app show the same data the kernel already tracks.

Without that discipline, the kernel grows but the user-
visible system stays frozen at chapter 70. With it, every
chapter is a perceptible upgrade for someone using the
machine.
