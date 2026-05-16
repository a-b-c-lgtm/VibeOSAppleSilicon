/*
 * userspace/taskbar/taskbar.c — milestone-47 desktop shell taskbar.
 *
 * A `screen_w x 28` borderless always-on-top window pinned to the
 * bottom of the framebuffer.  Polls SYS_GUI_LIST_WINDOWS every
 * ~150 ms and renders one cell per other window.  Click on a cell
 * raises that window.  Click on its own area is ignored.
 *
 * Screen dimensions are queried at startup via SYS_GUI_GET_SCREEN_
 * SIZE so the bar correctly stretches the full width and pins to
 * the bottom no matter what scanout resolution the kernel
 * negotiated with virtio-gpu (1280x800, 1920x1080, etc).
 *
 * The taskbar deliberately does NOT show itself in the cell list
 * (that would be reflexive and visually noisy) and does NOT show
 * any other ALWAYS_ON_TOP windows (taskbars don't list taskbars).
 *
 * Self-cells would also create a feedback loop where the user could
 * "raise" the taskbar, which is meaningless — pinned windows are
 * always on top by definition.
 */
#include "../libc/syscall.h"
#include "../libc/time.h"

/* Filled in by main() at startup via gui_get_screen_size().  Used
 * to compute BAR width, BAR Y position, and the right-aligned
 * clock's X position. */
static uint32_t g_screen_w = 1280;
static uint32_t g_screen_h = 800;

#define BAR_H           28
#define BAR_X           0
/* BAR_Y / CLOCK_X are now computed at runtime from g_screen_*. */
static int32_t g_bar_y  = 800 - 28;
static int32_t g_clock_x = 1280 - 80 - 8;

#define CELL_W          180
#define CELL_GAP        6
#define CELL_PADX       8

#define BG_BGRA         GUI_BGRA(0x18, 0x1C, 0x32)
#define BG_HI_BGRA      GUI_BGRA(0x22, 0x28, 0x42)
#define CELL_BGRA       GUI_BGRA(0x30, 0x40, 0x70)
#define CELL_FOCUS_BGRA GUI_BGRA(0x60, 0x90, 0xE0)
/* Milestone 51: a minimized window's cell is dimmed so the user
 * can tell at a glance that the window exists but is hidden. */
#define CELL_MIN_BGRA   GUI_BGRA(0x18, 0x20, 0x38)
#define CELL_BORDER     GUI_BGRA(0x60, 0x80, 0xC0)
#define CELL_MIN_BORDER GUI_BGRA(0x40, 0x50, 0x78)
#define TEXT_BGRA       GUI_BGRA(0xF0, 0xF0, 0xF0)
#define TEXT_MIN_BGRA   GUI_BGRA(0x90, 0x98, 0xB0)
#define TOP_LINE_BGRA   GUI_BGRA(0x60, 0x80, 0xC0)
#define CLOCK_BG_BGRA   GUI_BGRA(0x10, 0x14, 0x24)
#define CLOCK_FG_BGRA   GUI_BGRA(0xC0, 0xE0, 0xFF)

#define GLYPH_W   8
#define GLYPH_H   16

/* The clock occupies the rightmost CLOCK_W pixels of the bar.  Plus
 * a CLOCK_PAD pixel gap from the right edge.  HH:MM:SS = 8 glyphs =
 * 64 px, plus ~16 px padding on each side = 96 px total.  X is
 * computed at runtime once g_screen_w is known. */
#define CLOCK_PAD     8
#define CLOCK_W       80
#define CLOCK_X       (g_clock_x)
#define CLOCK_Y       4
#define CLOCK_H       (BAR_H - 8)

#define MAX_WINDOWS  16

static int g_self_id = -1;

/* A small cache of cells from the last frame.  Used by the click
 * handler to map (x, y) back to a window id without re-querying.
 * `focused` and `minimized` mirror what was in gui_window_info
 * the last time we rendered, so the click handler can pick the
 * right verb (minimize/restore/raise) without another syscall. */
struct cell {
    int      win_id;
    int32_t  x, y, w, h;
    int      focused;
    int      minimized;
};
static struct cell g_cells[MAX_WINDOWS];
static int         g_n_cells = 0;

/* ---------------- helpers ---------------- */

/* (unused for now — we truncate by glyph count instead of measuring
 * the string length.  Kept around for the M48 follow-up that adds a
 * right-aligned clock.) */
__attribute__((unused))
static size_t s_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---------------- rendering ---------------- */

static void draw_cell(int idx, const struct gui_window_info *info)
{
    struct cell *c = &g_cells[idx];
    int x = CELL_PADX + idx * (CELL_W + CELL_GAP);
    int y = 4;
    int w = CELL_W;
    int h = BAR_H - 8;
    int minimized = (info->flags & GUI_WIN_FLAG_MINIMIZED) ? 1 : 0;
    c->win_id    = info->id;
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->focused   = info->focused;
    c->minimized = minimized;

    /* Pick body, border, and text colours.  Minimized wins over
     * focused: a window can't be both hidden AND have keyboard
     * focus (the WM transfers focus on minimize), but we'd rather
     * fail safely if the kernel ever returns that combination. */
    uint32_t fill   = info->focused ? CELL_FOCUS_BGRA : CELL_BGRA;
    uint32_t border = CELL_BORDER;
    uint32_t fg     = TEXT_BGRA;
    if (minimized) {
        fill   = CELL_MIN_BGRA;
        border = CELL_MIN_BORDER;
        fg     = TEXT_MIN_BGRA;
    }
    gui_fill_rect(g_self_id, x, y, w, h, fill);
    gui_fill_rect(g_self_id, x,         y,         w, 1, border);
    gui_fill_rect(g_self_id, x,         y + h - 1, w, 1, border);
    gui_fill_rect(g_self_id, x,         y,         1, h, border);
    gui_fill_rect(g_self_id, x + w - 1, y,         1, h, border);

    /* Truncate label to fit. */
    char label[32];
    int max_chars = (w - 2 * 6) / GLYPH_W;
    if (max_chars > 31) max_chars = 31;
    int li = 0;
    for (; li < max_chars && info->title[li]; li++)
        label[li] = info->title[li];
    label[li] = '\0';

    int tx = x + 6;
    int ty = y + (h - GLYPH_H) / 2;
    gui_draw_text(g_self_id, tx, ty, label, fg, fill, 0);
}

static void render(const struct gui_window_info *infos, int n)
{
    /* Background. */
    gui_fill_rect(g_self_id, 0, 0, g_screen_w, BAR_H, BG_BGRA);
    /* 1px highlight along the top edge. */
    gui_fill_rect(g_self_id, 0, 0, g_screen_w, 1, TOP_LINE_BGRA);

    g_n_cells = 0;
    for (int i = 0; i < n && g_n_cells < MAX_WINDOWS; i++) {
        const struct gui_window_info *info = &infos[i];
        if (info->id == g_self_id) continue;
        if (info->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP) continue;
        if (info->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
        draw_cell(g_n_cells, info);
        g_n_cells++;
    }

    gui_flush(g_self_id);
}

/* ---------------- main loop ---------------- */

static int g_known_count = -1;       /* count from last render */
static int g_known_focus = -1;
static int g_known_minmask = 0;      /* bitmask of minimized cell ids */
static int g_last_clock_sec = -1;    /* last second value rendered */

/* Format civil time `ct` into "HH:MM:SS" at *out (UTC).  Used by
 * the taskbar clock for a stable 8-glyph render.  Pre-chapter-95
 * this was an uptime-since-boot formatter that wrapped at 100h;
 * now it shows the wall clock the kernel read from PL031.
 *
 * UTC is deliberate for the floor — chapter 95 doesn't ship a
 * timezone story.  When `/data/timezone` lands we'll add an
 * offset before this call. */
static void format_clock(const struct civil_time *ct, char out[9])
{
    static const char digits[] = "0123456789";
    out[0] = digits[(ct->hour / 10) % 10];
    out[1] = digits[ ct->hour       % 10];
    out[2] = ':';
    out[3] = digits[(ct->min  / 10) % 10];
    out[4] = digits[ ct->min        % 10];
    out[5] = ':';
    out[6] = digits[(ct->sec  / 10) % 10];
    out[7] = digits[ ct->sec        % 10];
    out[8] = '\0';
}

static void draw_clock(void)
{
    /* Pull wall time, fall back to uptime-derived seconds if
     * the syscall ever fails (it shouldn't — gettimeofday only
     * returns -EFAULT on a bad pointer, and ours is on stack). */
    struct timeval tv;
    int rc = gettimeofday(&tv);
    long secs_total = (rc == 0) ? (long)tv.tv_sec
                                : (long)(uptime_ms() / 1000ul);

    struct civil_time ct;
    gmtime_r((time_t)secs_total, &ct);

    char buf[9];
    format_clock(&ct, buf);

    /* Body. */
    gui_fill_rect(g_self_id, CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H,
                  CLOCK_BG_BGRA);
    gui_fill_rect(g_self_id, CLOCK_X,             CLOCK_Y,
                  CLOCK_W, 1, CELL_BORDER);
    gui_fill_rect(g_self_id, CLOCK_X,             CLOCK_Y + CLOCK_H - 1,
                  CLOCK_W, 1, CELL_BORDER);
    gui_fill_rect(g_self_id, CLOCK_X,             CLOCK_Y,
                  1,        CLOCK_H, CELL_BORDER);
    gui_fill_rect(g_self_id, CLOCK_X + CLOCK_W - 1, CLOCK_Y,
                  1,        CLOCK_H, CELL_BORDER);

    /* Centred glyphs.  HH:MM:SS = 8 chars * 8 px = 64 px. */
    int tx = CLOCK_X + (CLOCK_W - 8 * GLYPH_W) / 2;
    int ty = CLOCK_Y + (CLOCK_H - GLYPH_H) / 2;
    gui_draw_text(g_self_id, tx, ty, buf, CLOCK_FG_BGRA, CLOCK_BG_BGRA, 0);

    g_last_clock_sec = (int)secs_total;
}

static int needs_redraw(const struct gui_window_info *infos, int n)
{
    /* Count visible (non-self, non-pinned, non-wallpaper) entries. */
    int visible = 0;
    int focus_id = -1;
    int minmask = 0;
    for (int i = 0; i < n; i++) {
        if (infos[i].id == g_self_id) continue;
        if (infos[i].flags & GUI_WIN_FLAG_ALWAYS_ON_TOP) continue;
        if (infos[i].flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
        visible++;
        if (infos[i].focused) focus_id = infos[i].id;
        if (infos[i].flags & GUI_WIN_FLAG_MINIMIZED) {
            /* OR each id into a bitmask so we can detect any
             * change in WHICH windows are minimized, not just
             * how many.  Window ids are 0..WM_MAX_WINDOWS-1
             * (currently 16) so they fit in a 32-bit mask. */
            minmask |= (1 << infos[i].id);
        }
    }
    if (visible != g_known_count ||
        focus_id != g_known_focus ||
        minmask  != g_known_minmask) {
        g_known_count   = visible;
        g_known_focus   = focus_id;
        g_known_minmask = minmask;
        return 1;
    }
    return 0;
}

static void handle_click(int cx, int cy)
{
    for (int i = 0; i < g_n_cells; i++) {
        struct cell *c = &g_cells[i];
        if (cx < c->x || cx >= c->x + c->w) continue;
        if (cy < c->y || cy >= c->y + c->h) continue;
        /* Tri-state behaviour matching common desktop conventions:
         *   - focused, not minimized → minimize (hide it).
         *   - minimized            → restore + raise + focus.
         *   - background, visible  → raise + focus (no toggle).
         * Restore is done via gui_raise_window which now auto-
         * unhides as a side effect; that keeps the taskbar code
         * to one syscall per click in the common path. */
        if (c->minimized) {
            gui_raise_window(c->win_id);
        } else if (c->focused) {
            gui_set_minimized(c->win_id, 1);
        } else {
            gui_raise_window(c->win_id);
        }
        return;
    }
}

int main(void)
{
    /* Discover screen size before creating the bar window so we
     * stretch the full width and sit flush against the bottom
     * regardless of the framebuffer's actual resolution. */
    uint32_t sw = 0, sh = 0;
    if (gui_get_screen_size(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        g_screen_w = sw;
        g_screen_h = sh;
    }
    g_bar_y  = (int32_t)g_screen_h - BAR_H;
    g_clock_x = (int32_t)g_screen_w - CLOCK_W - CLOCK_PAD;

    g_self_id = gui_create_window_ex(
        g_screen_w, BAR_H, "taskbar",
        GUI_WIN_FLAG_NO_DECORATION | GUI_WIN_FLAG_ALWAYS_ON_TOP,
        BAR_X, g_bar_y);
    if (g_self_id < 0) {
        write(1, "[taskbar] gui_create_window_ex failed\n", 39);
        return 1;
    }

    /* Initial paint with empty list. */
    gui_fill_rect(g_self_id, 0, 0, g_screen_w, BAR_H, BG_BGRA);
    gui_fill_rect(g_self_id, 0, 0, g_screen_w, 1, TOP_LINE_BGRA);
    draw_clock();
    gui_flush(g_self_id);

    struct gui_window_info infos[16];

    for (;;) {
        /* Drain events FIRST so the cell snapshot from the previous
         * render still reflects what the user clicked on.  If we
         * called list_windows before draining, the click that just
         * arrived would have already moved focus to the taskbar
         * itself (the WM focuses any clicked window on left-down),
         * making the freshly-rendered cell think the launched
         * window had lost focus.  That broke the milestone-51
         * "click-focused-cell-to-minimize" toggle: handle_click
         * would see c->focused == 0 and fall through to a no-op
         * raise instead of calling gui_set_minimized. */
        struct gui_event ev;
        int saw_event = 0;
        while (gui_poll_event(&ev)) {
            saw_event = 1;
            if (ev.type == GUI_EVENT_KEY && ev.arg0 == 27) {
                gui_destroy_window(g_self_id);
                return 0;
            }
            if (ev.type == GUI_EVENT_CLOSE) {
                gui_destroy_window(g_self_id);
                return 0;
            }
            if (ev.type == GUI_EVENT_MOUSE_DOWN &&
                (ev.arg2 & GUI_BTN_LEFT)) {
                handle_click((int)ev.arg0, (int)ev.arg1);
                /* After raise/minimize the window list will report
                 * a different focused/minimized id, so force a
                 * redraw next iteration. */
                g_known_count = -1;
            }
        }

        int n = gui_list_windows(infos, 16);
        if (n < 0) n = 0;
        int redraw = needs_redraw(infos, n);
        if (redraw) {
            render(infos, n);
        }

        /* Tick the clock once per WALL-clock second.  If we
         * redraw cells we also have to redraw the clock since
         * render() repaints everything except the clock area.
         *
         * The comparison source MUST match what draw_clock
         * stores into g_last_clock_sec — wall-clock seconds
         * since chapter 95.  Using uptime here (as we did
         * pre-95) would never match the wall-clock value
         * draw_clock writes, so the clock would repaint every
         * poll iteration. */
        struct timeval tv_tick;
        long secs = (gettimeofday(&tv_tick) == 0)
                  ? (long)tv_tick.tv_sec
                  : (long)(uptime_ms() / 1000ul);
        if (redraw || (int)secs != g_last_clock_sec) {
            draw_clock();
            gui_flush(g_self_id);
        }

        if (!saw_event) sleep_ms(150);
    }
}
