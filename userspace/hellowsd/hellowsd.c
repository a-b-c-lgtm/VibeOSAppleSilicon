/*
 * userspace/hellowsd/hellowsd.c — chapter 117
 * /srv/wm exhibit app.
 *
 * Why this exists
 * ---------------
 *
 * Chapter 117 built wsd's compositor step by step: window
 * table, per-window framebuffers, MOVE, and window-local
 * DAMAGE.  But up to here the only thing that had exercised
 * the wire protocol was `wmtest`, a one-shot CLI that
 * connects, sends a scripted sequence, and exits.  The
 * user-memory iron rule
 * (/memories/apps-must-use-features.md) is that every
 * feature shipped in a chapter has a *user-visible app*
 * that demonstrates it, not just a regression test.
 *
 * `hellowsd` is the first such app: it imports the new
 * `libgui/wmclient.h`, creates a window, paints a fixed
 * pattern of distinctive BGRA pixels into the per-window
 * framebuffer, moves the window to a known scanout
 * position, damages a sub-rect of the painted area, and
 * exits cleanly.  Every wsd op the chapter introduces
 * (HELLO, CREATE, MAP_FB, MOVE, DAMAGE, DESTROY) runs
 * exactly once, in order, with no other failure paths to
 * mask a regression.
 *
 * What it deliberately does NOT do
 * --------------------------------
 *
 * No event loop, no keyboard input.  A later chapter introduces
 * WM_EVENT_PULL; until then, an app that wanted keystrokes
 * would have to fall back on the kernel `gui_poll_event`
 * syscall, which would couple the new app to the legacy
 * GUI path and defeat the point of this slice.  hellowsd
 * stays one-shot so the demo is honest about what /srv/wm
 * can do today.
 *
 * No text rendering.  `libgui/draw.h::draw_text` works
 * fine against the gui_fb embedded in wm_window, but
 * pulling in /srv/font would add a second IPC dependency
 * that has nothing to do with the wire-protocol changes in
 * this slice.  A later chapter can add a "hellowsd
 * with text" variant once the bare path is stable.
 *
 * Verification model
 * ------------------
 *
 * scripts/test_hellowsd.py boots the OS, runs hellowsd
 * from the shell, and pins three things in the kernel log:
 *   1. The wmclient session banner.
 *   2. The wsd-side damage log line, which carries both
 *      the window-local source rect AND the translated
 *      scanout dst rect plus a px=0x... readback of the
 *      first dst pixel.
 *   3. The wmclient destroy/disconnect log line, which
 *      confirms wsd's CREATE→DESTROY round-trip ran end-to-
 *      end through the new client library.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libgui/wmclient.h"

/* Pattern dimensions chosen distinct from wmtest's 800x600 /
 * 4x1 so a regression in either test can't accidentally pass
 * the other's assertions.  300x200 is small enough to fit at
 * any cascade position on a 1280x800 scanout. */
#define WIN_W   300u
#define WIN_H   200u

/* Magic BGRA value the test pins against.  Distinct from
 * wmtest's 0xff332211 so a stub handler that returned
 * wmtest's hardcoded value would fail here.
 *
 * Layout: B=0xAA, G=0x55, R=0x77, A=0xFF
 * Packed little-endian as uint32 it reads 0xff7755AA. */
#define MAGIC_B   0xAAu
#define MAGIC_G   0x55u
#define MAGIC_R   0x77u
#define MAGIC_A   0xFFu

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("[hellowsd] starting\n");

    struct wm_window win;
    if (wm_create_window(WIN_W, WIN_H, 0, &win) < 0) {
        printf("[hellowsd] FAIL reason=create\n");
        return 1;
    }

    /* Paint the entire window with the magic colour by
     * walking the mapped framebuffer.  No syscalls in the
     * loop; the kernel doesn't even know we're drawing
     * until the wm_window_dirty call below.  We use a
     * uint32 store per pixel because BGRA bytes line up
     * with little-endian uint32 on aarch64. */
    uint32_t pixel = ((uint32_t)MAGIC_A << 24)
                   | ((uint32_t)MAGIC_R << 16)
                   | ((uint32_t)MAGIC_G <<  8)
                   |  (uint32_t)MAGIC_B;
    for (uint32_t y = 0; y < win.fb.h; y++) {
        uint32_t *row = (uint32_t *)(win.fb.pixels
                                  + (size_t)y * win.fb.stride);
        for (uint32_t x = 0; x < win.fb.w; x++)
            row[x] = pixel;
    }
    printf("[hellowsd] painted %ux%u px=0x%lx\n",
           (unsigned)win.fb.w, (unsigned)win.fb.h,
           (unsigned long)pixel);

    /* Move to a known position so the dst coords in wsd's
     * damage log are pinnable.  (200, 120) is distinct from
     * wmtest's (100, 50) so a regression in either test
     * can't pass the other's assertion. */
    if (wm_window_move(&win, 200, 120) < 0) {
        printf("[hellowsd] FAIL reason=move\n");
        (void)wm_destroy_window(&win);
        return 1;
    }

    /* Damage a 4x1 strip at the window's top-left in
     * window-local coords; wsd translates to scanout
     * coords (200, 120 + title-bar-height) on a decorated
     * window (chapter 118 onwards) -- the title bar is
     * 24 px tall and is painted by wsd above the body, so
     * body row 0 lands at scanout y = 144.  Small rect
     * keeps the wsd compose loop fast and the synchronous
     * readback cheap. */
    if (wm_window_dirty(&win, 0, 0, 4, 1) < 0) {
        printf("[hellowsd] FAIL reason=damage\n");
        (void)wm_destroy_window(&win);
        return 1;
    }
    printf("[hellowsd] damage sent win=%u src=0,0,4,1 dst=200,144,4,1\n",
           (unsigned)win.id);

    if (wm_destroy_window(&win) < 0) {
        printf("[hellowsd] FAIL reason=destroy\n");
        return 1;
    }
    wm_disconnect();

    printf("[hellowsd] PASS\n");
    return 0;
}
