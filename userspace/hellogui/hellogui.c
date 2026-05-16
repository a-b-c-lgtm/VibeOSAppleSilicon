/*
 * userspace/hellogui/hellogui.c — milestone-40 GUI smoke test.
 *
 * Opens a 480x320 window, paints a gradient + greeting + the
 * pid + uptime, then waits for keystrokes.  Each typed key is
 * echoed back into the window in big text; ESC quits.
 *
 * Run from the shell:  hellogui [title]
 */

#include "../libc/syscall.h"

#define WIDTH   480
#define HEIGHT  320

static uint32_t pixels[WIDTH * HEIGHT];

static void paint_gradient(void)
{
    /* Vertical gradient from deep blue (top) to violet (bottom). */
    for (uint32_t y = 0; y < HEIGHT; y++) {
        uint8_t r = (uint8_t)(0x10 + (y * 0x80) / HEIGHT);
        uint8_t g = (uint8_t)(0x18 + (y * 0x10) / HEIGHT);
        uint8_t b = (uint8_t)(0x40 + (y * 0xA0) / HEIGHT);
        uint32_t row = GUI_BGRA(r, g, b);
        for (uint32_t x = 0; x < WIDTH; x++)
            pixels[y * WIDTH + x] = row;
    }
}

int main(int argc, char **argv)
{
    const char *title = (argc > 1) ? argv[1] : "hellogui";

    int win = gui_create_window(WIDTH, HEIGHT, title);
    if (win < 0) {
        write(1, "[hellogui] gui_create_window failed\n", 36);
        return 1;
    }

    /* Initial paint: gradient → present → text overlays → flush. */
    paint_gradient();
    if (gui_present(win, 0, 0, WIDTH, HEIGHT, pixels) < 0) {
        write(1, "[hellogui] gui_present failed\n", 30);
        gui_destroy_window(win);
        return 1;
    }

    /* Greeting text in white over a transparent background so the
     * gradient shows through. */
    gui_draw_text(win, 16, 24,
                  "Hello from the milestone-40 window manager!",
                  GUI_BGRA(0xFF, 0xFF, 0xFF), 0, 1);
    gui_draw_text(win, 16, 56,
                  "Running on aarch64 under HVF.",
                  GUI_BGRA(0xC0, 0xE0, 0xFF), 0, 1);
    gui_draw_text(win, 16, 88,
                  "Press keys to type into this window.",
                  GUI_BGRA(0xFF, 0xC0, 0x80), 0, 1);
    gui_draw_text(win, 16, 104,
                  "Press ESC to quit.",
                  GUI_BGRA(0xFF, 0x80, 0x80), 0, 1);

    /* Print pid for debugging. */
    {
        char pidbuf[40];
        int pid = getpid();
        const char *prefix = "pid = ";
        size_t i = 0;
        for (; prefix[i]; i++) pidbuf[i] = prefix[i];
        if (pid == 0) pidbuf[i++] = '0';
        else {
            char tmp[12]; int n = 0;
            while (pid > 0) { tmp[n++] = (char)('0' + pid % 10); pid /= 10; }
            while (n > 0) pidbuf[i++] = tmp[--n];
        }
        pidbuf[i] = '\0';
        gui_draw_text(win, 16, 144, pidbuf,
                      GUI_BGRA(0xC0, 0xC0, 0xC0), 0, 1);
    }

    gui_flush(win);

    /* Type-line input area: collect bytes until ESC, repaint.
     * We write keystrokes into a buffer and re-render after every
     * key so the window updates live. */
    char  line[128];
    size_t cursor = 0;
    line[0] = '\0';

    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) {
            yield();
            continue;
        }
        if (ev.type != GUI_EVENT_KEY) continue;
        char c = (char)(ev.arg0 & 0xFFu);

        if (c == 0x1B) break;          /* ESC quits */
        if (c == '\r' || c == '\n') {
            cursor = 0;
            line[0] = '\0';
        } else if (c == 0x7F || c == 0x08) {
            if (cursor > 0) line[--cursor] = '\0';
        } else if (cursor + 1 < sizeof(line)) {
            line[cursor++] = c;
            line[cursor]   = '\0';
        }

        /* Repaint the input row.  Easiest: blit gradient onto the
         * input row, then draw the line text on top. */
        for (uint32_t y = 200; y < 240; y++) {
            uint8_t r = (uint8_t)(0x10 + (y * 0x80) / HEIGHT);
            uint8_t g = (uint8_t)(0x18 + (y * 0x10) / HEIGHT);
            uint8_t b = (uint8_t)(0x40 + (y * 0xA0) / HEIGHT);
            uint32_t row = GUI_BGRA(r, g, b);
            for (uint32_t x = 0; x < WIDTH; x++)
                pixels[y * WIDTH + x] = row;
        }
        gui_present(win, 0, 200, WIDTH, 40, pixels + 200 * WIDTH);
        gui_draw_text(win, 16, 208, "input: ",
                      GUI_BGRA(0xFF, 0xFF, 0x80), 0, 1);
        gui_draw_text(win, 16 + 7 * 8, 208, line,
                      GUI_BGRA(0xFF, 0xFF, 0xFF), 0, 1);
        gui_flush(win);
    }

    gui_destroy_window(win);
    return 0;
}
