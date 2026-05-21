/*
 * userspace/libc/wm_proto.h — chapter 108d wire
 * protocol for /srv/wm, the userspace window-server bus.
 *
 * Shared between three callers (eventual end state):
 *
 *   - userspace/wsd/wsd.c       — service side; binds
 *                                  /srv/wm and answers
 *                                  requests.
 *   - userspace/libgui/...      — client lib used by every
 *                                  GUI app once the kernel
 *                                  SYS_GUI_* path is gone
 *                                  (Phase F).
 *   - userspace/wmtest/wmtest.c — chapter 108d smoke client.
 *
 * Wire shape: one chapter-107 IPC datagram = one request OR
 * one reply.  Fixed-size header per message; future variable-
 * length payloads (window-descriptor arrays for WM_LIST,
 * title strings for WM_WIN_TITLE) ride immediately after the
 * header inside the same datagram.
 *
 * The protocol is request/reply, ONE outstanding per conn.
 * Clients are expected to serialise their own use of a conn.
 * Multi-conn fan-out (each GUI app holds its own conn) is the
 * intended scaling path, mirroring fontd in chapter 108b.
 *
 * Chapter 108d scope: only WM_HELLO and WM_LIST are
 * implemented in the first slice.  WM_LIST always returns zero windows because
 * the kernel WM still owns the window list; wsd is just
 * proving the bus end-to-end.  Subsequent chapter 108d slices add ops by
 * picking the next stable wire number from the enum below —
 * never reusing an old one.
 */

#ifndef LIBC_WM_PROTO_H
#define LIBC_WM_PROTO_H

#include <stdint.h>

/* Bound by /bin/wsd.  Stable name. */
#define WM_SOCK_PATH        "/srv/wm"

/* Bumped on incompatible wire changes only.  Version 1 =
 * initial WM_HELLO + WM_LIST.  Version 2 = wm_win_desc grew
 * x/y; WM_WIN_CREATE reply now returns auto-assigned position
 * in b/c.  Version 3 = wm_win_desc grew a 64-byte title;
 * WM_WIN_TITLE now sets it.  Adding new ops without changing
 * existing layouts does NOT bump this; clients can call the
 * new ops if they want and fall back if WM_ERR_PROTO comes
 * back. */
#define WM_PROTO_VERSION    3u

/* Op codes.  Stable wire numbers — adding a new op gets a
 * new number, never reuses an old one.  Chapter 108d
 * implemented ops 1-13 and WM_WIN_CREATE_AT (14);
 * chapter 108e added WM_WIN_BIND_KERNEL (15) and
 * WM_WIN_RESTORE / WM_WIN_MINIMIZE (16-17). */
enum wm_op {
    WM_HELLO       = 1,  /* req: a=client_version; rep: a=session_id, b=wsd_version */
    WM_LIST        = 2,  /* req: -;                rep: a=n_windows, payload: n_windows*wm_win_desc */
    WM_WIN_CREATE  = 3,  /* req: a=w, b=h, c=flags; rep: a=window_id, b=auto_x, c=auto_y */
    WM_WIN_DESTROY = 4,  /* req: a=window_id;       rep: -           */
    WM_WIN_MAP_FB  = 5,  /* req: a=window_id;       rep: a=fb_id, b=w, c=h, d=stride */
    WM_WIN_DAMAGE  = 6,  /* req: a=window_id, b=src_x, c=src_y, d=(w<<16)|h; rep: status only.  Chapter 108d: src_x/src_y are WINDOW-LOCAL; wsd translates to scanout via the window's position. */
    WM_WIN_MOVE    = 7,  /* req: a=window_id, b=x, c=y; rep: status only.  Reposition window on the scanout. */

    /* Reserved (not implemented yet; numbering locked in
     * early so future phases just fill them in): */
    WM_WIN_RESIZE  = 8,
    WM_WIN_RAISE   = 9,
    WM_WIN_LOWER   = 10,
    WM_WIN_FOCUS   = 11,
    WM_WIN_TITLE   = 12, /* req: a=window_id, b=title_len; payload: title bytes (<=WM_TITLE_MAX) -- reserved, not impl yet */
    WM_EVENT_PULL  = 13,

    /* Chapter 108d — explicit-position create.
     * Used by apps that own a specific scanout slot (desktop
     * wallpaper at 0,0; taskbar at 0,H-bar; etc) and don't
     * want to perturb the cascade for subsequent cascade-
     * positioned clients.  Same reply shape as WM_WIN_CREATE.
     * If the client just wants cascade, use WM_WIN_CREATE; the
     * two ops exist side-by-side so legacy hellowsd / wmtest
     * binaries don't need to change. */
    WM_WIN_CREATE_AT = 14, /* req: a=w, b=h, c=flags, d=(x<<16)|y; rep: a=window_id, b=x, c=y */

    /* chapter 108e -- userspace decorations.  Once a client has
     * opened a kernel-WM "input shadow" window for event
     * routing (the wmclient.c GUI_WIN_FLAG_NO_DECORATION shadow
     * pattern), it tells wsd the shadow's kernel id with
     * WM_WIN_BIND_KERNEL so wsd can:
     *   - reposition the shadow via SYS_GUI_MOVE_WINDOW when
     *     the user title-bar-drags the wsd window (keeps body
     *     click hit-testing aligned).
     *   - inject GUI_EVENT_CLOSE into the shadow's event ring
     *     via SYS_GUI_DELIVER_EVENT when the user clicks the
     *     close button (the app's wm_poll_event loop sees it
     *     and exits cleanly).
     * Binding is once per window; re-binding overwrites. */
    WM_WIN_BIND_KERNEL = 15, /* req: a=wsd_window_id, b=kernel_window_id; rep: status */

    /* chapter 108e -- explicit visibility control.  The
     * minimize button click path is internal to wsd (poller
     * sees the click, calls a static helper) so no protocol
     * op exists for "hide".  But "restore" needs an external
     * caller -- the taskbar -- so it gets a protocol op.
     * Sets hidden=0, raises the window to the top of wsd's
     * z-order, calls gui_set_minimized(kernel_id, 0), and
     * full-recomposes.  No-op if the window is already
     * visible.  Idempotent.  Used by taskbar's click-to-
     * restore path (chapter 108e). */
    WM_WIN_RESTORE     = 16, /* req: a=wsd_window_id; rep: status */

    /* Symmetric to WM_WIN_RESTORE: hide a window from
     * compose + hit-test.  Sets hidden=1 and mirrors the
     * state into the kernel shadow via
     * gui_set_minimized(kernel_id, 1).  Used by long-lived
     * GUI daemons (the launcher) that want to behave like a
     * Start menu: close button / ESC / "I just launched
     * something" all collapse to "hide myself and stay
     * available for the next click on my taskbar cell".
     * Idempotent: no-op on a window that's already hidden. */
    WM_WIN_MINIMIZE    = 17, /* req: a=wsd_window_id; rep: status */

    WM_ERR         = 99, /* reply only: status carries -errno */
};

/* Status codes carried in the reply's `status` field.
 * 0 = success; negative = error.  Kept distinct from
 * Unix errno (no conflicts in practice; the negative
 * numbers are protocol-local). */
#define WM_OK               0
#define WM_ERR_PROTO       (-1)   /* malformed request   */
#define WM_ERR_NOSUCHWIN   (-2)   /* unknown window id   */
#define WM_ERR_NOMEM       (-3)   /* OOM in wsd          */
#define WM_ERR_BADVER      (-4)   /* WM_HELLO incompatible version */
#define WM_ERR_NOTIMPL     (-5)   /* op recognised but not handled yet */
#define WM_ERR_NOTOWNER    (-6)   /* op tried to touch a window owned by another conn */
#define WM_ERR_FULL        (-7)   /* WM_WIN_CREATE: window table full */

/* Window flags.  Bitmask.  Chapter 108d defines the slots;
 * actual semantics (decoration, always-on-top) are
 * meaningful when wsd starts composing.
 *
 * Values match GUI_WIN_FLAG_* in userspace/libc/syscall.h so a
 * client can pass a single `flags` word to both wsd
 * (WM_WIN_CREATE / WM_WIN_CREATE_AT) and the kernel
 * (gui_create_window_ex shadow) without translation. */
#define WM_WF_NODECORATION  0x0001u  /* skip title bar / borders   */
#define WM_WF_ALWAYS_ON_TOP 0x0002u  /* dock/overlay-style window  */
#define WM_WF_PIN_BOTTOM    0x0004u  /* pinned to the bottom of    *
                                      * the z-order; cannot be     *
                                      * raised by clicks.  Used    *
                                      * by the desktop wallpaper,  *
                                      * which must always sit      *
                                      * behind every other window. */

/* Sizing limits.  64 windows × 24-byte descriptor = 1.5 KiB
 * fits a single chapter-107 datagram with room to spare;
 * SRV_MSG_MAX is 64 KiB.  If we ever need more windows than
 * this, the per-list payload moves to a paginated op rather
 * than a single LIST. */
#define WM_MAX_WINDOWS      64u
#define WM_TITLE_MAX        96u   /* including NUL */

/* Fixed-size message header.  24 bytes.  Matches the
 * cap-on-stack pattern fontd uses; wsd doesn't need to
 * malloc per request.  All fields little-endian (the only
 * endianness anywhere in osdev).
 *
 * Field meanings depend on op + direction; see the comments
 * on each enum value above.  Unused fields MUST be zeroed by
 * the sender — a future phase that grows a field can then
 * detect "old client doesn't set this" by reading 0.
 *
 * Variable-length payloads (descriptor arrays, strings) come
 * directly after this header in the same datagram and use
 * `len` (one of a/b/c/d, op-defined) to delimit. */
struct wm_msg {
    uint32_t op;        /* WM_OP_*                              */
    int32_t  status;    /* reply only; 0 success or WM_ERR_*    */
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
};

/* One entry in a WM_LIST reply's payload.  Fixed-size so
 * the client can demarshal in a loop without per-entry
 * length prefixes.  Chapter 108d: grew a 64-byte
 * NUL-terminated title field so the taskbar can render
 * labels without a separate per-window RPC.  Older clients
 * reading the descriptor as 28 bytes will simply miss the
 * title (and any future fields beyond their struct size);
 * newer fields must always be APPENDED.
 * WM_PROTO_VERSION bumps to 3 for this layout. */
struct wm_win_desc {
    uint32_t win_id;
    uint32_t owner_session;
    uint32_t w;
    uint32_t h;
    uint32_t flags;
    uint32_t x;          /* chapter 108d: scanout-relative origin x */
    uint32_t y;          /* chapter 108d: scanout-relative origin y */
    char     title[64];  /* chapter 108d: NUL-terminated window label */
};

/* Largest possible WM_LIST reply: header + WM_MAX_WINDOWS
 * descriptors.  Clients allocate this much for the read so
 * a full table fits in one datagram. */
#define WM_LIST_REPLY_MAX \
    (sizeof(struct wm_msg) + (size_t)WM_MAX_WINDOWS * sizeof(struct wm_win_desc))

/* WM_WIN_DAMAGE pack/unpack helpers for the
 * (w<<16)|h convention in `wm_msg.d`.  Width and height are
 * limited to 16 bits (65535 px on each axis), which is well
 * above any plausible window dimension and above the
 * kernel-side WIN_FB_MAX_W/H cap (4096 each). */
#define WM_DAMAGE_PACK_WH(w, h) \
    (((uint32_t)((uint16_t)(w)) << 16) | (uint32_t)((uint16_t)(h)))
#define WM_DAMAGE_W(d)  ((uint32_t)((d) >> 16) & 0xFFFFu)
#define WM_DAMAGE_H(d)  ((uint32_t)(d) & 0xFFFFu)

#endif /* LIBC_WM_PROTO_H */
