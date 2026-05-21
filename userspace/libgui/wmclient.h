/*
 * userspace/libgui/wmclient.h — chapter 108d
 * client library for /srv/wm.
 *
 * What this file is
 * -----------------
 *
 * The thinnest possible userspace shim over the chapter-108d
 * wire protocol (wm_proto.h), exposing window lifecycle to
 * apps without exposing the IPC details.  Where the kernel
 * GUI path uses three calls (`gui_create_window`,
 * `gui_window_fb`, `gui_window_dirty`), the wsd path uses
 * three matching calls (`wm_create_window`, `wm_map_window`,
 * `wm_window_dirty`).  Same shape on purpose: apps porting
 * from one to the other change identifiers, not control flow.
 *
 * Programming model
 * -----------------
 *
 *   1. `wm_connect()` once at startup.  Returns 0 on success
 *      after WM_HELLO completes; -1 if /srv/wm is unreachable.
 *      Idempotent — the second call is a no-op.
 *
 *   2. `wm_create_window(w, h, flags, &win)` allocates a
 *      slot in wsd and the kernel-side per-window FB, then
 *      maps that FB into the caller's address space (so
 *      `win.fb` is immediately usable for pixel writes).
 *      This bundles wsd's CREATE + MAP_FB and the kernel's
 *      SYS_WIN_FB_MAP — three round-trips folded into one
 *      call because every realistic app does all three at
 *      startup anyway.
 *
 *   3. Per frame: write pixels through `win.fb.pixels`
 *      (a `struct gui_fb` exactly compatible with
 *      libgui/draw.h's primitives, so `draw_fill_rect`,
 *      `draw_text` etc. work unchanged), then call
 *      `wm_window_dirty(&win, x, y, w, h)` to push the
 *      damaged region to the scanout.
 *
 *   4. `wm_window_move(&win, x, y)` to reposition.  Cheap;
 *      doesn't trigger any compose.  Follow with a damage
 *      call if the visible pixels should update.
 *
 *   5. `wm_destroy_window(&win)` releases everything.
 *      Implicit on conn close / process exit; explicit is
 *      polite.  Sets `win.id` to 0 so accidental reuse
 *      becomes a clean -EPERM rather than a use-after-free.
 *
 * Cross-cuts with libgui/draw.h
 * -----------------------------
 *
 * `struct wm_window` embeds a `struct gui_fb` whose layout
 * matches the kernel-WM one byte-for-byte.  This is the
 * pivot that makes the chapter-108d long-tail port trivial: every
 * existing draw_* primitive is already written against
 * `gui_fb`, so the only thing that changes for an app being
 * ported is which window-lifecycle calls it uses.  The
 * inside of the paint loop stays exactly the same.
 *
 * One conn per process
 * --------------------
 *
 * wsd's accept loop fans out one conn per client (chapter
 * 107 IPC).  A process can have many windows; they all share
 * the single `/srv/wm` connection this module holds.  The
 * conn's session_id (returned by WM_HELLO) is implicit — wsd
 * stamps it on every window for ownership checks, but apps
 * don't need to see it.
 */

#ifndef LIBGUI_WMCLIENT_H
#define LIBGUI_WMCLIENT_H

#include "../libc/syscall.h"
#include "../libc/wm_proto.h"

/* A wsd-backed window.  Lifetime tied to the process; one
 * struct per `wm_create_window` call.  Embed the gui_fb at
 * a fixed offset (first field) so an app that already drove
 * the kernel-WM path can hand `&win.fb` to its existing
 * draw_* call sites with zero changes.
 *
 * `id` doubles as a "live" flag: 0 means "destroyed or never
 * created".  `wm_window_dirty` / `wm_window_move` /
 * `wm_destroy_window` all check id and return -1 if it's 0,
 * so accidental reuse can't hit the wire.
 *
 * `kernel_id` is the id of a NO_DECORATION kernel-WM window
 * that this process opened in parallel with the wsd window so
 * that the kernel can route pointer / keyboard events through
 * its existing per-pid event queues (chapter 108d keeps the input
 * path in the kernel WM; only compose moved to wsd).  Zero if
 * the caller didn't ask for input routing, in which case
 * `wm_poll_event` always returns 0. */
struct wm_window {
    struct gui_fb fb;            /* same layout as kernel-WM gui_fb */
    uint32_t      id;            /* wsd window id; 0 == not live */
    uint32_t      fb_id;         /* kernel win_fb id (for unmap on destroy) */
    uint32_t      x;             /* scanout-relative origin (set by wsd) */
    uint32_t      y;
    int32_t       kernel_id;     /* chapter-108d shadow window for input; -1 if none */
};

/* Connect to /srv/wm and complete WM_HELLO.  Returns 0 on
 * success, -1 if the bind isn't there yet (wsd not up) or
 * the version handshake failed.  Idempotent: the second
 * call returns 0 without doing any I/O.
 *
 * On the first call after a previous failure, the module
 * retries from scratch (clears the cached fd).  This matches
 * fontd-client behaviour: a transient outage doesn't poison
 * the rest of the process. */
int  wm_connect(void);

/* Close the /srv/wm connection.  wsd's gc_conn_windows will
 * reap any still-live windows on its side.  Apps don't
 * usually call this — process exit covers it. */
void wm_disconnect(void);

/* Create a window, allocate the per-window FB, and map it
 * into the caller's AS so `win->fb.pixels` is ready for
 * direct writes.  Returns 0 on success.  On any failure the
 * returned struct has `id == 0` so it's safe to pass to
 * `wm_destroy_window` (which will no-op).  Reasons for
 * failure logged to stderr (the chapter-107 IPC stack
 * doesn't surface errno on this path).
 *
 * No input routing — see `wm_create_window_input` if the
 * app needs to call `wm_poll_event`. */
int  wm_create_window(uint32_t w, uint32_t h, uint32_t flags,
                      struct wm_window *out);

/* Same as wm_create_window but also opens a shadow window in
 * the kernel WM at the same scanout position with the same
 * dimensions, owned by the calling pid.  The kernel WM
 * doesn't compose any more (chapter 108d), but it still owns
 * input — pointer hit-testing, focus, keyboard delivery —
 * and routes events to per-pid queues.  The shadow makes
 * `wm_poll_event` deliver real input.
 *
 * `title` is purely cosmetic in chapter 108d (decoration paint
 * is wsd-owned); pass "" or the app's name for use in the
 * taskbar title display. */
int  wm_create_window_input(uint32_t w, uint32_t h, uint32_t flags,
                            const char *title,
                            struct wm_window *out);

/* Chapter 108d — create a window at an explicit (x, y) on the
 * scanout, without perturbing the cascade counter for
 * subsequent cascade-positioned clients.  Used by the
 * desktop wallpaper (0, 0) and the taskbar (0, scanout_h -
 * bar_h).  `title` non-NULL means "also open the kernel
 * input shadow" (same semantics as wm_create_window_input);
 * pass NULL to skip input routing. */
int  wm_create_window_at(uint32_t w, uint32_t h, uint32_t flags,
                         uint32_t x, uint32_t y,
                         const char *title,
                         struct wm_window *out);

/* Pop one queued event for any window owned by this process.
 * Returns 1 if an event was written to *out, 0 if no events
 * pending, -1 on error.  Thin wrapper over the kernel
 * gui_poll_event syscall — the calling app must have created
 * its windows via `wm_create_window_input` for events to be
 * routed.  If the app used plain `wm_create_window`, this
 * function always returns 0 (no shadow → no input). */
int  wm_poll_event(struct gui_event *out);

/* Set the wsd-visible title for `win`.  Used by
 * the taskbar to label cells.  `title` may be NULL (clears
 * the title) or a NUL-terminated string of up to
 * WM_TITLE_MAX-1 bytes (anything longer is truncated).
 * Returns 0 on success, -1 on wsd error. */
int  wm_set_title(struct wm_window *win, const char *title);

/* Enumerate every wsd window across all clients.
 * Writes up to `max` descriptors into `out` and returns the
 * count actually written, or -1 on error.  The taskbar uses
 * this to build its cell list; ordinary apps generally
 * don't need it (each owns only its own windows). */
int  wm_list_windows(struct wm_win_desc *out, int max);

/* Push a damaged rect to wsd.  Coords are window-local --
 * wsd translates them to scanout coords using the window's
 * current position (set by wsd via WM_WIN_MOVE).  Out-of-bounds rects are
 * silently clipped by wsd; callers can pass `0, 0, fb.w,
 * fb.h` for a whole-window damage without bothering to
 * clip. */
int  wm_window_dirty(struct wm_window *win,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Reposition the window on the scanout.  Updates the local
 * record so subsequent damage calls land at the new position.
 * Does NOT auto-damage (the caller decides when the visible
 * pixels should change). */
int  wm_window_move(struct wm_window *win, uint32_t x, uint32_t y);

/* chapter 108e — restore a minimized window by id.  Used
 * by the taskbar (which sees windows via WM_LIST but does
 * NOT own them) to bring a window back from the minimized
 * state when the user clicks its cell.  No-op on a window
 * that isn't minimized. */
int  wm_window_restore_id(uint32_t win_id);

/* Symmetric to wm_window_restore_id: hide a window by id.
 * Used by long-lived GUI daemons (the launcher) to behave
 * like a Start menu — close button / ESC / post-launch all
 * collapse to "hide myself so my taskbar cell can summon
 * me again later".  No-op on an already-hidden window. */
int  wm_window_minimize_id(uint32_t win_id);

/* chapter 108e — refresh `win->fb` after a wsd-side resize.
 * Mandatory on the GUI_EVENT_RESIZE that arrives when the
 * user drags the bottom-right grip: at that point the
 * kernel has already torn down our old win_fb mapping (the
 * old VA now translation-faults), and wsd is waiting for
 * us to re-call SYS_WIN_FB_MAP so we can read the
 * post-resize geometry and install fresh pages into our AS.
 * Updates win->fb.pixels / fb.stride / fb.w / fb.h in place
 * and re-syncs win->fb_id.  Returns 0 on success, -1 if
 * either WM_WIN_MAP_FB or SYS_WIN_FB_MAP failed (in which
 * case win->fb.pixels is set to NULL so accidental drawing
 * during the same tick is a clean NULL-deref rather than a
 * silent translation-fault). */
int  wm_window_remap_fb(struct wm_window *win);

/* Free everything for this window.  Idempotent: a second
 * call is a no-op.  Zeros the struct so accidental reuse
 * dies cleanly. */
int  wm_destroy_window(struct wm_window *win);

#endif /* LIBGUI_WMCLIENT_H */
