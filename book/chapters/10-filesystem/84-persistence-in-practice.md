# Chapter 84 — Save As: dialogs, libraries, and the first widget toolkit

OSFS-2 (chapter 81) gave us a writable disk.  Chapter 83 made
those writes crash-consistent.  Chapter 51 gave us a notepad.
What we still didn't have, until this chapter, was the
boring-but-essential glue: a way for the user to **choose where
to save**.

Pre-84 notepad was hardwired:

- Bare-launch (`notepad` with no args) wrote to
  `/data/untitled.txt` on Ctrl-S.  Always the same path.
- Argv-launch (`notepad /tmp/np.txt`) wrote to whatever the
  user passed on the command line.

That's fine for a smoke test.  It's not a text editor a person
would use.  Two real-OS workflows are missing:

1. **"I just typed a fresh document; save it under the name
   I want."**  The user shouldn't have to know there's a file
   called `untitled.txt` they're going to silently overwrite,
   nor that a new bare-launch will silently overwrite *that*.
2. **"I want to save a copy under a different name."**  Old-
   school "Save As" — pick a different filename, original
   stays untouched.

This chapter ships a modal **Save As dialog** that handles
both, and — more importantly — it ships it as a **library**
rather than as inline notepad code.

## Architectural pivot: why a library?

The first cut of this chapter put the dialog directly inside
`userspace/notepad/notepad.c`.  It worked.  But the file went
from 7600 bytes to 10040 — nearly half of notepad was now
"how to draw a Save As dialog" rather than "how to edit
text."  That's a smell.

The same dialog will be wanted by:

- `/bin/paint` (chapter 70's BMP exporter)
- A future `/bin/notes2html` exporter
- Any future app that produces a file the user might want to
  name

If each of those copies the dialog inline, fixing a bug in
"the way we list `/data/`" turns into N coordinated edits.
That's the canonical reason operating systems factor common
UI into shared libraries (Win32's COMDLG32, GTK's
`GtkFileChooser`, NSSavePanel on macOS, etc).

We don't yet have a dynamic loader, and we don't yet have an
archiver step in the build.  But we **do** have multi-object
binaries — every `*_OBJS` list in the Makefile is just a list
of `.o` files.  We just hadn't used it.  This chapter
formalises the directory layout that lets us factor common
widget code:

```
userspace/
  libgui/
    save_dialog.h         <-- public API
    save_dialog.c         <-- implementation
  notepad/
    notepad.c             <-- consumer
```

and the Makefile change is one line:

```make
LIBGUI_OBJS  := $(BUILD)/userspace/libgui/save_dialog.o
NOTEPAD_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/notepad/notepad.o \
                $(LIBGUI_OBJS)
```

The pattern rule

```make
$(BUILD)/userspace/%.o: userspace/%.c
        $(CC) $(USER_CFLAGS) -c $< -o $@
```

picks up `userspace/libgui/save_dialog.c` automatically.

### Why direct-link `.o` and not `.a` archive?

For exactly one consumer, `.a` adds nothing.  When the second
consumer arrives we'll wrap `LIBGUI_OBJS` into
`$(BUILD)/libgui.a` and let the linker pull in only the
objects each app actually references.  That's an O(15-line)
Makefile change and zero source change.  Don't pre-build
infrastructure you don't need.

## The dialog API

```c
/* userspace/libgui/save_dialog.h */
typedef void (*gui_render_cb)(void *ud);

int gui_save_dialog(int win_id,
                    int win_w, int win_h,
                    const char *dir_prefix,
                    const char *initial_name,
                    gui_render_cb render_under, void *ud,
                    char *out_path, size_t cap);
```

Returns:

- `1` if the user confirmed; `out_path` contains the chosen
  full path (`/data/foo.txt`).
- `0` if the user cancelled (ESC).
- `-1` for invalid arguments.

**The call blocks** until confirm or cancel.  The library
runs its own `gui_poll_event` / `yield()` loop while the
dialog is open.  That keeps the API surface tiny — one
function call, no dialog-object lifecycle for the caller to
reason about — and matches how every other modal-dialog
toolkit ships (Win32's `GetSaveFileName`, NSSavePanel's
`runModal`, etc).

### The `render_under` callback

The dialog draws into the **same window** as the caller; we
don't yet have a "modal child window" concept in the WM.
That means whatever the editor was showing behind the dialog
will go stale unless somebody re-paints it each frame.

Two options:

1. **Stop re-painting.**  The editor freezes visually while
   the dialog is open.  Simple, but ugly: the caret stops
   blinking, status timers freeze, etc.
2. **Let the library re-paint the caller's window before each
   dialog frame.**  This is what we ship.

The dialog calls `render_under(ud)` once per dialog frame,
*before* drawing its panel overlay, then flushes once at the
end.  Notepad's wrapper is two lines:

```c
static void editor_repaint_under(void *ud) {
    (void)ud;
    render_to_buffer();    /* paint editor; do NOT flush */
}
```

**The callback must paint into the back-buffer but must NOT
call `gui_flush`.**  This is documented in `save_dialog.h` and
is the source of a subtle per-keystroke-flicker bug encountered
while building this chapter.  Each frame did:

1. Dialog calls `render_under(ud)` → notepad's `render()`
   runs and **flushes**.  At this instant the back-buffer
   has only the editor in it; the WM composes the editor
   *without the dialog* to the screen.
2. Dialog paints its panel on top in the back-buffer.
3. Dialog calls `gui_flush` — composes again, this time
   *with* the dialog visible.

Between (1) and (3) the user sees the bare editor for one
compose pass.  Each keystroke triggers a re-render, so the
dialog visibly flashes on every key.  The fix is to split
notepad's renderer into `render_to_buffer()` (no flush) and
`render()` (calls `render_to_buffer()` then flushes).  The
libgui callback uses the no-flush variant.  Lesson:
back-buffer painters compose; flushes serialise.  Mixing the
two across an API boundary is how you invent flicker.

The `ud` opaque pointer is threaded through verbatim.
Notepad doesn't need it (it has globals); a future cleaner
caller could pass `&editor_state`.

### Single-instance state

`save_dialog.c` keeps the file-list, field text, selection,
and scroll position in static buffers.  That's fine because:

- The function blocks until the dialog closes; you can never
  have two dialogs open in the same process.
- We don't (yet) support nested dialogs (an Open dialog on
  top of a Save dialog would need its own state, but that's
  Open from Save — doesn't happen).

If a future widget violates that assumption (e.g. a
non-blocking dialog), we'll switch the static buffers for an
opaque heap-allocated `struct gui_save_dialog *`.  Same
shape, just ownership shifts to the caller.

## What the dialog actually does

Render layout (centred in the caller's window):

```
+-- Save As ----------------------+
| Save in: /data/                 |
|                                 |
| +---------------------------+   |
| | foo.txt              42 b |   |  <-- list of /data/* leaves
| | bar.txt              17 b |   |      (selection auto-fills
| | untitled.txt         12 b |   |       the field below)
| +---------------------------+   |
|                                 |
| Filename: [untitled.txt|   ]    |  <-- editable field, blue cursor
|                                 |
| (will overwrite)                |  <-- shown iff name matches a
|                                 |      list entry
| Up/Down: pick  Enter: save  ESC |
+---------------------------------+
```

Key bindings:

- **Up / Down** — navigate the file list.  Selecting a row
  auto-fills the filename field (matches every native Save
  As since the 90s; lets the user "save under this exact
  name" with one keystroke and Enter).
- **Left / Right / Home / End** — move the field cursor.
- **Backspace** — delete one char to the left of the
  cursor.  Detaches the highlight from the list (typing in
  the field invalidates the "you're saving under that
  selected name" assumption).
- **Printable ASCII** — inserted into the field at the
  cursor.
- **Enter** — confirm; the dialog returns 1 with
  `out_path = dir_prefix + field`.
- **ESC** — cancel; the dialog returns 0.

### Why list directory contents?

Showing the file list is what turns an opaque "type the
filename" prompt into a Save As **dialog** — the user can
see what's already in the directory and:

- choose a unique name (don't pick `foo.txt` if there's
  already a `foo.txt`),
- knowingly overwrite (pick `foo.txt`, see the "(will
  overwrite)" hint, hit Enter),
- match an existing file's naming convention.

Implementation is straight `listdir(idx, name, cap, &size)`
in a loop, filtered to direct children of `dir_prefix`
(entries whose leaf contains no further `/`).

## Notepad changes

Almost everything dialog-related left notepad.c.  What
remained:

```c
static int g_path_chosen = 0;     /* 1 = path is real; 0 = ask on save */

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && argv[1][0]) {
        s_copy(g_path, argv[1], sizeof(g_path));
        g_path_chosen = 1;        /* argv-launch: keep the path */
    } else {
        s_copy(g_path, "/data/untitled.txt", sizeof(g_path));
        g_path_chosen = 0;        /* bare-launch: pop dialog on save */
    }
    ...
}
```

And in the keystroke handler:

```c
if (c == CTRL_S) {
    if (!g_path_chosen) {
        run_save_as();            /* libgui call; blocks */
        break;
    }
    int rc = save_file(g_path);
    ...
}
```

`run_save_as` is a thin wrapper that builds the initial
filename (the leaf of `g_path`), calls `gui_save_dialog(...)`,
and on confirm both saves the file and remembers the chosen
path so subsequent Ctrl-S writes straight through.

That's "Save vs Save As" semantics in 8 lines.

## A coordinate-agnostic dialog detector

The smoke test (`scripts/test_notepad_save_as.py`) faces a
peculiar problem: **the test can't easily compute where the
dialog will appear on screen**.  The WM auto-cascades windows
by 32 px per spawn, and notepad's exact (x, y) depends on
what else has been spawned ahead of it (taskbar, launcher,
wallpaper).  The test's first attempt hardcoded
`DLG_X = WIN_X + (WIN_W - DLG_W) / 2`, missed by 36 px, and
reported "dialog not detected" even though the dialog was
on-screen perfectly.

The fix is the same trick we used in the headless browser
chapters: **detect a unique colour anywhere in the frame
buffer.**  The dialog panel BG is `GUI_BGRA(0xF0,0xF0,0xF4)`
— a "white with a hint of blue" that doesn't appear in the
editor (warm off-white), the WM frame (saturated blue), the
taskbar cells (navy), the wallpaper, or any glyph rendering.
Counting that pixel across the whole framebuffer goes from 0
(dialog not visible) to ~15 000 (dialog visible), which is
unambiguous.

The dialog **frame** colour is navy `(0x30, 0x40, 0x70)`,
which we initially also wanted to count — but the taskbar
cells use the *exact same* navy.  Lesson: when introducing a
new widget colour, grep the codebase for the RGB tuple
before committing to it.  Either pick a unique colour, or
design tests to use only colours unique to the new widget.

## Files

### New
- `userspace/libgui/save_dialog.h` — public API.
- `userspace/libgui/save_dialog.c` — implementation
  (~410 lines: render, event loop, file-list refresh, field
  editing).
- `scripts/test_notepad_save_as.py` — end-to-end smoke
  test.

### Changed
- `userspace/notepad/notepad.c` — added `g_path_chosen`,
  `editor_repaint_under`, `run_save_as`; removed ~280 lines
  of inline dialog code.
- `Makefile` — added `LIBGUI_OBJS` and appended it to
  `NOTEPAD_OBJS`.

## What's deferred

The original chapter-84 stub also planned shell history
persistence and browser cookie storage in `/data/`.  Those
move to chapter 85 (split off because they have nothing to
do with GUI dialogs and combining them in one chapter would
have made for a 4000-line walk).

## What this unlocks

- Every future GUI app can include `<libgui/save_dialog.h>`
  and get a Save As prompt for free.
- The pattern — a widget directory, a single header, a
  blocking call with a render-under callback — generalises
  to:
  - `open_dialog.h` (pick a file to read)
  - `message_box.h` (OK / Cancel alert)
  - `color_picker.h` (paint's palette selection)
- Multi-translation-unit userspace apps are now a known-good
  build pattern, not a theoretical Makefile feature.

## See also

- Chapter 51 — notepad's original single-path save logic.
- Chapter 81 — OSFS-2's flat namespace (which is why the
  dialog rejects `/` in filenames).
- Chapter 83 — the journal that makes the saved file
  durable across crashes.
- Chapter 85 — (next) shell history and browser cookies on
  `/data/`.

## Bugs found and fixed in this chapter

- **Per-keystroke dialog flicker.**  The first cut of
  `editor_repaint_under` called notepad's `render()`, which
  flushes.  Each frame the user briefly saw the bare editor
  before the dialog overlay re-flushed.  Fixed by splitting
  notepad's render into `render_to_buffer()` (no flush) and
  `render()` (paint + flush), and pointing the callback at
  the former.  Lesson recorded in
  `save_dialog.h`: "the callback must paint, must not
  flush."
- **Test sampled at hardcoded coordinates.**  The first cut
  of the test computed where the dialog should be on screen
  from `WIN_X + (WIN_W - DLG_W) / 2`.  The WM auto-cascades
  windows in 32-px steps, so the dialog landed 36 px below
  the test's expectation.  Fixed by counting unique-colour
  pixels across the whole framebuffer instead of sampling
  at exact coords.
- **Reused a colour the taskbar already owned.**  The
  dialog frame is navy `(0x30,0x40,0x70)` — same as the
  taskbar cell BG.  Tried to use it as an absolute presence
  detector; failed.  Now the test uses the panel BG
  `(0xF0,0xF0,0xF4)` (which is unique to libgui) for
  presence, and the navy delta only as a corroborating
  signal.