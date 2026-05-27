/*
 * userspace/hellogui/hellogui.c — GUI smoke test,
 * rewritten in chapter 116 to draw directly into the window's
 * mapped framebuffer instead of going through gui_present /
 * gui_fill_rect / gui_draw_text syscalls.
 *
 * Before 108c this app was the worst syscall-per-paint
 * offender: a 480x320 pixels[] global, a gui_present every
 * frame (kernel memcpy of 614400 bytes for each refresh) plus
 * one gui_draw_text syscall per line of overlay text.  After
 * 108c it owns the pixel storage outright (no shadow buffer)
 * and every paint primitive is a normal memory store inside
 * its own address space — the kernel only sees the
 * gui_window_dirty call at the end.
 *
 * Run from the shell:  hellogui [title]
 */

#include "../libc/syscall.h"
#include "../libgui/draw.h"

#define WIDTH   480
#define HEIGHT  320

/* Paint the vertical gradient directly into the mapped fb.
 * No shadow buffer — the gradient lives in window memory the
 * moment it's written. */
static void paint_gradient(struct gui_fb *fb)
{
    for (uint32_t y = 0; y < HEIGHT && y < fb->h; y++) {
        uint8_t r = (uint8_t)(0x10 + (y * 0x80) / HEIGHT);
        uint8_t g = (uint8_t)(0x18 + (y * 0x10) / HEIGHT);
        uint8_t b = (uint8_t)(0x40 + (y * 0xA0) / HEIGHT);
        uint32_t row = GUI_BGRA(r, g, b);
        uint32_t *line = (uint32_t *)(fb->pixels + (size_t)y * fb->stride);
        for (uint32_t x = 0; x < WIDTH && x < fb->w; x++)
            line[x] = row;
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

    /* Map the window's pixel storage into our address space once;
     * we hold the mapping for the lifetime of the window. */
    struct gui_fb fb;
    if (gui_window_fb(win, &fb) < 0) {
        write(1, "[hellogui] gui_window_fb failed\n", 32);
        gui_destroy_window(win);
        return 1;
    }

    /* Initial paint: gradient + text overlays, then a single
     * damage call covering the whole window. */
    paint_gradient(&fb);

    /* Greeting text in white over a transparent background so the
     * gradient shows through. */
    draw_text(&fb, 16, 24,
              "Hello from the window manager!",
              GUI_BGRA(0xFF, 0xFF, 0xFF), 0, 1);
    draw_text(&fb, 16, 56,
              "Running on aarch64 under HVF.",
              GUI_BGRA(0xC0, 0xE0, 0xFF), 0, 1);
    draw_text(&fb, 16, 88,
              "Press keys to type into this window.",
              GUI_BGRA(0xFF, 0xC0, 0x80), 0, 1);
    draw_text(&fb, 16, 104,
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
        draw_text(&fb, 16, 144, pidbuf,
                  GUI_BGRA(0xC0, 0xC0, 0xC0), 0, 1);
    }

    /* One damage for the whole window after the cold paint. */
    gui_window_dirty(&fb, 0, 0, fb.w, fb.h);

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

        /* Repaint just the input row: regenerate the gradient
         * pixels for rows 200..239, draw the labels on top,
         * then damage that strip.  No syscall in the loop body
         * apart from one damage call at the end. */
        for (uint32_t y = 200; y < 240 && y < fb.h; y++) {
            uint8_t r = (uint8_t)(0x10 + (y * 0x80) / HEIGHT);
            uint8_t g = (uint8_t)(0x18 + (y * 0x10) / HEIGHT);
            uint8_t b = (uint8_t)(0x40 + (y * 0xA0) / HEIGHT);
            uint32_t row = GUI_BGRA(r, g, b);
            uint32_t *lp = (uint32_t *)(fb.pixels + (size_t)y * fb.stride);
            for (uint32_t x = 0; x < WIDTH && x < fb.w; x++)
                lp[x] = row;
        }
        draw_text(&fb, 16, 208, "input: ",
                  GUI_BGRA(0xFF, 0xFF, 0x80), 0, 1);
        /* Measure the prefix width so the typed text lines up
         * with the label exactly, regardless of which font path
         * answers (TTF advances vary slightly from the old
         * bitmap-font fixed-width assumption). */
        int label_w = draw_measure_text("input: ");
        draw_text(&fb, 16 + label_w, 208, line,
                  GUI_BGRA(0xFF, 0xFF, 0xFF), 0, 1);
        gui_window_dirty(&fb, 0, 200, fb.w, 40);
    }

    gui_destroy_window(win);
    return 0;
}
