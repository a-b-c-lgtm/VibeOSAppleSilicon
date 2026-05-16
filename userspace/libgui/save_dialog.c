/*
 * userspace/libgui/save_dialog.c — implementation of the modal
 * Save As dialog widget.  See save_dialog.h for the contract.
 *
 * Internals
 * ---------
 *
 * One dialog instance per call.  All state (dir list, field
 * buffer, selection, scroll position) lives in static buffers
 * inside this file — there's never more than one Save As
 * dialog open at a time across the whole process, and the
 * dialog runs to completion before returning, so a static
 * pool is fine and avoids the heap dependency.
 *
 * Layout (relative to the caller's window, centred):
 *
 *     +-- Save As ----------------------+
 *     | Save in: <current_dir>          |
 *     |                                 |
 *     | +---------------------------+   |  file list
 *     | | ..                  <DIR> |   |  (directories first,
 *     | | notes/              <DIR> |   |   then files; '..'
 *     | | foo.txt              42 b |   |   appears whenever
 *     | | bar.txt              17 b |   |   we're not at the
 *     | +---------------------------+   |   floor passed in
 *     |                                 |   by the caller.)
 *     | Filename: [____________]        |  text-entry field
 *     |                                 |
 *     | (will overwrite)                |  appears iff the field
 *     |                                 |  matches a list entry
 *     | Up/Down: pick   Enter: save     |
 *     | Ctrl-N: new folder   ESC: cancel|
 *     +---------------------------------+
 *
 * Input model
 * -----------
 *
 * The caller's event loop is paused for the dialog's lifetime;
 * we run our own gui_poll_event / yield() loop here.  All
 * keystrokes go to the field by default; Up/Down navigate the
 * list (which auto-fills the field with the highlighted file
 * name — directory rows leave the field alone so the user can
 * keep their typed filename while browsing); Enter on a
 * directory row navigates into it; Enter on a file row (or
 * with a typed filename) confirms; ESC cancels.
 *
 * Render order each frame:
 *   1. render_under(ud)  — caller paints the underlying window
 *   2. dialog panel + frame
 *   3. file list rows
 *   4. text-entry field + cursor
 *   5. overwrite warning (if applicable)
 *   6. hint line
 *   7. gui_flush(win_id)
 *
 * The first step is critical: without it, anything behind the
 * dialog (a status bar timer, a blinking cursor) would freeze
 * the moment the dialog opened.  Forcing the caller to provide
 * the callback also keeps the widget app-agnostic — we don't
 * know what notepad or paint or any future caller wants behind
 * us.
 *
 * Chapter 85 — directory navigation
 * ---------------------------------
 *
 * The dialog now knows about subdirectories under /data/:
 *
 *   - We track a `current_dir` separately from the `root_dir`
 *     the caller passed in.  We never navigate above root.
 *   - The list is built via listdir_at(current_dir, …) so each
 *     entry comes with a type tag (FILE / DIR).  Directories
 *     are listed first and rendered with a "<DIR>" suffix.
 *   - A synthesized ".." row appears at the top of the list
 *     whenever current_dir != root_dir.  Selecting+Enter on
 *     it pops the last path component.
 *   - Selecting+Enter on a DIR row pushes that name onto
 *     current_dir and refreshes the list.
 *   - Ctrl-N (0x0E) enters "new folder" mode: the field
 *     becomes a folder-name field, the hint flips, and the
 *     next Enter calls mkdir(current_dir + field) and refreshes.
 *     ESC during new-folder mode cancels just the mode (not
 *     the whole dialog).
 *
 * No '.' or '..' entries are stored in the on-disk OSFS-2
 * directory format (chapter 85 doc); the dialog synthesizes the
 * ".." row purely as UI candy.
 */

#include "save_dialog.h"
#include "../libc/syscall.h"

/* ---------------- sizing ---------------- */

#define GLYPH_W    8
#define GLYPH_H    16

#define DLG_W   500
#define DLG_H   340
#define DLG_TITLE_H   22
#define DLG_PAD       10

#define DIR_LIST_VISIBLE   10
#define DLG_LIST_H         (DIR_LIST_VISIBLE * GLYPH_H)

#define DLG_FIELD_H        (GLYPH_H + 6)

/* Hard caps that match the rest of the system.  NAME_FIELD_MAX
 * is 64 bytes — comfortably more than OSFS-2's 60-byte name
 * limit (chapter 81), with room for a trailing NUL.
 *
 * MAX_DIR is the longest current_dir we can hold; deep enough
 * for any reasonable hierarchy under /data/ and bounded so we
 * don't stack-overflow when copying it around. */
#define NAME_FIELD_MAX     64
#define DIR_LIST_MAX       64
#define MAX_DIR           128

/* ---------------- colours ---------------- */

#define DLG_BG       GUI_BGRA(0xF0, 0xF0, 0xF4)  /* light grey panel */
#define DLG_FRAME    GUI_BGRA(0x30, 0x40, 0x70)  /* navy frame */
#define DLG_TITLE_BG GUI_BGRA(0x30, 0x40, 0x70)
#define DLG_TITLE_FG GUI_BGRA(0xFF, 0xFF, 0xFF)
#define DLG_FG       GUI_BGRA(0x10, 0x10, 0x18)
#define DLG_FIELD_BG GUI_BGRA(0xFF, 0xFF, 0xFF)
#define DLG_LIST_BG  GUI_BGRA(0xFF, 0xFF, 0xFF)
#define DLG_SEL_BG   GUI_BGRA(0xC0, 0xD0, 0xF0)  /* light blue highlight */
#define DLG_WARN_FG  GUI_BGRA(0xC0, 0x40, 0x10)  /* red-ish for overwrite */
#define DLG_DIM      GUI_BGRA(0x80, 0x80, 0x80)
#define DLG_CUR_BG   GUI_BGRA(0x20, 0x60, 0xC0)  /* blue field cursor */
#define DLG_DIR_FG   GUI_BGRA(0x20, 0x40, 0x80)  /* darker blue for dir rows */
#define DLG_NF_BG    GUI_BGRA(0xFF, 0xF0, 0xC8)  /* warm cream — new folder mode */

/* ---------------- per-call state ---------------- */

/* Floor: never navigate above this.  Always matches the
 * dir_prefix arg passed in by the caller (e.g. "/data/").  Always
 * has a trailing slash. */
static char         g_root[MAX_DIR];
static int          g_root_len;

/* Current directory the dialog is showing.  Always has a trailing
 * slash; equals g_root at start; grows when the user enters a
 * subdir, shrinks when they hit "..". */
static char         g_dir[MAX_DIR];
static int          g_dir_len;

/* Pseudo-entry for ".." \u2014 only included in g_entries when we're
 * deeper than g_root. */
#define ENTRY_PARENT 1
#define ENTRY_DIR    2
#define ENTRY_FILE   3

static char         g_entries[DIR_LIST_MAX][NAME_FIELD_MAX];
static unsigned int g_sizes[DIR_LIST_MAX];
static int          g_kinds[DIR_LIST_MAX];   /* one of ENTRY_PARENT / DIR / FILE */
static int          g_count;
static int          g_sel;       /* -1 = nothing highlighted */
static int          g_top;       /* topmost visible row */

static char         g_field[NAME_FIELD_MAX];
static int          g_field_len;
static int          g_field_cur;

#define MODE_NORMAL     0
#define MODE_NEW_FOLDER 1
static int          g_mode;

/* When the user navigated to the highlighted row (vs typed into
 * the field), we set this so the next Enter on a dir row counts
 * as "navigate" rather than "save with typed name".  Cleared by
 * any field edit. */
static int          g_field_from_selection;

/* ---------------- tiny string helpers ---------------- */

static size_t s_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void s_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void s_append(char *dst, const char *src, size_t cap)
{
    size_t i = s_strlen(dst);
    while (*src && i + 1 < cap) dst[i++] = *src++;
    dst[i] = '\0';
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

static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

/* ---------------- file-list machinery ---------------- */

/* Strip the trailing '/' from g_dir into out (so listdir_at gets
 * "/data/notes" instead of "/data/notes/" \u2014 the kernel accepts
 * both for the root, but rejects double-slashes mid-path on
 * deeper paths). */
static void dir_for_listdir(char *out, size_t cap)
{
    s_copy(out, g_dir, cap);
    int n = (int)s_strlen(out);
    if (n > 1 && out[n - 1] == '/') out[n - 1] = '\0';
}

/* Walk listdir_at(current_dir) and populate g_entries, putting
 * directories first.  Inserts a leading ".." entry whenever we're
 * deeper than the floor. */
static void refresh_list(void)
{
    g_count = 0;

    /* Synthesised ".." row \u2014 only when we're below the floor. */
    if (g_dir_len > g_root_len) {
        s_copy(g_entries[g_count], "..", sizeof(g_entries[0]));
        g_kinds[g_count] = ENTRY_PARENT;
        g_sizes[g_count] = 0;
        g_count++;
    }

    char dir_for_call[MAX_DIR];
    dir_for_listdir(dir_for_call, sizeof(dir_for_call));

    /* Two passes: first the directories, then the files.  Keeps
     * the visual ordering predictable (folders on top, like the
     * 1990s dialogs every desktop OS still imitates). */
    for (int pass = 0; pass < 2; pass++) {
        unsigned int want = (pass == 0)
            ? LISTDIR_TYPE_DIR : LISTDIR_TYPE_FILE;
        char         name[128];
        unsigned int size = 0;
        unsigned int type = 0;
        for (int idx = 0; idx < 1024; idx++) {
            long n = listdir_at(dir_for_call, idx, name, sizeof(name),
                                &size, &type);
            if (n < 0) break;
            if (type != want) continue;
            if (g_count >= DIR_LIST_MAX) return;
            s_copy(g_entries[g_count], name, sizeof(g_entries[0]));
            g_kinds[g_count] = (type == LISTDIR_TYPE_DIR)
                ? ENTRY_DIR : ENTRY_FILE;
            g_sizes[g_count] = size;
            g_count++;
        }
    }
}

/* Returns the index of the FILE entry whose name equals the
 * current field contents, or -1 if none.  Used both to decide
 * whether to show "(will overwrite)" and to keep the list
 * highlight in sync with what the user is typing. */
static int find_match(void)
{
    if (g_field_len == 0) return -1;
    for (int i = 0; i < g_count; i++) {
        if (g_kinds[i] != ENTRY_FILE) continue;
        if (s_eq(g_entries[i], g_field)) return i;
    }
    return -1;
}

static void select_row(int new_sel)
{
    if (g_count == 0) return;
    if (new_sel < 0) new_sel = 0;
    if (new_sel >= g_count) new_sel = g_count - 1;
    g_sel = new_sel;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + DIR_LIST_VISIBLE)
        g_top = g_sel - DIR_LIST_VISIBLE + 1;
    /* Auto-fill field from a FILE row; for ".." or DIR rows
     * leave the field alone so the user can keep their typed
     * filename while browsing.  This matches the convention
     * every native Save As dialog has had since the 1990s. */
    if (g_kinds[g_sel] == ENTRY_FILE) {
        s_copy(g_field, g_entries[g_sel], sizeof(g_field));
        g_field_len = (int)s_strlen(g_field);
        g_field_cur = g_field_len;
        g_field_from_selection = 1;
    }
}

/* ---------------- field editing ---------------- */

static void field_insert(char c)
{
    if (g_field_len + 1 >= NAME_FIELD_MAX) return;
    for (int i = g_field_len; i > g_field_cur; i--)
        g_field[i] = g_field[i - 1];
    g_field[g_field_cur] = c;
    g_field_len++;
    g_field_cur++;
    g_field[g_field_len] = '\0';
    g_field_from_selection = 0;
}

static void field_backspace(void)
{
    if (g_field_cur == 0) return;
    for (int i = g_field_cur - 1; i < g_field_len; i++)
        g_field[i] = g_field[i + 1];
    g_field_len--;
    g_field_cur--;
    g_field[g_field_len] = '\0';
    g_field_from_selection = 0;
}

/* ---------------- directory stack ---------------- */

/* Append "leaf/" onto g_dir.  Caller is expected to have checked
 * that there's room (leaf <= NAME_FIELD_MAX, dir+leaf+1 < MAX_DIR). */
static void dir_push(const char *leaf)
{
    int n = (int)s_strlen(leaf);
    if (g_dir_len + n + 2 >= MAX_DIR) return;
    for (int i = 0; i < n; i++) g_dir[g_dir_len + i] = leaf[i];
    g_dir[g_dir_len + n]     = '/';
    g_dir[g_dir_len + n + 1] = '\0';
    g_dir_len += n + 1;
}

/* Pop the trailing path component.  Refuses to go above g_root. */
static void dir_pop(void)
{
    if (g_dir_len <= g_root_len) return;
    int i = g_dir_len - 2;        /* skip trailing '/' */
    while (i >= g_root_len && g_dir[i] != '/') i--;
    if (i < g_root_len) i = g_root_len - 1;
    g_dir[i + 1] = '\0';
    g_dir_len = i + 1;
}

/* ---------------- rendering ---------------- */

static void render(int win_id, int win_w, int win_h,
                   gui_render_cb render_under, void *ud)
{
    /* 1. Underlying window. */
    if (render_under) render_under(ud);

    int dlg_x = (win_w - DLG_W) / 2;
    int dlg_y = (win_h - DLG_H) / 2;

    /* 2. Frame: 2px navy border around panel. */
    gui_fill_rect(win_id, (uint32_t)(dlg_x - 2), (uint32_t)(dlg_y - 2),
                  DLG_W + 4, DLG_H + 4, DLG_FRAME);
    /* Title bar. */
    gui_fill_rect(win_id, (uint32_t)dlg_x, (uint32_t)dlg_y,
                  DLG_W, DLG_TITLE_H, DLG_TITLE_BG);
    const char *title = (g_mode == MODE_NEW_FOLDER)
        ? "Save As — New Folder"
        : "Save As";
    gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD),
                  (uint32_t)(dlg_y + 4),
                  title, DLG_TITLE_FG, DLG_TITLE_BG, 0);
    /* Body background. */
    gui_fill_rect(win_id, (uint32_t)dlg_x, (uint32_t)(dlg_y + DLG_TITLE_H),
                  DLG_W, DLG_H - DLG_TITLE_H, DLG_BG);

    int y = dlg_y + DLG_TITLE_H + DLG_PAD;

    /* "Save in: <current dir>" line. */
    char where[160];
    s_copy(where, "Save in: ", sizeof(where));
    s_append(where, g_dir, sizeof(where));
    gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)y,
                  where, DLG_FG, DLG_BG, 0);
    y += GLYPH_H + 6;

    /* 3. File list box. */
    int list_x = dlg_x + DLG_PAD;
    int list_y = y;
    int list_w = DLG_W - 2 * DLG_PAD;
    int list_h = DLG_LIST_H + 4;
    gui_fill_rect(win_id, (uint32_t)list_x, (uint32_t)list_y,
                  list_w, list_h, DLG_LIST_BG);
    /* 1px frame around list. */
    gui_fill_rect(win_id, (uint32_t)list_x, (uint32_t)list_y,
                  list_w, 1, DLG_DIM);
    gui_fill_rect(win_id, (uint32_t)list_x,
                  (uint32_t)(list_y + list_h - 1), list_w, 1, DLG_DIM);
    gui_fill_rect(win_id, (uint32_t)list_x, (uint32_t)list_y,
                  1, list_h, DLG_DIM);
    gui_fill_rect(win_id, (uint32_t)(list_x + list_w - 1),
                  (uint32_t)list_y, 1, list_h, DLG_DIM);

    if (g_count == 0) {
        gui_draw_text(win_id, (uint32_t)(list_x + 6), (uint32_t)(list_y + 4),
                      "(empty -- type a name below)",
                      DLG_DIM, DLG_LIST_BG, 0);
    } else {
        for (int i = 0; i < DIR_LIST_VISIBLE; i++) {
            int idx = g_top + i;
            if (idx >= g_count) break;
            int row_y = list_y + 2 + i * GLYPH_H;
            uint32_t row_bg = DLG_LIST_BG;
            if (idx == g_sel) {
                row_bg = DLG_SEL_BG;
                gui_fill_rect(win_id, (uint32_t)(list_x + 1),
                              (uint32_t)(row_y - 1),
                              list_w - 2, GLYPH_H, DLG_SEL_BG);
            }
            uint32_t row_fg = (g_kinds[idx] == ENTRY_FILE) ? DLG_FG : DLG_DIR_FG;
            gui_draw_text(win_id, (uint32_t)(list_x + 6), (uint32_t)row_y,
                          g_entries[idx], row_fg, row_bg, 0);
            const char *suffix = NULL;
            char        sz[40];
            if (g_kinds[idx] == ENTRY_PARENT) {
                suffix = "<UP>";
            } else if (g_kinds[idx] == ENTRY_DIR) {
                suffix = "<DIR>";
            } else {
                char num[24]; utoa((unsigned long)g_sizes[idx], num);
                s_copy(sz, num, sizeof(sz));
                s_append(sz, " b", sizeof(sz));
                suffix = sz;
            }
            int sz_chars = (int)s_strlen(suffix);
            int sz_x = list_x + list_w - 6 - sz_chars * GLYPH_W;
            gui_draw_text(win_id, (uint32_t)sz_x, (uint32_t)row_y,
                          suffix, DLG_DIM, row_bg, 0);
        }
    }
    y = list_y + list_h + 12;

    /* 4. "Filename:" or "Folder:" label + field. */
    const char *label = (g_mode == MODE_NEW_FOLDER) ? "Folder:  " : "Filename:";
    gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)y,
                  label, DLG_FG, DLG_BG, 0);
    int field_x = dlg_x + DLG_PAD + 10 * GLYPH_W;
    int field_y = y - 2;
    int field_w = DLG_W - DLG_PAD - (field_x - dlg_x);
    uint32_t fbg = (g_mode == MODE_NEW_FOLDER) ? DLG_NF_BG : DLG_FIELD_BG;
    gui_fill_rect(win_id, (uint32_t)field_x, (uint32_t)field_y,
                  field_w, DLG_FIELD_H, fbg);
    gui_fill_rect(win_id, (uint32_t)field_x, (uint32_t)field_y,
                  field_w, 1, DLG_DIM);
    gui_fill_rect(win_id, (uint32_t)field_x,
                  (uint32_t)(field_y + DLG_FIELD_H - 1),
                  field_w, 1, DLG_DIM);
    gui_fill_rect(win_id, (uint32_t)field_x, (uint32_t)field_y,
                  1, DLG_FIELD_H, DLG_DIM);
    gui_fill_rect(win_id, (uint32_t)(field_x + field_w - 1),
                  (uint32_t)field_y, 1, DLG_FIELD_H, DLG_DIM);
    gui_draw_text(win_id, (uint32_t)(field_x + 4), (uint32_t)(field_y + 3),
                  g_field, DLG_FG, fbg, 0);
    /* Field cursor (block). */
    int cur_px = field_x + 4 + g_field_cur * GLYPH_W;
    if (cur_px < field_x + field_w - GLYPH_W) {
        gui_fill_rect(win_id, (uint32_t)cur_px, (uint32_t)(field_y + 3),
                      GLYPH_W, GLYPH_H, DLG_CUR_BG);
        if (g_field_cur < g_field_len) {
            char one[2] = { g_field[g_field_cur], '\0' };
            gui_draw_text(win_id, (uint32_t)cur_px, (uint32_t)(field_y + 3),
                          one, fbg, DLG_CUR_BG, 0);
        }
    }
    y = field_y + DLG_FIELD_H + 12;

    /* 5. Overwrite warning (only meaningful in normal save mode). */
    if (g_mode == MODE_NORMAL && find_match() >= 0) {
        gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)y,
                      "(will overwrite)", DLG_WARN_FG, DLG_BG, 0);
    }

    /* 6. Hint lines at the bottom of the dialog. */
    int hint2_y = dlg_y + DLG_H - GLYPH_H - DLG_PAD;
    int hint1_y = hint2_y - GLYPH_H;
    if (g_mode == MODE_NEW_FOLDER) {
        gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)hint1_y,
                      "Type folder name, then Enter to create.",
                      DLG_DIM, DLG_BG, 0);
        gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)hint2_y,
                      "ESC: cancel new-folder",
                      DLG_DIM, DLG_BG, 0);
    } else {
        gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)hint1_y,
                      "Up/Down: pick   Enter: open dir / save",
                      DLG_DIM, DLG_BG, 0);
        gui_draw_text(win_id, (uint32_t)(dlg_x + DLG_PAD), (uint32_t)hint2_y,
                      "Ctrl-N: new folder   ESC: cancel",
                      DLG_DIM, DLG_BG, 0);
    }

    /* 7. Flush. */
    gui_flush(win_id);
}

/* ---------------- public entry point ---------------- */

int gui_save_dialog(int win_id,
                    int win_w, int win_h,
                    const char *dir_prefix,
                    const char *initial_name,
                    gui_render_cb render_under, void *ud,
                    char *out_path, size_t cap)
{
    if (win_id < 0 || cap < 32 || !dir_prefix || !out_path) return -1;
    int n = (int)s_strlen(dir_prefix);
    if (n == 0 || n >= MAX_DIR) return -1;
    if (dir_prefix[n - 1] != '/') return -1;

    /* Initialise per-call state. */
    s_copy(g_root, dir_prefix, sizeof(g_root));
    g_root_len = n;
    s_copy(g_dir, dir_prefix, sizeof(g_dir));
    g_dir_len = n;
    g_mode = MODE_NORMAL;
    g_field_from_selection = 0;
    refresh_list();
    if (initial_name && initial_name[0])
        s_copy(g_field, initial_name, sizeof(g_field));
    else
        g_field[0] = '\0';
    g_field_len = (int)s_strlen(g_field);
    g_field_cur = g_field_len;
    g_sel = find_match();
    g_top = 0;
    if (g_sel >= DIR_LIST_VISIBLE)
        g_top = g_sel - DIR_LIST_VISIBLE + 1;

    /* First paint. */
    render(win_id, win_w, win_h, render_under, ud);

    /* Modal event loop. */
    for (;;) {
        struct gui_event ev;
        if (!gui_poll_event(&ev)) { yield(); continue; }
        if (ev.type == GUI_EVENT_CLOSE) {
            /* Window itself is closing — treat as cancel; the
             * caller will see win_id is gone shortly. */
            return 0;
        }
        if (ev.type != GUI_EVENT_KEY) continue;

        int dirty = 0;
        switch (ev.arg0) {
        case GUI_KEY_UP:    select_row(g_sel - 1); dirty = 1; break;
        case GUI_KEY_DOWN:
            if (g_sel < 0) select_row(0);
            else           select_row(g_sel + 1);
            dirty = 1;
            break;
        case GUI_KEY_LEFT:
            if (g_field_cur > 0) g_field_cur--;
            dirty = 1;
            break;
        case GUI_KEY_RIGHT:
            if (g_field_cur < g_field_len) g_field_cur++;
            dirty = 1;
            break;
        case GUI_KEY_HOME:  g_field_cur = 0;          dirty = 1; break;
        case GUI_KEY_END:   g_field_cur = g_field_len; dirty = 1; break;
        default: {
            char c = (char)(ev.arg0 & 0xFF);
            if (c == 0) break;
            if (c == 0x1B) {                /* ESC */
                if (g_mode == MODE_NEW_FOLDER) {
                    /* Cancel just the new-folder prompt; keep
                     * the dialog open with the field cleared
                     * and the list intact. */
                    g_mode = MODE_NORMAL;
                    g_field[0] = '\0';
                    g_field_len = 0;
                    g_field_cur = 0;
                    dirty = 1;
                    break;
                }
                return 0;
            }
            if (c == 0x0E) {                /* Ctrl-N \u2014 new folder */
                if (g_mode == MODE_NORMAL) {
                    g_mode = MODE_NEW_FOLDER;
                    g_field[0] = '\0';
                    g_field_len = 0;
                    g_field_cur = 0;
                    g_sel = -1;
                    dirty = 1;
                }
                break;
            }
            if (c == '\r' || c == '\n') {   /* Enter */
                if (g_mode == MODE_NEW_FOLDER) {
                    if (g_field_len == 0) break;
                    /* Reject '/' in folder names. */
                    int bad = 0;
                    for (int i = 0; i < g_field_len; i++) {
                        if (g_field[i] == '/') { bad = 1; break; }
                    }
                    if (bad) break;
                    char path[MAX_DIR + NAME_FIELD_MAX];
                    s_copy(path, g_dir, sizeof(path));
                    s_append(path, g_field, sizeof(path));
                    int rc = mkdir(path);
                    if (rc != 0) {
                        /* Bounce out of new-folder mode but keep
                         * the dialog open so the user can try
                         * another name (or just save into the
                         * current dir).  We don't surface the
                         * errno today \u2014 a future iteration could
                         * add a status line. */
                        g_mode = MODE_NORMAL;
                        g_field[0] = '\0';
                        g_field_len = 0;
                        g_field_cur = 0;
                        dirty = 1;
                        break;
                    }
                    /* Success \u2014 leave the dialog at the new dir
                     * so the user can save into it directly. */
                    if (g_dir_len + g_field_len + 2 < MAX_DIR) {
                        dir_push(g_field);
                    }
                    g_mode = MODE_NORMAL;
                    g_field[0] = '\0';
                    g_field_len = 0;
                    g_field_cur = 0;
                    refresh_list();
                    g_sel = (g_count > 0) ? 0 : -1;
                    g_top = 0;
                    g_field_from_selection = 0;
                    dirty = 1;
                    break;
                }
                /* Normal save mode. */
                /* If the highlighted row is "..", navigate up. */
                if (g_sel >= 0 && g_kinds[g_sel] == ENTRY_PARENT &&
                    g_field_from_selection == 0 && g_field_len == 0) {
                    dir_pop();
                    refresh_list();
                    g_sel = (g_count > 0) ? 0 : -1;
                    g_top = 0;
                    dirty = 1;
                    break;
                }
                /* If the highlighted row is a directory and the
                 * user hasn't typed a custom filename, navigate
                 * into it. */
                if (g_sel >= 0 && g_kinds[g_sel] == ENTRY_DIR &&
                    (g_field_len == 0 ||
                     (g_field_from_selection == 0 &&
                      s_eq(g_field, g_entries[g_sel])))) {
                    dir_push(g_entries[g_sel]);
                    refresh_list();
                    g_field[0] = '\0';
                    g_field_len = 0;
                    g_field_cur = 0;
                    g_sel = (g_count > 0) ? 0 : -1;
                    g_top = 0;
                    g_field_from_selection = 0;
                    dirty = 1;
                    break;
                }
                if (g_field_len == 0) {
                    /* Empty filename — treat as no-op so user
                     * can keep typing. */
                    break;
                }
                /* Reject names containing '/' — the dialog
                 * navigates between dirs explicitly; users can't
                 * smuggle a path component into the filename. */
                int bad = 0;
                for (int i = 0; i < g_field_len; i++) {
                    if (g_field[i] == '/') { bad = 1; break; }
                }
                if (bad) break;
                /* Build the full path. */
                if ((size_t)(g_dir_len + g_field_len + 1) > cap) return -1;
                s_copy(out_path, g_dir, cap);
                s_append(out_path, g_field, cap);
                return 1;
            }
            if (c == 0x7F || c == 0x08) {   /* Backspace */
                field_backspace();
                g_sel = find_match();
                dirty = 1;
                break;
            }
            if (c >= 0x20 && c < 0x7F) {
                field_insert(c);
                g_sel = find_match();
                dirty = 1;
            }
            break;
        }
        }

        if (dirty) {
            render(win_id, win_w, win_h, render_under, ud);
        }
    }
}
