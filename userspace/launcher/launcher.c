/*
 * userspace/launcher/launcher.c — milestone-44 GUI app launcher,
 * ported to /srv/wm in chapter 108d, then reshaped
 * into a Start-menu-style panel.
 *
 * A small undecorated panel pinned just above the taskbar.  Four
 * big buttons; click one and the launcher spawn()s the
 * corresponding GUI program.  The launched program owns its own
 * window via the WM, completely independent of the launcher.
 *
 * Show/hide model
 * ---------------
 *
 * The launcher is spawned once at boot by init and stays alive
 * for the whole session.  It is HIDDEN by default — at startup
 * we paint once into our FB, then immediately WM_WIN_MINIMIZE
 * so the panel doesn't appear until the user asks for it.
 *
 * The taskbar owns a "Start" button at its leftmost edge.
 * Clicking it toggles the launcher's visibility:
 *     hidden  -> restore (becomes visible, takes focus)
 *     visible -> minimize (hidden again)
 *
 * From the launcher's own side, three actions all collapse to
 * "hide myself" so the user sees the menu disappear after the
 * intended action:
 *     - picking an app from the menu (after spawn succeeds)
 *     - pressing ESC inside the panel
 *     - any injected GUI_EVENT_CLOSE
 *
 * No title bar / close / minimize buttons are painted: the
 * panel is created with WM_WF_NODECORATION so wsd skips
 * decoration paint and hit-test entirely.  WM_WF_ALWAYS_ON_TOP
 * keeps the panel out of the taskbar's cell list (the taskbar
 * already filters ALWAYS_ON_TOP windows from cells) and ensures
 * it overlays normal app windows when visible.
 *
 * This is the first GUI app whose entire user model is the mouse.
 *
 * Children spawned by the launcher are not wait()ed for: the
 * kernel will leak the (small) thread struct of an exited child
 * until the launcher itself exits or until someone else reaps it.
 * Acceptable for a milestone-44 demo with at most a handful of
 * launches per session.
 *
 * Chapter 108d port notes:
 *   - wsd owns the per-window FB and compose.  wmclient maps the
 *     FB into our AS so paint primitives stay in-process.
 *   - wm_create_window_at takes a `title` argument that doubles
 *     as the "also open a kernel input shadow" switch — we pass
 *     "launcher" so wm_poll_event delivers real input events.
 */
#include "../libc/syscall.h"
#include "../libgui/draw.h"
#include "../libgui/wmclient.h"

#define WIN_W   240
#define WIN_H   232

#define BG_BGRA       GUI_BGRA(0xE8, 0xEC, 0xF0)
#define TEXT_BGRA     GUI_BGRA(0x10, 0x18, 0x28)
#define BTN_HOVER_BGRA GUI_BGRA(0xD2, 0xDE, 0xF2)

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

static struct wm_window g_win;
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
    draw_button_chrome(&g_win.fb, BTN_X, by, BTN_W, BTN_H);
    if (i == g_hover) {
        /* Hover tint inside the shared chrome so launcher keeps
         * its existing rollover affordance while sharing shape. */
        draw_fill_rect(&g_win.fb, BTN_X + 2, by + 2,
                       BTN_W - 4, BTN_H - 4, BTN_HOVER_BGRA);
    }
    /* Centred label. */
    int pix_w = draw_measure_text(g_buttons[i].label);
    int tx    = BTN_X + (BTN_W - pix_w) / 2;
    int ty    = by + (BTN_H - GLYPH_H) / 2;
    draw_text(&g_win.fb, tx, ty, g_buttons[i].label, TEXT_BGRA, 0, 1);
}

static void render(void)
{
    draw_fill_rect(&g_win.fb, 0, 0, g_win.fb.w, g_win.fb.h, BG_BGRA);
    for (int i = 0; i < N_BUTTONS; i++) draw_button(i);
    /* Single damage covering the whole window — cheaper than
     * tracking per-button dirty rects, and wsd clips. */
    wm_window_dirty(&g_win, 0, 0, g_win.fb.w, g_win.fb.h);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Anchor above the taskbar (taskbar.c BAR_H = 28).  The
     * launcher is undecorated (no title bar / buttons; the
     * taskbar's Start button is the close affordance) and
     * ALWAYS_ON_TOP so the taskbar's own filter hides us
     * from its cell list -- summon/dismiss is the Start
     * button's job, not a cell click. */
    uint32_t sw = 1280, sh = 800;
    (void)gui_get_screen_size(&sw, &sh);
    const uint32_t TASKBAR_H = 28u;
    uint32_t lx = 0;
    uint32_t ly = (sh > WIN_H + TASKBAR_H) ? sh - TASKBAR_H - WIN_H : 0;

    if (wm_create_window_at(WIN_W, WIN_H,
                            WM_WF_NODECORATION | WM_WF_ALWAYS_ON_TOP,
                            lx, ly,
                            "launcher", &g_win) < 0) {
        write(1, "[launcher] wm_create_window_at failed\n", 38);
        return 1;
    }
    render();

    /* Hidden by default.  The taskbar's Start button toggles
     * visibility via WM_WIN_RESTORE / WM_WIN_MINIMIZE; this
     * initial hide is the symmetric peer of the close-button
     * minimize from a few lines down. */
    (void)wm_window_minimize_id(g_win.id);

    for (;;) {
        struct gui_event ev;
        if (wm_poll_event(&ev) <= 0) { yield(); continue; }
        switch (ev.type) {
        case GUI_EVENT_CLOSE:
            /* No close button exists on a NODECORATION window,
             * but if anyone ever injects CLOSE we still hide
             * rather than exit. */
            (void)wm_window_minimize_id(g_win.id);
            g_hover = -1;
            break;
        case GUI_EVENT_KEY:
            if ((char)(ev.arg0 & 0xFF) == 0x1B) {  /* ESC */
                (void)wm_window_minimize_id(g_win.id);
                g_hover = -1;
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
            int rc = spawn(g_buttons[i].path, g_buttons[i].args);
            /* Auto-hide on successful spawn so the new app
             * isn't immediately occluded by us.  On spawn
             * failure stay visible so the user sees their
             * click had no effect and can try again. */
            if (rc >= 0) {
                (void)wm_window_minimize_id(g_win.id);
                g_hover = -1;
            }
            break;
        }
        default:
            break;
        }
    }
}
