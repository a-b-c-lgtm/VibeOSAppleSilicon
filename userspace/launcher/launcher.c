/*
 * userspace/launcher/launcher.c — milestone-44 GUI app launcher.
 *
 * A small floating window with three big buttons.  Click one and
 * the launcher spawn()s the corresponding GUI program.  The
 * launched program owns its own window via the WM, completely
 * independent of the launcher.
 *
 * This is the first GUI app whose entire user model is the mouse.
 * Keystrokes in the launcher window are ignored (ESC quits, just
 * for ergonomics).  Movement, focus, and the close button are all
 * handled by the WM, which means the launcher implementation is
 * unbelievably small — about 200 LOC total.
 *
 * Children spawned by the launcher are not wait()ed for: the
 * kernel will leak the (small) thread struct of an exited child
 * until the launcher itself exits or until someone else reaps it.
 * Acceptable for a milestone-44 demo with at most a handful of
 * launches per session.
 */
#include "../libc/syscall.h"

#define WIN_W   240
#define WIN_H   232

#define BG_BGRA       GUI_BGRA(0xE8, 0xEC, 0xF0)
#define TEXT_BGRA     GUI_BGRA(0x10, 0x18, 0x28)
#define BTN_BGRA      GUI_BGRA(0xC0, 0xD0, 0xE8)
#define BTN_HOVER     GUI_BGRA(0xA0, 0xB8, 0xE0)
#define BTN_BORDER    GUI_BGRA(0x40, 0x60, 0xA0)

#define BTN_X     16
#define BTN_W     (WIN_W - 2 * BTN_X)        /* 208 */
#define BTN_H     36
#define BTN_GAP   8
#define BTN_TOP   16

#define GLYPH_W   8
#define GLYPH_H   16

struct button {
    const char *label;     /* visible text */
    const char *path;      /* absolute path to spawn */
    const char *args;      /* args string passed to spawn ("" = none) */
};

static const struct button g_buttons[] = {
    { "gui_term", "/bin/gui_term", ""     },
    { "paint",    "/bin/paint",    ""     },
    { "notepad",  "/bin/notepad",  ""     },
    { "browser",  "/bin/browser",  "--gui" },
};
#define N_BUTTONS  ((int)(sizeof(g_buttons) / sizeof(g_buttons[0])))

static int g_win_id = -1;
static int g_hover  = -1;     /* index of button under cursor, or -1 */

/* ---------------- helpers ---------------- */

static int btn_y(int i)
{
    return BTN_TOP + i * (BTN_H + BTN_GAP);
}

static int hit_test(int cx, int cy)
{
    if (cx < BTN_X || cx >= BTN_X + BTN_W) return -1;
    for (int i = 0; i < N_BUTTONS; i++) {
        int by = btn_y(i);
        if (cy >= by && cy < by + BTN_H) return i;
    }
    return -1;
}

/* ---------------- rendering ---------------- */

static void draw_button(int i)
{
    int by = btn_y(i);
    uint32_t fill = (i == g_hover) ? BTN_HOVER : BTN_BGRA;
    /* Filled body. */
    gui_fill_rect(g_win_id, BTN_X, by, BTN_W, BTN_H, fill);
    /* 1-pixel border on each edge. */
    gui_fill_rect(g_win_id, BTN_X,             by,             BTN_W, 1,     BTN_BORDER);
    gui_fill_rect(g_win_id, BTN_X,             by + BTN_H - 1, BTN_W, 1,     BTN_BORDER);
    gui_fill_rect(g_win_id, BTN_X,             by,             1,     BTN_H, BTN_BORDER);
    gui_fill_rect(g_win_id, BTN_X + BTN_W - 1, by,             1,     BTN_H, BTN_BORDER);
    /* Centred label. Chapter 102 -- measure the rendered width
     * with the proportional kernel font instead of `len * 8`. */
    int pix_w = gui_measure_text(g_buttons[i].label);
    int tx    = BTN_X + (BTN_W - pix_w) / 2;
    int ty    = by + (BTN_H - GLYPH_H) / 2;
    gui_draw_text(g_win_id, tx, ty, g_buttons[i].label, TEXT_BGRA, fill, 0);
}

static void render(void)
{
    gui_fill_rect(g_win_id, 0, 0, WIN_W, WIN_H, BG_BGRA);
    for (int i = 0; i < N_BUTTONS; i++) draw_button(i);
    gui_flush(g_win_id);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_win_id = gui_create_window(WIN_W, WIN_H, "launcher");
    if (g_win_id < 0) {
        write(1, "[launcher] gui_create_window failed\n", 36);
        return 1;
    }
    render();

    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) { yield(); continue; }
        switch (ev.type) {
        case GUI_EVENT_CLOSE:
            gui_destroy_window(g_win_id);
            return 0;
        case GUI_EVENT_KEY:
            if ((char)(ev.arg0 & 0xFF) == 0x1B) {  /* ESC */
                gui_destroy_window(g_win_id);
                return 0;
            }
            break;
        case GUI_EVENT_MOUSE_MOVE: {
            int new_hover = hit_test((int)ev.arg0, (int)ev.arg1);
            if (new_hover != g_hover) {
                g_hover = new_hover;
                render();
            }
            break;
        }
        case GUI_EVENT_MOUSE_DOWN: {
            if (!(ev.arg2 & GUI_BTN_LEFT)) break;
            int i = hit_test((int)ev.arg0, (int)ev.arg1);
            if (i < 0) break;
            /* Fire-and-forget spawn.  We don't keep the tid;
             * the new process owns its own window. */
            (void)spawn(g_buttons[i].path, g_buttons[i].args);
            break;
        }
        default:
            break;
        }
    }
}
