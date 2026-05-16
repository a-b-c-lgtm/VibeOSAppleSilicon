/*
 * userspace/notify/notify.c — milestone-49 toast notification.
 *
 * Spawned as `/bin/notify "<message>"` (or with multiple words —
 * we join argv with single spaces).  Pops up an undecorated,
 * always-on-top window in the top-right corner that displays the
 * message for ~3 seconds, then auto-dismisses.
 *
 * Design notes:
 *   - Always-on-top so the toast is visible over any focused
 *     window without stealing focus.
 *   - NO_DECORATION because the user can't act on it (no close
 *     button needed; it auto-closes).
 *   - Sized to fit one line of text.  The message is truncated
 *     to the cell width — long messages wrap to the next call.
 *   - Spawning multiple notifications stacks them: each new
 *     toast is offset 8 px down/left from the previous one
 *     (cascade), but for v1 we put every toast at the same
 *     position because the only existing call site is notepad's
 *     "saved." which never fires twice in quick succession.
 */
#include "../libc/syscall.h"

#define WIN_W           360
#define WIN_H           80
#define MARGIN          16
/* WIN_X is computed at runtime from the discovered screen width. */

#define BG_BGRA         GUI_BGRA(0x20, 0x28, 0x40)
#define BORDER_BGRA     GUI_BGRA(0x80, 0xA0, 0xE0)
#define ACCENT_BGRA     GUI_BGRA(0x40, 0x80, 0xFF)
#define TEXT_BGRA       GUI_BGRA(0xF0, 0xF4, 0xFF)
#define TITLE_BGRA      GUI_BGRA(0xC0, 0xE0, 0xFF)

#define GLYPH_W   8
#define GLYPH_H   16

#define MAX_MSG_LEN   80
#define DISMISS_MS    3000

static int g_win = -1;

static void s_strcpy_max(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void s_join_argv(char *out, int max, int argc, char **argv)
{
    int oi = 0;
    for (int a = 1; a < argc && oi < max - 1; a++) {
        if (a > 1 && oi < max - 1) out[oi++] = ' ';
        const char *p = argv[a];
        while (p && *p && oi < max - 1) out[oi++] = *p++;
    }
    out[oi] = '\0';
}

/* Draw the toast.  Layout:
 *
 *   ┌──────────────────────────────────────┐
 *   │ ▏ Notice                             │
 *   │                                      │
 *   │ <message text>                       │
 *   └──────────────────────────────────────┘
 *
 * The 4-px-wide accent bar on the left is purely visual —
 * mimics every modern desktop's toast style. */
static void draw_toast(const char *msg)
{
    /* Background. */
    gui_fill_rect(g_win, 0, 0, WIN_W, WIN_H, BG_BGRA);

    /* 1-px border on all four sides. */
    gui_fill_rect(g_win, 0,         0,           WIN_W, 1, BORDER_BGRA);
    gui_fill_rect(g_win, 0,         WIN_H - 1,   WIN_W, 1, BORDER_BGRA);
    gui_fill_rect(g_win, 0,         0,           1, WIN_H,  BORDER_BGRA);
    gui_fill_rect(g_win, WIN_W - 1, 0,           1, WIN_H,  BORDER_BGRA);

    /* Left accent bar (4 px wide, full height inside border). */
    gui_fill_rect(g_win, 1, 1, 4, WIN_H - 2, ACCENT_BGRA);

    /* Title row. */
    gui_draw_text(g_win, 16, 12, "Notice", TITLE_BGRA, BG_BGRA, 0);

    /* Body row. Chapter 102 -- truncate by measured pixel width
     * with the proportional kernel font, not by character count. */
    int avail_w = WIN_W - 32;
    char trunc[MAX_MSG_LEN + 1];
    int gi = 0;
    while (gi < MAX_MSG_LEN && msg[gi]) {
        trunc[gi] = msg[gi];
        trunc[gi + 1] = '\0';
        if (gui_measure_text(trunc) > avail_w) {
            trunc[gi] = '\0';
            break;
        }
        gi++;
    }
    trunc[gi] = '\0';
    gui_draw_text(g_win, 16, 40, trunc, TEXT_BGRA, BG_BGRA, 0);

    gui_flush(g_win);
}

int main(int argc, char **argv)
{
    char msg[MAX_MSG_LEN + 1];
    if (argc <= 1) {
        s_strcpy_max(msg, "(no message)", sizeof(msg));
    } else {
        s_join_argv(msg, sizeof(msg), argc, argv);
    }

    /* Anchor the toast in the top-right corner relative to the
     * actual scanout, not a hardcoded 1280-wide assumption. */
    uint32_t sw = 1280, sh = 800;
    gui_get_screen_size(&sw, &sh);
    int32_t win_x = (int32_t)sw - WIN_W - MARGIN;
    if (win_x < 0) win_x = 0;

    g_win = gui_create_window_ex(
        WIN_W, WIN_H, "notify",
        GUI_WIN_FLAG_NO_DECORATION | GUI_WIN_FLAG_ALWAYS_ON_TOP,
        win_x, MARGIN);
    if (g_win < 0) return 1;

    draw_toast(msg);

    /* Wait for either DISMISS_MS to elapse or for any KEY/MOUSE
     * event (user-acknowledge dismisses early). */
    unsigned long deadline = uptime_ms() + DISMISS_MS;
    for (;;) {
        struct gui_event ev;
        int saw = 0;
        while (gui_poll_event(&ev)) {
            saw = 1;
            if (ev.type == GUI_EVENT_KEY ||
                ev.type == GUI_EVENT_MOUSE_DOWN ||
                ev.type == GUI_EVENT_CLOSE) {
                gui_destroy_window(g_win);
                return 0;
            }
        }
        if (uptime_ms() >= deadline) break;
        if (!saw) sleep_ms(50);
    }

    gui_destroy_window(g_win);
    return 0;
}
