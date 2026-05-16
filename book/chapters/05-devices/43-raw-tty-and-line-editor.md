# Chapter 43 — Raw TTY mode and the shell line editor

Up to now `read(0, buf, n)` on the console has been **cooked**: the
kernel echoes each typed byte, handles backspace locally with
`\b \b`, and only returns when the user presses Enter.  This works
fine for `cat` reading stdin, but it makes a real shell impossible —
the kernel reader has no way to recognise the `ESC [ A` sequence sent
by the up-arrow key, or to run application logic in response to it.

This chapter adds:

1. A **per-thread raw-mode flag** (`tty_raw`) toggled via a new
   `SYS_TTY_RAW` syscall.  In raw mode the kernel returns one byte at
   a time with no echo and no buffering.
2. A **userspace line editor in `/bin/sh`** that runs in raw mode and
   handles every keystroke itself.  It supports backspace at end of
   line, Ctrl-C to cancel the current edit, Ctrl-D on empty line as
   EOF, and arrow-key history (Up/Down).
3. A small history ring (16 entries) that dedupes identical-to-newest
   commits.

## Why per-thread, not per-fd

The cleanest API would be `tcsetattr` per file descriptor, like POSIX.
We pick **per-thread** instead because:

- Our console (`fd_kind == FD_CONSOLE`) is conceptually a single
  global resource shared by every thread — there is only one PL011 RX
  FIFO.
- The shell wants raw mode while editing; spawned children expect
  cooked.  Making the flag per-thread means `vfs_read`'s console
  branch can just check `t->tty_raw` and the kernel doesn't have to
  track which thread "owns" the tty.
- `thread_create` forces `tty_raw = 0` on every new user thread, so
  children automatically run in cooked mode regardless of what their
  parent was doing.  No save/restore dance required around `spawn`.

This doesn't generalise well — if we ever support multiple terminals
or pseudo-ttys we'll need real per-fd termios state — but for one
console and one foreground reader it's exactly right.

## The kernel side

`struct thread` gains:

```c
int tty_raw;     /* 0 = cooked, 1 = raw single-byte */
```

initialised to 0 at all three thread-creation sites (boot,
`thread_create`, `user_thread_create`).

`SYS_TTY_RAW = 26` toggles it for the calling thread and returns the
previous value:

```c
static long sys_tty_raw(long enable)
{
    struct thread *t = thread_current();
    int prev = t->tty_raw;
    t->tty_raw = enable ? 1 : 0;
    return (long)prev;
}
```

`vfs_read`'s console branch grows a raw fast-path:

```c
if (t->tty_raw) {
    char c;
    while (!serial_try_getc(&c)) yield();
    dst[0] = c;
    return 1;
}
/* otherwise fall through to existing cooked-mode loop */
```

That's the entire kernel change.  No echo, no backspace, no newline
handling — those are now the shell's job.

## The userspace side

`tty_raw(int)` is a one-line libc shim around `SYS_TTY_RAW`.  The
shell calls `tty_raw(1)` once at startup and never turns it off until
it exits.

The line editor is the whole show.  It reads one byte at a time and
dispatches based on what arrived:

| byte         | action                                           |
|--------------|--------------------------------------------------|
| `\r` or `\n` | echo `\r\n`, NUL-terminate, return               |
| `0x7f`/`0x08`| backspace at end (writes `\b \b` to erase echo)  |
| `0x03`       | Ctrl-C: print `^C\r\n`, return empty             |
| `0x04`       | Ctrl-D on empty line: return -1 (EOF)            |
| `0x1b`       | start ESC sequence parser (`[`, then `A/B/C/D`)  |
| 0x20..0x7e   | append + echo                                    |

The ESC sequence parser is a tiny state machine: read the next byte,
if `[`, read the next byte and switch on it.  `A` = up, `B` = down,
`C`/`D` are accepted but ignored (no in-line cursor yet).

### Erasing and redrawing the line

When the user presses Up/Down, the visible line has to change to show
the history entry.  We use ANSI:

```c
write(1, "\x1b[2K\r", 5);   /* erase entire line, move to col 0 */
prompt();                   /* redraw the "$ " (or whatever) */
write(1, buf, len);          /* and the new content */
```

QEMU's `-nographic -serial mon:stdio` puts the host terminal into raw
mode automatically (the terminal then forwards arrow-key escape codes
to the guest), and macOS Terminal/iTerm2 both handle ANSI escape
codes natively.  So `ESC[2K\r` is interpreted by the host terminal —
not by us — and produces the desired in-place erase.

### History ring

```c
#define HISTORY_SIZE 16
static char g_hist[HISTORY_SIZE][LINE_MAX];
static int  g_hist_n;
```

`hist_push(line)` does two things:

- Skip empty lines.
- Skip lines identical to the most recent entry (matches bash's
  `HISTCONTROL=ignoredups`).

If the ring is full, `hist_push` shifts the array left by one to drop
the oldest entry.  At 16 × 128 bytes that's 2 KiB of `memmove`, which
is fine for a tiny shell.

### Navigation state

Inside `read_line_raw`:

```c
int  hist_i  = g_hist_n;     /* index; ==g_hist_n means "scratch" */
char scratch[LINE_MAX];      /* saves the in-progress line on first up */
int  scratch_len = 0;
int  scratch_saved = 0;
```

- **Up arrow** at `hist_i > 0`: save scratch on first press, decrement
  `hist_i`, replace the visible line with `g_hist[hist_i]`.
- **Down arrow** at `hist_i < g_hist_n`: increment `hist_i`.  If now
  past the newest entry, restore scratch.  Otherwise replace with
  `g_hist[hist_i]`.
- **Any printable key after navigating**: if `hist_i != g_hist_n`,
  reset both — the user has started a new edit, so the old scratch is
  no longer relevant and any subsequent up should re-save.

The cursor is always at end-of-line; we don't yet support left/right
arrow cursor movement or insert-at-position, which keeps the
implementation under 200 LOC.  Backspace edits the most recent
character.

## Why the kernel can't just inherit raw mode across spawn

If `tty_raw` were inherited, then a child like `cat` (with no args,
reading stdin) would suddenly get one byte at a time with no echo —
breaking every `<cmd> | cat` pipeline that interleaves keyboard input.
Forcing children to cooked mode is the right default.  The shell's
own `read_line_raw` runs only in the shell's own thread, which keeps
its raw flag through every spawn/wait cycle.

## What's deferred

- **Left/right arrow cursor + insert-at-position**: would require
  rewriting `redraw_line` to also position the cursor with `ESC[<n>G`
  or similar, plus a `pos` separate from `len` in the editor.  Not
  hard, just busywork.
- **Reverse-i-search (Ctrl-R)**: classic bash feature; needs a
  separate prompt mode and incremental matching across the ring.
- **Multi-line editing**: needs to know terminal width, which we
  don't query.
- **Real signals**: Ctrl-C just cancels the current edit; it does not
  deliver `SIGINT` to a running child.  That's a future signal-system
  milestone — see chapter 30 for the previous mention.
- **Persistent history** across reboots: needs disk-backed writable
  FS (chapter 41 only gives us tmpfs, which evaporates on reboot).

## Files changed

- `kernel/core/thread.h`: `tty_raw` field added to `struct thread`.
- `kernel/core/thread.c`: initialise `tty_raw = 0` at all three
  creation sites (boot, kernel thread, user thread).
- `kernel/core/syscall.h`: `SYS_TTY_RAW = 26`.
- `kernel/core/syscall.c`: `sys_tty_raw` impl + dispatch case.
- `kernel/core/vfs.c`: `vfs_read` console branch picks raw vs cooked
  based on `thread_current()->tty_raw`.
- `userspace/libc/syscall.h`: `SYS_TTY_RAW` enum entry; `tty_raw()`
  wrapper.
- `userspace/sh/sh.c`: `read_line_raw` + history ring + `hist_push` +
  `redraw_line`; `main` switches to raw mode on entry; help text
  updated.
- `kernel/core/main.c`: banner -> milestone 34.

## What this unlocks

- The shell now feels interactive in the way you'd expect from any
  Unix shell.  Arrow-key recall is the single biggest UX improvement
  to the shell since pipelines.
- Future TUIs (text editors, top-style monitors, password prompts)
  can all use `tty_raw(1)` without coordinating with the kernel side.
- The same per-thread flag is the natural place to add `tty_echo` and
  other termios bits later — they'd just become extra fields on
  `struct thread`.
