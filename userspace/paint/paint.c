/*
 * userspace/paint/paint.c — milestone-41 mouse demo.
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
 */
#include "../libc/syscall.h"

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

static uint32_t pixels[WIDTH * HEIGHT];

static void clear_canvas(void)
{
    for (int i = 0; i < WIDTH * HEIGHT; i++)
        pixels[i] = GUI_BGRA(0xF8, 0xF8, 0xF8);
}

static void stamp_at(int win, int cx, int cy, uint32_t bgra)
{
    /* Place a single BRUSH x BRUSH square centred on (cx, cy)
     * without flushing.  Caller is responsible for gui_flush(). */
    int x0 = cx - BRUSH/2;
    int y0 = cy - BRUSH/2;
    int x1 = x0 + BRUSH;
    int y1 = y0 + BRUSH;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WIDTH)  x1 = WIDTH;
    if (y1 > HEIGHT) y1 = HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    gui_fill_rect(win, (uint32_t)x0, (uint32_t)y0,
                  (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), bgra);
}

static void stamp(int win, int cx, int cy, uint32_t bgra)
{
    stamp_at(win, cx, cy, bgra);
    gui_flush(win);
}

/* Draw a continuous brush trail from (x0,y0) to (x1,y1).
 *
 * Why this matters: the WM coalesces consecutive MOUSE_MOVE events
 * in its per-window ring, which means under fast motion paint can
 * see one move per poll cycle covering many pixels.  Without
 * interpolation that becomes a dotted line.  We step every
 * BRUSH/3 pixels along the longest axis (overlapping by ~8 px)
 * and emit ONE flush at the end. */
static void stamp_line(int win, int x0, int y0, int x1, int y1,
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
        stamp_at(win, xx, yy, bgra);
    }
    /* Always include the endpoint exactly. */
    stamp_at(win, x1, y1, bgra);
    gui_flush(win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int win = gui_create_window(WIDTH, HEIGHT, "paint");
    if (win < 0) {
        write(1, "[paint] gui_create_window failed\n", 33);
        return 1;
    }
    clear_canvas();
    gui_present(win, 0, 0, WIDTH, HEIGHT, pixels);

    /* On-screen prompt drawn into the canvas. */
    gui_draw_text(win, 16, 16,
                  "left-click to paint, right-click to cycle colour, ESC to quit",
                  GUI_BGRA(0x30, 0x30, 0x30), 0, 1);
    gui_flush(win);

    uint32_t colour = PALETTE[0];
    unsigned palette_idx = 0;
    int dragging = 0;
    int last_x = 0, last_y = 0;     /* most recent stamp during drag */

    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) {
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
                stamp(win, last_x, last_y, colour);
            } else if (ev.arg2 == GUI_BTN_RIGHT) {
                palette_idx = (palette_idx + 1) % PALETTE_LEN;
                colour = PALETTE[palette_idx];
                /* Tiny corner swatch shows the active colour. */
                gui_fill_rect(win, WIDTH - 24, 8, 16, 16, colour);
                gui_flush(win);
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
                 * the new position.  Required because the WM
                 * coalesces consecutive MOUSE_MOVE events under
                 * fast drags, so successive deliveries can cover
                 * many pixels in one step. */
                stamp_line(win, last_x, last_y, nx, ny, colour);
                last_x = nx;
                last_y = ny;
            }
            break;
        default:
            break;
        }
    }

done:
    gui_destroy_window(win);
    return 0;
}
