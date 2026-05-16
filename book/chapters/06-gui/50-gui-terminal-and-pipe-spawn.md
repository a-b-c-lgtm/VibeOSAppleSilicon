# Chapter 50 — gui_term: a terminal in a window, and the synchronous-pipe spawn pattern

> *Milestone 42.  We have a window manager (chapter 48), a focused
> keyboard (chapter 47), and a working pointer (chapter 49).  The
> next obvious thing is to do something useful inside a window.
> This chapter builds `gui_term` — a small text terminal that runs
> ordinary userspace programs (`uptime`, `ls`, `cat`, `echo`, `wc`,
> `grep`, …) and renders their stdout into the window's
> scrollback.  Along the way we hit the central question: how does
> a GUI program capture a child process's output without growing
> a new IPC subsystem?  The answer turns out to be a four-line
> recipe built entirely from primitives we already have.*

## What the program looks like, top-down

`gui_term` is a single-file userspace binary that lives at
`userspace/gui_term/gui_term.c`.  Reduced to its essential shape it
is:

```c
int main(void) {
    int win_id = gui_create_window(WIN_W, WIN_H, "gui_term");
    history_push_str("gui_term ready ...");
    render();

    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) { yield(); continue; }
        if (ev.type == GUI_EVENT_CLOSE) { gui_destroy_window(win_id); return 0; }
        if (ev.type == GUI_EVENT_KEY)   handle_key((char)ev.arg0);
    }
}
```

Three things stand out:

1. **No event-driven plumbing.**  We poll, yield when there's
   nothing, and act when there is.  Same idle pattern as `paint`
   from the previous chapter.
2. **Re-render on every keystroke.**  The window is a passive
   bitmap; rather than maintain a dirty-rect system, we clear it
   to the background colour and redraw the visible scrollback
   plus the prompt edit line each time anything changes.  At
   8x16 glyph cost and ≤25 visible rows it is fast enough that
   you can't see it.
3. **Built-in commands first, otherwise spawn.**  `exit`, `quit`,
   `clear`, and `help` are handled in-process; everything else is
   resolved against `/bin/` and run.

The interesting machinery is "everything else."

## The capture problem

VibeOS solved this with a global "stdout capture" buffer in the
kernel: when a GUI app wanted to run a child, it called a special
syscall that swapped the child's stdout to point at a kernel-side
ring, then read from the ring once the child exited.  It works,
but it adds a global piece of mutable state per process group, a
new path through the syscall layer, and a special-case for stdout
that nobody else uses.

We already have a better tool: real pipes.  Chapter 30 added
`pipe()` and `spawn_pipe()` precisely so that one process can hand
a child a different file descriptor for stdout (or stdin).  Pipes
are how `sh` builds pipelines like `ls | wc -l`.  There is no
reason a GUI program can't be one end of the same plumbing.

## The recipe

The whole "run a command and slurp its output" sequence is five
lines:

```c
int fds[2];
pipe(fds);
int tid = spawn_pipe(path, args, /*stdin*/ -1, /*stdout*/ fds[1]);
close(fds[1]);                          /* (1) crucial */
char chunk[256]; long n;
while ((n = read(fds[0], chunk, sizeof(chunk))) > 0)
    history_push_bytes(chunk, n);       /* (2) drain */
close(fds[0]);
int code = 0; wait(&code);              /* (3) reap */
```

Three rules make this work, and getting any of them wrong leaves
you with a mysterious hang or a leaked descriptor.

### Rule 1 — close your copy of the write end *before* draining

`pipe()` gives both ends to the calling process.  When we call
`spawn_pipe()` the kernel duplicates `fds[1]` into the child's fd 1
(stdout).  If we don't then close *our* `fds[1]`, the kernel's
pipe-write reference count stays positive forever (the child holds
one ref, we hold the second).  That means after the child exits
the pipe is still considered "writable" from the kernel's point of
view, so `read()` blocks waiting for more data that will never come.
The whole GUI freezes, looking exactly like a deadlock.

The right ordering is:

1. `pipe(fds)` → kernel allocates the pipe; refs are `r=1, w=1` for us.
2. `spawn_pipe(..., fds[1])` → child inherits a copy of the write
   end; refs are `r=1, w=2`.
3. `close(fds[1])` immediately afterwards → refs are `r=1, w=1`,
   only the child holds the write side.
4. We `read(fds[0])` in a loop.  When the child exits, the kernel
   drops *its* write ref (`w=0`).  The next `read` returns 0
   (EOF) and the loop exits.

This is exactly what `pipe.c::pipe_read` already does:

```c
if (w_refs == 0) return 0;             /* writers gone -> EOF */
if (buffer_empty)  block_until_data;   /* writers exist -> wait */
```

So the close ordering is not a libc convention or a courtesy; it is
the only signal the kernel has that tells it "no more data is ever
coming."  Forget the close, and your reader sleeps forever.

### Rule 2 — use small reads

We pull bytes in 256-byte chunks even though the pipe buffer is
4096 bytes.  Why not match the buffer size?

Two reasons:

- **Progressive rendering.**  If a child writes a single 50-byte
  line and then exits, we want to push that line into the
  scrollback as soon as it arrives, not wait for 4 KiB of slack.
  A 256-byte read returns whatever bytes are present, up to the
  cap.
- **Reentrant safety.**  `history_push_bytes` walks the chunk and
  splits on `'\n'`.  Smaller chunks mean smaller stack-allocated
  scratch and a tighter render granularity if we later decide to
  redraw mid-drain.

There is no correctness concern with picking 256: the pipe is a
byte stream, splits don't matter, and partial reads are normal.

### Rule 3 — `wait()` after EOF, not before

Calling `wait()` before draining the pipe can deadlock the *child*.
If the child writes more than 4 KiB and then exits, those last
bytes are sitting in the kernel pipe buffer waiting for the reader.
If the parent is asleep in `wait()` instead of reading, the child
blocks in `write()` and never reaches `exit()`.  By the time we
`wait()`, we have already drained all the bytes (the read loop
returned 0), so the reap is immediate.

This is exactly the same dance a Unix shell does for command
substitution; we just happen to be a GUI program instead of a
terminal.

## What we deliberately did *not* build

- **A scrollback navigator.**  PgUp / PgDn would be one render
  function and a `top_line` cursor; we left the hooks in place
  (the history ring stores the last 256 lines) but the bindings
  aren't wired up yet.
- **A non-blocking command run.**  While a command is executing,
  `gui_term` is sat in `read()` and will not service GUI events.
  Click on the window, drag it, hit ESC — nothing happens until
  the child exits.  For commands like `uptime` and `ls` this is
  invisible.  The proper fix is to make `read()` return `EAGAIN`
  when the pipe is empty, drive the read in the same poll loop
  as GUI events, and redraw incrementally — material for a future
  chapter.
- **Child stdin.**  When a window has WM focus the WM steals every
  keystroke from the virtio-keyboard, so a child that calls
  `read(0, ...)` will block forever.  We simply don't run
  interactive children.  The real solution is to add a
  `gui_term`-side keyboard pipe and pass it as the child's stdin;
  also future work.

## Rendering the buffer

The history is a fixed-size ring of 256 rows of 88 columns:

```c
static char history[HISTORY_ROWS][COLS + 1];
static int  history_count;            /* monotonic */
```

`history_count` is monotonic; we map it to a slot via
`history_count % HISTORY_ROWS`.  This means "scrolling" is just a
matter of choosing where to start drawing — we never copy bytes
around when the ring wraps.  In `render()`:

```c
int first = (history_count > VISIBLE_ROWS)
          ? history_count - VISIBLE_ROWS : 0;
for (int r = first; r < history_count; r++) {
    int slot = r % HISTORY_ROWS;
    gui_draw_text(win_id, GUTTER_X,
                  GUTTER_Y + (r - first) * GLYPH_H,
                  history[slot], FG, BG, 1);
}
```

The prompt row is anchored at the bottom (one row of margin), the
input string is drawn after the green `$ ` glyphs, and a solid
8x16 block is filled at the cursor position.  Then a single
`gui_flush()` pushes the dirty window region to the WM compositor.

Eighty-eight columns × twenty-five rows is the same shape as the
classic IBM 80x25 text mode, plus a margin column on each side and
a little extra horizontal room.  For a 720x440 window the spacing
feels right.

## Pushing raw bytes into a line buffer

Children write a byte stream, not lines.  `history_push_bytes`
splits on `'\n'`, strips trailing `'\r'` (so CRLF is collapsed to
LF), and hard-wraps any segment longer than `COLS` onto multiple
lines.  Empty lines (two `\n`s in a row) become an explicit blank
line in the scrollback.  The implementation is a single forward
pass over the buffer with no malloc:

```c
for (long i = 0; i <= n; i++) {
    char c = (i < n) ? buf[i] : '\n';
    if (c == '\n') {
        long len = i - start;
        while (len > 0 && buf[start+len-1] == '\r') len--;
        long off = 0;
        while (off < len) {
            long take = (len - off > COLS) ? COLS : len - off;
            history_push_line(&buf[start+off], take);
            off += take;
        }
        if (len == 0) history_push_line("", 0);
        start = i + 1;
    }
}
```

The `i <= n` upper bound and the synthetic `'\n'` sentinel at the
end mean we don't need a separate "flush trailing partial line"
branch.  Children that write `"hello"` with no newline still get
one line in the scrollback when the chunk ends.

## Built-ins

Four built-ins live in the program rather than being separate
binaries:

| name    | behaviour                                |
|---------|------------------------------------------|
| `exit`  | `gui_destroy_window(); exit(0);`         |
| `quit`  | alias of `exit`                          |
| `clear` | `history_count = 0; render();`           |
| `help`  | pushes two reminder lines into history   |

`clear` is interesting because it doesn't actually zero the
storage — setting `history_count = 0` makes the renderer's `first`
calculation start over, and old bytes simply get overwritten the
next time the ring wraps.  No `memset` involved.

## Smoke test

`scripts/test_gui_term.py` boots the system headless, types
`gui_term\n` into the virtio-keyboard via QMP, then types
`uptime\n` and `echo hi-there\n`, takes a `screendump` after each,
and counts non-background pixels inside the window's content area.
Every command must add new pixels (otherwise its output never
made it through the pipe + render path).

The shifted underscore in `gui_term` is the only fiddly bit:
QEMU's `input-send-event` works at the keycode level, so to type
`_` the test must press shift, press minus, release minus, release
shift — four events in a single QMP message so the modifier
ordering is unambiguous.

A successful run prints:

```
PASS: shell ready
PASS: gui_term window opened
PASS: uptime output rendered into window
PASS: echo output rendered

MILESTONE 42: ALL TESTS PASSED
```

Visually the window looks like:

```
+--- gui_term -----------------------------[X]+
| gui_term ready.  type a command and press   |
| built-ins: exit, quit, clear, help.         |
| $ uptime                                    |
| uptime: 0h 00m 02.600s  (2600 ms)           |
|                                             |
| $ echo hi-there                             |
| hi-there                                    |
|                                             |
| $ |                                         |
+---------------------------------------------+
```

…with a green `$` prompt and a blinking-style block cursor at the
bottom.

## What this unlocks

`gui_term` is the first userland program that does something
recognisably "computer-like" inside the WM.  More importantly it
is the proof that **all the kernel pieces we've built since
chapter 30 — pipes, `spawn_pipe`, `wait`, the VFS, the heap, the
scheduler, the WM, the GUI syscalls, the bitmap font — compose
cleanly into a real desktop application without any new
infrastructure.**  The next two chapters use the same pattern to
build a notepad and a desktop shell.
