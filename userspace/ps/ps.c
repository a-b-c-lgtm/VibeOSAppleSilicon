/* userspace/ps/ps.c — chapter 101 process listing.
 *
 * Snapshots /proc by walking SYS_LISTDIR_AT on "/proc"; for each
 * numeric leaf, opens /proc/<pid>/status and parses the textual
 * key:value lines we ship from kernel/core/procfs.c.  Prints one
 * row per process in `ps`-ish columns.
 *
 * Deliberately small: no flags, no sorting, no formatting tricks.
 * `top` (next door) reuses the same parser via copy-paste — keep
 * the layout matching procfs.c::render_pid_status so a single
 * edit there only requires one update here.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

static int str_eq_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

/* Skip any spaces or tabs starting at `s`. */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Parse a leading non-negative decimal integer.  Stops at the
 * first non-digit and leaves *end_out at it.  Returns -1 when
 * the first character isn't a digit. */
static int parse_int(const char *s, const char **end_out)
{
    if (*s < '0' || *s > '9') { *end_out = s; return -1; }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    *end_out = s;
    return v;
}

/* Copy from `src` into `dst[cap]` until newline or NUL.
 * Always NUL-terminates within cap. */
static void copy_to_eol(const char *src, char *dst, int cap)
{
    int i = 0;
    while (src[i] && src[i] != '\n' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Per-pid summary populated from /proc/<pid>/status. */
struct proc_row {
    int  pid;
    int  ppid;
    int  cpu;
    char state;            /* R/S/D/W/Z */
    char name[32];
};

/* Read the whole file into `buf[cap]` and NUL-terminate.  Returns
 * the byte count (excluding NUL) or -1 on error. */
static long slurp(const char *path, char *buf, int cap)
{
    int fd = open(path, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total + 1 < cap) {
        long got = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (got <= 0) break;
        total += got;
    }
    buf[total] = '\0';
    close(fd);
    return total;
}

/* Parse one /proc/<pid>/status blob in place.  Looks only for
 * the fields we render in `ps`; ignores anything else (forward
 * compatible if procfs starts emitting more lines). */
static int parse_status(const char *text, struct proc_row *out)
{
    out->pid = -1; out->ppid = 0; out->cpu = 0; out->state = '?';
    out->name[0] = '\0';

    const char *p = text;
    while (*p) {
        if (str_eq_n(p, "Name:", 5)) {
            copy_to_eol(skip_ws(p + 5), out->name, (int)sizeof(out->name));
        } else if (str_eq_n(p, "Pid:", 4)) {
            const char *e;
            int v = parse_int(skip_ws(p + 4), &e);
            if (v >= 0) out->pid = v;
        } else if (str_eq_n(p, "PPid:", 5)) {
            const char *e;
            int v = parse_int(skip_ws(p + 5), &e);
            if (v >= 0) out->ppid = v;
        } else if (str_eq_n(p, "Cpu:", 4)) {
            const char *e;
            int v = parse_int(skip_ws(p + 4), &e);
            if (v >= 0) out->cpu = v;
        } else if (str_eq_n(p, "State:", 6)) {
            const char *s = skip_ws(p + 6);
            if (*s) out->state = *s;
        }
        /* Advance past the newline. */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return (out->pid >= 0) ? 0 : -1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Column header — fixed width so the eyeball-diff against
     * Linux `ps -e` is recognisable. */
    printf("%5s %5s %3s %s %s\n", "PID", "PPID", "CPU", "S", "CMD");

    char path[64];
    char status_buf[1024];
    struct proc_row row;

    for (int idx = 0; ; idx++) {
        char leaf[64];
        unsigned int type = 0;
        long got = listdir_at("/proc", idx, leaf, sizeof(leaf),
                              0, &type);
        if (got < 0) break;
        /* Skip the static top-level files: only iterate the
         * per-pid directories. */
        if (type != LISTDIR_TYPE_DIR) continue;
        /* Build "/proc/<leaf>/status" path. */
        int n = 0;
        const char prefix[] = "/proc/";
        for (int i = 0; prefix[i] && n + 1 < (int)sizeof(path); i++)
            path[n++] = prefix[i];
        for (int i = 0; leaf[i] && n + 1 < (int)sizeof(path); i++)
            path[n++] = leaf[i];
        const char suffix[] = "/status";
        for (int i = 0; suffix[i] && n + 1 < (int)sizeof(path); i++)
            path[n++] = suffix[i];
        path[n] = '\0';

        if (slurp(path, status_buf, sizeof(status_buf)) <= 0) continue;
        if (parse_status(status_buf, &row) != 0) continue;
        printf("%5d %5d %3d %c %s\n",
               row.pid, row.ppid, row.cpu, row.state, row.name);
    }
    return 0;
}
