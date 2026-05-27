/*
 * userspace/pixapp/pixapp.c — chapter 117 port.
 *
 * Opens a 300x200 window via the wmclient (/srv/wm) path and
 * paints a horizontal red→blue gradient by writing the BGRA
 * bytes directly into the wsd-mapped framebuffer.  No
 * syscalls in the hot loop — the cost scales with bytes
 * touched, not with primitive count.
 *
 * This is the chapter-108d descendant of the chapter-108a demo.  In
 * chapter 114 pixapp called `gui_create_window` +
 * `gui_window_fb` + `gui_window_damage`; the kernel WM
 * composed.  After chapter 117 the kernel WM no longer
 * composes (wsd does), so pixapp's three syscalls become
 * `wm_create_window` + (implicit FB mapping) +
 * `wm_window_dirty`.  The paint loop is identical -- the
 * pivot is `struct wm_window::fb` having the same layout as
 * the chapter-108a `gui_fb`.
 *
 * Sits in a wm_poll_event loop until the window is closed or
 * ESC is pressed.  Prints a self-describing line to fd 1
 * after the gradient is painted so the regression test can
 * grep for it without screen-scraping.
 *
 * Run from the shell:  pixapp [title]
 */

#include "../libc/syscall.h"
#include "../libgui/wmclient.h"

#define WIDTH   300
#define HEIGHT  200

/* Print a NUL-terminated string to fd 1 without depending on
 * printf — keeps the binary small and dodges the printf-vs-
 * memset trap from the early printf chapter. */
static void puts1(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

/* Paint a horizontal gradient: column 0 is full red, column
 * WIDTH-1 is full blue, the middle column is magenta.  Uses
 * direct BGRA writes (no draw_text / fill_rect syscalls). */
static void paint_gradient(uint8_t *pixels, uint32_t stride)
{
    for (uint32_t y = 0; y < HEIGHT; y++) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < WIDTH; x++) {
            /* Blue ramps up left→right; red ramps down. */
            uint8_t blue  = (uint8_t)((x * 255u) / (WIDTH - 1));
            uint8_t red   = (uint8_t)(255u - blue);
            uint8_t green = 0;
            /* BGRA byte order: B, G, R, A. */
            row[x * 4 + 0] = blue;
            row[x * 4 + 1] = green;
            row[x * 4 + 2] = red;
            row[x * 4 + 3] = 0xFF;
        }
    }
}

int main(int argc, char **argv)
{
    const char *title = (argc > 1) ? argv[1] : "pixapp";

    /* Open a window through /srv/wm with input routing so
     * ESC + close events reach this process.  wmclient
     * allocates a NO_DECORATION kernel shadow window
     * for the input path; pixel data flows only through the
     * wsd-side FB. */
    struct wm_window win;
    if (wm_create_window_input(WIDTH, HEIGHT, 0, title, &win) < 0) {
        puts1("[pixapp] wm_create_window failed\n");
        return 1;
    }
    if (win.fb.w != WIDTH || win.fb.h != HEIGHT) {
        puts1("[pixapp] mapping had unexpected dimensions\n");
        wm_destroy_window(&win);
        return 1;
    }

    /* Hot path: paint then damage.  Zero syscalls inside
     * paint_gradient — the cost scales with bytes touched,
     * not with primitive count. */
    paint_gradient(win.fb.pixels, win.fb.stride);
    wm_window_dirty(&win, 0, 0, WIDTH, HEIGHT);

    puts1("[pixapp] painted gradient\n");

    /* Idle loop: yield while there's nothing to do, repaint
     * on any user input so the demo also exercises the
     * mapping-stays-valid-across-events guarantee. */
    int frame = 0;
    for (;;) {
        struct gui_event ev;
        if (wm_poll_event(&ev) <= 0) {
            yield();
            continue;
        }
        if (ev.type == GUI_EVENT_CLOSE) break;
        if (ev.type == GUI_EVENT_KEY) {
            char c = (char)(ev.arg0 & 0xFFu);
            if (c == 0x1B) break;   /* ESC quits */
        }
        /* Every event triggers a tiny visible change (top-left
         * 16x16 square cycles between red and blue) so a manual
         * smoke check can verify the mapping survives. */
        frame++;
        uint8_t b = (frame & 1) ? 0xFF : 0x00;
        uint8_t r = (frame & 1) ? 0x00 : 0xFF;
        for (uint32_t y = 0; y < 16; y++) {
            uint8_t *row = win.fb.pixels + (size_t)y * win.fb.stride;
            for (uint32_t x = 0; x < 16; x++) {
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 0xFF;
            }
        }
        wm_window_dirty(&win, 0, 0, 16, 16);
    }

    wm_destroy_window(&win);
    return 0;
}
