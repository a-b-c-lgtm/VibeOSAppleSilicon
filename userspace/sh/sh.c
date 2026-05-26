/* userspace/sh/sh.c — line-mode shell with PATH-style lookup,
 * env vars, time builtin, cwd builtins.
 *
 * Read a line from stdin (kernel does cooked-mode echo for us),
 * trim whitespace, split first token off as the program name and
 * the rest as the args string.  Bare command names (no leading
 * '/') are resolved by walking the PATH env var: each ':'-
 * separated entry is tried as a prefix until one yields a file
 * that opens successfully.  If PATH is unset we fall back to a
 * literal "/bin/" prepend.
 *
 * Built-ins (checked in order, all before the spawn path):
 *   exit [code]      — exit the shell
 *   help             — list built-ins and known programs
 *   pwd              — print cwd
 *   cd [dir]         — change cwd, default "/"
 *   export KEY=VAL   — set env var
 *   unset KEY        — remove env var
 *   time [cmd]       — either print uptime or time a child
 *
 * Splitting the args string into argv[] for the child happens
 * kernel-side inside SYS_SPAWN.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/env.h"

#define LINE_MAX 128
#define PATH_MAX 96

static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

static long parse_uint(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* Strip trailing newline / spaces. */
static int trim(char *line, int len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '  || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    /* Also strip leading whitespace. */
    int start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    if (start > 0) {
        int j = 0;
        while (line[start + j]) { line[j] = line[start + j]; j++; }
        line[j] = '\0';
        len -= start;
    }
    return len;
}

static void prompt(void)
{
    char cwd[96];
    long n = __sys_getcwd(cwd, sizeof(cwd));
    if (n > 0) {
        /* __sys_getcwd returns bytes including NUL; subtract one. */
        write(1, cwd, (size_t)(n - 1));
    }
    write(1, "$ ", 2);
}

static void print_help(void)
{
    puts("built-ins:");
    puts("  exit [code]      exit the shell");
    puts("  help             this message");
    puts("  time <cmd>       measure wall-clock duration of <cmd>");
    puts("  time             print current uptime in seconds");
    puts("  cd <dir>         change current working directory");
    puts("  pwd              print current working directory");
    puts("  export KEY=VAL   set an env variable");
    puts("  unset KEY        remove an env variable");
    puts("valid directories: /  /bin  /mnt");
    puts("variables expand: $VAR, ${VAR}, $? (last exit code)");
    puts("quoting        : 'literal'  \"$still expanded\"");
    puts("redirection    : cmd < FILE  (route stdin from FILE)");
    puts("                 cmd > /tmp/F   (truncate, route stdout to tmpfs)");
    puts("                 cmd >> /tmp/F  (append, route stdout to tmpfs)");
    puts("pipelines      : cmd1 | cmd2 [| cmd3 ...]");
    puts("rm /tmp/...    : delete a writable tmpfs file");
    puts("mkdir /data/...: create a directory under /data/ (chapter 85)");
    puts("line editor    : up/down arrows cycle history; backspace edits;");
    puts("                 left/right arrows move cursor; Ctrl-A/E line start/end;");
    puts("                 Ctrl-K/U/W kill (to EOL/BOL/prev word); Ctrl-Y yank;");
    puts("                 Ctrl-C cancels current line; Ctrl-D exits");
    puts("relative paths : ./prog (cwd-relative), ../prog (parent)");
    puts("known programs (try `ls` for the full list):");
    puts("  hello       greet from EL0 and exit");
    puts("  ls          list every file in the FS");
    puts("  cat <path>  print a file");
    puts("  echo ...    echo arguments");
    puts("  uptime      print monotonic ms since boot");
    puts("  env         print environment variables");
    puts("  grep PAT P  print lines of P containing PAT");
    puts("  wc P        count lines / words / bytes in P");
    puts("  head [-N] P print first N lines of P (default 10)");
    puts("  tail [-N] P print last N lines of P (default 10)");
    puts("  sleep N     pause for N seconds (or N.MMM)");
    puts("  heaptest    exercise malloc/free");
    puts("  printftest  exercise the libc printf");
}

/* Look up `name` as a path.  If it starts with '/', use as-is.
 * Otherwise walk PATH (':'-separated) and use the first entry
 * for which `entry/name` opens successfully.  Falls back to
 * "/bin/"+name when PATH is unset OR no entry matches — the
 * latter so the spawn() call still produces a useful errno. */
static int file_exists(const char *path)
{
    int fd = open(path, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void prepend_bin(const char *name, char *out)
{
    static const char prefix[] = "/bin/";
    int i = 0;
    for (int k = 0; prefix[k] && i < PATH_MAX - 1; k++) out[i++] = prefix[k];
    for (int k = 0; name[k] && i < PATH_MAX - 1; k++) out[i++] = name[k];
    out[i] = '\0';
}

static void resolve_path(const char *name, char *out)
{
    if (name[0] == '/') {
        int i = 0;
        while (name[i] && i < PATH_MAX - 1) { out[i] = name[i]; i++; }
        out[i] = '\0';
        return;
    }

    /* `./foo` — relative to current cwd. */
    if (name[0] == '.' && name[1] == '/') {
        char cwd[96];
        long cwn = __sys_getcwd(cwd, sizeof(cwd));
        int i = 0;
        if (cwn > 0) {
            for (long k = 0; k < cwn - 1 && i < PATH_MAX - 1; k++)
                out[i++] = cwd[k];
            /* Append '/' if cwd doesn't already end in one. */
            if (i > 0 && out[i - 1] != '/' && i < PATH_MAX - 1)
                out[i++] = '/';
        }
        for (int k = 2; name[k] && i < PATH_MAX - 1; k++)
            out[i++] = name[k];
        out[i] = '\0';
        return;
    }

    /* `../foo` — flat namespace has no parents.  Best-effort:
     * treat as if `./foo` from the parent of cwd, which for our
     * three-directory namespace means root for /bin and /mnt and
     * an error for /.  Cheap implementation: just chop the last
     * cwd segment and recurse on a built path. */
    if (name[0] == '.' && name[1] == '.' && name[2] == '/') {
        char cwd[96];
        long cwn = __sys_getcwd(cwd, sizeof(cwd));
        int i = 0;
        if (cwn > 1) {
            int last_slash = -1;
            for (long k = 0; k < cwn - 1; k++)
                if (cwd[k] == '/') last_slash = (int)k;
            int copy_len = last_slash <= 0 ? 1 : last_slash;
            for (int k = 0; k < copy_len && i < PATH_MAX - 1; k++)
                out[i++] = cwd[k];
            if (i > 0 && out[i - 1] != '/' && i < PATH_MAX - 1)
                out[i++] = '/';
        } else {
            if (i < PATH_MAX - 1) out[i++] = '/';
        }
        for (int k = 3; name[k] && i < PATH_MAX - 1; k++)
            out[i++] = name[k];
        out[i] = '\0';
        return;
    }

    /* Read PATH from env.  POSIX getenv returns a pointer into
     * the env arena (or NULL); no caller buffer needed. */
    const char *path_env_p = getenv("PATH");
    if (!path_env_p) { prepend_bin(name, out); return; }
    char path_env[256];
    {
        int i = 0;
        while (path_env_p[i] && i < (int)sizeof(path_env) - 1) {
            path_env[i] = path_env_p[i];
            i++;
        }
        path_env[i] = '\0';
    }

    /* Walk ':'-separated entries.  For each, build
     * "<entry>/<name>" in `cand` and open() it; first hit wins. */
    char *start = path_env;
    char  cand[PATH_MAX];
    while (*start) {
        char *colon = start;
        while (*colon && *colon != ':') colon++;
        int i = 0;
        for (char *p = start; p < colon && i < PATH_MAX - 2; p++)
            cand[i++] = *p;
        if (i > 0 && cand[i - 1] != '/') cand[i++] = '/';
        for (int k = 0; name[k] && i < PATH_MAX - 1; k++) cand[i++] = name[k];
        cand[i] = '\0';
        if (file_exists(cand)) {
            for (int k = 0; k <= i; k++) out[k] = cand[k];
            return;
        }
        start = (*colon == ':') ? colon + 1 : colon;
    }

    /* No PATH entry matched.  Hand `name` (or its /bin/ form)
     * back so spawn() returns a real errno. */
    prepend_bin(name, out);
}

/* Last-spawn exit code, surfaced as $? in the next command. */
static int g_last_exit = 0;

/* Append `s` to `dst` starting at `*pos`, advancing `*pos`.
 * Truncates silently if dst would overflow `cap`. */
static void append_str(char *dst, int *pos, int cap, const char *s)
{
    while (*s && *pos < cap - 1) dst[(*pos)++] = *s++;
    dst[*pos] = '\0';
}

/* Append the decimal form of int `v` to dst at *pos. */
static void append_int(char *dst, int *pos, int cap, int v)
{
    char tmp[12];
    int  ti = 0;
    int  neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[ti++] = '0'; }
    while (v > 0) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
    if (neg && *pos < cap - 1) dst[(*pos)++] = '-';
    while (ti > 0 && *pos < cap - 1) dst[(*pos)++] = tmp[--ti];
    dst[*pos] = '\0';
}

/* Whether `c` is a valid character inside an unbraced var name
 * (after the leading letter / underscore).  We allow [A-Za-z_0-9]
 * which matches the POSIX shell name production. */
static int is_var_cont(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Expand $VAR, ${VAR}, and $? in `src` into `dst`.  Unknown vars
 * expand to the empty string (silent — matches bash with set +u).
 * Plain text is copied unchanged.  Anything past `cap-1` is
 * silently truncated.
 *
 * Quoting (matches POSIX shell semantics):
 *   'literal'   — copied byte-for-byte; '$' is NOT expanded;
 *                 the surrounding single quotes are consumed.
 *   "expanded"  — '$' IS expanded; the surrounding double
 *                 quotes are consumed.
 *   \\x         — backslash escape: copy `x` literally even
 *                 inside double quotes.
 *
 * We do NOT support nested quotes, backtick / $() substitution,
 * or escape sequences other than the trivial \\x form.
 */
static void expand_vars(const char *src, char *dst, int cap)
{
    int  pos     = 0;
    int  in_dq   = 0;       /* inside double-quoted run */

    while (*src && pos < cap - 1) {
        char c = *src;

        /* Single-quoted run: copy verbatim (incl. '$') until
         * the closing quote, then drop both quotes from output. */
        if (c == '\'' && !in_dq) {
            src++;
            while (*src && *src != '\'' && pos < cap - 1)
                dst[pos++] = *src++;
            if (*src == '\'') src++;        /* consume closing quote */
            continue;
        }

        /* Double-quote toggle: do not emit the quote character. */
        if (c == '"') {
            in_dq = !in_dq;
            src++;
            continue;
        }

        /* Backslash escape: next character literal (handy for
         * embedding " or $ in double-quoted text). */
        if (c == '\\' && src[1]) {
            dst[pos++] = src[1];
            src += 2;
            continue;
        }

        if (c != '$') {
            dst[pos++] = *src++;
            continue;
        }
        src++;                                  /* eat '$' */
        if (*src == '?') {
            append_int(dst, &pos, cap, g_last_exit);
            src++;
            continue;
        }
        if (*src == '$') {
            /* $$ — literal $ for now (bash uses pid; we don't
             * need that yet and it would force a getpid()). */
            dst[pos++] = '$';
            src++;
            continue;
        }
        char name[32];
        int  ni = 0;
        if (*src == '{') {
            src++;
            while (*src && *src != '}' && ni < (int)sizeof(name) - 1)
                name[ni++] = *src++;
            if (*src == '}') src++;
        } else {
            while (is_var_cont(*src) && ni < (int)sizeof(name) - 1)
                name[ni++] = *src++;
        }
        name[ni] = '\0';
        if (ni == 0) {
            /* Lone '$' — emit verbatim. */
            if (pos < cap - 1) dst[pos++] = '$';
            continue;
        }
        const char *val = getenv(name);
        if (val) append_str(dst, &pos, cap, val);
        /* Unknown var: silent empty expansion. */
    }
    dst[pos] = '\0';
}

/* History ring + line editor.
 *
 * The shell switches stdin to raw mode (SYS_TTY_RAW) and does its
 * own per-keystroke processing.  This lets us handle arrow-key
 * escape sequences (ESC [ A / B / C / D) for history navigation
 * — something the cooked-mode kernel reader can't do.
 *
 * History is a simple bounded ring: HISTORY_SIZE entries kept
 * oldest -> newest.  Pushing onto a full ring shifts left by 1.
 * Identical-to-newest entries are deduped (matches bash with
 * HISTCONTROL=ignoredups).
 *
 * Edit cursor is always at end-of-line — we don't yet support
 * left/right cursor movement, so backspace edits the most recent
 * character.  Up/down arrows replace the visible line entirely
 * with a history entry (or the saved scratch when going past
 * the newest entry).
 */
#define HISTORY_SIZE  16
static char     g_hist[HISTORY_SIZE][LINE_MAX];
static int      g_hist_n = 0;     /* number of stored entries (<= HISTORY_SIZE) */

/* Kill ring (single slot — emacs convention is multi-slot but
 * one is enough for our purposes).  Holds the most recent text
 * removed by Ctrl-K / Ctrl-U / Ctrl-W; Ctrl-Y inserts it back
 * at the cursor.  Empty when g_kill_len == 0. */
static char     g_kill[LINE_MAX];
static int      g_kill_len = 0;

static void kill_set(const char *src, int n)
{
    if (n < 0) n = 0;
    if (n > LINE_MAX - 1) n = LINE_MAX - 1;
    for (int i = 0; i < n; i++) g_kill[i] = src[i];
    g_kill[n]  = '\0';
    g_kill_len = n;
}

static int sh_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void hist_push(const char *line)
{
    if (!line || !line[0]) return;
    if (g_hist_n > 0 && sh_streq(g_hist[g_hist_n - 1], line)) return;
    if (g_hist_n == HISTORY_SIZE) {
        /* Shift left to drop oldest. */
        for (int i = 0; i < HISTORY_SIZE - 1; i++) {
            for (int k = 0; k < LINE_MAX; k++)
                g_hist[i][k] = g_hist[i + 1][k];
        }
        g_hist_n--;
    }
    int i = 0;
    while (line[i] && i < LINE_MAX - 1) {
        g_hist[g_hist_n][i] = line[i];
        i++;
    }
    g_hist[g_hist_n][i] = '\0';
    g_hist_n++;
}

/* Erase the current input line (everything after the prompt) and
 * redraw it with `buf`, length `len`, leaving the cursor at
 * position `cur` (0..len).  Uses ANSI ESC[2K to wipe the entire
 * line then '\r' to return to col 0; then re-emits the prompt
 * and content.  Cursor is repositioned by emitting `len-cur`
 * backspaces — portable and doesn't require knowing the prompt's
 * printed width.
 *
 * Note: this assumes the prompt + line don't wrap to the next
 * terminal row.  For a typical 80-col terminal and our short
 * prompt that gives ~76 chars of edit space, well under
 * LINE_MAX (128). */
static void redraw_line(const char *buf, int len, int cur)
{
    /* ESC [ 2 K  -> erase entire line; \r -> return to col 0 */
    write(1, "\x1b[2K\r", 5);
    prompt();
    if (len > 0) write(1, buf, (size_t)len);
    /* Walk the cursor back from end-of-line to `cur`. */
    for (int i = cur; i < len; i++) write(1, "\b", 1);
}

/* Read one line in raw mode.  Returns the number of characters
 * placed in `out` (NOT including the trailing NUL).  Always
 * NUL-terminates.  Handles backspace mid-line, ESC [ A/B/C/D for
 * history and cursor movement, Ctrl-A/E for line start/end,
 * Ctrl-C (cancel current line and return 0), Ctrl-D on empty
 * line (returns -1 to signal EOF). */
static int read_line_raw(char *out, int cap)
{
    int  pos     = 0;             /* current edit length */
    int  cur     = 0;             /* cursor position, 0..pos */
    int  hist_i  = g_hist_n;      /* index into history; ==g_hist_n means "scratch" */
    char scratch[LINE_MAX];       /* saved current line when navigating history */
    int  scratch_len = 0;
    int  scratch_saved = 0;       /* have we already saved scratch this excursion? */

    out[0] = '\0';

    for (;;) {
        char c;
        long got = read(0, &c, 1);
        if (got != 1) continue;

        /* Enter / Carriage return: commit. */
        if (c == '\r' || c == '\n') {
            write(1, "\r\n", 2);
            out[pos] = '\0';
            return pos;
        }

        /* Backspace / DEL: erase char immediately left of cursor. */
        if (c == 0x7f || c == 0x08) {
            if (cur > 0) {
                /* Shift bytes [cur..pos] left by 1. */
                for (int i = cur - 1; i < pos - 1; i++) out[i] = out[i + 1];
                pos--;
                cur--;
                out[pos] = '\0';
                redraw_line(out, pos, cur);
            }
            continue;
        }

        /* Ctrl-A: jump cursor to start. */
        if (c == 0x01) {
            cur = 0;
            redraw_line(out, pos, cur);
            continue;
        }
        /* Ctrl-E: jump cursor to end. */
        if (c == 0x05) {
            cur = pos;
            redraw_line(out, pos, cur);
            continue;
        }

        /* Ctrl-K: kill from cursor to end of line.  Saves the
         * killed text into the single-slot kill ring (replaces
         * any previous contents). */
        if (c == 0x0b) {
            if (cur < pos) {
                kill_set(&out[cur], pos - cur);
                pos        = cur;
                out[pos]   = '\0';
                redraw_line(out, pos, cur);
            }
            continue;
        }
        /* Ctrl-U: kill from start of line to cursor. */
        if (c == 0x15) {
            if (cur > 0) {
                kill_set(out, cur);
                /* Shift bytes [cur..pos] left to position 0. */
                for (int i = 0; i + cur < pos; i++) out[i] = out[i + cur];
                pos -= cur;
                cur  = 0;
                out[pos] = '\0';
                redraw_line(out, pos, cur);
            }
            continue;
        }
        /* Ctrl-W: kill the word immediately left of the cursor.
         * Boundary = whitespace.  Skips trailing whitespace then
         * eats back to the next whitespace (or start of line). */
        if (c == 0x17) {
            if (cur > 0) {
                int end = cur;
                int start = cur;
                /* skip trailing whitespace */
                while (start > 0 && (out[start - 1] == ' ' || out[start - 1] == '\t'))
                    start--;
                /* eat word characters */
                while (start > 0 && out[start - 1] != ' ' && out[start - 1] != '\t')
                    start--;
                if (start < end) {
                    kill_set(&out[start], end - start);
                    /* Shift bytes [end..pos] left to fill gap. */
                    for (int i = start; i + (end - start) < pos; i++)
                        out[i] = out[i + (end - start)];
                    pos -= (end - start);
                    cur  = start;
                    out[pos] = '\0';
                    redraw_line(out, pos, cur);
                }
            }
            continue;
        }
        /* Ctrl-Y: yank — insert kill ring contents at cursor. */
        if (c == 0x19) {
            if (g_kill_len > 0 && pos + g_kill_len < cap) {
                /* Shift bytes [cur..pos] right by g_kill_len. */
                for (int i = pos - 1; i >= cur; i--)
                    out[i + g_kill_len] = out[i];
                for (int i = 0; i < g_kill_len; i++)
                    out[cur + i] = g_kill[i];
                pos += g_kill_len;
                cur += g_kill_len;
                out[pos] = '\0';
                redraw_line(out, pos, cur);
            }
            continue;
        }

        /* Ctrl-C: cancel line, print ^C, return empty. */
        if (c == 0x03) {
            write(1, "^C\r\n", 4);
            out[0] = '\0';
            return 0;
        }

        /* Ctrl-D on empty line: EOF. */
        if (c == 0x04) {
            if (pos == 0) {
                out[0] = '\0';
                return -1;
            }
            continue;
        }

        /* ESC sequence: parse '[' then 'A'/'B'/'C'/'D'. */
        if (c == 0x1b) {
            char b1, b2;
            if (read(0, &b1, 1) != 1) continue;
            if (b1 != '[') continue;
            if (read(0, &b2, 1) != 1) continue;

            if (b2 == 'A') {            /* up */
                if (hist_i > 0) {
                    /* Save scratch on first up-press. */
                    if (!scratch_saved) {
                        scratch_len = pos;
                        for (int i = 0; i < pos; i++) scratch[i] = out[i];
                        scratch[pos] = '\0';
                        scratch_saved = 1;
                    }
                    hist_i--;
                    /* Load history entry. */
                    int i = 0;
                    while (g_hist[hist_i][i] && i < cap - 1) {
                        out[i] = g_hist[hist_i][i]; i++;
                    }
                    out[i] = '\0';
                    pos = i;
                    cur = pos;          /* cursor at end of loaded line */
                    redraw_line(out, pos, cur);
                }
                continue;
            }
            if (b2 == 'B') {            /* down */
                if (hist_i < g_hist_n) {
                    hist_i++;
                    if (hist_i == g_hist_n) {
                        /* Past newest -> restore scratch. */
                        for (int i = 0; i < scratch_len; i++) out[i] = scratch[i];
                        out[scratch_len] = '\0';
                        pos = scratch_len;
                    } else {
                        int i = 0;
                        while (g_hist[hist_i][i] && i < cap - 1) {
                            out[i] = g_hist[hist_i][i]; i++;
                        }
                        out[i] = '\0';
                        pos = i;
                    }
                    cur = pos;
                    redraw_line(out, pos, cur);
                }
                continue;
            }
            if (b2 == 'C') {            /* right */
                if (cur < pos) {
                    cur++;
                    /* Move cursor right one cell (ESC[C, or just
                     * re-emit the char we passed over since we
                     * have a copy in our buffer). */
                    write(1, &out[cur - 1], 1);
                }
                continue;
            }
            if (b2 == 'D') {            /* left */
                if (cur > 0) {
                    cur--;
                    write(1, "\b", 1);
                }
                continue;
            }
            /* Other ESC sequences ignored. */
            continue;
        }

        /* Printable byte: insert at cursor position. */
        if (c >= 0x20 && c < 0x7f && pos < cap - 1) {
            if (cur == pos) {
                /* Append-at-end fast path: just echo the byte. */
                out[pos++] = c;
                cur++;
                out[pos]   = '\0';
                write(1, &c, 1);
            } else {
                /* Mid-line insert: shift bytes [cur..pos] right
                 * by 1, then full redraw with cursor advanced. */
                for (int i = pos; i > cur; i--) out[i] = out[i - 1];
                out[cur] = c;
                pos++;
                cur++;
                out[pos] = '\0';
                redraw_line(out, pos, cur);
            }
            /* Any keystroke invalidates the in-progress history
             * navigation: subsequent up/down should save the new
             * line as scratch on first press. */
            if (hist_i != g_hist_n) {
                hist_i        = g_hist_n;
                scratch_saved = 0;
            }
        }
    }
}

int main(void)
{
    puts("[sh] tiny shell ready.  type 'help' for a list of commands.");

    /* Switch console to raw mode for the shell's own line editor. */
    int prev_raw = tty_raw(1);
    (void)prev_raw;

    char raw[LINE_MAX];
    char line[LINE_MAX];
    for (;;) {
        prompt();
        int n = read_line_raw(raw, LINE_MAX);
        if (n < 0) {
            /* EOF (Ctrl-D on empty line). */
            tty_raw(0);
            puts("[sh] EOF on stdin, exiting");
            return 0;
        }
        if (n == 0) continue;
        /* trim() expects a NUL-terminated buffer; raw is already
         * NUL-terminated by read_line_raw.  Trim trailing CR/LF
         * still in case anything snuck in. */
        n = trim(raw, n);
        if (n == 0) continue;
        /* Push to history BEFORE expansion so up-arrow shows the
         * literal user input (matching bash behaviour). */
        hist_push(raw);

        /* Variable expansion happens before tokenization so
         * builtins (`export FOO=$BAR`, `cd $HOME`) and child
         * spawns (`cat $FILE`) all see expanded text. */
        expand_vars(raw, line, LINE_MAX);
        n = (long)strlen(line);
        if (n == 0) continue;

        if (streq(line, "help")) { print_help(); continue; }
        if (streq(line, "exit")) return 0;
        if (starts_with(line, "exit ")) return (int)parse_uint(line + 5);

        /* `pwd` builtin. */
        if (streq(line, "pwd")) {
            char cwd[96];
            long got = __sys_getcwd(cwd, sizeof(cwd));
            if (got > 0) {
                write(1, cwd, (size_t)(got - 1));
                write(1, "\n", 1);
            }
            continue;
        }

        /* `cd <dir>` builtin.  Bare `cd` resets to /. */
        if (streq(line, "cd")) {
            (void)chdir("/");
            continue;
        }
        if (starts_with(line, "cd ")) {
            const char *target = line + 3;
            while (*target == ' ' || *target == '\t') target++;
            int rc = chdir(target);
            if (rc != 0) {
                write(1, "cd: ", 4);
                write(1, target, (size_t)strlen(target));
                write(1, ": no such directory (errno=", 28);
                putd(errno);
                write(1, ")\n", 2);
            }
            continue;
        }

        /* `export KEY=VAL` builtin.  Splits on first '='. */
        if (starts_with(line, "export ")) {
            char *kv = line + 7;
            while (*kv == ' ' || *kv == '\t') kv++;
            char *eq = kv;
            while (*eq && *eq != '=') eq++;
            if (*eq != '=') {
                puts("export: usage: export KEY=VAL");
                continue;
            }
            *eq = '\0';
            const char *val = eq + 1;
            int rc = setenv(kv, val, 1);
            if (rc != 0) {
                write(1, "export: setenv failed errno=", 28);
                putd(errno);
                write(1, "\n", 1);
            }
            continue;
        }

        /* `unset KEY` builtin. */
        if (starts_with(line, "unset ")) {
            const char *key = line + 6;
            while (*key == ' ' || *key == '\t') key++;
            int rc = unsetenv(key);
            if (rc != 0) {
                write(1, "unset: errno=", 13);
                putd(errno);
                write(1, "\n", 1);
            }
            continue;
        }

        /* `rm <path>` builtin.  Today only deletes /tmp/<name>;
         * everything else returns -EINVAL from the kernel and we
         * surface it as an error message.  Multiple args
         * supported (whitespace-split, no quoting). */
        if (starts_with(line, "rm ")) {
            char *p = line + 3;
            while (*p == ' ' || *p == '\t') p++;
            int any_err = 0;
            while (*p) {
                char *start = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                char saved = *p;
                *p = '\0';
                int rc = unlink(start);
                if (rc != 0) {
                    write(1, "rm: ", 4);
                    write(1, start, (size_t)strlen(start));
                    write(1, ": errno=", 8);
                    putd(errno);
                    write(1, "\n", 1);
                    any_err = 1;
                }
                *p = saved;
                while (*p == ' ' || *p == '\t') p++;
            }
            g_last_exit = any_err ? 1 : 0;
            continue;
        }

        /* `mkdir <path>...` builtin (chapter 85).  Each argument
         * is taken as a single directory to create.  No -p:
         * intermediate parents must already exist.  Today only
         * /data/<...>/<name> is accepted by the kernel. */
        if (starts_with(line, "mkdir ")) {
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            int any_err = 0;
            while (*p) {
                char *start = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                char saved = *p;
                *p = '\0';
                int rc = mkdir(start, 0755);
                if (rc != 0) {
                    write(1, "mkdir: ", 7);
                    write(1, start, (size_t)strlen(start));
                    write(1, ": errno=", 8);
                    putd(errno);
                    write(1, "\n", 1);
                    any_err = 1;
                }
                *p = saved;
                while (*p == ' ' || *p == '\t') p++;
            }
            g_last_exit = any_err ? 1 : 0;
            continue;
        }

        /* `time <cmd>` — measure wall-clock duration of <cmd>.
         * Bare `time` alone just prints current uptime.  Strip
         * the prefix and remember to print the delta after wait. */
        int   timed = 0;
        char *cmd   = line;
        if (streq(line, "time")) {
            unsigned long ms = uptime_ms();
            write(1, "uptime ", 7);
            putd((int)(ms / 1000UL));
            write(1, ".", 1);
            /* Tiny fixed-width 3-digit ms part. */
            unsigned long ms_part = ms % 1000UL;
            char dig[3] = { (char)('0' + (ms_part / 100UL) % 10UL),
                            (char)('0' + (ms_part /  10UL) % 10UL),
                            (char)('0' +  ms_part         % 10UL) };
            write(1, dig, 3);
            write(1, "s\n", 2);
            continue;
        }
        if (starts_with(line, "time ")) {
            timed = 1;
            cmd = line + 5;
            while (*cmd == ' ' || *cmd == '\t') cmd++;
            if (*cmd == '\0') continue;
        }

        /* `&` background suffix (chapter 79b promised this; it
         * lands here in chapter 106b because the busy-desktop
         * repro test needs to start `httpd 8080 &` from the
         * kernel sh before opening gui_term).
         *
         * Detection: strip whitespace at end-of-cmd, see if the
         * very last character is `&`, and that this `&` isn't
         * inside a `>>` / `<<` / `&&` chain (we don't support
         * those operators yet -- if anyone ever adds them this
         * check must move BEFORE their parsing).
         *
         * Side effect on the spawn path: skip the post-spawn
         * `wait(&code)` and instead print `[bg] tid=N` and move
         * on.  Zombies of bg children are reaped opportunistic-
         * ally at the top of every prompt loop via the non-
         * blocking waitpid() pass that already exists for
         * SIGCHLD bookkeeping (added below). */
        int bg = 0;
        {
            int n = (int)strlen(cmd);
            while (n > 0 && (cmd[n-1] == ' ' || cmd[n-1] == '\t')) n--;
            if (n > 0 && cmd[n-1] == '&') {
                bg = 1;
                cmd[n-1] = '\0';
                /* Strip whitespace before the now-removed `&`. */
                while (n > 1 && (cmd[n-2] == ' ' || cmd[n-2] == '\t')) {
                    cmd[n-2] = '\0';
                    n--;
                }
            }
        }

        /* Input redirection: `<` (anywhere, whitespace-bounded
         * or glued to next word).  Currently runs AFTER quote
         * expansion, so `echo '<'` is a syntax error rather than
         * a literal `<`.  See chapter 37. */
        const char *redir_in = 0;
        char redir_path[PATH_MAX];
        {
            char *p = cmd;
            while (*p) {
                if (*p == '<') {
                    char *lt = p;
                    char *q  = p + 1;
                    while (*q == ' ' || *q == '\t') q++;
                    char *ps = q;
                    while (*q && *q != ' ' && *q != '\t' && *q != '<' && *q != '>' && *q != '|') q++;
                    char *pe = q;
                    if (pe == ps) {
                        puts("sh: syntax error near `<`");
                        redir_in = (const char *)1;   /* poisoned */
                        break;
                    }
                    int plen = (int)(pe - ps);
                    if (plen >= PATH_MAX) plen = PATH_MAX - 1;
                    for (int i = 0; i < plen; i++) redir_path[i] = ps[i];
                    redir_path[plen] = '\0';
                    redir_in = redir_path;
                    /* Splice out `<...path` from cmd. */
                    char *src = pe;
                    char *dst = lt;
                    if (dst != cmd && dst[-1] != ' ' && dst[-1] != '\t')
                        *dst++ = ' ';
                    while (*src) *dst++ = *src++;
                    *dst = '\0';
                    char *end = dst - 1;
                    while (end >= cmd && (*end == ' ' || *end == '\t'))
                        *end-- = '\0';
                    break;
                }
                p++;
            }
        }
        if (redir_in == (const char *)1) {
            g_last_exit = 2;
            continue;
        }

        /* Output redirection: `>` (truncate) or `>>` (append).
         * See chapters 41 and 42. */
        const char *redir_out = 0;
        int redir_out_append = 0;
        char redir_out_path[PATH_MAX];
        {
            char *p = cmd;
            while (*p) {
                if (*p == '>') {
                    char *gt = p;
                    int two = (p[1] == '>');
                    char *q  = p + (two ? 2 : 1);
                    while (*q == ' ' || *q == '\t') q++;
                    char *ps = q;
                    while (*q && *q != ' ' && *q != '\t' && *q != '>' && *q != '<' && *q != '|') q++;
                    char *pe = q;
                    if (pe == ps) {
                        puts("sh: syntax error near `>`");
                        redir_out = (const char *)1;
                        break;
                    }
                    int plen = (int)(pe - ps);
                    if (plen >= PATH_MAX) plen = PATH_MAX - 1;
                    for (int i = 0; i < plen; i++) redir_out_path[i] = ps[i];
                    redir_out_path[plen] = '\0';
                    redir_out = redir_out_path;
                    redir_out_append = two;
                    char *src = pe;
                    char *dst = gt;
                    if (dst != cmd && dst[-1] != ' ' && dst[-1] != '\t')
                        *dst++ = ' ';
                    while (*src) *dst++ = *src++;
                    *dst = '\0';
                    char *end = dst - 1;
                    while (end >= cmd && (*end == ' ' || *end == '\t'))
                        *end-- = '\0';
                    break;
                }
                p++;
            }
        }
        if (redir_out == (const char *)1) {
            g_last_exit = 2;
            continue;
        }

        /* Pipeline support: `cmd1 | cmd2 [| cmd3 ...]`.  redir_in
         * (if any) wires onto the first segment's stdin; redir_out
         * (if any) wires onto the last segment's stdout.  See
         * chapters 40 and 41. */
        {
            int has_pipe = 0;
            for (char *q = cmd; *q; q++) if (*q == '|') { has_pipe = 1; break; }
            if (has_pipe) {
                /* Collect segments by NUL-splitting in place. */
                #define MAX_SEGMENTS 8
                char *segs[MAX_SEGMENTS];
                int   nseg = 0;
                segs[nseg++] = cmd;
                for (char *q = cmd; *q; q++) {
                    if (*q == '|') {
                        *q = '\0';
                        if (nseg >= MAX_SEGMENTS) {
                            puts("sh: too many pipeline stages (max 8)");
                            nseg = -1;
                            break;
                        }
                        char *next = q + 1;
                        while (*next == ' ' || *next == '\t') next++;
                        segs[nseg++] = next;
                    }
                }
                if (nseg < 0) { g_last_exit = 2; continue; }
                /* Trim trailing whitespace on each segment. */
                int empty_seg = 0;
                for (int i = 0; i < nseg; i++) {
                    char *s = segs[i];
                    while (*s == ' ' || *s == '\t') s++;
                    segs[i] = s;
                    char *e = s + strlen(s);
                    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
                    if (*s == '\0') { empty_seg = 1; break; }
                }
                if (empty_seg) {
                    puts("sh: empty pipeline segment");
                    g_last_exit = 2;
                    continue;
                }

                /* Open redir files (if any) into shell-side fds. */
                int sh_in_fd  = -1;
                int sh_out_fd = -1;
                if (redir_in) {
                    sh_in_fd = open(redir_in, 0 /*O_RDONLY*/);
                    if (sh_in_fd < 0) {
                        write(1, "[sh] redirect: cannot open ", 27);
                        write(1, redir_in, (size_t)strlen(redir_in));
                        write(1, "\n", 1);
                        g_last_exit = 1;
                        continue;
                    }
                }
                if (redir_out) {
                    int oflags = 1 /*O_WRONLY*/ | 0100 /*O_CREAT*/;
                    if (redir_out_append) oflags |= 02000 /*O_APPEND*/;
                    else                  oflags |= 01000 /*O_TRUNC*/;
                    sh_out_fd = open(redir_out, oflags);
                    if (sh_out_fd < 0) {
                        write(1, "[sh] redirect: cannot open ", 27);
                        write(1, redir_out, (size_t)strlen(redir_out));
                        write(1, "\n", 1);
                        if (sh_in_fd >= 0) close(sh_in_fd);
                        g_last_exit = 1;
                        continue;
                    }
                }

                /* Allocate the n-1 pipes. */
                int pipes[MAX_SEGMENTS - 1][2];
                int npipe = nseg - 1;
                int alloc_ok = 1;
                int allocated = 0;
                for (int i = 0; i < npipe; i++) {
                    if (pipe(pipes[i]) != 0) { alloc_ok = 0; break; }
                    allocated++;
                }
                if (!alloc_ok) {
                    puts("sh: pipe() failed");
                    for (int i = 0; i < allocated; i++) {
                        close(pipes[i][0]); close(pipes[i][1]);
                    }
                    if (sh_in_fd  >= 0) close(sh_in_fd);
                    if (sh_out_fd >= 0) close(sh_out_fd);
                    g_last_exit = 1;
                    continue;
                }

                unsigned long t0 = timed ? uptime_ms() : 0;

                /* Spawn each segment.  For each, split first token
                 * off as the path, the rest is the args string. */
                int tids[MAX_SEGMENTS];
                int spawn_failed = 0;
                for (int i = 0; i < nseg; i++) {
                    char *s_args = segs[i];
                    while (*s_args && *s_args != ' ' && *s_args != '\t') s_args++;
                    if (*s_args) {
                        *s_args++ = '\0';
                        while (*s_args == ' ' || *s_args == '\t') s_args++;
                    }
                    char p[PATH_MAX];
                    resolve_path(segs[i], p);
                    int sin_fd, sout_fd;
                    if (i == 0)             sin_fd  = sh_in_fd;
                    else                    sin_fd  = pipes[i - 1][0];
                    if (i == nseg - 1)      sout_fd = sh_out_fd;
                    else                    sout_fd = pipes[i][1];
                    int tid = spawn_pipe(p, s_args, sin_fd, sout_fd);
                    if (tid < 0) {
                        write(1, "[sh] no such command: ", 22);
                        write(1, segs[i], (size_t)strlen(segs[i]));
                        write(1, " (errno=", 8);
                        putd(errno);
                        write(1, ")\n", 2);
                        spawn_failed = 1;
                        tids[i] = -1;
                    } else {
                        tids[i] = tid;
                    }
                }

                /* Close all pipe fds in the shell — the children
                 * hold their own references via spawn_pipe.  This
                 * is critical: leaving a writer open in the shell
                 * means the consumer never sees EOF. */
                for (int i = 0; i < npipe; i++) {
                    close(pipes[i][0]);
                    close(pipes[i][1]);
                }
                if (sh_in_fd  >= 0) close(sh_in_fd);
                if (sh_out_fd >= 0) close(sh_out_fd);

                /* Wait for every child we successfully spawned.
                 * Designate the LAST stage as foreground so Ctrl-C
                 * gets routed there (it owns the tty after the
                 * pipeline finishes producing).  For our purposes
                 * any child in the pipeline is reasonable; the
                 * last is the most user-visible. */
                int last_code = 0;
                int waits = 0;
                for (int i = 0; i < nseg; i++) if (tids[i] >= 0) waits++;
                int fg_target = -1;
                for (int i = nseg - 1; i >= 0; i--) {
                    if (tids[i] >= 0) { fg_target = tids[i]; break; }
                }
                if (fg_target > 0) set_fg_pid(fg_target);
                for (int i = 0; i < waits; i++) {
                    int code = 0;
                    int tid  = wait(&code);
                    last_code = code;
                    (void)tid;
                }
                set_fg_pid(0);
                g_last_exit = spawn_failed ? 127 : last_code;

                if (timed) {
                    unsigned long elapsed = uptime_ms() - t0;
                    write(1, "[time] ", 7);
                    putd((int)(elapsed / 1000UL));
                    write(1, ".", 1);
                    unsigned long ms_part = elapsed % 1000UL;
                    char dig[3] = { (char)('0' + (ms_part / 100UL) % 10UL),
                                    (char)('0' + (ms_part /  10UL) % 10UL),
                                    (char)('0' +  ms_part         % 10UL) };
                    write(1, dig, 3);
                    write(1, "s real\n", 7);
                }
                continue;
            }
        }

        /* Split first whitespace-separated token off as the
         * command name, everything after as the args string. */
        char *args = cmd;
        while (*args && *args != ' ' && *args != '\t') args++;
        if (*args) {
            *args++ = '\0';
            while (*args == ' ' || *args == '\t') args++;
        }

        /* Resolve bare names through /bin/. */
        char path[PATH_MAX];
        resolve_path(cmd, path);

        unsigned long t0 = timed ? uptime_ms() : 0;

        int tid;
        int sh_in_fd  = -1;
        int sh_out_fd = -1;

        if (redir_out) {
            /* Output redirection requested: open the target with
             * O_WRONLY|O_CREAT|(O_TRUNC or O_APPEND) and use
             * spawn_pipe.  If we ALSO have input redirection,
             * open that too and pass it as stdin_fd. */
            int oflags = 1 /*O_WRONLY*/ | 0100 /*O_CREAT*/;
            if (redir_out_append) oflags |= 02000 /*O_APPEND*/;
            else                  oflags |= 01000 /*O_TRUNC*/;
            sh_out_fd = open(redir_out, oflags);
            if (sh_out_fd < 0) {
                write(1, "[sh] redirect: cannot open ", 27);
                write(1, redir_out, (size_t)strlen(redir_out));
                write(1, " for writing (errno=", 20);
                putd(errno);
                write(1, ")\n", 2);
                g_last_exit = 1;
                continue;
            }
            if (redir_in) {
                sh_in_fd = open(redir_in, 0 /*O_RDONLY*/);
                if (sh_in_fd < 0) {
                    write(1, "[sh] redirect: cannot open ", 27);
                    write(1, redir_in, (size_t)strlen(redir_in));
                    write(1, "\n", 1);
                    close(sh_out_fd);
                    g_last_exit = 1;
                    continue;
                }
            }
            tid = spawn_pipe(path, args, sh_in_fd, sh_out_fd);
        } else if (redir_in) {
            tid = spawn_redir(path, args, redir_in);
        } else {
            tid = spawn(path, args);
        }
        if (tid < 0) {
            if ((redir_in || redir_out) && errno == ENOENT) {
                write(1, "[sh] redirect: cannot open ", 27);
                if (redir_in)  write(1, redir_in,  (size_t)strlen(redir_in));
                else           write(1, redir_out, (size_t)strlen(redir_out));
                write(1, "\n", 1);
            } else {
                write(1, "[sh] no such command: ", 22);
                write(1, cmd, (size_t)strlen(cmd));
                write(1, " (errno=", 8);
                putd(errno);
                write(1, ")\n", 2);
            }
            if (sh_in_fd  >= 0) close(sh_in_fd);
            if (sh_out_fd >= 0) close(sh_out_fd);
            /* Surface the spawn error as $?: bash uses 127 for
             * "command not found", so do the same. */
            g_last_exit = 127;
            continue;
        }
        if (sh_in_fd  >= 0) close(sh_in_fd);
        if (sh_out_fd >= 0) close(sh_out_fd);
        int code = 0;
        if (bg) {
            /* Backgrounded by trailing `&`.  Don't wait; print a
             * job announcement and continue.  The child stays
             * our parent for now (no setpgid yet); when it exits
             * it becomes a zombie until the next prompt loop's
             * non-blocking waitpid() pass reaps it.  We do NOT
             * set_fg_pid so Ctrl-C still goes to the foreground
             * (in this branch, the foreground is the SHELL).
             *
             * Chapter 106b: this is the minimum-viable backgrounding
             * needed to spawn `httpd 8080 &` from /bin/sh before
             * opening a gui_term to drive the browser-in-desktop
             * regression test.  No `jobs`, `fg`, `bg`, `wait %N`
             * builtins yet -- and crucially no setpgid, so a
             * Ctrl-C in the foreground does NOT also kill bg
             * jobs (good for our test, surprising vs. bash). */
            write(1, "[bg] tid=", 9);
            putd(tid);
            write(1, "\n", 1);
            g_last_exit = 0;
            /* Skip the `timed` post-print; backgrounded jobs have
             * no meaningful wall time at spawn-completion. */
            continue;
        }
        /* Foreground process for Ctrl-C: the just-spawned child. */
        set_fg_pid(tid);
        wait(&code);
        set_fg_pid(0);
        g_last_exit = code;
        if (timed) {
            unsigned long elapsed = uptime_ms() - t0;
            write(1, "[time] ", 7);
            putd((int)(elapsed / 1000UL));
            write(1, ".", 1);
            unsigned long ms_part = elapsed % 1000UL;
            char dig[3] = { (char)('0' + (ms_part / 100UL) % 10UL),
                            (char)('0' + (ms_part /  10UL) % 10UL),
                            (char)('0' +  ms_part         % 10UL) };
            write(1, dig, 3);
            write(1, "s real\n", 7);
        }
        if (code != 0) {
            write(1, "[sh] exit ", 10);
            putd(code);
            write(1, "\n", 1);
        }
    }
}
