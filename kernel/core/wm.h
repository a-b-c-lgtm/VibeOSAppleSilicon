/*
 * kernel/core/wm.h — minimal in-kernel window manager (milestone 40).
 *
 * The WM owns the framebuffer (via fb.h) once at least one window
 * exists.  Each window is a kernel-allocated BGRA pixel buffer
 * paired with title/position/z-order metadata.  Compositing is a
 * single-buffer painter's algorithm: clear the screen, walk the
 * window list in z-order (low to high), blit each window plus its
 * decorations to the framebuffer, then push the dirty rectangle
 * out via virtio-gpu.
 *
 * Input flow
 * ----------
 * `console_in` polls the virtio-input keyboard ring as before.  If
 * the WM has at least one window, every byte coming back from the
 * keyboard is steered into the focused window's per-process GUI
 * event queue (`wm_keyboard_byte`) and is NOT delivered to the
 * serial-stdin path.  This means `/bin/hellogui` running in the
 * Cocoa window gets keystrokes; the shell on serial is reached by
 * typing into the QEMU terminal that opens stdio.
 *
 * Userspace contract
 * ------------------
 * - SYS_GUI_CREATE_WINDOW(w, h, title)   -> id  (>=0) or negative err
 * - SYS_GUI_DESTROY_WINDOW(id)           -> 0 / negative
 * - SYS_GUI_PRESENT(id, rect, src)       -> 0   (copies BGRA from user)
 * - SYS_GUI_FILL_RECT(id, rect, color)   -> 0
 * - SYS_GUI_DRAW_TEXT(id, x, y, s, fg, bg, transparent) -> 0
 * - SYS_GUI_FLUSH(id)                    -> 0   (composite + present)
 * - SYS_GUI_POLL_EVENT(struct gui_event *out) -> 1 if event, 0 if empty
 *
 * Events delivered to the focused window:
 *   GUI_EVENT_KEY       (arg0 = ASCII byte; or 0 for ANSI escape)
 *   GUI_EVENT_CLOSE     (currently unused; reserved for ALT+F4)
 *
 * Mouse events are reserved for milestone 41 (virtio-tablet).
 */

#ifndef KERNEL_CORE_WM_H
#define KERNEL_CORE_WM_H

#include <stdint.h>

#define WM_MAX_WINDOWS         16
#define WM_TITLE_MAX           63
#define WM_EVENT_QUEUE_CAP     64

/* Minimum / maximum window content dimensions (excluding decorations). */
#define WM_MIN_WIDTH           80
#define WM_MIN_HEIGHT          40
#define WM_MAX_WIDTH           2048
#define WM_MAX_HEIGHT          2048

/* Per-window decorations.  Title bar height is fixed; the optional
 * resize grip is a small square in the bottom-right corner of
 * decorated windows that have GUI_WIN_FLAG_RESIZABLE. */
#define WM_TITLE_HEIGHT        24
#define WM_BORDER              1
/* Side length (px) of the resize grip square in the bottom-right
 * corner of decorated, RESIZABLE windows.  Sized to be comfortably
 * clickable but not visually dominant against the 24-px title. */
#define WM_GRIP_SIZE           14

/* GUI event types delivered to userspace.  Numbers chosen to leave
 * 0 as a "no event" sentinel. */
#define GUI_EVENT_NONE         0
#define GUI_EVENT_KEY          1
#define GUI_EVENT_CLOSE        2
#define GUI_EVENT_MOUSE_MOVE   3   /* arg0=x, arg1=y (window-relative)    */
#define GUI_EVENT_MOUSE_DOWN   4   /* arg0=x, arg1=y, arg2=button bitmap  */
#define GUI_EVENT_MOUSE_UP     5   /* arg0=x, arg1=y, arg2=button bitmap  */
#define GUI_EVENT_RESIZE       6   /* arg0=new content w, arg1=new content h
                                    *
                                    * Delivered to a RESIZABLE window after
                                    * the WM has already replaced its pixel
                                    * buffer with the new dimensions and
                                    * cleared any new area to a default
                                    * gray.  Apps that re-derive their
                                    * layout from window size should rebuild
                                    * and re-paint on this event.  Apps
                                    * that ignore it just see their old
                                    * content top-left-anchored in the new
                                    * buffer with gray padding around it.  */

/* Mouse-button bitmap shared with virtio_tablet.c. */
#define GUI_BTN_LEFT           0x1u
#define GUI_BTN_RIGHT          0x2u
#define GUI_BTN_MIDDLE         0x4u

/* Extended GUI_EVENT_KEY codes for non-ASCII keys.  Apps that don't
 * care about cursor / navigation keys can keep masking arg0 with
 * 0xFF and they will simply see no event for these — none of the
 * extended codes alias the printable-ASCII range.  Apps that DO
 * care should compare arg0 to these symbolic constants directly.
 *
 * Reserved range: 0x100 .. 0x1FF.  Below 0x100 is reserved for
 * raw bytes (so 0..127 = ASCII, 128..255 = future high-byte). */
#define GUI_KEY_UP             0x101u
#define GUI_KEY_DOWN           0x102u
#define GUI_KEY_RIGHT          0x103u
#define GUI_KEY_LEFT           0x104u
#define GUI_KEY_HOME           0x105u
#define GUI_KEY_END            0x106u
#define GUI_KEY_PGUP           0x107u
#define GUI_KEY_PGDN           0x108u

/* Window flags (milestone 47).  Pass these to gui_create_window_ex.
 *
 *   NO_DECORATION   — no title bar, no border, no close button.  The
 *                     window's content area starts at (w->x, w->y)
 *                     and occupies exactly w->w * w->h pixels.
 *                     Drag/close gestures are disabled; every click
 *                     inside the rect is forwarded to the app as
 *                     MOUSE_DOWN/UP with content-relative coords.
 *   ALWAYS_ON_TOP   — the painter draws these AFTER all other
 *                     windows regardless of z, so a regular window
 *                     can never be raised above them.  Used for
 *                     taskbars and notification popups.
 *   PIN_TO_BOTTOM   — the painter draws these BEFORE all other
 *                     windows regardless of z, AND clicks fall
 *                     through them as if they didn't exist.  The
 *                     desktop wallpaper uses this so a real
 *                     userspace process can own a screen-sized
 *                     window without ever stealing focus or
 *                     getting raised above app windows on click.
 */
#define GUI_WIN_FLAG_NO_DECORATION   0x1u
#define GUI_WIN_FLAG_ALWAYS_ON_TOP   0x2u
#define GUI_WIN_FLAG_PIN_TO_BOTTOM   0x4u
/* Milestone 63: opt-in user-resizable window.  Decorated windows
 * with this flag set get a small resize grip in their bottom-right
 * corner.  Dragging the grip reallocates the window's pixel buffer
 * to the new size, copies the existing content to the top-left of
 * the new buffer (clipped to the smaller dimension), fills any
 * newly-revealed area with the default gray, and pushes a
 * GUI_EVENT_RESIZE event.  No effect on NO_DECORATION windows
 * (their owners drive their own size).  Apps that don't set this
 * flag remain at their create-time dimensions. */
#define GUI_WIN_FLAG_RESIZABLE       0x10u
/* Milestone 51 — reported by wm_list_windows when a window is
 * currently hidden via SYS_GUI_SET_MINIMIZED.  This bit is
 * read-only on the gui_create_window_ex path (rejected) and
 * exists only as a status flag in gui_window_info.flags. */
#define GUI_WIN_FLAG_MINIMIZED       0x8u

/* Sentinel for "use the WM's default cascade position" in the x/y
 * args of gui_create_window_ex.  Any negative value works. */
#define GUI_WIN_POS_AUTO             (-1)

/* Public event shape that crosses the user/kernel boundary.
 * MUST stay layout-compatible with userspace/libc/syscall.h. */
struct gui_event {
    uint32_t type;          /* GUI_EVENT_* */
    int32_t  window_id;
    uint32_t arg0;          /* KEY: ASCII byte (0 if non-printable) */
    uint32_t arg1;          /* reserved */
    uint32_t arg2;
    uint32_t arg3;
};

/* Tight rectangle.  Coordinates are window-content-area relative
 * (0,0 == just below the title bar, just inside the border). */
struct gui_rect {
    uint32_t x, y, w, h;
};

/* Snapshot of one WM window (milestone 47).  Returned by
 * gui_list_windows.  Layout is shared with userspace/libc/syscall.h. */
struct gui_window_info {
    int32_t  id;
    uint32_t flags;        /* GUI_WIN_FLAG_* */
    int32_t  x, y;         /* origin of content area in fb pixels */
    uint32_t w, h;
    uint32_t z;
    int32_t  focused;      /* 1 if this window currently has focus */
    uint64_t owner_pid;
    char     title[64];    /* NUL-terminated, truncated to fit */
};

/* Initialise WM bookkeeping.  Safe to call before fb_init; the
 * first call to wm_create_window will paint the wallpaper if the
 * framebuffer is ready. */
void wm_init(void);

/* 1 if at least one window exists.  Used by console_in to decide
 * whether to steer keystrokes to the GUI or to stdin. */
int wm_has_windows(void);

/* Push one ASCII byte from the keyboard into the focused window's
 * GUI event queue.  Returns 1 if the byte was consumed by the WM
 * (and therefore should NOT be delivered to stdin), 0 otherwise.
 * Non-blocking, IRQ-safe.
 *
 * The WM runs a small ANSI-CSI parser on the byte stream so that
 * an arrow-key escape sequence (ESC [ A/B/C/D) emerges as a SINGLE
 * GUI_EVENT_KEY whose arg0 is one of the GUI_KEY_* constants — not
 * three separate events the first of which looks like a bare ESC
 * keypress.  See wm_flush_pending_keys for the boundary rule. */
int wm_keyboard_byte(char c);

/* Drain any partial CSI sequence held by the WM's keyboard parser
 * as a bare ESC keypress.  Call this at the end of every batch of
 * wm_keyboard_byte() calls (e.g. after pumping virtio-input dry).
 * For real arrow-key sequences this is a no-op — they always
 * arrive as a contiguous 3-byte burst and the parser completes
 * inside the same drain loop.  For a bare ESC press, the parser
 * cannot tell "user pressed ESC" from "first byte of an arrow
 * sequence" without seeing what (if anything) comes next, so we
 * defer the decision until the batch ends. */
void wm_flush_pending_keys(void);

/* Pointer input from virtio_tablet (or any future pointing device).
 * Coordinates are in framebuffer pixels.  `wm_pointer_button`'s
 * `button` is one of GUI_BTN_*; `down` is non-zero for press. */
void wm_pointer_move(int32_t sx, int32_t sy);
void wm_pointer_button(uint32_t button, int down);

/* Syscalls — all return 0 on success or negative errno. */
long wm_create_window(uint64_t pid, uint32_t w, uint32_t h,
                      const char *title_user);
/* Milestone 47: extended create.  flags is a bitmask of
 * GUI_WIN_FLAG_*.  x,y == GUI_WIN_POS_AUTO means cascade. */
long wm_create_window_ex(uint64_t pid, uint32_t w, uint32_t h,
                         const char *title_user,
                         uint32_t flags, int32_t x, int32_t y);
long wm_destroy_window(uint64_t pid, int32_t id);
long wm_present(uint64_t pid, int32_t id,
                uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                const uint8_t *src_user);
long wm_fill_rect(uint64_t pid, int32_t id,
                  uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h,
                  uint32_t bgra);
long wm_draw_text(uint64_t pid, int32_t id,
                  uint32_t x, uint32_t y,
                  const char *s_user,
                  uint32_t fg_bgra, uint32_t bg_bgra,
                  int transparent);

/* Chapter 102 -- return the pixel width that wm_draw_text would
 * paint for `s_user` using the kernel's default font. Honours
 * per-glyph advance widths so callers can position carets and
 * centre labels accurately even with the proportional TTF font.
 * Stops at '\n'. Returns the width in pixels (>= 0), or -EFAULT
 * if the string pointer isn't readable. */
long wm_measure_text(const char *s_user);
long wm_flush(uint64_t pid, int32_t id);
long wm_poll_event(uint64_t pid, struct gui_event *out_user);
/* Milestone 47: enumerate windows.  Copies up to `max` snapshots
 * to `out_user` and returns the number copied (>=0).  Caller-supplied
 * buffer must be at least max*sizeof(struct gui_window_info) bytes. */
long wm_list_windows(uint64_t pid, struct gui_window_info *out_user,
                     int32_t max);
/* Milestone 47: programmatically raise + focus a window by id.
 * Used by the taskbar.  Returns 0 / -errno. */
long wm_raise_window(uint64_t pid, int32_t id);

/* Milestone 51: hide / show a window without destroying it.
 *   on != 0 — mark window minimized: compositor skips it,
 *            hit-test ignores it, focus drops to topmost
 *            non-minimized window if needed.  No-op if already
 *            minimized.
 *   on == 0 — restore: clear the bit, raise to top z, take
 *            focus.  No-op if not minimized.
 * Pin-to-bottom windows (the wallpaper) cannot be minimized. */
long wm_set_minimized(uint64_t pid, int32_t id, int on);

/* Drop every window owned by `pid`.  Called from sys_exit so a
 * process that crashes does not leak its windows. */
void wm_destroy_owner(uint64_t pid);

/* Chapter 108a \u2014 userspace access to window pixel buffers.
 *
 * `wm_map_window` allocates one page-aligned 4 KiB frame per page
 * of the window's pixel storage, copies the current contents of
 * `w->pixels` into them, and installs the run RW + EL0 + tagged
 * DESC_SW_WM_WINDOW into the calling thread's address space.  The
 * resulting user VA appears in *va_out; *stride_out receives the
 * row stride in bytes (always `w * 4` for now \u2014 no padding); the
 * window dimensions go into *w_out / *h_out.  The window remembers
 * the mapping so destroy / owner-exit can tear it down.
 *
 * Only one mapping per window per owner.  Re-calling on an
 * already-mapped window returns the SAME VA without re-allocating.
 * Mappings are NOT inherited across fork / clone (the AS code
 * skips DESC_SW_WM_WINDOW pages, so the child sees nothing at the
 * old VA).
 *
 * Errors:
 *   -EPERM   not the window owner.
 *   -ENOTSUP window is RESIZABLE (chapter 108a defers resize
 *            coherence; this lifts once 108b lands).
 *   -ENOMEM  pmem exhausted or AS layer refused.
 *   -EFAULT  one of the user output pointers is not writable. */
long wm_map_window(uint64_t pid, int32_t id,
                   uint64_t *va_out, uint32_t *stride_out,
                   uint32_t *w_out, uint32_t *h_out);

/* Chapter 108a \u2014 inverse of wm_map_window.  Drops the user-AS
 * descriptors and frees the backing 4 KiB frames.  Idempotent on
 * an already-unmapped window (returns 0).  Implicit on
 * wm_destroy_window / wm_destroy_owner so callers needn't bother
 * on a clean exit, but explicit unmap is needed if userspace wants
 * to release the bump-pointer VA range without exiting. */
long wm_unmap_window(uint64_t pid, int32_t id);

/* Chapter 108a \u2014 declare a sub-rectangle of a mapped window
 * dirty.  The WM copies that rect from the user-visible pages
 * into the compositor's authoritative `w->pixels` buffer and
 * triggers a recompose.  Rect coordinates are window-content
 * relative (same convention as wm_fill_rect / wm_present).  A
 * rect that exceeds the window is clipped.  Returns 0 on
 * success; -EPERM if not the owner; -ENOENT if the window has
 * no mapping installed yet; -EINVAL if rw==0 or rh==0. */
long wm_damage(uint64_t pid, int32_t id,
               uint32_t x, uint32_t y, uint32_t rw, uint32_t rh);

/* chapter 108e -- expose the current pointer state to userspace.
 * Used by wsd to paint the cursor sprite and run hit-tests for
 * title-bar drags / close-button clicks in userspace, X-server
 * style.  Writes scanout coords + the GUI_BTN_* bitmap into the
 * three user pointers (any may be NULL).  Returns 0 on success,
 * -EFAULT on a bad user pointer.  Idempotent and side-effect
 * free; intended for the wsd input poller to call at ~60 Hz. */
long wm_pointer_state(int32_t *out_x_user, int32_t *out_y_user,
                      uint32_t *out_btn_user);

/* chapter 108e -- relocate a window on the scanout without going
 * through the usual title-bar-drag path.  Any caller may move
 * any window (wsd is the only legitimate caller today, since it
 * owns decoration hit-testing; restricting to a "compositor"
 * pid is a future tightening).  The window's body-rect anchor
 * (x, y) is set verbatim; no clipping.  Returns 0 on success,
 * -EINVAL if the id is out of range, -ENOENT if the slot is
 * free.  Does not push events to the window's app. */
long wm_move_window(int32_t id, int32_t x, int32_t y);

/* chapter 108e -- inject a synthesised gui_event into the per-
 * window event ring so the app's next wm_poll_event returns it.
 * Used by wsd to deliver GUI_EVENT_CLOSE (from close-button
 * clicks) and future synthesised pointer events.  The `ev.window_id`
 * field is overwritten by the kernel with `id` for consistency,
 * but the rest of the event is copied verbatim.  Returns 0 on
 * success, -EINVAL if id is out of range, -ENOENT if the slot
 * is free, -EFAULT on a bad user pointer, -ENOSPC if the
 * window's event ring is full (caller can retry). */
long wm_deliver_event(int32_t id, const struct gui_event *ev_user);

/* chapter 108e -- toggle "wsd-routed" mode on a kernel WM shadow.
 *
 * When on != 0, the kernel's pointer router skips this window
 * entirely (hit-test pretends it's not there; the focused-window
 * MOVE / non-left forwarding bails before pushing).  wsd then has
 * exclusive control over which window receives mouse events for
 * the pixels this shadow occupies, and routes them via
 * wm_deliver_event in wsd's own z-order.  Keyboard input still
 * flows via the kernel's g_focus_id, which wsd keeps in sync by
 * calling wm_raise_window after each click-to-raise.
 *
 * When on == 0, the kernel resumes auto-routing pointer events
 * to this window (the chapter-30 default).
 *
 * Idempotent.  Returns 0 on success, -EINVAL for an invalid id,
 * -ENOENT if the slot is free. */
long wm_set_input_passthrough(int32_t id, int on);

#endif /* KERNEL_CORE_WM_H */
