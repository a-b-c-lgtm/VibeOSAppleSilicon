/*
 * userspace/taskbar/taskbar.c — milestone-47 desktop shell taskbar,
 * ported to /srv/wm in chapter 108d.
 *
 * A `screen_w x 28` borderless always-on-top window pinned to the
 * bottom of the framebuffer.  Polls wm_list_windows every ~150 ms
 * and renders one cell per other wsd window.  Click on a cell
 * raises that window.  Click on its own area is ignored.
 *
 * Screen dimensions are queried at startup via gui_get_screen_size
 * (still a kernel-side syscall; it just reads scanout dims).
 * The bar correctly stretches the full width and pins to the
 * bottom no matter what scanout resolution the kernel negotiated
 * with virtio-gpu (1280x800, 1920x1080, etc).
 *
 * The taskbar deliberately does NOT show itself in the cell list
 * (that would be reflexive and visually noisy) and does NOT show
 * any other ALWAYS_ON_TOP windows (taskbars don't list taskbars).
 *
 * Chapter 108d port notes:
 *   - wsd owns the per-window FB and compose; wmclient maps the
 *     FB into our AS so paint primitives stay in-process.
 *   - wm_create_window_at puts us at (0, scanout_h - BAR_H) without
 *     perturbing the cascade for cascade-positioned apps.
 *   - wm_list_windows returns wsd's window table including titles
 *     so we don't need a separate per-window title RPC.
 *   - Focus / minimize state wasn't tracked in chapter 108d,
 *     so cells rendered as plain non-focused entries there.
 *     chapter 108e moved input/focus routing into wsd and
 *     finished the click-to-raise path.
 */
#include "../libc/syscall.h"
#include "../libc/time.h"
#include "../libc/printf.h"
#include "../libgui/draw.h"
#include "../libgui/wmclient.h"

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

/* Start button geometry.  Sits leftmost on the bar; cell strip
 * starts after it.  Width matches the "Start" label (~5 chars *
 * 8 px) with generous padding for an easy click target. */
#define START_BTN_X     8
#define START_BTN_Y     4
#define START_BTN_W     60
#define START_BTN_H     (BAR_H - 8)
#define START_BTN_LABEL "Start"
/* Where cells begin: clear of the start button + a small gap. */
#define CELLS_X0        (START_BTN_X + START_BTN_W + 8)

#define BG_BGRA         GUI_BGRA(0x18, 0x1C, 0x32)
#define BG_HI_BGRA      GUI_BGRA(0x22, 0x28, 0x42)
#define CELL_BGRA       GUI_BGRA(0x30, 0x40, 0x70)
#define CELL_FOCUS_BGRA GUI_BGRA(0x60, 0x90, 0xE0)
#define CELL_MIN_BGRA   GUI_BGRA(0x18, 0x20, 0x38)
#define CELL_BORDER     GUI_BGRA(0x60, 0x80, 0xC0)
#define CELL_MIN_BORDER GUI_BGRA(0x40, 0x50, 0x78)
#define TEXT_BGRA       GUI_BGRA(0xF0, 0xF0, 0xF0)
#define TEXT_MIN_BGRA   GUI_BGRA(0x90, 0x98, 0xB0)
#define TOP_LINE_BGRA   GUI_BGRA(0x60, 0x80, 0xC0)
#define CLOCK_BG_BGRA   GUI_BGRA(0x10, 0x14, 0x24)
#define CLOCK_FG_BGRA   GUI_BGRA(0xC0, 0xE0, 0xFF)
/* Start button palette.  Pressed = the launcher panel is
 * currently visible; idle = the launcher is hidden.  Hover
 * lights up either state on cursor enter (lazy: we only
 * recolour on click since the taskbar doesn't poll cursor
 * position between WM_LIST ticks). */
#define START_BG_IDLE    GUI_BGRA(0x30, 0x60, 0x30)
#define START_BG_ACTIVE  GUI_BGRA(0x60, 0xB0, 0x60)
#define START_BORDER     GUI_BGRA(0x80, 0xC0, 0x80)
#define START_FG_BGRA    GUI_BGRA(0xF0, 0xF0, 0xF0)

#define GLYPH_W   8
#define GLYPH_H   16

#define CLOCK_PAD     8
#define CLOCK_W       80
#define CLOCK_X       (g_clock_x)
#define CLOCK_Y       4
#define CLOCK_H       (BAR_H - 8)

#define MAX_WINDOWS  16

static struct wm_window g_win;

/* chapter 108e — per-cell state captured at last render so
 * the click handler can resolve "the user clicked at x="
 * back to a win_id WITHOUT re-listing (the list could have
 * changed between the click arriving and our handling it,
 * and the user clicked at THESE pixels, not at the future
 * pixels).  Also lets needs_redraw() detect a minimize-state
 * flip on an unchanged window count. */
struct cell_record {
    uint32_t win_id;
    int      minimized;   /* 0/1 — also tells us which style to use */
};
static struct cell_record g_cells[MAX_WINDOWS];
static int g_cell_count = 0;

/* Cached launcher window state, refreshed each WM_LIST tick.
 * Used by the Start button: click toggles between minimize
 * (if visible) and restore (if hidden).  win_id == 0 means
 * "launcher not yet visible in WM_LIST" — Start button greys
 * out in that case rather than firing a no-op RPC. */
static uint32_t g_launcher_id        = 0;
static int      g_launcher_minimized = 0;

/* ---------------- helpers ---------------- */

__attribute__((unused))
static size_t s_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

/* ---------------- rendering ---------------- */

static void draw_start_button(void)
{
    /* Pressed-in look when the launcher panel is currently
     * visible -- the same affordance as a Start button on
     * Windows.  Greys out (idle palette) when the launcher
     * doesn't exist yet (g_launcher_id == 0). */
    int pressed = (g_launcher_id != 0) && !g_launcher_minimized;
    uint32_t fill = pressed ? START_BG_ACTIVE : START_BG_IDLE;
    draw_fill_rect(&g_win.fb, START_BTN_X, START_BTN_Y,
                   START_BTN_W, START_BTN_H, fill);
    draw_hline(&g_win.fb, START_BTN_X, START_BTN_Y,
               START_BTN_W, START_BORDER);
    draw_hline(&g_win.fb, START_BTN_X, START_BTN_Y + START_BTN_H - 1,
               START_BTN_W, START_BORDER);
    draw_vline(&g_win.fb, START_BTN_X, START_BTN_Y,
               START_BTN_H, START_BORDER);
    draw_vline(&g_win.fb, START_BTN_X + START_BTN_W - 1, START_BTN_Y,
               START_BTN_H, START_BORDER);
    int tw = draw_measure_text(START_BTN_LABEL);
    int tx = START_BTN_X + (START_BTN_W - tw) / 2;
    int ty = START_BTN_Y + (START_BTN_H - GLYPH_H) / 2;
    draw_text(&g_win.fb, tx, ty, START_BTN_LABEL,
              START_FG_BGRA, fill, 0);
}

static int point_in_start_button(int cx, int cy)
{
    return cx >= START_BTN_X && cx < START_BTN_X + START_BTN_W
        && cy >= START_BTN_Y && cy < START_BTN_Y + START_BTN_H;
}

static void draw_cell(int idx, const struct wm_win_desc *info)
{
    int x = CELLS_X0 + idx * (CELL_W + CELL_GAP);
    int y = 4;
    int w = CELL_W;
    int h = BAR_H - 8;
    /* chapter 108e — when a window is minimized, draw its
     * cell in the dim CELL_MIN_* palette so the user can
     * see at a glance which apps are hidden and which are
     * visible.  The cell is still clickable; clicking a
     * minimized cell sends WM_WIN_RESTORE to wsd. */
    int is_min = (info->flags & GUI_WIN_FLAG_MINIMIZED) != 0;
    uint32_t fill   = is_min ? CELL_MIN_BGRA   : CELL_BGRA;
    uint32_t border = is_min ? CELL_MIN_BORDER : CELL_BORDER;
    uint32_t fg     = is_min ? TEXT_MIN_BGRA   : TEXT_BGRA;

    draw_fill_rect(&g_win.fb, x, y, w, h, fill);
    draw_hline(&g_win.fb, x,         y,         w, border);
    draw_hline(&g_win.fb, x,         y + h - 1, w, border);
    draw_vline(&g_win.fb, x,         y,         h, border);
    draw_vline(&g_win.fb, x + w - 1, y,         h, border);

    /* Truncate label to fit. */
    char label[32];
    int avail_w = w - 2 * 6;
    int li = 0;
    while (li < 31 && info->title[li]) {
        label[li] = info->title[li];
        label[li + 1] = '\0';
        if (draw_measure_text(label) > avail_w) {
            label[li] = '\0';
            break;
        }
        li++;
    }
    label[li] = '\0';

    int tx = x + 6;
    int ty = y + (h - GLYPH_H) / 2;
    draw_text(&g_win.fb, tx, ty, label, fg, fill, 0);
}

static void render(const struct wm_win_desc *infos, int n)
{
    /* Background. */
    draw_fill_rect(&g_win.fb, 0, 0, g_screen_w, BAR_H, BG_BGRA);
    /* 1px highlight along the top edge. */
    draw_hline(&g_win.fb, 0, 0, g_screen_w, TOP_LINE_BGRA);

    /* Refresh the launcher cache BEFORE drawing the Start
     * button so its pressed/idle state reflects the current
     * tick. */
    g_launcher_id = 0;
    g_launcher_minimized = 0;
    for (int i = 0; i < n; i++) {
        if (s_eq(infos[i].title, "launcher")) {
            g_launcher_id        = infos[i].win_id;
            g_launcher_minimized =
                (infos[i].flags & GUI_WIN_FLAG_MINIMIZED) != 0;
            break;
        }
    }
    draw_start_button();

    int n_cells = 0;
    for (int i = 0; i < n && n_cells < MAX_WINDOWS; i++) {
        const struct wm_win_desc *info = &infos[i];
        /* Skip ourselves and any ALWAYS_ON_TOP / PIN_TO_BOTTOM
         * windows: taskbars don't list taskbars, and the
         * wallpaper isn't an entry the user wants to raise.
         * Skip empty-title windows too -- they're either the
         * kernel-shadow input windows the wmclient owns or
         * apps that never called wm_set_title and have nothing
         * to label.  Skip the launcher too: the Start button
         * to our left is its single dedicated affordance, and
         * a second entry in the cell strip would be redundant
         * (and visually noisy when the launcher is hidden). */
        if (info->win_id == g_win.id) continue;
        if (info->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP) continue;
        if (info->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
        if (info->title[0] == 0) continue;
        if (s_eq(info->title, "launcher")) continue;
        draw_cell(n_cells, info);
        /* chapter 108e — remember which win_id this cell
         * paints, so the click handler can resolve a click
         * x-coordinate back to the right window. */
        g_cells[n_cells].win_id    = info->win_id;
        g_cells[n_cells].minimized =
            (info->flags & GUI_WIN_FLAG_MINIMIZED) != 0;
        n_cells++;
    }
    g_cell_count = n_cells;

    /* One damage covering the whole bar up to the clock; the
     * clock keeps its own damage call.  Includes the Start
     * button (which sits at x=8). */
    wm_window_dirty(&g_win, 0, 0, (uint32_t)g_clock_x, BAR_H);
}

/* ---------------- main loop ---------------- */

static int g_known_count = -1;       /* count from last render */
static int g_last_clock_sec = -1;    /* last second value rendered */

/* Format civil time `ct` into "HH:MM:SS" at *out (UTC). */
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
    struct timeval tv;
    int rc = gettimeofday(&tv);
    long secs_total = (rc == 0) ? (long)tv.tv_sec
                                : (long)(uptime_ms() / 1000ul);

    struct civil_time ct;
    gmtime_r((time_t)secs_total, &ct);

    char buf[9];
    format_clock(&ct, buf);

    /* Body. */
    draw_fill_rect(&g_win.fb, CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H,
                   CLOCK_BG_BGRA);
    draw_hline(&g_win.fb, CLOCK_X,             CLOCK_Y,
               CLOCK_W, CELL_BORDER);
    draw_hline(&g_win.fb, CLOCK_X,             CLOCK_Y + CLOCK_H - 1,
               CLOCK_W, CELL_BORDER);
    draw_vline(&g_win.fb, CLOCK_X,             CLOCK_Y,
               CLOCK_H, CELL_BORDER);
    draw_vline(&g_win.fb, CLOCK_X + CLOCK_W - 1, CLOCK_Y,
               CLOCK_H, CELL_BORDER);

    int tx = CLOCK_X + (CLOCK_W - draw_measure_text(buf)) / 2;
    int ty = CLOCK_Y + (CLOCK_H - GLYPH_H) / 2;
    draw_text(&g_win.fb, tx, ty, buf, CLOCK_FG_BGRA, CLOCK_BG_BGRA, 0);

    wm_window_dirty(&g_win, (uint32_t)CLOCK_X, (uint32_t)CLOCK_Y,
                    CLOCK_W, CLOCK_H);

    g_last_clock_sec = (int)secs_total;
}

static int needs_redraw(const struct wm_win_desc *infos, int n)
{
    /* Build the would-be cell list (filtered, in the order
     * render() would walk them) and compare to what we last
     * painted.  Triggers a redraw when:
     *   - a window appeared / disappeared
     *   - a window's minimized bit flipped (chapter 108e)
     *   - the order changed (a raise reorders WM_LIST)
     *   - the launcher's visibility flipped (drives the
     *     Start button's pressed/idle look) */
    uint32_t now_launcher_id        = 0;
    int      now_launcher_minimized = 0;
    struct cell_record now[MAX_WINDOWS];
    int n_now = 0;
    for (int i = 0; i < n; i++) {
        const struct wm_win_desc *info = &infos[i];
        if (s_eq(info->title, "launcher")) {
            now_launcher_id        = info->win_id;
            now_launcher_minimized =
                (info->flags & GUI_WIN_FLAG_MINIMIZED) != 0;
        }
        if (n_now >= MAX_WINDOWS) continue;
        if (info->win_id == g_win.id) continue;
        if (info->flags & GUI_WIN_FLAG_ALWAYS_ON_TOP) continue;
        if (info->flags & GUI_WIN_FLAG_PIN_TO_BOTTOM) continue;
        if (info->title[0] == 0) continue;
        if (s_eq(info->title, "launcher")) continue;
        now[n_now].win_id    = info->win_id;
        now[n_now].minimized =
            (info->flags & GUI_WIN_FLAG_MINIMIZED) != 0;
        n_now++;
    }
    if (now_launcher_id        != g_launcher_id ||
        now_launcher_minimized != g_launcher_minimized) {
        return 1;
    }
    if (n_now != g_known_count) {
        g_known_count = n_now;
        return 1;
    }
    for (int i = 0; i < n_now; i++) {
        if (now[i].win_id    != g_cells[i].win_id)    return 1;
        if (now[i].minimized != g_cells[i].minimized) return 1;
    }
    return 0;
}

int main(void)
{
    uint32_t sw = 0, sh = 0;
    if (gui_get_screen_size(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        g_screen_w = sw;
        g_screen_h = sh;
    }
    g_bar_y  = (int32_t)g_screen_h - BAR_H;
    g_clock_x = (int32_t)g_screen_w - CLOCK_W - CLOCK_PAD;

    if (wm_create_window_at(g_screen_w, BAR_H,
                            GUI_WIN_FLAG_NO_DECORATION
                            | GUI_WIN_FLAG_ALWAYS_ON_TOP,
                            (uint32_t)BAR_X, (uint32_t)g_bar_y,
                            "taskbar", &g_win) < 0) {
        write(1, "[taskbar] wm_create_window_at failed\n", 37);
        return 1;
    }
    /* wm_create_window_at already published the title to wsd
     * (since we passed it in the create call), so taskbar
     * cells from other apps will see us in WM_LIST -- which
     * is fine, taskbar's own render filter (see render())
     * skips self by win_id. */

    /* Initial paint with empty list. */
    draw_fill_rect(&g_win.fb, 0, 0, g_screen_w, BAR_H, BG_BGRA);
    draw_hline(&g_win.fb, 0, 0, g_screen_w, TOP_LINE_BGRA);
    draw_clock();
    wm_window_dirty(&g_win, 0, 0, (uint32_t)g_clock_x, BAR_H);

    struct wm_win_desc infos[MAX_WINDOWS];

    for (;;) {
        int n = wm_list_windows(infos, MAX_WINDOWS);
        if (n < 0) n = 0;
        int redraw = needs_redraw(infos, n);
        if (redraw) {
            render(infos, n);
        }

        /* Tick the clock once per WALL-clock second. */
        struct timeval tv_tick;
        long secs = (gettimeofday(&tv_tick) == 0)
                  ? (long)tv_tick.tv_sec
                  : (long)(uptime_ms() / 1000ul);
        if (redraw || (int)secs != g_last_clock_sec) {
            draw_clock();
        }

        /* chapter 108e — drain whatever pointer events the
         * kernel shadow has queued for us since the last
         * tick.  A MOUSE_DOWN with LEFT inside one of our
         * cells maps back to that cell's win_id (captured at
         * paint time in g_cells[]) and we ship a
         * WM_WIN_RESTORE.  wsd treats restore on an already-
         * visible window as a no-op, so a click on a non-
         * minimized cell is harmless (and gives the user a
         * "click to raise" affordance basically for free,
         * even though wsd's restore handler also calls
         * z_raise + gui_raise_window). */
        struct gui_event ev;
        while (wm_poll_event(&ev) > 0) {
            if (ev.type != GUI_EVENT_MOUSE_DOWN) continue;
            if (!(ev.arg2 & GUI_BTN_LEFT))       continue;
            int cx = (int)ev.arg0;
            int cy = (int)ev.arg1;

            /* Start button takes priority over the cell strip
             * (it sits to the left of where cells begin, so a
             * cell hit is impossible inside its rect, but
             * checking it first keeps the dispatch ordering
             * explicit).  Click toggles the launcher between
             * visible and hidden; if the launcher hasn't yet
             * registered with wsd (g_launcher_id == 0), the
             * click is a no-op rather than a wild-pointer RPC. */
            if (point_in_start_button(cx, cy)) {
                if (g_launcher_id == 0) {
                    printf("[taskbar] start click but launcher "
                           "not yet in WM_LIST\n");
                    continue;
                }
                if (g_launcher_minimized) {
                    printf("[taskbar] start -> show launcher "
                           "win_id=%u\n", (unsigned)g_launcher_id);
                    (void)wm_window_restore_id(g_launcher_id);
                } else {
                    printf("[taskbar] start -> hide launcher "
                           "win_id=%u\n", (unsigned)g_launcher_id);
                    (void)wm_window_minimize_id(g_launcher_id);
                }
                continue;
            }

            /* Clicks outside the cell strip (e.g. on the
             * clock) are ignored. */
            if (cy < 4 || cy >= BAR_H - 4) continue;
            if (cx < CELLS_X0)             continue;
            int idx = (cx - CELLS_X0) / (CELL_W + CELL_GAP);
            if (idx < 0 || idx >= g_cell_count) continue;
            int cell_x0 = CELLS_X0 + idx * (CELL_W + CELL_GAP);
            if (cx >= cell_x0 + CELL_W)    continue;   /* hit the gap */
            uint32_t target = g_cells[idx].win_id;
            if (target == 0 || target == g_win.id) continue;
            printf("[taskbar] cell %d clicked -> restore win_id=%u\n",
                   idx, (unsigned)target);
            (void)wm_window_restore_id(target);
        }

        sleep_ms(150);
    }
}
