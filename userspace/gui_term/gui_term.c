/*
 * userspace/gui_term/gui_term.c — chapter 79b rewrite.
 *
 * Synopsis: gui_term is a terminal in a window.  Pre-79b it was
 * a one-shot command runner: read a line of input from the
 * window, spawn one binary against an in-memory pipe, drain the
 * pipe to EOF, render, repeat.  No interactive programs, no
 * pipelines, no Ctrl-C; ESC quit gui_term itself.
 *
 * Post-79b: gui_term spawns a real /bin/sh over a pty.  The
 * shell sees its fd 0/1/2 as the pty slave; gui_term holds the
 * master.  Keystrokes sent to the master pass through a kernel
 * line discipline (Ctrl-C => SIGINT to the shell's fg pid,
 * dropped from the byte stream); everything else is forwarded
 * verbatim.  Output written by the shell (or any of its
 * children) appears on the master read side and is rendered
 * into the window.
 *
 * What this gives us for free vs. the old design:
 *   - Every shell builtin: cd, pwd, exit, time, env, history.
 *   - Every shell pipeline: `cat /mnt/poem.txt | wc -l`.
 *   - Every redirect: `ls > /tmp/x; cat /tmp/x`.
 *   - Ctrl-C at the prompt or to a long-running command (works
 *     because thread_signal_pid was taught in this chapter to
 *     wake THREAD_BLOCKED targets so a shell sitting in
 *     pipe_read aborts with -EINTR).
 *   - Arrow-key history navigation (we translate GUI_KEY_*
 *     codes back into CSI sequences so sh's raw-mode line
 *     editor sees them).
 *
 * Limitations the chapter intentionally leaves for later:
 *   - PgUp/PgDn navigate the in-memory history ring (2048
 *     lines deep, set by HISTORY_ROWS); there is NO disk-
 *     backed scrollback yet.
 *   - No SIGTSTP / Ctrl-Z; SIGSTOP doesn't exist as a signum
 *     yet.  Lands with chapter 79.
 *   - One terminal emulator per gui_term process; no `tmux`-
 *     style multiplexing.
 */
#include "../libc/syscall.h"
#include "../libc/clipboard.h"
#include "../libgui/draw.h"
#include "../libgui/wmclient.h"

#define WIN_W        720
#define WIN_H        440

/* Font is fixed 8x16 (kernel bitmap font). */
#define GLYPH_W      8
#define GLYPH_H      16

#define GUTTER_X     8
#define GUTTER_Y     6
#define COLS         ((WIN_W - 2 * GUTTER_X) / GLYPH_W)         /* 88 */
#define HISTORY_ROWS 2048
#define VISIBLE_ROWS (((WIN_H) - 2 * GUTTER_Y - GLYPH_H) / GLYPH_H)

#define BG_BGRA      GUI_BGRA(0x10, 0x18, 0x28)
#define FG_BGRA      GUI_BGRA(0xE0, 0xF0, 0xFF)

/* Ring of completed (newline-terminated) lines.  Latest
 * VISIBLE_ROWS-1 are rendered above the in-progress current
 * line. */
static char history[HISTORY_ROWS][COLS + 1];
static int  history_count = 0;       /* monotonic */

/* The "current line" being assembled — bytes received from the
 * shell since the last '\n' / '\r'.  Rendered as the bottom row
 * of the window so the shell's prompt and the user's typed input
 * appear in real time without waiting for an Enter. */
static char cur_line[COLS + 1];
static int  cur_len = 0;

static int  win_id    = -1;
static int  master_fd = -1;
static struct wm_window g_win;

/* Scrollback offset in rows.  0 means "show the most recent
 * lines"; positive values shift the view backwards in time by
 * that many rows.  Clamped against the history ring depth and
 * total number of committed lines in render() / page_keys.
 *
 * UX rules (see PageUp/PageDown handling and the auto-snap on
 * shell output):
 *   - PgUp / PgDn adjust by (VISIBLE_ROWS - 1) rows.
 *   - Any printable / control byte the user types snaps the
 *     view back to the bottom (offset = 0) so they see what
 *     they're typing in real time.
 *   - Fresh shell output also snaps back, so a long-running
 *     command can't scribble off-screen behind the user's
 *     back. */
static int  scroll_offset = 0;

/* ---------------- terminal emulator ---------------- */

/* Push the current line into the history ring and reset.  Called
 * on '\n' and on hard column wrap. */
static void commit_current(void)
{
    cur_line[cur_len] = '\0';
    int slot = history_count % HISTORY_ROWS;
    for (int i = 0; i <= cur_len; i++) history[slot][i] = cur_line[i];
    history_count++;
    cur_len = 0;
    cur_line[0] = '\0';
}

/* Process one byte of shell output through the tiny terminal
 * emulator.  Recognises:
 *   '\n'  : finalise current line.  Whether or not a '\r'
 *           immediately preceded it, the typed/echoed content
 *           in cur_line is committed -- this is what makes the
 *           user's typed command appear in scrollback on Enter
 *           (the shell echoes the byte stream and then writes
 *           "\r\n" to terminate the line).
 *   '\r'  : carriage return.  We DON'T wipe cur_line on its
 *           own -- that would discard the just-echoed command
 *           in the "\r\n" pair the shell sends on Enter.
 *           Instead we set a flag and look at the next byte:
 *             - if it's '\n', the commit path above runs and
 *               the flag is cleared;
 *             - if it's anything else, the shell is doing the
 *               classic "carriage return + redraw" trick for
 *               in-place line editing -- we reset cur_len so
 *               the following bytes overwrite from column 0.
 *   '\b'  : non-destructive backspace (move cursor left).  sh
 *           writes "\b \b" to delete a char.
 *   '\x1b': start of an ANSI CSI escape (e.g. "\x1b[2K" to
 *           erase the line as part of redraw_line()).  We
 *           absorb the sequence through to its final byte
 *           (0x40..0x7E) so it never appears as literal
 *           garbage in cur_line.  "\x1b[2K" is handled as
 *           "clear current line".
 *   else  : append printable; hard-wrap at COLS.
 */
enum esc_state {
    ESC_NONE = 0,    /* default */
    ESC_AFTER_ESC,   /* just saw \x1b, expecting '['  */
    ESC_IN_CSI,      /* inside \x1b[ ... <final>      */
};
static int esc_state    = ESC_NONE;
static int saw_cr       = 0;
static int csi_param_n  = 0;          /* numeric value of trailing CSI param */
static int csi_has_param = 0;

static void emu_byte(uint8_t c)
{
    /* ---- ANSI CSI absorbing state machine ---- */
    if (esc_state == ESC_AFTER_ESC) {
        if (c == '[') {
            esc_state     = ESC_IN_CSI;
            csi_param_n   = 0;
            csi_has_param = 0;
        } else {
            esc_state = ESC_NONE;     /* unknown ESC X -- drop */
        }
        return;
    }
    if (esc_state == ESC_IN_CSI) {
        if (c >= '0' && c <= '9') {
            csi_param_n = csi_param_n * 10 + (c - '0');
            csi_has_param = 1;
            return;
        }
        if ((c >= 0x20 && c <= 0x2F) || c == ';') {
            return;                   /* intermediate -- ignore */
        }
        if (c >= 0x40 && c <= 0x7E) {
            /* Final byte -- act on the few sequences sh uses. */
            if (c == 'K') {
                /* ESC [ K   -> erase from cursor to end of line.
                 * ESC [ 2 K -> erase entire line.
                 * In our column-less buffer both collapse to
                 * "drop everything in cur_line". */
                cur_len      = 0;
                cur_line[0]  = '\0';
            }
            /* Other CSI finals (cursor moves, colours, etc.)
             * are silently dropped. */
            esc_state = ESC_NONE;
            return;
        }
        /* Garbage in CSI -- abort the sequence so we don't
         * eat the rest of the stream. */
        esc_state = ESC_NONE;
        return;
    }

    if (c == 0x1B) { esc_state = ESC_AFTER_ESC; return; }

    /* ---- \r / \n pair handling ---- */
    if (c == '\r') { saw_cr = 1; return; }
    if (c == '\n') {
        commit_current();
        saw_cr = 0;
        return;
    }
    if (saw_cr) {
        /* \r followed by non-\n -- shell is rewriting the
         * line in place.  Reset the buffer so the incoming
         * bytes start at column 0. */
        cur_len     = 0;
        cur_line[0] = '\0';
        saw_cr      = 0;
    }

    if (c == '\b' || c == 0x7F) {
        if (cur_len > 0) {
            cur_len--;
            cur_line[cur_len] = '\0';
        }
        return;
    }
    if (c < 0x20 && c != '\t') return;   /* drop other control bytes */
    if (cur_len >= COLS) commit_current();
    cur_line[cur_len++] = (char)c;
    cur_line[cur_len]   = '\0';
}

static void emu_bytes(const char *buf, long n)
{
    for (long i = 0; i < n; i++) emu_byte((uint8_t)buf[i]);
}

/* Push a NUL-terminated string into the emulator with an
 * implicit trailing newline.  Used for gui_term's own status
 * messages (boot banner, error reports). */
static void emu_status(const char *s)
{
    while (*s) emu_byte((uint8_t)*s++);
    emu_byte('\n');
}

/* ---------------- rendering ---------------- */

static void render(void)
{
    draw_fill_rect(&g_win.fb, 0, 0, WIN_W, WIN_H, BG_BGRA);

    /* Show last (VISIBLE_ROWS - 1) committed lines, leaving the
     * bottom row for the in-progress cur_line.  When
     * scroll_offset > 0 the view is shifted backwards by that
     * many rows so older history becomes visible. */
    int rows_to_show = VISIBLE_ROWS - 1;
    int total = history_count;

    /* Clamp scroll_offset against the actually-available history
     * (you can't scroll back past row 0, and you can't scroll
     * back past what the ring still remembers). */
    int max_back = total - rows_to_show;
    if (max_back < 0) max_back = 0;
    int max_ring = HISTORY_ROWS - rows_to_show;
    if (max_ring < 0) max_ring = 0;
    if (max_back > max_ring) max_back = max_ring;
    if (scroll_offset > max_back) scroll_offset = max_back;
    if (scroll_offset < 0) scroll_offset = 0;

    int first = total - rows_to_show - scroll_offset;
    if (first < 0) first = 0;
    int last  = first + rows_to_show;
    if (last  > total) last = total;

    for (int r = first; r < last; r++) {
        int slot = r % HISTORY_ROWS;
        uint32_t y = (uint32_t)(GUTTER_Y + (r - first) * GLYPH_H);
        draw_text(&g_win.fb, GUTTER_X, y, history[slot],
                  FG_BGRA, BG_BGRA, 1);
    }

    /* In-progress current line (shell prompt + typed input).
     * When scrolled back, hide the live cursor so the bottom
     * row of the visible window is clearly historical. */
    uint32_t cur_y = (uint32_t)(GUTTER_Y + rows_to_show * GLYPH_H);
    if (scroll_offset == 0) {
        if (cur_len > 0) {
            draw_text(&g_win.fb, GUTTER_X, cur_y, cur_line,
                      FG_BGRA, BG_BGRA, 1);
        }
        /* Block cursor at the end of cur_line. Chapter 102 --
         * the proportional kernel font means we measure the
         * rendered width rather than counting characters * 8. */
        uint32_t cur_x = (uint32_t)GUTTER_X;
        if (cur_len > 0) cur_x += (uint32_t)draw_measure_text(cur_line);
        draw_fill_rect(&g_win.fb, cur_x, cur_y, GLYPH_W, GLYPH_H, FG_BGRA);
    } else {
        /* Scrollback indicator: a dim banner at the bottom row
         * reminds the user the live shell is hidden above. */
        char banner[COLS + 1];
        const char *msg = "-- scrollback (PgDn to return) --";
        int i = 0;
        while (msg[i] && i < COLS) { banner[i] = msg[i]; i++; }
        banner[i] = '\0';
        draw_text(&g_win.fb, GUTTER_X, cur_y, banner,
                  FG_BGRA, BG_BGRA, 1);
    }

    wm_window_dirty(&g_win, 0, 0, WIN_W, WIN_H);
}

/* ---------------- key translation ---------------- */

/* Convert one GUI_EVENT_KEY arg0 into 0..3 bytes that the shell
 * expects on its raw stdin.  Printable / control-byte ASCII
 * passes through; the GUI_KEY_* extended codes for arrows etc.
 * are reconstituted as the original CSI escape sequences sh's
 * line editor parses. */
static int key_to_bytes(uint32_t key, char *out)
{
    if (key < 0x100) {
        out[0] = (char)key;
        return 1;
    }
    out[0] = 0x1B;
    out[1] = '[';
    switch (key) {
    case GUI_KEY_UP:    out[2] = 'A'; return 3;
    case GUI_KEY_DOWN:  out[2] = 'B'; return 3;
    case GUI_KEY_RIGHT: out[2] = 'C'; return 3;
    case GUI_KEY_LEFT:  out[2] = 'D'; return 3;
    case GUI_KEY_HOME:  out[2] = 'H'; return 3;
    case GUI_KEY_END:   out[2] = 'F'; return 3;
    default:            return 0;     /* unknown - drop */
    }
}

/* ---------------- main loop ---------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (wm_create_window_input(WIN_W, WIN_H, 0, "gui_term", &g_win) < 0) {
        write(1, "[gui_term] wm_create_window failed\n", 35);
        return 1;
    }
    win_id = (int)g_win.id;

    emu_status("gui_term: spawning /bin/sh...");
    render();

    /* Allocate the pty before forking so both halves of the fd
     * pair are present in the child's inherited fd table. */
    int slave_fd;
    if (openpty(&master_fd, &slave_fd) < 0) {
        emu_status("[gui_term] openpty failed");
        render();
        return 1;
    }

    int child = fork();
    if (child < 0) {
        emu_status("[gui_term] fork failed");
        render();
        close(master_fd);
        close(slave_fd);
        return 1;
    }
    if (child == 0) {
        /* Child: rewire fds 0/1/2 onto the slave end, drop the
         * master and the bare slave fd, exec /bin/sh.  Any
         * failure here just exits with a sentinel; the parent
         * sees the shell vanish via waitpid. */
        close(master_fd);
        dup2(slave_fd, 0);
        dup2(slave_fd, 1);
        dup2(slave_fd, 2);
        close(slave_fd);
        char *sh_argv[] = { (char *)"/bin/sh", 0 };
        execv("/bin/sh", sh_argv);
        exit(127);
    }

    /* Parent (gui_term): we only use the master end. */
    close(slave_fd);

    for (;;) {
        /* 1. Drain GUI events. */
        struct gui_event ev;
        while (wm_poll_event(&ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                /* Tell the shell to leave (SIGINT first; if it
                 * was sitting at the prompt that just clears
                 * the line, but the shell will then notice its
                 * stdin closed - see the master close below). */
                kill(child, SIGINT);
                close(master_fd);   /* drops m2s.w_refs to 0 ->
                                     * shell's read returns 0 ->
                                     * sh exits */
                /* Reap the shell so it doesn't become a zombie
                 * for init to mop up. */
                (void)waitpid(child, 0, 0);
                wm_destroy_window(&g_win);
                return 0;
            }
            if (ev.type == GUI_EVENT_KEY) {
                /* Scrollback navigation: PgUp / PgDn never
                 * reach the shell.  We swallow them locally
                 * and re-render with an updated offset. */
                if (ev.arg0 == GUI_KEY_PGUP) {
                    scroll_offset += VISIBLE_ROWS - 1;
                    render();
                    continue;
                }
                if (ev.arg0 == GUI_KEY_PGDN) {
                    scroll_offset -= VISIBLE_ROWS - 1;
                    if (scroll_offset < 0) scroll_offset = 0;
                    render();
                    continue;
                }
                /* Any other key is real shell input.  Snap the
                 * view to the bottom so the user sees what
                 * they're typing — same convention as xterm /
                 * iTerm2. */
                if (scroll_offset != 0) {
                    scroll_offset = 0;
                    render();
                }
                /* Chapter 108 -- Ctrl-V pastes the system
                 * clipboard onto the shell's stdin.  Ctrl-C
                 * stays SIGINT (line discipline routes 0x03
                 * to the foreground pgid via the pty); Ctrl-V
                 * was previously a no-op for the shell, so we
                 * can repurpose it without breaking anything.
                 *
                 * Newlines in the payload are translated to
                 * \r so the shell's line editor sees them as
                 * Enter -- matches the KC_ENTER mapping in
                 * virtio_input.c.  Non-printable bytes (other
                 * than the newline conversion) are dropped to
                 * keep stray control sequences from poisoning
                 * the readline state.  Write is one-shot:
                 * chapter-39 pipes / chapter-79b ptys handle a
                 * blocked-on-buffer-full case by parking the
                 * caller, which on a clipboard of <= 32 KiB
                 * resolves in a few schedule ticks. */
                if (ev.arg0 == 0x16 /* Ctrl-V */) {
                    static uint8_t cb[CLIP_DATA_MAX];
                    uint32_t cblen = 0;
                    int n = clip_get(cb, sizeof(cb), &cblen);
                    if (n >= 0 && cblen > 0) {
                        static uint8_t out_buf[CLIP_DATA_MAX];
                        uint32_t out_len = 0;
                        for (uint32_t i = 0; i < cblen; i++) {
                            uint8_t b = cb[i];
                            if (b == '\n') b = '\r';
                            /* Allow \r (Enter), \t (tab), and
                             * printable ASCII through. */
                            if (b == '\r' || b == '\t' ||
                                (b >= 0x20 && b < 0x7F)) {
                                out_buf[out_len++] = b;
                            }
                        }
                        uint32_t off = 0;
                        while (off < out_len) {
                            long w = write(master_fd, out_buf + off,
                                           out_len - off);
                            if (w <= 0) break;
                            off += (uint32_t)w;
                        }
                        /* Audit line for the cross-app paste
                         * regression -- inherits stdout from the
                         * outer (serial-attached) shell, so this
                         * is the only signal a host-side test
                         * can grep for to confirm the keystroke
                         * actually reached gui_term and pasted
                         * the expected payload. */
                        char hdr[64];
                        const char *p = "[gui_term] pasted ";
                        int hi = 0;
                        while (p[hi] && hi < (int)sizeof(hdr) - 12) {
                            hdr[hi] = p[hi]; hi++;
                        }
                        /* small itoa for out_len */
                        char num[16]; int ni = 0;
                        if (out_len == 0) num[ni++] = '0';
                        else {
                            char tmp[16]; int ti = 0;
                            uint32_t v = out_len;
                            while (v && ti < 15) {
                                tmp[ti++] = '0' + (char)(v % 10);
                                v /= 10;
                            }
                            while (ti > 0) num[ni++] = tmp[--ti];
                        }
                        for (int j = 0; j < ni && hi < (int)sizeof(hdr) - 8; j++)
                            hdr[hi++] = num[j];
                        const char *t = " bytes\n";
                        for (int j = 0; t[j] && hi < (int)sizeof(hdr); j++)
                            hdr[hi++] = t[j];
                        (void)write(1, hdr, (size_t)hi);
                    } else {
                        const char *err = "[gui_term] paste empty/failed\n";
                        int el = 0; while (err[el]) el++;
                        (void)write(1, err, (size_t)el);
                    }
                    continue;
                }
                char buf[3];
                int  n = key_to_bytes(ev.arg0, buf);
                if (n > 0) (void)write(master_fd, buf, (size_t)n);
            }
        }

        /* 2. Drain shell output (non-blocking on the master).
         * Loop until master_fd has nothing left so a flurry of
         * output (e.g. `ls /bin`) doesn't lose bytes between
         * yields.  Fresh output also snaps the view back to
         * the bottom: a `make` running in the background can't
         * scribble unread data off-screen. */
        int painted = 0;
        for (;;) {
            char buf[256];
            long n = read(master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            emu_bytes(buf, n);
            painted = 1;
        }
        if (painted && scroll_offset != 0) scroll_offset = 0;

        /* 3. Reap the shell if it has exited. */
        int status = 0;
        int reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child) {
            close(master_fd);
            wm_destroy_window(&g_win);
            return 0;
        }

        if (painted) render();
        yield();
    }
}
