/*
 * userspace/notepad/notepad.c — milestone-43 GUI text editor,
 *                                upgraded in chapters 82 + 84.
 *
 * A small but real "editor in a window" built on top of the WM
 * (chapter 48) and the virtio-input keyboard (chapter 47).
 *
 * Features:
 *   - Window with edit area + status bar at the bottom.
 *   - In-memory line buffer (256 lines × 256 cols).
 *   - Insert / backspace / Enter (line split + line join).
 *   - Vertical scrolling: cursor stays inside the visible band by
 *     adjusting `top_row` lazily during render.
 *   - Ctrl-S saves the buffer.  If the editor was launched with a
 *     path argument (e.g. `notepad /data/foo.txt`), Ctrl-S saves
 *     to that path.  If launched bare, Ctrl-S opens a modal Save
 *     As dialog (chapter 84) so the user can name the file and
 *     choose to overwrite an existing one.  Re-saves after a
 *     successful Save As go straight to the chosen path.
 *   - Ctrl-Q / ESC quits without saving (ESC inside the Save As
 *     dialog cancels the dialog instead of quitting).
 *   - Status bar shows: filename, line/total, modified flag, hints.
 *
 * Chapter 82 changes:
 *   - Default save target moved from /tmp/untitled.txt to
 *     /data/untitled.txt (now that OSFS-2 is the canonical writable
 *     mount).  Existing /tmp paths still work for ephemeral notes.
 *   - save_file calls fsync(fd) before close(fd).  The kernel's
 *     write-back block cache otherwise leaves the data sitting in
 *     RAM until the periodic flusher runs (5 s); fsync makes
 *     "Saved." status mean what users expect: it survives reboot.
 *
 * Chapter 84 changes:
 *   - Save As dialog over /data/.  Lists existing entries (so the
 *     user can see what would be overwritten), provides a text
 *     field for the new filename, and shows a "(will overwrite)"
 *     warning when the typed name matches an existing entry.
 *   - g_path_chosen tracks whether the editor knows where to
 *     save; bare-launch starts with chosen=0 so the first Ctrl-S
 *     pops the dialog.
 *   - The dialog itself lives in userspace/libgui/save_dialog.c
 *     and is linked into notepad as a separate object file.
 *     Notepad is the first multi-translation-unit userspace app
 *     in the tree; see Makefile NOTEPAD_OBJS.  The split lets
 *     future GUI apps reuse the same dialog without copy-paste.
 */
#include "../libc/syscall.h"
#include "../libgui/save_dialog.h"

#define WIN_W      720
#define WIN_H      440

#define GLYPH_W    8
#define GLYPH_H    16

#define GUTTER     6
#define STATUS_H   20    /* one row of glyphs + a little padding */

#define COLS       ((WIN_W - 2 * GUTTER) / GLYPH_W)            /* 88 */
#define ROWS       (((WIN_H) - 2 * GUTTER - STATUS_H) / GLYPH_H)

#define MAX_LINES      256
#define MAX_LINE_LEN   256
#define MAX_PATH       128

#define BG_BGRA      GUI_BGRA(0xF8, 0xF8, 0xF0)  /* warm off-white */
#define FG_BGRA      GUI_BGRA(0x10, 0x10, 0x18)
#define CUR_BGRA     GUI_BGRA(0x20, 0x60, 0xC0)  /* blue cursor */
#define STATUS_BG    GUI_BGRA(0x28, 0x40, 0x70)  /* dark blue bar */
#define STATUS_FG    GUI_BGRA(0xF0, 0xF0, 0xFF)
#define MOD_FG       GUI_BGRA(0xFF, 0xC0, 0x40)  /* amber for "*" */

/* Ctrl-letter keystrokes from virtio_input (see chapter 47). */
#define CTRL_S   0x13
#define CTRL_Q   0x11

/* ---------------- editor state ---------------- */

static char  g_lines[MAX_LINES][MAX_LINE_LEN];
static int   g_line_len[MAX_LINES];
static int   g_line_count = 1;     /* always at least one (possibly empty) line */
static int   g_cur_row = 0;
static int   g_cur_col = 0;
static int   g_top_row = 0;
static int   g_dirty   = 0;
static int   g_win_id  = -1;
static char  g_path[MAX_PATH];
static char  g_status[128];
static int   g_status_until_render = 0;   /* one-shot status overrides */

/* Path-chosen flag (chapter 84): when 0, the next Ctrl-S opens
 * the Save As dialog; when 1, Ctrl-S writes straight to g_path.
 * Bare-launch starts at 0; argv-launch starts at 1. */
static int   g_path_chosen = 0;

/* ---------------- minimal helpers ---------------- */

static size_t s_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static char *s_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return dst;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

static char *s_append(char *dst, const char *src, size_t cap)
{
    size_t i = s_strlen(dst);
    while (*src && i + 1 < cap) dst[i++] = *src++;
    dst[i] = '\0';
    return dst;
}

static void utoa(unsigned long v, char *out)
{
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = '\0';
}

/* ---------------- buffer ops ---------------- */

static void buffer_init_empty(void)
{
    for (int i = 0; i < MAX_LINES; i++) g_line_len[i] = 0;
    g_line_count = 1;
    g_lines[0][0] = '\0';
    g_cur_row = g_cur_col = g_top_row = 0;
    g_dirty = 0;
}

static void push_byte_at_eof(char c)
{
    if (g_line_count == 0) {
        g_line_count = 1; g_line_len[0] = 0; g_lines[0][0] = '\0';
    }
    int row = g_line_count - 1;
    if (c == '\n') {
        if (g_line_count >= MAX_LINES) return;
        g_line_count++;
        g_line_len[g_line_count - 1] = 0;
        g_lines[g_line_count - 1][0] = '\0';
        return;
    }
    if (c == '\r') return;   /* normalise CRLF -> LF on load */
    if (g_line_len[row] >= MAX_LINE_LEN - 1) return;   /* drop excess */
    g_lines[row][g_line_len[row]++] = c;
    g_lines[row][g_line_len[row]] = '\0';
}

static int load_file(const char *path)
{
    int fd = open(path, 0 /* O_RDONLY */);
    buffer_init_empty();
    if (fd < 0) return -1;          /* file does not exist; start blank */
    char chunk[256];
    long n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0)
        for (long i = 0; i < n; i++) push_byte_at_eof(chunk[i]);
    close(fd);
    g_dirty = 0;
    return 0;
}

/* O_WRONLY (1) | O_CREAT (0100=64) | O_TRUNC (01000=512) = 577. */
#define OPEN_WRITE_TRUNC 577

static int save_file(const char *path)
{
    int fd = open(path, OPEN_WRITE_TRUNC);
    if (fd < 0) return fd;
    for (int r = 0; r < g_line_count; r++) {
        if (g_line_len[r] > 0)
            (void)write(fd, g_lines[r], (size_t)g_line_len[r]);
        if (r != g_line_count - 1)
            (void)write(fd, "\n", 1);
    }
    /* Chapter 82 — force the kernel's write-back cache to disk
     * before we declare "saved".  Without this, all the writes
     * above are buffered in 4 KiB cache slots; close() does not
     * imply durability (fsync does).  fsync on a non-OSFS-2 fd
     * is a no-op, so this is safe even when path is /tmp/foo. */
    (void)fsync(fd);
    close(fd);
    g_dirty = 0;
    return 0;
}

static void shift_right(char *line, int from, int len_total)
{
    for (int i = len_total; i > from; i--) line[i] = line[i - 1];
}

static void shift_left(char *line, int from, int len_total)
{
    for (int i = from; i < len_total; i++) line[i] = line[i + 1];
}

static void insert_char(char c)
{
    int row = g_cur_row;
    if (g_line_len[row] >= MAX_LINE_LEN - 1) return;
    shift_right(g_lines[row], g_cur_col, g_line_len[row] + 1);
    g_lines[row][g_cur_col] = c;
    g_line_len[row]++;
    g_lines[row][g_line_len[row]] = '\0';
    g_cur_col++;
    g_dirty = 1;
}

static void newline(void)
{
    if (g_line_count >= MAX_LINES) return;
    /* Insert a new empty row right after current row, shifting the
     * tail down by one. */
    for (int r = g_line_count; r > g_cur_row + 1; r--) {
        for (int i = 0; i <= g_line_len[r - 1]; i++)
            g_lines[r][i] = g_lines[r - 1][i];
        g_line_len[r] = g_line_len[r - 1];
    }
    /* New row inherits the tail of the current row from cur_col on. */
    int new_row = g_cur_row + 1;
    int tail = g_line_len[g_cur_row] - g_cur_col;
    for (int i = 0; i < tail; i++)
        g_lines[new_row][i] = g_lines[g_cur_row][g_cur_col + i];
    g_lines[new_row][tail] = '\0';
    g_line_len[new_row] = tail;
    /* Truncate the current row at cur_col. */
    g_lines[g_cur_row][g_cur_col] = '\0';
    g_line_len[g_cur_row] = g_cur_col;
    g_line_count++;
    g_cur_row = new_row;
    g_cur_col = 0;
    g_dirty = 1;
}

static void backspace(void)
{
    if (g_cur_col > 0) {
        shift_left(g_lines[g_cur_row], g_cur_col - 1, g_line_len[g_cur_row]);
        g_line_len[g_cur_row]--;
        g_lines[g_cur_row][g_line_len[g_cur_row]] = '\0';
        g_cur_col--;
        g_dirty = 1;
        return;
    }
    if (g_cur_row == 0) return;     /* at start of buffer; nothing to do */
    /* Join with previous row. */
    int prev = g_cur_row - 1;
    int prev_len = g_line_len[prev];
    int cur_len  = g_line_len[g_cur_row];
    int can = MAX_LINE_LEN - 1 - prev_len;
    if (can < 0) can = 0;
    if (cur_len > can) cur_len = can;   /* clip if would overflow */
    for (int i = 0; i < cur_len; i++)
        g_lines[prev][prev_len + i] = g_lines[g_cur_row][i];
    g_line_len[prev] = prev_len + cur_len;
    g_lines[prev][g_line_len[prev]] = '\0';
    /* Shift remaining rows up. */
    for (int r = g_cur_row; r < g_line_count - 1; r++) {
        for (int i = 0; i <= g_line_len[r + 1]; i++)
            g_lines[r][i] = g_lines[r + 1][i];
        g_line_len[r] = g_line_len[r + 1];
    }
    g_line_count--;
    g_cur_row = prev;
    g_cur_col = prev_len;
    g_dirty = 1;
}

/* ---------------- cursor movement ---------------- */

static void cur_left(void)
{
    if (g_cur_col > 0) {
        g_cur_col--;
        return;
    }
    /* Wrap to end of previous line. */
    if (g_cur_row == 0) return;
    g_cur_row--;
    g_cur_col = g_line_len[g_cur_row];
}

static void cur_right(void)
{
    if (g_cur_col < g_line_len[g_cur_row]) {
        g_cur_col++;
        return;
    }
    /* Wrap to start of next line. */
    if (g_cur_row + 1 >= g_line_count) return;
    g_cur_row++;
    g_cur_col = 0;
}

static void cur_up(void)
{
    if (g_cur_row == 0) { g_cur_col = 0; return; }
    g_cur_row--;
    if (g_cur_col > g_line_len[g_cur_row])
        g_cur_col = g_line_len[g_cur_row];
}

static void cur_down(void)
{
    if (g_cur_row + 1 >= g_line_count) {
        g_cur_col = g_line_len[g_cur_row];
        return;
    }
    g_cur_row++;
    if (g_cur_col > g_line_len[g_cur_row])
        g_cur_col = g_line_len[g_cur_row];
}

static void cur_home(void) { g_cur_col = 0; }
static void cur_end(void)  { g_cur_col = g_line_len[g_cur_row]; }

/* ---------------- rendering ---------------- */

static void scroll_to_cursor(void)
{
    if (g_cur_row < g_top_row) g_top_row = g_cur_row;
    if (g_cur_row >= g_top_row + ROWS) g_top_row = g_cur_row - ROWS + 1;
    if (g_top_row < 0) g_top_row = 0;
}

static void render_status(void)
{
    /* The status bar lives in a STATUS_H tall strip at the bottom. */
    uint32_t y = (uint32_t)(WIN_H - STATUS_H);
    gui_fill_rect(g_win_id, 0, y, WIN_W, STATUS_H, STATUS_BG);

    char line[128];
    s_copy(line, " ", sizeof(line));
    s_append(line, g_path, sizeof(line));
    s_append(line, "  ", sizeof(line));
    char num[24]; utoa((unsigned long)(g_cur_row + 1), num);
    s_append(line, num, sizeof(line));
    s_append(line, "/", sizeof(line));
    utoa((unsigned long)g_line_count, num);
    s_append(line, num, sizeof(line));
    s_append(line, "  Ctrl-S Save  Ctrl-Q Quit", sizeof(line));
    gui_draw_text(g_win_id, GUTTER, y + 2, line, STATUS_FG, STATUS_BG, 0);

    if (g_dirty) {
        /* Modified marker on the right edge of the status bar. */
        uint32_t mx = (uint32_t)(WIN_W - GUTTER - 2 * GLYPH_W);
        gui_draw_text(g_win_id, mx, y + 2, "*", MOD_FG, STATUS_BG, 0);
    }

    if (g_status[0] && g_status_until_render > 0) {
        /* Overlay one-shot status on top of the bar. */
        gui_fill_rect(g_win_id, 0, y, WIN_W, STATUS_H, STATUS_BG);
        gui_draw_text(g_win_id, GUTTER, y + 2, g_status, MOD_FG, STATUS_BG, 0);
        g_status_until_render--;
    }
}

/* Paint the editor into the window's back-buffer.  Does NOT call
 * gui_flush — the caller is responsible for that.  Split out
 * from render() so the libgui Save As dialog can use it as a
 * "redraw what's underneath" callback without causing a flicker:
 * if this routine flushed, the user would see the bare editor
 * for one compose pass before the dialog overlay landed in the
 * back-buffer and the second flush ran.  See chapter 84. */
static void render_to_buffer(void)
{
    scroll_to_cursor();

    gui_fill_rect(g_win_id, 0, 0, WIN_W, WIN_H, BG_BGRA);

    /* Lines. */
    for (int r = 0; r < ROWS; r++) {
        int row = g_top_row + r;
        if (row >= g_line_count) break;
        uint32_t y = (uint32_t)(GUTTER + r * GLYPH_H);
        if (g_line_len[row] > 0)
            gui_draw_text(g_win_id, GUTTER, y, g_lines[row],
                          FG_BGRA, BG_BGRA, 0);
    }

    /* Cursor (block). */
    int vrow = g_cur_row - g_top_row;
    if (vrow >= 0 && vrow < ROWS) {
        int col = g_cur_col;
        if (col > COLS - 1) col = COLS - 1;        /* clip horizontally */
        uint32_t cx = (uint32_t)(GUTTER + col * GLYPH_W);
        uint32_t cy = (uint32_t)(GUTTER + vrow * GLYPH_H);
        gui_fill_rect(g_win_id, cx, cy, GLYPH_W, GLYPH_H, CUR_BGRA);
        /* Re-draw the glyph under the cursor in the bg colour so it's
         * visible against the blue block. */
        if (col < g_line_len[g_cur_row]) {
            char one[2] = { g_lines[g_cur_row][col], '\0' };
            gui_draw_text(g_win_id, cx, cy, one, BG_BGRA, CUR_BGRA, 0);
        }
    }

    render_status();
}

static void render(void)
{
    render_to_buffer();
    gui_flush(g_win_id);
}

/* ---------------- main loop ---------------- */

static void set_status(const char *s, int frames)
{
    s_copy(g_status, s, sizeof(g_status));
    g_status_until_render = frames;
}

/* Wrapper used by libgui's gui_save_dialog as the "render the
 * underlying window" callback.  It paints the editor into the
 * back-buffer but does NOT flush — the dialog will lay its
 * overlay on top and flush once at the end.  Calling gui_flush
 * here would briefly show the bare editor (without the dialog)
 * to the user every frame, producing a per-keystroke flicker.
 * See chapter 84 for the full explanation. */
static void editor_repaint_under(void *ud)
{
    (void)ud;
    render_to_buffer();
}

/* Pop the modal Save As dialog and, on confirm, save to the
 * chosen path and remember it for next time.  Used by Ctrl-S
 * when g_path_chosen is 0. */
static void run_save_as(void)
{
    char chosen[MAX_PATH];
    char leaf[MAX_PATH];
    /* Default field text: the leaf of whatever path we already
     * have (typically "untitled.txt" for a fresh editor). */
    {
        size_t n = s_strlen(g_path);
        size_t i = n;
        while (i > 0 && g_path[i - 1] != '/') i--;
        s_copy(leaf, g_path + i, sizeof(leaf));
    }
    int rc = gui_save_dialog(g_win_id, WIN_W, WIN_H,
                             "/data/", leaf,
                             editor_repaint_under, NULL,
                             chosen, sizeof(chosen));
    if (rc != 1) {
        /* User cancelled (rc=0) or dialog rejected the args
         * (rc<0).  Either way: re-paint the editor and bail. */
        render();
        return;
    }
    int wrc = save_file(chosen);
    if (wrc < 0) {
        set_status("save failed", 2);
        render();
        return;
    }
    s_copy(g_path, chosen, sizeof(g_path));
    g_path_chosen = 1;
    set_status("saved.", 2);
    spawn("/bin/notify", "saved!");
    render();
}

int main(int argc, char **argv)
{
    /* argv[0] = "/bin/notepad" by convention; argv[1] (if any) is
     * the path to open / save.  When argv[1] is provided we treat
     * the path as already-chosen and Ctrl-S saves directly; this
     * preserves the launch-from-shell workflow (and the existing
     * test_notepad.py regression).  Bare-launch starts with
     * g_path_chosen=0 so the first Ctrl-S pops the Save As dialog
     * and lets the user name the file. */
    if (argc >= 2 && argv[1] && argv[1][0]) {
        s_copy(g_path, argv[1], sizeof(g_path));
        g_path_chosen = 1;
    } else {
        s_copy(g_path, "/data/untitled.txt", sizeof(g_path));
        g_path_chosen = 0;
    }

    g_win_id = gui_create_window(WIN_W, WIN_H, "notepad");
    if (g_win_id < 0) {
        write(1, "[notepad] gui_create_window failed\n", 35);
        return 1;
    }

    if (load_file(g_path) < 0)
        set_status("(new file)", 1);

    render();

    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) { yield(); continue; }
        switch (ev.type) {
        case GUI_EVENT_CLOSE:
            gui_destroy_window(g_win_id);
            return 0;
        case GUI_EVENT_KEY: {
            /* Extended (non-ASCII) keys arrive as GUI_KEY_*
             * (0x101..) — handle these BEFORE narrowing arg0
             * to a char, otherwise GUI_KEY_LEFT (0x104) would
             * be mistaken for the ASCII byte 0x04. */
            switch (ev.arg0) {
            case GUI_KEY_LEFT:  cur_left();  render(); continue;
            case GUI_KEY_RIGHT: cur_right(); render(); continue;
            case GUI_KEY_UP:    cur_up();    render(); continue;
            case GUI_KEY_DOWN:  cur_down();  render(); continue;
            case GUI_KEY_HOME:  cur_home();  render(); continue;
            case GUI_KEY_END:   cur_end();   render(); continue;
            default: break;
            }
            char c = (char)(ev.arg0 & 0xFF);
            if (c == 0) break;
            if (c == 0x1B || c == CTRL_Q) {     /* ESC or Ctrl-Q */
                gui_destroy_window(g_win_id);
                return 0;
            }
            if (c == CTRL_S) {
                if (!g_path_chosen) {
                    /* Library call — blocks until the user picks
                     * a path or hits ESC.  See run_save_as for
                     * what happens on confirm. */
                    run_save_as();
                    break;
                }
                int rc = save_file(g_path);
                if (rc < 0) {
                    set_status("save failed", 2);
                } else {
                    set_status("saved.", 2);
                    /* Fire-and-forget toast.  /bin/notify creates
                     * its own window, sleeps ~3 s, exits.  We
                     * don't wait() — leaving notepad responsive.
                     * The kernel's exit path reaps unwaited
                     * children when the parent exits. */
                    spawn("/bin/notify", "saved!");
                }
                render();
                break;
            }
            if (c == '\r' || c == '\n') {
                newline(); render(); break;
            }
            if (c == 0x7F || c == 0x08) {
                backspace(); render(); break;
            }
            if (c == '\t') {
                /* 4 spaces — keep things simple. */
                for (int i = 0; i < 4; i++) insert_char(' ');
                render(); break;
            }
            if (c >= 0x20 && c < 0x7F) {
                insert_char(c); render();
            }
            break;
        }
        default:
            break;
        }
    }
}
