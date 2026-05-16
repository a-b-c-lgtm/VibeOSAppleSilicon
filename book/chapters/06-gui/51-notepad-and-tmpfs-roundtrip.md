# Chapter 51 — notepad: a real text editor in a window, and the writable-tmpfs round-trip

> *Milestone 43.  We have a window manager (chapter 48), a focused
> keyboard with shift+ctrl modifiers (chapter 47), a pointer
> (chapter 49), and a way to run other programs from inside a
> window (chapter 50).  This chapter builds the second classic
> desktop application — a small but real text editor that opens,
> edits, and saves files to the writable tmpfs.  Along the way we
> exercise three pieces of plumbing we have not used together
> before: the GUI keyboard delivering Ctrl-letter bytes,
> `vfs_open(O_CREAT|O_TRUNC|O_WRONLY)` against `/tmp/...`, and the
> kernel `FD_TMPFS_RW` write path that backs `>` redirection.*

## What is "notepad"?

Notepad opens a 720×440 window with two regions:

- A scrollable edit area (about 25 visible rows × 88 columns of
  the 8×16 bitmap font — chapter 102 swapped this for DejaVu
  Sans, so the visible column count is now approximate) showing
  the contents of a text file.
- A one-row dark-blue status bar at the bottom showing the file
  path, the current line / total lines, the keybindings, and an
  amber `*` modified-since-save indicator on the right.

The keystroke vocabulary is intentionally small:

| key                 | action                              |
|---------------------|-------------------------------------|
| printable ASCII     | insert at cursor                    |
| Backspace / Delete  | delete previous character; if at column 0, join with previous line |
| Enter               | split current line at cursor        |
| Tab                 | insert four spaces                  |
| Ctrl-S              | save buffer to file                 |
| Ctrl-Q / ESC        | quit (no save)                      |
| close button (red X)| same as Ctrl-Q                      |

There is deliberately no arrow-key navigation in this milestone
— our virtio-input driver translates only printable / control
ASCII, and the WM drops keystrokes that don't have an ASCII
representation.  You move the cursor by typing, backspacing, or
hitting Enter.  That's enough to demonstrate the edit + save
round-trip; arrow keys are a one-day refactor we'll do when we
need them for a richer editor.

## In-memory line representation

Two parallel arrays:

```c
static char  g_lines[MAX_LINES][MAX_LINE_LEN];
static int   g_line_len[MAX_LINES];
static int   g_line_count = 1;
static int   g_cur_row = 0, g_cur_col = 0;
static int   g_top_row = 0;
```

This is about 64 KiB of `.bss` (256 lines × 256 bytes), which is
fine because BSS is now NOBITS in the user linker script (see
chapter 49, the milestone-41 detour) — the on-disk binary stays
about 7 KiB regardless.

A "line" is a fixed-capacity char array with a tracked length and
a NUL terminator at `g_line_len[r]`.  Edits are simple memmoves
within one row, plus occasional whole-array shifts for newline
insertion / line join.  No malloc is involved at any point.

### insert_char

```c
shift_right(g_lines[row], g_cur_col, g_line_len[row] + 1);
g_lines[row][g_cur_col] = c;
g_line_len[row]++;
g_cur_col++;
```

Where `shift_right(line, from, len_total)` copies `[from..len_total)`
one position to the right, clearing room for the new character.
Capacity check first — if the line is already full we silently
drop the character (a richer editor would beep or word-wrap).

### newline (Enter)

This is the only operation that touches more than one row at a
time.  Steps:

1. Check `g_line_count < MAX_LINES`; bail if at capacity.
2. Shift every line below the cursor down by one slot (a memmove
   of pointers — but we have arrays, not pointers, so we copy
   row by row).
3. Set the new row to the tail of the current row from `cur_col`
   onward.
4. Truncate the current row at `cur_col`.
5. Increment `g_line_count`, advance the cursor to row+1 col 0.

Cost is O(line_count × line_len) in the worst case — fine for our
260 KiB total buffer, would not be fine for a megabyte-class
editor.

### backspace at column 0 — the line join

The non-trivial case: if `cur_col == 0` and `cur_row > 0`, we're
deleting the implicit `'\n'` between rows `cur_row-1` and
`cur_row`.  The result is that the current row's contents are
appended to the previous row (clipped to MAX_LINE_LEN-1 if too
long), and the row count drops by one.  After the join, the
cursor sits at the position where the rows meet — i.e. the
length of the original previous row.

This is the analogue of `<BS>` joining lines in any conventional
editor; without it, deleting the leading character of a wrapped
paragraph wouldn't make sense.

## Rendering with a cursor that lives *under* a glyph

Most renderers draw text first, then either invert a region for
the cursor or composite a transparent block on top.  We do
something even simpler:

1. Clear the entire window to background (`gui_fill_rect`).
2. For each visible row, `gui_draw_text` with foreground = black,
   background = warm off-white.  Because we pass `transparent =
   0`, the renderer paints both fg and bg pixels — every glyph is
   crisp regardless of what was there before.
3. Compute the cursor's window-relative `(cx, cy)` from
   `g_cur_row - g_top_row`.
4. Fill an 8×16 rectangle at `(cx, cy)` with the cursor blue.
5. If a glyph occupies that cell (`col < line_len`), redraw it
   *one more time* in the background colour with `bg = cursor
   blue` — i.e. the same glyph is rendered twice, the second time
   using the cursor as its background and the editor's background
   as the foreground.

The visual result is "glyph appears in negative through the
cursor block" — exactly what readers expect from a conventional
text-mode cursor — but the implementation is two `gui_draw_text`
calls and one `gui_fill_rect`, with no compositing logic on the
caller side.  The kernel font renderer already paints both fg
and bg pixels per glyph cell, so the inversion is automatic.

## Scrolling: anchor the cursor, lazily

Notepad doesn't keep an explicit "scroll position" updated on
every keystroke.  Instead, every render starts with a 4-line
function:

```c
static void scroll_to_cursor(void) {
    if (g_cur_row < g_top_row) g_top_row = g_cur_row;
    if (g_cur_row >= g_top_row + ROWS) g_top_row = g_cur_row - ROWS + 1;
    if (g_top_row < 0) g_top_row = 0;
}
```

The contract is "the cursor must be visible after this call."
Up-arrow style scrolling becomes "decrement `g_cur_row`, render"
— the scroll position adjusts on its own.  We never compute
"smooth scroll" deltas.

## Loading and saving — the writable-tmpfs round trip

`load_file(path)` is straightforward: `open(path, O_RDONLY)`,
read in 256-byte chunks, hand each byte to `push_byte_at_eof`
which honours `'\n'` and skips `'\r'` (CRLF normalisation).  If
`open` returns negative (file does not exist), we silently start
with an empty buffer and flash a "(new file)" status.

`save_file(path)` is where we exercise machinery the project has
had for several chapters but never used outside the shell:

```c
#define OPEN_WRITE_TRUNC 577   /* O_WRONLY | O_CREAT | O_TRUNC */

int fd = open(path, OPEN_WRITE_TRUNC);
for (int r = 0; r < g_line_count; r++) {
    if (g_line_len[r] > 0)
        write(fd, g_lines[r], g_line_len[r]);
    if (r != g_line_count - 1)
        write(fd, "\n", 1);
}
close(fd);
```

This compiles down to `vfs_open` with the `O_CREAT | O_TRUNC`
combo which is allowed only on the `/tmp` mount (other mounts
are read-only).  Every `write(fd, ...)` lands in
`sys_write`'s `FD_TMPFS_RW` branch, which calls `tmpfs_write`
in 256-byte chunks.

The smoke test verifies the round trip end-to-end:

```python
type_text(qmp, "notepad /tmp/np.txt\n")
type_text(qmp, "hello notepad\n")
type_text(qmp, "second line here\n")
send_ctrl(qmp, "s")
send_ctrl(qmp, "q")
type_text(qmp, "cat /tmp/np.txt\n")
assert b"hello notepad" in serial and b"second line here" in serial
```

If any of `vfs_open`, `tmpfs_write`, or our save loop is broken,
the `cat` at the end won't see the right bytes.

## Why 577 magic-numbered into a `#define`

The userspace `syscall.h` doesn't currently re-export `O_CREAT`
and `O_TRUNC` from `vfs.h` — the kernel-side header isn't on
the user include path.  Rather than copy three `#define`s into a
hot syscall header that other apps also include, we computed
the bitwise OR by hand and wrote a comment.  When the user libc
grows a `<fcntl.h>` we'll replace the literal with the symbolic
form; until then, the magic number is purely local to notepad.

Why not just write 1 (O_WRONLY)?  Because without `O_CREAT` the
first save would fail if the file doesn't exist yet, and without
`O_TRUNC` re-saving a shorter file would leave stale bytes on
disk.  All three flags are necessary for the obvious "save"
semantics.

## Sending Ctrl-letter from QMP

The smoke test had to type Ctrl-S and Ctrl-Q.  QEMU's
`input-send-event` works at the keycode layer, so we have to
press the modifier first, then the letter, then release the
letter, then release the modifier — all in one QMP message so
the ordering can't race with virtio-input event delivery:

```python
def send_ctrl(qmp, qcode):
    events = [
        {"type":"key","data":{"down":True, "key":{"type":"qcode","data":"ctrl"}}},
        {"type":"key","data":{"down":True, "key":{"type":"qcode","data":qcode}}},
        {"type":"key","data":{"down":False,"key":{"type":"qcode","data":qcode}}},
        {"type":"key","data":{"down":False,"key":{"type":"qcode","data":"ctrl"}}},
    ]
    qmp_send({"execute":"input-send-event","arguments":{"events":events}})
```

The kernel side of this lives in `kernel/device/virtio_input.c`:
shift and ctrl modifier state is tracked across events
(`g_shift_down`, `g_ctrl_down`), and at the moment a printable
key fires, ctrl-letter is folded down to the corresponding
control byte (1..26).  The byte is then ring-pushed and reaches
the GUI via the same path as a plain ASCII keystroke.  Notepad
gets `0x13` for Ctrl-S and `0x11` for Ctrl-Q in `ev.arg0` and
matches them against `#define CTRL_S 0x13`.

## The "modified" indicator

Every editing operation sets `g_dirty = 1`.  Every successful
save sets it back to 0.  The status renderer checks it and, if
true, draws an amber `*` near the right edge of the status bar
(which is otherwise dark blue text on the same background, so
the asterisk pops).

This is the one piece of richer GUI vocabulary the editor needs
to be useful — without it, the user has no idea whether their
edits are persisted.  It costs four lines of code.

## What we deliberately did *not* build

- **Cursor movement keys.**  No arrows, Home/End, PgUp/PgDn.
  These all need our virtio-input driver to start emitting
  beyond-ASCII codes through a new GUI key event channel; that
  is a chapter on its own.
- **Selection / cut / copy / paste.**  Requires a clipboard
  service and modifier+letter handling beyond Ctrl-S/Q.
- **Multiple files / tabs.**  The window itself is the document.
- **Open dialog.**  The file path comes from `argv[1]`; you launch
  notepad from the shell or from gui_term with `notepad
  /tmp/foo.txt`.
- **Word wrap.**  Lines longer than 88 columns scroll off the
  right edge; we just don't draw beyond the window.
- **Undo.**  Requires a history of edit operations.

Each of these is a milestone in its own right, but they aren't
on the critical path to a usable text editor.

## Putting it together

A normal editing session now looks like:

1. From the shell (or from inside `gui_term`), type
   `notepad /tmp/foo.txt`.
2. The window opens; if the file exists its contents appear, if
   not the buffer is empty and the status bar shows `(new file)`.
3. Type some text.  The amber `*` appears.
4. Press Ctrl-S; the status bar briefly shows `saved.` and the
   `*` disappears.
5. Press Ctrl-Q to exit.
6. Run `cat /tmp/foo.txt` and the bytes are there, identical to
   what you typed.

That last step — the read-back via a completely independent
process — is what makes this milestone the end of a long arc.
The disk image, the virtio-blk driver, the OSFS directory, the
tmpfs writable mount, the per-process address space, the VFS
fd table, the GUI window manager, the bitmap font, the kernel
keyboard driver: all of them are doing their jobs invisibly,
because if any of them weren't, the round trip would fail.
