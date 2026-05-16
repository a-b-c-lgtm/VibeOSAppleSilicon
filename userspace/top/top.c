/* userspace/top/top.c — chapter 99 live process viewer.
 *
 * Same data as `ps` but redraws every second.  Clears the
 * terminal with the ANSI sequence "\x1b[2J\x1b[H" between
 * frames — gui_term handles VT100, /bin/sh on the kernel
 * console mostly ignores them (you'll see appended frames),
 * which is fine because the meaningful output is the most
 * recent block at the bottom.
 *
 * Exits after MAX_FRAMES iterations so test scripts can run
 * /bin/top from a non-interactive shell without it pinning
 * a thread forever.  Press whatever key you like; we don't
 * read stdin.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Pre-tested column-width matches ps.c — same key:value parser. */

static int str_eq_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

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

static void copy_to_eol(const char *src, char *dst, int cap)
{
    int i = 0;
    while (src[i] && src[i] != '\n' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

struct proc_row {
    int  pid;
    int  ppid;
    int  cpu;
    char state;
    char name[32];
};

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
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return (out->pid >= 0) ? 0 : -1;
}

static void render_frame(unsigned long iter)
{
    /* ANSI clear-screen + home cursor.  No-op when stdout is the
     * kernel console (it doesn't decode CSI); harmless either
     * way. */
    printf("\x1b[2J\x1b[H");

    /* Header: uptime + meminfo summary. */
    char buf[1024];
    if (slurp("/proc/uptime", buf, sizeof(buf)) > 0) {
        printf("uptime: %s", buf);  /* file ends with \n */
    }
    if (slurp("/proc/meminfo", buf, sizeof(buf)) > 0) {
        /* meminfo is multi-line; print first three lines (Total,
         * Free, Used) to keep the header tight. */
        int lines = 0;
        for (int i = 0; buf[i] && lines < 3; i++) {
            printf("%c", buf[i]);
            if (buf[i] == '\n') lines++;
        }
    }
    printf("frame %lu — refresh /1s\n", iter);
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
        if (type != LISTDIR_TYPE_DIR) continue;
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
}

int main(int argc, char **argv)
{
    /* Optional first arg: number of frames before exit (default
     * 3 so test scripts don't hang forever).  -1 (or any negative
     * number) means "run until killed". */
    int frames = 3;
    if (argc > 1) {
        const char *p = argv[1];
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        int v = 0;
        int saw = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; saw = 1; }
        if (saw) frames = neg ? -1 : v;
    }
    unsigned long iter = 0;
    while (frames < 0 || iter < (unsigned long)frames) {
        render_frame(iter++);
        sleep_ms(1000);
    }
    return 0;
}
