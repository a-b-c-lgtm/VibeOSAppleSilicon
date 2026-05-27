/*
 * userspace/paint/paint.c — mouse demo.
 *
 * Opens a 600x400 window with a white canvas.  Wherever the user
 * left-clicks (or drags with the left button held), a 12x12 colour
 * square is painted into the canvas using gui_fill_rect.  Right-
 * click cycles through eight palette colours.  Esc, GUI_EVENT_CLOSE
 * (close button), or any other unhandled key quits.
 *
 * Demonstrates the four new GUI events:
 *   GUI_EVENT_MOUSE_MOVE   (with button bitmap in arg2)
 *   GUI_EVENT_MOUSE_DOWN
 *   GUI_EVENT_MOUSE_UP
 *   GUI_EVENT_CLOSE
 *
 * Chapter 116: every brush stamp is now a direct write into
 * the window's mapped framebuffer.  The hot path during a drag
 * used to be one gui_fill_rect per stamp + one gui_flush per
 * pointer move — two syscalls per pixel of trail.  Now it's a
 * single gui_window_dirty over the brush rect, with the body
 * of the stamp written by an in-process loop.  Latency on a
 * fast drag dropped from a faintly visible chunkiness to
 * smooth, partly because the syscall cost is gone and partly
 * because we no longer give the compositor a chance to render
 * a partial trail (one damage per stamp = one composite).
 *
 * Chapter 117: ported off the kernel-WM
 * (gui_create_window / gui_window_fb / gui_window_dirty /
 * gui_poll_event / gui_destroy_window) onto the wsd-backed
 * wmclient.  The kernel WM no longer composes pixels; wsd
 * does.  Each drag stamp still hits exactly one damage call
 * (now `wm_window_dirty`), and wsd composites only the damaged
 * rect.
 */
#include "../libc/syscall.h"
#include "../libgui/draw.h"
#include "../libgui/wmclient.h"

#define WIDTH   600
#define HEIGHT  400
#define BRUSH    12

static const uint32_t PALETTE[] = {
    GUI_BGRA(0xC0, 0x30, 0x30),     /* red    */
    GUI_BGRA(0x30, 0xC0, 0x30),     /* green  */
    GUI_BGRA(0x30, 0x60, 0xC0),     /* blue   */
    GUI_BGRA(0xC0, 0xC0, 0x30),     /* yellow */
    GUI_BGRA(0xC0, 0x30, 0xC0),     /* magenta*/
    GUI_BGRA(0x30, 0xC0, 0xC0),     /* cyan   */
    GUI_BGRA(0x10, 0x10, 0x10),     /* near-black */
    GUI_BGRA(0xFF, 0xFF, 0xFF),     /* white  (eraser) */
};
#define PALETTE_LEN (sizeof(PALETTE) / sizeof(PALETTE[0]))

/* Chapter 116 removed the canvas shadow buffer — the mapped
 * framebuffer IS the canvas, no copy needed.  clear_canvas
 * just memsets the mapped pages. */

static void clear_canvas(struct gui_fb *fb)
{
    draw_fill_rect(fb, 0, 0, fb->w, fb->h,
                   GUI_BGRA(0xF8, 0xF8, 0xF8));
}

static void stamp_at(struct gui_fb *fb, int cx, int cy, uint32_t bgra)
{
    /* Place a single BRUSH x BRUSH square centred on (cx, cy)
     * without damaging.  Caller is responsible for the damage
     * call (covers the union of all stamps in one event). */
    int x0 = cx - BRUSH/2;
    int y0 = cy - BRUSH/2;
    draw_fill_rect(fb, x0, y0, BRUSH, BRUSH, bgra);
}

static void stamp(struct wm_window *win, int cx, int cy, uint32_t bgra)
{
    int x0 = cx - BRUSH/2;
    int y0 = cy - BRUSH/2;
    draw_fill_rect(&win->fb, x0, y0, BRUSH, BRUSH, bgra);
    /* Damage just the stamp's bounding rect.  draw_fill_rect
     * already clipped; wsd clips again for safety. */
    int dx = x0 < 0 ? 0 : x0;
    int dy = y0 < 0 ? 0 : y0;
    wm_window_dirty(win, (uint32_t)dx, (uint32_t)dy, BRUSH, BRUSH);
}

/* Draw a continuous brush trail from (x0,y0) to (x1,y1).
 *
 * Why this matters: wsd coalesces consecutive MOUSE_MOVE events
 * in its per-window ring, which means under fast motion paint can
 * see one move per poll cycle covering many pixels.  Without
 * interpolation that becomes a dotted line.  We step every
 * BRUSH/3 pixels along the longest axis (overlapping by ~8 px)
 * and emit ONE damage at the end covering the whole trail. */
static void stamp_line(struct wm_window *win, int x0, int y0, int x1, int y1,
                       uint32_t bgra)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int span = adx > ady ? adx : ady;
    if (span == 0) {
        stamp(win, x1, y1, bgra);
        return;
    }
    int step = BRUSH / 3;       /* 4-pixel step → 8-pixel overlap */
    if (step < 1) step = 1;
    for (int s = 0; s <= span; s += step) {
        int xx = x0 + dx * s / span;
        int yy = y0 + dy * s / span;
        stamp_at(&win->fb, xx, yy, bgra);
    }
    /* Always include the endpoint exactly. */
    stamp_at(&win->fb, x1, y1, bgra);
    /* One damage call covering the trail's bounding box. */
    int lx = x0 < x1 ? x0 : x1;
    int ly = y0 < y1 ? y0 : y1;
    int rx = x0 > x1 ? x0 : x1;
    int ry = y0 > y1 ? y0 : y1;
    int bx = lx - BRUSH/2; if (bx < 0) bx = 0;
    int by = ly - BRUSH/2; if (by < 0) by = 0;
    int bw = (rx - lx) + BRUSH;
    int bh = (ry - ly) + BRUSH;
    wm_window_dirty(win, (uint32_t)bx, (uint32_t)by,
                    (uint32_t)bw, (uint32_t)bh);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct wm_window win;
    if (wm_create_window_input(WIDTH, HEIGHT, 0, "paint", &win) < 0) {
        write(1, "[paint] wm_create_window_input failed\n", 39);
        return 1;
    }
    struct gui_fb *fb = &win.fb;
    clear_canvas(fb);

    /* On-screen prompt drawn into the canvas. */
    draw_text(fb, 16, 16,
              "left-click to paint, right-click to cycle colour, ESC to quit",
              GUI_BGRA(0x30, 0x30, 0x30), 0, 1);
    wm_window_dirty(&win, 0, 0, fb->w, fb->h);

    uint32_t colour = PALETTE[0];
    unsigned palette_idx = 0;
    int dragging = 0;
    int last_x = 0, last_y = 0;     /* most recent stamp during drag */

    /* chapter 118 follow-up -- draw the initial colour swatch
     * in the top-right corner so the user can see the current
     * brush colour from the moment the app opens.  Without
     * this, the swatch only appeared after the first right-
     * click, leaving new users wondering which colour was
     * active.  Uses the same rect/coords as the right-click
     * handler below so the two paths stay visually identical. */
    draw_fill_rect(fb, WIDTH - 24, 8, 16, 16, colour);
    /* The window-wide dirty above already covers this rect; no
     * additional damage call needed. */

    for (;;) {
        struct gui_event ev;
        if (!wm_poll_event(&ev)) {
            yield();
            continue;
        }
        switch (ev.type) {
        case GUI_EVENT_KEY:
            if ((char)(ev.arg0 & 0xFF) == 0x1B) goto done;     /* ESC */
            break;
        case GUI_EVENT_CLOSE:
            goto done;
        case GUI_EVENT_MOUSE_DOWN:
            if (ev.arg2 == GUI_BTN_LEFT) {
                dragging = 1;
                last_x = (int)ev.arg0;
                last_y = (int)ev.arg1;
                stamp(&win, last_x, last_y, colour);
            } else if (ev.arg2 == GUI_BTN_RIGHT) {
                palette_idx = (palette_idx + 1) % PALETTE_LEN;
                colour = PALETTE[palette_idx];
                /* Tiny corner swatch shows the active colour. */
                draw_fill_rect(fb, WIDTH - 24, 8, 16, 16, colour);
                wm_window_dirty(&win, WIDTH - 24, 8, 16, 16);
            }
            break;
        case GUI_EVENT_MOUSE_UP:
            if (ev.arg2 == GUI_BTN_LEFT) dragging = 0;
            break;
        case GUI_EVENT_MOUSE_MOVE:
            if (dragging && (ev.arg2 & GUI_BTN_LEFT)) {
                int nx = (int)ev.arg0;
                int ny = (int)ev.arg1;
                /* Draw a continuous trail from the last stamp to
                 * the new position.  Required because wsd
                 * coalesces consecutive MOUSE_MOVE events under
                 * fast drags, so successive deliveries can cover
                 * many pixels in one step. */
                stamp_line(&win, last_x, last_y, nx, ny, colour);
                last_x = nx;
                last_y = ny;
            }
            break;
        default:
            break;
        }
    }

done:
    wm_destroy_window(&win);
    return 0;
}
