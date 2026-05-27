# Chapter 43 — Cursor movement and readline keybindings

Chapter 42 gave us a raw-mode line editor with arrow-key history but
cursor-stuck-at-end semantics: backspace erased the most recent
character, and there was no way to insert into the middle of a line
or jump around within it.  This chapter completes the line editor
with the smallest set of `readline`-style keybindings that turn
"history-only" into "real editing":

- **Left / Right arrows** (`ESC [ D` / `ESC [ C`): move the cursor
  one cell within the current line.
- **Backspace mid-line**: erases the character to the left of the
  cursor and shifts the rest left.
- **Insert mid-line**: any printable byte is inserted at the cursor
  and shifts everything after it right.
- **Ctrl-A**: jump cursor to start of line (column 0 of the edit area).
- **Ctrl-E**: jump cursor to end of line.

All entirely in the shell — no kernel changes required.  The kernel
side from chapter 42 (single-byte raw read, no echo, no buffering)
already gives us everything we need.

## The mental model

The line editor now tracks two integers instead of one:

| field | meaning                                              |
|-------|------------------------------------------------------|
| `pos` | total length of the line (0..LINE_MAX-1)             |
| `cur` | cursor position within the line (0..`pos`, inclusive)|

Every operation modifies these and then either updates the screen
incrementally (append-at-end fast path, simple cursor moves) or does
a full line redraw with the cursor repositioned.

`redraw_line(buf, len, cur)` is the one place that knows how to put
the screen back into a known-good state:

```c
write(1, "\x1b[2K\r", 5);              /* erase line + return to col 0 */
prompt();                               /* redraw the prompt */
if (len > 0) write(1, buf, len);        /* redraw content */
for (int i = cur; i < len; i++)         /* walk cursor back to `cur` */
    write(1, "\b", 1);
```

The cursor-positioning trick — emit `len - cur` backspaces — sidesteps
having to know the prompt's printed width.  Backspace doesn't erase,
just moves the cursor left, which is exactly what we want here.

This redraws every time something mid-line changes.  For our line
sizes (max 128 chars at most over a 9600-baud-equivalent serial that
is actually multi-megabit) the redraw is invisibly fast.  A more
sophisticated editor would do incremental updates with `ESC[<n>P` (delete
char) and `ESC[<n>@` (insert char), but that buys us nothing here.

## Append-at-end fast path

Most keystrokes appended to a line don't require a redraw — the new
char just echoes and the cursor naturally advances.  We special-case
this path:

```c
if (cur == pos) {
    /* Append-at-end: just echo the byte. */
    out[pos++] = c;
    cur++;
    out[pos] = '\0';
    write(1, &c, 1);
} else {
    /* Mid-line insert: shift bytes [cur..pos] right by 1, redraw. */
    for (int i = pos; i > cur; i--) out[i] = out[i - 1];
    out[cur] = c;
    pos++;
    cur++;
    out[pos] = '\0';
    redraw_line(out, pos, cur);
}
```

So normal typing has zero perceptible overhead; only mid-line edits
trigger the (still-cheap) full redraw.

## Backspace

```c
if (c == 0x7f || c == 0x08) {
    if (cur > 0) {
        for (int i = cur - 1; i < pos - 1; i++) out[i] = out[i + 1];
        pos--;  cur--;  out[pos] = '\0';
        redraw_line(out, pos, cur);
    }
    continue;
}
```

End-of-line backspace could keep the chapter-43 fast path
(`\b \b`), but for code size and clarity the unified redraw covers
both cases at trivial cost.

## Left / Right arrows

```c
if (b2 == 'C') {                /* right arrow */
    if (cur < pos) {
        cur++;
        write(1, &out[cur - 1], 1);   /* re-echo the char we passed over */
    }
}
if (b2 == 'D') {                /* left arrow */
    if (cur > 0) {
        cur--;
        write(1, "\b", 1);
    }
}
```

For right arrow we cheat: instead of emitting `ESC[C`, we re-echo the
character the cursor is now over.  The cursor naturally advances
past it.  This avoids any dependency on `ESC[C` being supported (it
is, on every modern terminal, but re-echoing is also one byte
shorter and easier to reason about).

## Ctrl-A / Ctrl-E

```c
if (c == 0x01) { cur = 0;   redraw_line(out, pos, cur); continue; }
if (c == 0x05) { cur = pos; redraw_line(out, pos, cur); continue; }
```

Ctrl-A (start) and Ctrl-E (end) both fall back to the unified redraw
path.  In principle they could be optimised to "emit `cur` backspaces"
and "emit `pos - cur` re-echoes" but the redraw is one or two
millisecond's worth of bytes for our line sizes, so we keep it
simple.

> **QEMU caveat.** `qemu-system-aarch64 -nographic -serial mon:stdio`
> intercepts `Ctrl-A` as the **monitor escape prefix** before the
> guest ever sees it.  This means automated tests piped through
> stdin can't exercise Ctrl-A, and interactive users typing Ctrl-A
> are talking to QEMU's monitor, not to the shell.  To use Ctrl-A as
> the line-editor "start of line" key, run with
> `-serial chardev:char0 -chardev stdio,id=char0,signal=off` (no
> monitor multiplex), or just use `Home` if your terminal sends
> `ESC[H` for it.  Ctrl-E (`0x05`) and Ctrl-D (`0x04`) are
> uninterfered.

## History interaction

Loading a history entry now sets `cur = pos` (cursor at end of the
loaded line) so the user can immediately edit or commit.  This
matches readline / bash behaviour.

## What's still deferred

- **Ctrl-K** (kill to end of line) and **Ctrl-U** (kill from start to
  cursor): trivial extensions; both are O(LINE_MAX) shifts plus a
  redraw.  Not added because there's no kill-ring yet for paste.
- **Ctrl-W** (kill previous word) and word-boundary cursor moves
  (`Alt-B` / `Alt-F`): require a "what's a word boundary" predicate.
- **Multi-line wrapping**: still assumes prompt + line fit on one
  terminal row.  At LINE_MAX=128 and a typical 80-col terminal the
  user can edit a line that wraps physically; the editor will get
  out of sync with the terminal cursor on those wrapping cases.
- **Ctrl-R** reverse-i-search: needs a separate edit mode that holds
  a search query and shows matches incrementally.
- **Bracketed paste** (`ESC[200~ ... ESC[201~`): not handled; pasted
  multi-line input may behave oddly.

These are all polishings that would each take 20–80 LOC.  None are
necessary for any program we ship today.

## Files changed

- `userspace/sh/sh.c`:
  - `redraw_line` gains a `cur` parameter and emits `len - cur`
    backspaces after redrawing.
  - `read_line_raw` gains a `cur` integer alongside `pos`.
  - `ESC[C` / `ESC[D` (right/left arrow) handlers added.
  - Ctrl-A (`0x01`) and Ctrl-E (`0x05`) handlers added.
  - Backspace + insert paths refactored to be cursor-aware.
  - Append-at-end keeps the original fast path.
  - History-load sets `cur = pos`.
  - Help text updated.

## Verification

```
echo abc<Left><Left><Left>XYZ<Ctrl-E>_END<Enter>
   -> echo XYZabc_END
   -> XYZabc_END
echo abcdef<Left><Left><Left>X<Enter>
   -> echo abcXdef
   -> abcXdef
echo 123456<Left><Left><Backspace><Enter>
   -> echo 12356
   -> 12356
```

(Ctrl-A is intercepted by QEMU's monitor and not shown here, but the
code path is symmetric to Ctrl-E and is exercised by the same
redraw helper.)
