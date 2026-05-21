/*
 * userspace/libgui/save_dialog.h — modal "Save As" dialog widget.
 *
 * The first member of the libgui userspace widget library
 * (chapter 84).  Apps include this header and link against
 * `userspace/libgui/save_dialog.o` to get a complete, self-
 * contained "ask the user where to save" dialog without having
 * to grow that code into the app itself.
 *
 * Chapter 108d — ported off the kernel WM syscalls
 * (gui_fill_rect / gui_draw_text / gui_flush / gui_poll_event)
 * onto the wsd-backed libgui primitives (draw_fill_rect /
 * draw_text / wm_window_dirty / wm_poll_event).  The dialog
 * draws straight into the caller's per-window FB; the only
 * IPC round-trip per frame is the single WM_WIN_DAMAGE that
 * `wm_window_dirty` issues at the end.
 *
 * Why a library?
 * --------------
 *
 * A Save As dialog is ~250 lines of widget code (file-list
 * scrolling, a text-entry field with cursor, an overwrite
 * warning, modal event capture, dialog rendering) that isn't
 * really part of any one app — the Notepad doesn't care HOW the
 * dialog works, only that it can ask "where should I save this?"
 * and get back a path.  Any future app (paint, text-mode
 * browser bookmarks, anything that writes files) wants the
 * same thing.
 *
 * The build system has supported multi-object apps from day one
 * (every `*_OBJS` list in the Makefile is a list of .o files);
 * we just hadn't used it.  This header + its .c file is the
 * first time we factor app-shared code into a separately-
 * compiled translation unit and link it into the consuming app.
 *
 * Programming model
 * -----------------
 *
 * One blocking call.  The dialog runs its own event loop until
 * the user either confirms (Enter) or cancels (ESC).  Because
 * the dialog steals input while open, the caller must give it
 * a callback that re-paints the underlying window — otherwise
 * the editor area behind the dialog would never update if (say)
 * a status bar timer kept changing.
 *
 * The dialog draws into the caller's existing window.  It does
 * NOT create a new GUI window — the WM has no "modal child"
 * concept yet, and one window with overlay is closer to how
 * most desktop OSes ship dialogs (a single top-level window per
 * app, modals layered into it).
 *
 * Future widgets in this directory:
 *   - open_dialog.h   — same shape but for opening a file
 *   - message_box.h   — a tiny "OK / Cancel" alert
 *   - color_picker.h  — for /bin/paint's palette selection
 */
#ifndef LIBGUI_SAVE_DIALOG_H
#define LIBGUI_SAVE_DIALOG_H

#include <stddef.h>
#include "wmclient.h"

/* Background-render callback.  Called once per dialog frame,
 * BEFORE the dialog overlay is drawn, with `ud` passed through
 * from the gui_save_dialog() call.  May be NULL — in which case
 * the area outside the dialog panel is left as whatever the
 * window had when gui_save_dialog() was entered.
 *
 * IMPORTANT: the callback must paint into the window's back-
 * buffer (draw_fill_rect / draw_text), but must NOT call
 * wm_window_dirty.  The dialog issues a single whole-window
 * damage itself at the end of each frame after its overlay has
 * been drawn on top of the callback's output.  If the callback
 * also damaged, the user would see the underlying view
 * (without the dialog) flash on screen between the callback's
 * damage and the dialog's damage; per-keystroke flicker is the
 * symptom. */
typedef void (*gui_render_cb)(void *ud);

/* Open a modal Save As dialog inside an already-open window.
 *
 *   win           target wsd-backed window — the dialog renders
 *                 into win->fb and damages via wm_window_dirty.
 *                 Must have been created by wm_create_window_input
 *                 so events arrive at wm_poll_event.
 *   win_w, win_h  window dimensions; the dialog centres itself.
 *   dir_prefix    directory to enumerate and prepend to the
 *                 returned path (e.g. "/data/").  Must end in
 *                 '/'.  Only direct children of this prefix
 *                 are listed.
 *   initial_name  pre-filled filename text.  Pass "" to start
 *                 the field blank, or e.g. "untitled.txt".
 *   render_under  callback (or NULL) that re-paints the
 *                 underlying window between dialog frames.
 *   ud            opaque pointer threaded into render_under.
 *   out_path      receives the chosen FULL path on confirm
 *                 (e.g. "/data/foo.txt"), NUL-terminated.
 *   cap           size of out_path in bytes; must be >= 32.
 *
 * Returns:
 *    1  user confirmed; out_path holds the chosen full path.
 *    0  user cancelled (ESC).
 *   -1  invalid arguments (cap too small, dir_prefix without
 *       trailing slash, win == NULL or win->id == 0).
 *
 * Blocks until confirm or cancel.  Uses wm_poll_event /
 * yield() in a tight loop; the caller's event loop is paused.
 */
int gui_save_dialog(struct wm_window *win,
                    int win_w, int win_h,
                    const char *dir_prefix,
                    const char *initial_name,
                    gui_render_cb render_under, void *ud,
                    char *out_path, size_t cap);

#endif /* LIBGUI_SAVE_DIALOG_H */
