/*
 * userspace/procd/procd.c — chapter 145 userspace /proc daemon.
 *
 * Replaces the kernel's chapter-99 procfs.c.  Mounts /proc via
 * the chapter-114 userfs channel and serves the same file set
 * the kernel used to:
 *
 *   /proc                      directory listing
 *   /proc/uptime               wall-time + idle (we duplicate
 *                              uptime in both slots since we
 *                              don't track idle yet)
 *   /proc/meminfo              MemTotal / MemFree / MemUsed /
 *                              PageSize / KernelHeap
 *   /proc/cpuinfo              one line per CPU
 *   /proc/sched                per-CPU runqueue lengths
 *   /proc/<pid>                directory listing (status, cmdline, trace)
 *   /proc/<pid>/status         human-readable state dump
 *   /proc/<pid>/cmdline        argv as the spawn saw it
 *   /proc/<pid>/trace          chapter-100 syscall ring drain
 *
 * Each open() renders the file's text into a malloc'd buffer
 * stored in a per-handle slot.  Reads serve from the cached
 * buffer; close frees it.  Re-rendering on every open keeps the
 * data fresh without burning RAM for files no one is reading.
 *
 * Underlying state comes from three chapter-114e syscalls
 * (SYS_KSTAT / SYS_THREAD_SNAPSHOT / SYS_STRACE_RENDER) defined
 * in kernel/core/syscall.c.  The ABI structs live in
 * userspace/libc/proc_stat.h.
 */

#include "../libfs/userfs.h"
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/proc_stat.h"

/* ------------------------------------------------------------------
 * Handle table.  Each open() reserves a slot; close() releases it.
 * Slot 0 is reserved as "no handle" so the libfs caller can use
 * non-zero values to mean "valid".  Handle id sent over the wire
 * is `slot_index + 1`.
 * ------------------------------------------------------------------ */

#define PROCD_MAX_HANDLES 16
#define PROCD_FILE_CAP    8192u   /* same as kernel's PROCFS_MAX_FILE */
#define PROCD_TRACE_CAP   16384u  /* trace ring is larger */

struct slot {
    int       used;
    char     *buf;
    uint32_t  len;
};
static struct slot g_slots[PROCD_MAX_HANDLES];

static uint32_t slot_alloc(char *buf, uint32_t len)
{
    for (int i = 0; i < PROCD_MAX_HANDLES; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used = 1;
            g_slots[i].buf  = buf;
            g_slots[i].len  = len;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

static void slot_free(uint32_t h)
{
    if (h == 0 || (int)h > PROCD_MAX_HANDLES) return;
    struct slot *s = &g_slots[h - 1];
    if (!s->used) return;
    if (s->buf) free(s->buf);
    s->used = 0;
    s->buf  = (char *)0;
    s->len  = 0;
}

/* ------------------------------------------------------------------
 * Tiny formatter — same shape as the kernel's pf_* helpers in
 * the chapter-99 procfs.c.  We deliberately don't use the libc
 * printf because every renderer hand-formats numbers into a
 * single growing buffer; printf would require us to first
 * snprintf into stack buffers, then concatenate, which is more
 * code and more stack pressure than this loop.
 * ------------------------------------------------------------------ */

static void pf_putc(char *buf, uint32_t cap, uint32_t *pos, char c)
{
    if (*pos + 1 < cap) buf[(*pos)++] = c;
}

static void pf_puts(char *buf, uint32_t cap, uint32_t *pos, const char *s)
{
    while (*s) pf_putc(buf, cap, pos, *s++);
}

static void pf_putu(char *buf, uint32_t cap, uint32_t *pos, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) { pf_putc(buf, cap, pos, '0'); return; }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0) pf_putc(buf, cap, pos, tmp[n]);
}

static void pf_puti(char *buf, uint32_t cap, uint32_t *pos, int v)
{
    if (v < 0) {
        pf_putc(buf, cap, pos, '-');
        pf_putu(buf, cap, pos, (uint64_t)-v);
    } else {
        pf_putu(buf, cap, pos, (uint64_t)v);
    }
}

static void pf_putu_w(char *buf, uint32_t cap, uint32_t *pos,
                      uint64_t v, int width)
{
    int n = 1;
    uint64_t t = v;
    while (t >= 10) { n++; t /= 10; }
    for (int i = n; i < width; i++) pf_putc(buf, cap, pos, ' ');
    pf_putu(buf, cap, pos, v);
}

static void pf_put_secs_cs(char *buf, uint32_t cap, uint32_t *pos, uint64_t ms)
{
    pf_putu(buf, cap, pos, ms / 1000);
    pf_putc(buf, cap, pos, '.');
    uint64_t cs = (ms % 1000) / 10;
    if (cs < 10) pf_putc(buf, cap, pos, '0');
    pf_putu(buf, cap, pos, cs);
}

static char state_letter(int st)
{
    switch (st) {
    case 0: return 'R';
    case 1: return 'R';
    case 2: return 'W';
    case 3: return 'S';
    case 4: return 'D';
    case 5: return 'Z';
    default: return '?';
    }
}

static const char *state_word(int st)
{
    switch (st) {
    case 0: return "ready";
    case 1: return "running";
    case 2: return "waiting";
    case 3: return "sleeping";
    case 4: return "blocked";
    case 5: return "exited";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------
 * Renderers.  Each takes a buffer (capacity bytes) and returns
 * the rendered length, or -1 if the target doesn't exist.
 * ------------------------------------------------------------------ */

static long render_uptime(char *out, uint32_t cap)
{
    struct kstat_pub k;
    if (sys_kstat(&k) < 0) return -1;
    uint32_t pos = 0;
    pf_put_secs_cs(out, cap, &pos, k.uptime_ms);
    pf_putc(out, cap, &pos, ' ');
    pf_put_secs_cs(out, cap, &pos, k.uptime_ms);
    pf_putc(out, cap, &pos, '\n');
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static long render_meminfo(char *out, uint32_t cap)
{
    struct kstat_pub k;
    if (sys_kstat(&k) < 0) return -1;
    uint64_t used = (k.pmem_total_pages >= k.pmem_free_pages)
                     ? (k.pmem_total_pages - k.pmem_free_pages) : 0;
    const uint64_t PAGE_KB = 4;
    uint32_t pos = 0;
    pf_puts(out, cap, &pos, "MemTotal:    ");
    pf_putu(out, cap, &pos, k.pmem_total_pages * PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");
    pf_puts(out, cap, &pos, "MemFree:     ");
    pf_putu(out, cap, &pos, k.pmem_free_pages * PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");
    pf_puts(out, cap, &pos, "MemUsed:     ");
    pf_putu(out, cap, &pos, used * PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");
    pf_puts(out, cap, &pos, "PageSize:    ");
    pf_putu(out, cap, &pos, PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");
    pf_puts(out, cap, &pos, "KernelHeap:  ");
    pf_putu(out, cap, &pos, k.kheap_used_bytes);
    pf_puts(out, cap, &pos, " B\n");
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static long render_cpuinfo(char *out, uint32_t cap)
{
    struct kstat_pub k;
    if (sys_kstat(&k) < 0) return -1;
    uint32_t n = k.cpu_count;
    if (n == 0 || n > PROC_MAX_CPUS) n = 1;
    uint32_t pos = 0;
    pf_puts(out, cap, &pos, "cpus:        ");
    pf_putu(out, cap, &pos, n);
    pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "max_cpus:    ");
    pf_putu(out, cap, &pos, (uint64_t)PROC_MAX_CPUS);
    pf_putc(out, cap, &pos, '\n');
    for (uint32_t i = 0; i < n; i++) {
        pf_puts(out, cap, &pos, "cpu");
        pf_putu(out, cap, &pos, i);
        pf_puts(out, cap, &pos, ":        online\n");
    }
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static long render_sched(char *out, uint32_t cap)
{
    struct kstat_pub k;
    if (sys_kstat(&k) < 0) return -1;
    uint32_t n = k.cpu_count;
    if (n == 0 || n > PROC_MAX_CPUS) n = 1;
    uint32_t pos = 0;
    pf_puts(out, cap, &pos, "threads_live: ");
    pf_putu(out, cap, &pos, (uint64_t)k.live_threads);
    pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "cpu  runqueue\n");
    for (uint32_t i = 0; i < n; i++) {
        pf_putu_w(out, cap, &pos, (uint64_t)i, 3);
        pf_putc(out, cap, &pos, ' ');
        pf_putu_w(out, cap, &pos, (uint64_t)k.runq_len[i], 8);
        pf_putc(out, cap, &pos, '\n');
    }
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static int snap_one(int pid, struct thread_snap_pub *s)
{
    long r = sys_thread_snapshot(pid, s, 1);
    return r == 1 ? 1 : 0;
}

static long render_pid_status(int pid, char *out, uint32_t cap)
{
    struct thread_snap_pub s;
    if (!snap_one(pid, &s)) return -1;
    uint32_t pos = 0;
    pf_puts(out, cap, &pos, "Name:    "); pf_puts(out, cap, &pos, s.name); pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "Pid:     "); pf_puti(out, cap, &pos, s.id); pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "PPid:    "); pf_puti(out, cap, &pos, s.parent_id); pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "State:   ");
    pf_putc(out, cap, &pos, state_letter(s.state));
    pf_puts(out, cap, &pos, " (");
    pf_puts(out, cap, &pos, state_word(s.state));
    pf_puts(out, cap, &pos, ")\n");
    pf_puts(out, cap, &pos, "Cpu:     "); pf_putu(out, cap, &pos, (uint64_t)s.home_cpu); pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "TtyRaw:  "); pf_puti(out, cap, &pos, s.tty_raw); pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "Cwd:     "); pf_puts(out, cap, &pos, s.cwd); pf_putc(out, cap, &pos, '\n');
    if (s.state == 5) {
        pf_puts(out, cap, &pos, "ExitCode:");
        pf_putc(out, cap, &pos, ' ');
        pf_puti(out, cap, &pos, s.exit_code);
        pf_putc(out, cap, &pos, '\n');
    }
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static long render_pid_cmdline(int pid, char *out, uint32_t cap)
{
    struct thread_snap_pub s;
    if (!snap_one(pid, &s)) return -1;
    uint32_t pos = 0;
    if (s.args[0] == '\0') {
        pf_putc(out, cap, &pos, '[');
        pf_puts(out, cap, &pos, s.name);
        pf_putc(out, cap, &pos, ']');
    } else {
        pf_puts(out, cap, &pos, s.args);
    }
    pf_putc(out, cap, &pos, '\n');
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

/* ------------------------------------------------------------------
 * Static name tables — identical to the kernel original.
 * ------------------------------------------------------------------ */

static const char *const ROOT_FILES[] = { "uptime", "meminfo", "cpuinfo", "sched" };
#define ROOT_FILE_COUNT ((int)(sizeof(ROOT_FILES)/sizeof(ROOT_FILES[0])))

static const char *const PID_LEAVES[] = { "status", "cmdline", "trace" };
#define PID_LEAF_COUNT  ((int)(sizeof(PID_LEAVES)/sizeof(PID_LEAVES[0])))

static long render_root_dir(char *out, uint32_t cap)
{
    uint32_t pos = 0;
    for (int i = 0; i < ROOT_FILE_COUNT; i++) {
        pf_puts(out, cap, &pos, "f  /proc/");
        pf_puts(out, cap, &pos, ROOT_FILES[i]);
        pf_putc(out, cap, &pos, '\n');
    }
    /* Live pid subdirs.  Pull a snapshot — we cap at the static
     * SNAP_MAX below; if more threads exist we silently truncate
     * the listing the same way the kernel original did. */
    enum { SNAP_MAX = 64 };
    static struct thread_snap_pub snaps[SNAP_MAX];
    long n = sys_thread_snapshot(-1, snaps, SNAP_MAX);
    if (n < 0) n = 0;
    for (long i = 0; i < n; i++) {
        pf_puts(out, cap, &pos, "d  /proc/");
        pf_putu(out, cap, &pos, (uint64_t)snaps[i].id);
        pf_puts(out, cap, &pos, "/\n");
    }
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static long render_pid_dir(int pid, char *out, uint32_t cap)
{
    struct thread_snap_pub s;
    if (!snap_one(pid, &s)) return -1;
    uint32_t pos = 0;
    for (int i = 0; i < PID_LEAF_COUNT; i++) {
        pf_puts(out, cap, &pos, "f  /proc/");
        pf_putu(out, cap, &pos, (uint64_t)pid);
        pf_putc(out, cap, &pos, '/');
        pf_puts(out, cap, &pos, PID_LEAVES[i]);
        pf_putc(out, cap, &pos, '\n');
    }
    if (pos < cap) out[pos] = '\0';
    return (long)pos;
}

static long render_pid_trace(int pid, char *out, uint32_t cap)
{
    long r = sys_strace_render(pid, out, (unsigned long)cap);
    if (r < 0) {
        /* Map -ESRCH to "not found" so on_open reports -ENOENT. */
        return -1;
    }
    return r;
}

/* ------------------------------------------------------------------
 * Path parsing helpers.
 * ------------------------------------------------------------------ */

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int parse_pid(const char *s, const char **end)
{
    if (*s < '0' || *s > '9') return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    *end = s;
    return v;
}

/* Returns the rendered length on success or a negative value on
 * failure.  Picks the right buffer size by file (trace ring is
 * bigger than the regular text files). */
static long render_path(const char *path, char **buf_out, uint32_t *cap_out)
{
    /* Choose buffer size first so the caller can route the
     * trace file's heavier output to the larger buffer.       */
    int is_trace = 0;
    const char *p = path;
    if (p[0] >= '0' && p[0] <= '9') {
        const char *end;
        (void)parse_pid(p, &end);
        if (*end == '/' && eq(end + 1, "trace")) is_trace = 1;
    }
    uint32_t cap = is_trace ? PROCD_TRACE_CAP : PROCD_FILE_CAP;
    char *buf = (char *)malloc(cap);
    if (!buf) return -1;

    long n;
    if (*path == '\0') {
        n = render_root_dir(buf, cap);
    } else if (eq(path, "uptime")) {
        n = render_uptime(buf, cap);
    } else if (eq(path, "meminfo")) {
        n = render_meminfo(buf, cap);
    } else if (eq(path, "cpuinfo")) {
        n = render_cpuinfo(buf, cap);
    } else if (eq(path, "sched")) {
        n = render_sched(buf, cap);
    } else {
        const char *end;
        int pid = parse_pid(path, &end);
        if (pid < 0) { free(buf); return -1; }
        if (*end == '\0') {
            n = render_pid_dir(pid, buf, cap);
        } else if (*end != '/') {
            free(buf); return -1;
        } else {
            const char *leaf = end + 1;
            if (*leaf == '\0')             n = render_pid_dir(pid, buf, cap);
            else if (eq(leaf, "status"))   n = render_pid_status(pid, buf, cap);
            else if (eq(leaf, "cmdline"))  n = render_pid_cmdline(pid, buf, cap);
            else if (eq(leaf, "trace"))    n = render_pid_trace(pid, buf, cap);
            else                            n = -1;
        }
    }

    if (n < 0) { free(buf); return -1; }
    *buf_out = buf;
    *cap_out = (uint32_t)n;
    return n;
}

/* ------------------------------------------------------------------
 * userfs callbacks.
 * ------------------------------------------------------------------ */

static int on_open(void *ud, const char *path, int flags, uint32_t *handle_out)
{
    (void)ud;
    /* Read-only filesystem.  Reject any write/create/trunc bit
     * (O_WRONLY=1, O_RDWR=2, O_CREAT=0100, O_TRUNC=01000). */
    int rw = flags & 3;
    if (rw == 1 || rw == 2) return -13;  /* -EACCES */
    if (flags & 0100)        return -13;
    if (flags & 01000)       return -13;

    char *buf = (char *)0;
    uint32_t len = 0;
    long n = render_path(path, &buf, &len);
    if (n < 0) return -2;                /* -ENOENT */

    uint32_t h = slot_alloc(buf, len);
    if (h == 0) { free(buf); return -24; } /* -EMFILE */
    *handle_out = h;
    return 0;
}

static int on_read(void *ud, uint32_t h, uint64_t off, void *buf, uint32_t cap)
{
    (void)ud;
    if (h == 0 || (int)h > PROCD_MAX_HANDLES) return -9; /* -EBADF */
    struct slot *s = &g_slots[h - 1];
    if (!s->used || !s->buf) return -9;
    if (off >= s->len) return 0;
    uint32_t avail = s->len - (uint32_t)off;
    uint32_t take  = avail < cap ? avail : cap;
    const uint8_t *src = (const uint8_t *)s->buf + off;
    uint8_t *dst       = (uint8_t *)buf;
    for (uint32_t i = 0; i < take; i++) dst[i] = src[i];
    return (int)take;
}

static int on_write(void *ud, uint32_t h, uint64_t off,
                    const void *buf, uint32_t n)
{
    (void)ud; (void)h; (void)off; (void)buf; (void)n;
    return -13;                          /* -EACCES — /proc is RO */
}

static int on_close(void *ud, uint32_t h)
{
    (void)ud;
    slot_free(h);
    return 0;
}

static int on_listdir(void *ud, const char *path, int idx,
                      char *name, uint32_t cap, uint32_t *type)
{
    (void)ud;
    if (idx < 0) return -2;

    /* Root directory. */
    if (path[0] == '\0') {
        if (idx < ROOT_FILE_COUNT) {
            const char *s = ROOT_FILES[idx];
            uint32_t k = 0;
            while (s[k] && k + 1 < cap) { name[k] = s[k]; k++; }
            name[k] = '\0';
            *type = 1;                    /* file */
            return (int)k;
        }
        int rel = idx - ROOT_FILE_COUNT;
        enum { SNAP_MAX = 64 };
        static struct thread_snap_pub snaps[SNAP_MAX];
        long ntot = sys_thread_snapshot(-1, snaps, SNAP_MAX);
        if (ntot < 0 || rel >= (int)ntot) return -2;
        /* Format the pid as decimal. */
        int v = snaps[rel].id;
        if (v < 0) v = 0;
        int len = 1;
        int t = v;
        while (t >= 10) { len++; t /= 10; }
        if ((uint32_t)len + 1 > cap) len = (int)cap - 1;
        name[len] = '\0';
        for (int i = len - 1; i >= 0; i--) { name[i] = (char)('0' + v % 10); v /= 10; }
        *type = 2;                        /* directory */
        return len;
    }

    /* Per-pid directory.  path must parse fully as a pid. */
    const char *end;
    int pid = parse_pid(path, &end);
    if (pid < 0 || *end != '\0') return -2;
    struct thread_snap_pub s;
    if (!snap_one(pid, &s)) return -2;
    if (idx >= PID_LEAF_COUNT) return -2;
    const char *leaf = PID_LEAVES[idx];
    uint32_t k = 0;
    while (leaf[k] && k + 1 < cap) { name[k] = leaf[k]; k++; }
    name[k] = '\0';
    *type = 1;                            /* file */
    return (int)k;
}

static int on_is_dir(void *ud, const char *path)
{
    (void)ud;
    if (path[0] == '\0') return 1;       /* root */
    /* Top-level scalar files. */
    if (eq(path, "uptime") || eq(path, "meminfo") ||
        eq(path, "cpuinfo") || eq(path, "sched")) return 0;
    const char *end;
    int pid = parse_pid(path, &end);
    if (pid < 0) return -2;
    struct thread_snap_pub s;
    if (!snap_one(pid, &s)) return -2;
    if (*end == '\0') return 1;           /* "/proc/<pid>" */
    if (*end != '/')  return -2;
    const char *leaf = end + 1;
    if (*leaf == '\0') return 1;
    if (eq(leaf, "status") || eq(leaf, "cmdline") ||
        eq(leaf, "trace")) return 0;
    return -2;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct userfs_handler h;
    h.on_open    = on_open;
    h.on_read    = on_read;
    h.on_write   = on_write;
    h.on_close   = on_close;
    h.on_listdir = on_listdir;
    h.on_unlink  = (int (*)(void *, const char *))0;
    h.on_mkdir   = (int (*)(void *, const char *))0;
    h.on_is_dir  = on_is_dir;
    h.userdata   = (void *)0;

    int r = userfs_serve_flags("/proc", &h, USERFS_MOUNT_RO);
    printf("procd: serve returned %d\n", r);
    return r < 0 ? 1 : 0;
}
