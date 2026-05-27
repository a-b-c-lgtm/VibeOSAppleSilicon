# Chapter 44 — Kill ring: Ctrl-K, Ctrl-U, Ctrl-W, Ctrl-Y

Chapter 43 gave us cursor movement and mid-line editing.  This
chapter adds the four readline / Emacs **kill-ring** keybindings
that turn the editor into something you'd actually want to use for
real command construction:

| keystroke | action                                               |
|-----------|------------------------------------------------------|
| `Ctrl-K`  | kill from cursor to end of line                      |
| `Ctrl-U`  | kill from start of line to cursor                    |
| `Ctrl-W`  | kill the word immediately left of the cursor         |
| `Ctrl-Y`  | yank — re-insert the most recently killed text       |

"Kill" is `readline`-speak for "cut": the removed text doesn't
just disappear, it gets stashed in a **kill ring** so a later
`Ctrl-Y` can paste it back.  Real Emacs has a multi-slot rotating
ring; ours has a single slot, which is enough for the
"oh, I deleted the wrong thing" use case that motivates 90 % of
yanks in practice.

## The kill ring

```c
static char g_kill[LINE_MAX];
static int  g_kill_len = 0;

static void kill_set(const char *src, int n)
{
    if (n < 0) n = 0;
    if (n > LINE_MAX - 1) n = LINE_MAX - 1;
    for (int i = 0; i < n; i++) g_kill[i] = src[i];
    g_kill[n]  = '\0';
    g_kill_len = n;
}
```

Every kill operation calls `kill_set` to **replace** the ring's
contents.  We do not accumulate (Emacs's "consecutive kills append";
ours don't) and we do not rotate.  Both are easy follow-ups; both
buy little for our current usage.

## Ctrl-K — kill to end of line

```c
if (c == 0x0b) {
    if (cur < pos) {
        kill_set(&out[cur], pos - cur);
        pos        = cur;
        out[pos]   = '\0';
        redraw_line(out, pos, cur);
    }
    continue;
}
```

Truncate the buffer at `cur`, save what was after it.  No need to
shift bytes around — we're just discarding the tail.

## Ctrl-U — kill to start of line

```c
if (c == 0x15) {
    if (cur > 0) {
        kill_set(out, cur);
        for (int i = 0; i + cur < pos; i++) out[i] = out[i + cur];
        pos -= cur;
        cur  = 0;
        out[pos] = '\0';
        redraw_line(out, pos, cur);
    }
    continue;
}
```

Save bytes `[0..cur)`, then shift the right side of the line left
by `cur` positions.  The cursor lands at column 0.

(Bash's `Ctrl-U` is "kill the entire line" rather than "kill to BOL".
We follow `readline`'s default — the difference is irrelevant when
the cursor is at end-of-line, which is the common case.)

## Ctrl-W — kill previous word

The shape of a "word" matters.  We use the simplest possible
definition: a maximal run of non-whitespace.  Trailing whitespace
to the left of the cursor is also eaten so that
`echo foo<space><space><Ctrl-W>` removes both `foo` and the
trailing spaces, leaving an empty buffer.

```c
if (c == 0x17) {
    if (cur > 0) {
        int end = cur;
        int start = cur;
        while (start > 0 && (out[start - 1] == ' ' || out[start - 1] == '\t'))
            start--;                          /* skip trailing whitespace */
        while (start > 0 && out[start - 1] != ' ' && out[start - 1] != '\t')
            start--;                          /* eat word characters */
        if (start < end) {
            kill_set(&out[start], end - start);
            for (int i = start; i + (end - start) < pos; i++)
                out[i] = out[i + (end - start)];
            pos -= (end - start);
            cur  = start;
            out[pos] = '\0';
            redraw_line(out, pos, cur);
        }
    }
    continue;
}
```

This matches Emacs's `backward-kill-word` behaviour with the
default `*-syntax* = whitespace` definition.  A more sophisticated
shell would treat shell metacharacters as word boundaries too
(`/`, `=`, `:`) — easy follow-up.

## Ctrl-Y — yank

```c
if (c == 0x19) {
    if (g_kill_len > 0 && pos + g_kill_len < cap) {
        for (int i = pos - 1; i >= cur; i--)
            out[i + g_kill_len] = out[i];        /* shift right */
        for (int i = 0; i < g_kill_len; i++)
            out[cur + i] = g_kill[i];             /* paste */
        pos += g_kill_len;
        cur += g_kill_len;
        out[pos] = '\0';
        redraw_line(out, pos, cur);
    }
    continue;
}
```

Identical structure to mid-line `printable insert` from chapter 43,
but inserting `g_kill_len` bytes instead of one.  The cursor lands
at the end of the yanked region — same as Emacs.

## Verified

```
echo abcdef + 3<Left> + <Ctrl-K> + <Enter>
  -> "echo abc"      -> "abc"
echo hello world + <Ctrl-W> + <Enter>
  -> "echo hello "   -> "hello"
echo abcdef + 3<Left> + <Ctrl-K> + <Ctrl-Y> + <Enter>
  -> "echo abcdef"   -> "abcdef"   (kill+yank is identity at same pos)
```

## What's deferred (the "real Emacs" features)

- **Multi-slot rotating kill ring**: needs a small array plus an
  "index of last yank" that `Alt-Y` rotates.
- **Consecutive-kills-append**: track whether the previous keystroke
  was also a kill operation; if so, append to ring instead of
  replacing.
- **`Alt-D`** (forward kill word): symmetric to `Ctrl-W`.  Not added
  because Alt sequences require two-byte ESC parsing in our editor.
- **`Ctrl-T`** (transpose chars): not added; trivial to implement.
- **`Ctrl-_`** (undo): would need a snapshot history.

Each is 20–60 LOC of pure userspace.  None unblock anything else
we'd want to build.

## Files changed

- `userspace/sh/sh.c`:
  - Added `g_kill[]`, `g_kill_len`, `kill_set()`.
  - Added `Ctrl-K` (`0x0b`) handler — kill to EOL.
  - Added `Ctrl-U` (`0x15`) handler — kill to BOL.
  - Added `Ctrl-W` (`0x17`) handler — kill previous word.
  - Added `Ctrl-Y` (`0x19`) handler — yank from kill ring.
  - Help text updated.

No kernel or libc changes — the entire feature lives in `sh.c`,
which is fitting because the kill ring is purely an editor concept.
