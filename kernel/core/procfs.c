/*
 * kernel/core/procfs.c — chapter 99 read-only /proc pseudo-FS.
 *
 * Each file's content is rendered into a caller buffer on
 * demand.  vfs_open allocates that buffer; close releases it.
 * We never store generated text in the procfs module itself.
 *
 * Formatting is hand-rolled because the kernel does not link
 * a printf — see the pf_put* helpers below.  They're enough
 * for the simple numeric + string output every /proc file in
 * this chapter produces.
 */

#include "procfs.h"
#include "thread.h"
#include "timer.h"
#include "pmem.h"
#include "heap.h"
#include "../arch/cpu.h"
#include <stddef.h>
#include <stdint.h>

/* TICK_INTERVAL_MS is defined in timer.h. */

/* ------------------------------------------------------------------
 * Tiny formatter.  Every public render_* writes into a
 * caller-provided buffer of size cap; we track the running
 * length in *pos.  Overflow is detected by every put helper —
 * once *pos hits cap-1 we drop further bytes (leaving room for
 * the NUL the top-level render adds).  Truncation is silent;
 * callers size buffers based on PROCFS_MAX_FILE and our outputs
 * are all far below that.
 * ------------------------------------------------------------------ */

static void pf_putc(char *buf, size_t cap, size_t *pos, char c)
{
    if (*pos + 1 < cap) buf[(*pos)++] = c;
}

static void pf_puts(char *buf, size_t cap, size_t *pos, const char *s)
{
    while (*s) pf_putc(buf, cap, pos, *s++);
}

/* Print a uint64 in base 10.  No padding; minimum width 1
 * (so "0" prints as "0" rather than "").  Buffer-of-20 is
 * enough for 2^64 = "18446744073709551615" (20 digits). */
static void pf_putu(char *buf, size_t cap, size_t *pos, uint64_t v)
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

/* Signed-int convenience. */
static void pf_puti(char *buf, size_t cap, size_t *pos, int v)
{
    if (v < 0) { pf_putc(buf, cap, pos, '-'); pf_putu(buf, cap, pos, (uint64_t)-v); }
    else        pf_putu(buf, cap, pos, (uint64_t)v);
}

/* Pad an integer to a minimum field width with spaces on the
 * left.  Used for `ps`-style column alignment in /proc/sched. */
static void pf_putu_w(char *buf, size_t cap, size_t *pos,
                       uint64_t v, int width)
{
    /* Count digits without printing. */
    int n = 1;
    uint64_t t = v;
    while (t >= 10) { n++; t /= 10; }
    for (int i = n; i < width; i++) pf_putc(buf, cap, pos, ' ');
    pf_putu(buf, cap, pos, v);
}

/* Two seconds-and-fractional formats Linux's /proc/uptime uses:
 *   "<int>.<2-digit>"      — 0.01 second resolution.
 * We expose `ms / 10` as the fractional centiseconds. */
static void pf_put_secs_cs(char *buf, size_t cap, size_t *pos, uint64_t ms)
{
    pf_putu(buf, cap, pos, ms / 1000);
    pf_putc(buf, cap, pos, '.');
    uint64_t cs = (ms % 1000) / 10;
    if (cs < 10) pf_putc(buf, cap, pos, '0');
    pf_putu(buf, cap, pos, cs);
}

/* Map enum thread_state -> short char.  Same letters Linux's
 * /proc uses: R=Running/Runnable, S=Sleeping (interruptible),
 * D=Disk-wait (we map BLOCKED to this), W=Waiting (legacy),
 * Z=Zombie (EXITED, awaiting reap).  We don't have stopped/
 * traced yet. */
static char state_letter(int st)
{
    switch (st) {
    case 0: return 'R';   /* THREAD_READY  */
    case 1: return 'R';   /* THREAD_RUNNING */
    case 2: return 'W';   /* THREAD_WAITING — blocked in wait() */
    case 3: return 'S';   /* THREAD_SLEEPING */
    case 4: return 'D';   /* THREAD_BLOCKED  — I/O wait */
    case 5: return 'Z';   /* THREAD_EXITED   — zombie */
    default: return '?';
    }
}

/* Same as state_letter but returns the full word; used by
 * /proc/<pid>/status for friendlier readability. */
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
 * Renderers per file.
 * ------------------------------------------------------------------ */

static long render_uptime(char *out, size_t cap)
{
    size_t pos = 0;
    uint64_t ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
    /* Linux /proc/uptime is "<uptime_secs> <idle_secs>".  We
     * don't track idle separately (no per-CPU idle accumulator
     * yet) so we just print uptime twice — programs that parse
     * the file see two well-formed numbers regardless. */
    pf_put_secs_cs(out, cap, &pos, ms);
    pf_putc(out, cap, &pos, ' ');
    pf_put_secs_cs(out, cap, &pos, ms);
    pf_putc(out, cap, &pos, '\n');
    out[pos] = '\0';
    return (long)pos;
}

static long render_meminfo(char *out, size_t cap)
{
    size_t pos = 0;
    uint64_t total_pages = (uint64_t)pmem_total_pages();
    uint64_t free_pages  = (uint64_t)pmem_free_pages();
    uint64_t used_pages  = (total_pages >= free_pages)
                            ? (total_pages - free_pages) : 0;
    /* Page size is 4 KiB (chapter 5 invariant — referenced
     * everywhere from boot.s to pmem.c).  Hard-code rather
     * than pulling in arch headers for one constant. */
    const uint64_t PAGE_KB = 4;
    uint64_t heap_used   = (uint64_t)kheap_used();

    pf_puts(out, cap, &pos, "MemTotal:    ");
    pf_putu(out, cap, &pos, total_pages * PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");

    pf_puts(out, cap, &pos, "MemFree:     ");
    pf_putu(out, cap, &pos, free_pages * PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");

    pf_puts(out, cap, &pos, "MemUsed:     ");
    pf_putu(out, cap, &pos, used_pages * PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");

    pf_puts(out, cap, &pos, "PageSize:    ");
    pf_putu(out, cap, &pos, PAGE_KB);
    pf_puts(out, cap, &pos, " kB\n");

    pf_puts(out, cap, &pos, "KernelHeap:  ");
    pf_putu(out, cap, &pos, heap_used);
    pf_puts(out, cap, &pos, " B\n");

    out[pos] = '\0';
    return (long)pos;
}

static long render_cpuinfo(char *out, size_t cap)
{
    size_t pos = 0;
    uint32_t n = smp_cpu_count();
    if (n == 0 || n > SMP_MAX_CPUS) n = 1;

    pf_puts(out, cap, &pos, "cpus:        ");
    pf_putu(out, cap, &pos, n);
    pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "max_cpus:    ");
    pf_putu(out, cap, &pos, (uint64_t)SMP_MAX_CPUS);
    pf_putc(out, cap, &pos, '\n');
    /* Per-CPU lines: just the id and "online" flag.  No model
     * name yet (we'd need to decode MIDR_EL1 across vendors).
     * Chapter 86 documented the PSCI bring-up; we read the
     * cpu_state directly from g_cpus[]. */
    for (uint32_t i = 0; i < n; i++) {
        pf_puts(out, cap, &pos, "cpu");
        pf_putu(out, cap, &pos, i);
        pf_puts(out, cap, &pos, ":        online\n");
    }
    out[pos] = '\0';
    return (long)pos;
}

static long render_sched(char *out, size_t cap)
{
    size_t pos = 0;
    uint32_t n = smp_cpu_count();
    if (n == 0 || n > SMP_MAX_CPUS) n = 1;
    int total = thread_live_count();
    pf_puts(out, cap, &pos, "threads_live: ");
    pf_putu(out, cap, &pos, (uint64_t)total);
    pf_putc(out, cap, &pos, '\n');
    pf_puts(out, cap, &pos, "cpu  runqueue\n");
    for (uint32_t i = 0; i < n; i++) {
        pf_putu_w(out, cap, &pos, (uint64_t)i, 3);
        pf_putc(out, cap, &pos, ' ');
        pf_putu_w(out, cap, &pos,
                   (uint64_t)thread_runqueue_len(i), 8);
        pf_putc(out, cap, &pos, '\n');
    }
    out[pos] = '\0';
    return (long)pos;
}

static long render_pid_status(int pid, char *out, size_t cap)
{
    struct thread_snap s;
    if (!thread_snapshot_pid(pid, &s)) return -1;

    size_t pos = 0;
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
    if (s.state == 5) {     /* THREAD_EXITED */
        pf_puts(out, cap, &pos, "ExitCode:");
        pf_putc(out, cap, &pos, ' ');
        pf_puti(out, cap, &pos, s.exit_code);
        pf_putc(out, cap, &pos, '\n');
    }
    out[pos] = '\0';
    return (long)pos;
}

/* ------------------------------------------------------------------
 * Static name tables.  Used both by the renderers below (so they
 * can format the textual listing for `cat /proc`) and by the
 * directory iteration entry points further down (`procfs_listdir`,
 * called from sys_listdir_at).  Two consumers, one source of
 * truth — adding a new top-level file is a one-line change here.
 * ------------------------------------------------------------------ */

static const char *const PROCFS_ROOT_FILES[] = {
    "uptime",
    "meminfo",
    "cpuinfo",
    "sched",
};
#define PROCFS_ROOT_FILE_COUNT \
    ((int)(sizeof(PROCFS_ROOT_FILES) / sizeof(PROCFS_ROOT_FILES[0])))

static const char *const PROCFS_PID_LEAVES[] = {
    "status",
    "cmdline",
};
#define PROCFS_PID_LEAF_COUNT \
    ((int)(sizeof(PROCFS_PID_LEAVES) / sizeof(PROCFS_PID_LEAVES[0])))

static long render_pid_cmdline(int pid, char *out, size_t cap)
{
    struct thread_snap s;
    if (!thread_snapshot_pid(pid, &s)) return -1;

    size_t pos = 0;
    /* args is a single space-separated argv string set by spawn().
     * Kernel threads and the boot thread have args == "" — Linux
     * uses "[name]" for those (e.g. "[kthreadd]"), which makes
     * `cat /proc/<pid>/cmdline` always emit something readable
     * rather than a bare newline. */
    if (s.args[0] == '\0') {
        pf_putc(out, cap, &pos, '[');
        pf_puts(out, cap, &pos, s.name);
        pf_putc(out, cap, &pos, ']');
    } else {
        pf_puts(out, cap, &pos, s.args);
    }
    pf_putc(out, cap, &pos, '\n');
    out[pos] = '\0';
    return (long)pos;
}

/* Render the root /proc directory as a textual table — used when
 * the user runs `cat /proc` (no leaf).  Linux's procfs returns
 * EISDIR here, but our shell has no special handling for that and
 * showing the listing is more informative than an error.  The
 * format mimics `ls -l` columns so it's still parseable. */
static long render_proc_root_dir(char *out, size_t cap)
{
    size_t pos = 0;
    /* Static top-level files first. */
    for (int i = 0; i < PROCFS_ROOT_FILE_COUNT; i++) {
        pf_puts(out, cap, &pos, "f  /proc/");
        pf_puts(out, cap, &pos, PROCFS_ROOT_FILES[i]);
        pf_putc(out, cap, &pos, '\n');
    }
    /* Live pid subdirectories. */
    struct thread_snap snaps[32];
    int n = thread_snapshot(snaps, 32);
    for (int i = 0; i < n; i++) {
        pf_puts(out, cap, &pos, "d  /proc/");
        pf_putu(out, cap, &pos, (uint64_t)snaps[i].id);
        pf_puts(out, cap, &pos, "/\n");
    }
    out[pos] = '\0';
    return (long)pos;
}

/* Render a per-pid directory as text.  Same motivation as
 * render_proc_root_dir — turn `cat /proc/<pid>` into something
 * useful instead of ENOENT. */
static long render_pid_dir(int pid, char *out, size_t cap)
{
    struct thread_snap s;
    if (!thread_snapshot_pid(pid, &s)) return -1;
    size_t pos = 0;
    for (int i = 0; i < PROCFS_PID_LEAF_COUNT; i++) {
        pf_puts(out, cap, &pos, "f  /proc/");
        pf_putu(out, cap, &pos, (uint64_t)pid);
        pf_putc(out, cap, &pos, '/');
        pf_puts(out, cap, &pos, PROCFS_PID_LEAVES[i]);
        pf_putc(out, cap, &pos, '\n');
    }
    out[pos] = '\0';
    return (long)pos;
}

/* ------------------------------------------------------------------
 * Path dispatcher.  `path` is the suffix after "/proc/" — see
 * vfs_open for the prefix stripping.
 * ------------------------------------------------------------------ */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == *b);
}

/* Parse a leading non-negative decimal integer.  Stops at the
 * first non-digit and sets *end_out to point at it.  Returns -1
 * if the first character isn't a digit. */
static int parse_pid(const char *s, const char **end_out)
{
    if (*s < '0' || *s > '9') return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    *end_out = s;
    return v;
}

long procfs_render(const char *path, char *out, size_t cap)
{
    if (!path || !out || cap < 2) return -1;

    /* Empty suffix → "/proc" (or "/proc/") itself.  Render the
     * directory listing as text so `cat /proc` is useful. */
    if (*path == '\0') return render_proc_root_dir(out, cap);

    /* Top-level scalar files. */
    if (str_eq(path, "uptime"))   return render_uptime(out, cap);
    if (str_eq(path, "meminfo"))  return render_meminfo(out, cap);
    if (str_eq(path, "cpuinfo"))  return render_cpuinfo(out, cap);
    if (str_eq(path, "sched"))    return render_sched(out, cap);

    /* Per-pid lookup.  Either "<pid>" (whole-dir listing) or
     * "<pid>/<leaf>" (single file). */
    const char *p = path;
    const char *after_pid;
    int pid = parse_pid(p, &after_pid);
    if (pid < 0) return -1;
    if (*after_pid == '\0') return render_pid_dir(pid, out, cap);
    if (*after_pid != '/')  return -1;
    const char *leaf = after_pid + 1;
    /* Trailing slash on the pid ("/proc/12/") also renders the
     * pid's directory listing. */
    if (*leaf == '\0') return render_pid_dir(pid, out, cap);
    if (str_eq(leaf, "status"))  return render_pid_status(pid, out, cap);
    if (str_eq(leaf, "cmdline")) return render_pid_cmdline(pid, out, cap);
    return -1;
}

/* ------------------------------------------------------------------
 * Directory iteration.
 *
 *   subdir == "" or NULL      → root: uptime, meminfo, cpuinfo,
 *                                sched + one entry per live pid
 *   subdir == "<pid>"          → that pid's leaves: status, cmdline
 *
 * The static name tables (PROCFS_ROOT_FILES, PROCFS_PID_LEAVES)
 * are declared at the top of the file — both these iterators and
 * the textual directory renderers above consume them.
 *
 * Index ordering is stable for a single iterator pass but NOT
 * across calls — pids come from the live thread list, which can
 * shuffle if a thread exits between calls.  Same caveat as
 * Linux's /proc.
 * ------------------------------------------------------------------ */

/* Copy `s` into name[] and report its length.  Truncates to
 * cap-1 plus NUL. */
static int copy_leaf_name(const char *s, char *name, size_t cap)
{
    size_t n = 0;
    while (s[n] && n + 1 < cap) { name[n] = s[n]; n++; }
    name[n] = '\0';
    return (int)n;
}

/* Decimal length of a non-negative int. */
static int decimal_len(int v)
{
    if (v <= 0) return 1;
    int n = 0;
    while (v > 0) { n++; v /= 10; }
    return n;
}

/* Format a non-negative int into name[].  Returns its length. */
static int format_int(int v, char *name, size_t cap)
{
    if (v < 0) v = 0;
    int n = decimal_len(v);
    if ((size_t)n + 1 > cap) n = (int)cap - 1;
    name[n] = '\0';
    for (int i = n - 1; i >= 0; i--) { name[i] = (char)('0' + v % 10); v /= 10; }
    return n;
}

int procfs_listdir(const char *subdir, int idx,
                    char *name, size_t cap, uint32_t *type_out)
{
    if (!name || cap == 0) return -1;
    if (idx < 0) return -1;

    /* Root directory. */
    if (!subdir || !*subdir) {
        if (idx < PROCFS_ROOT_FILE_COUNT) {
            if (type_out) *type_out = 1;       /* file */
            return copy_leaf_name(PROCFS_ROOT_FILES[idx], name, cap);
        }
        /* Past the static files — enumerate live pids. */
        int rel = idx - PROCFS_ROOT_FILE_COUNT;
        /* We don't keep a sorted snapshot here because the
         * caller will iterate from idx=0 to past-end anyway —
         * just take a full snapshot, pick the rel-th entry.
         * Stack-allocate a small buffer; on overflow give up
         * (any system with >256 threads has bigger problems
         * than `ls /proc` returning a partial list).  Each
         * snap is fairly large (~256 B) — sized at 32 to keep
         * the stack usage bounded; bump if needed. */
        struct thread_snap snaps[32];
        int n = thread_snapshot(snaps, 32);
        if (rel >= n) return -1;
        if (type_out) *type_out = 2;            /* directory */
        return format_int(snaps[rel].id, name, cap);
    }

    /* Per-pid directory.  subdir must parse fully as a pid. */
    const char *end;
    int pid = parse_pid(subdir, &end);
    if (pid < 0 || *end != '\0') return -1;
    /* Validate the pid is actually live. */
    struct thread_snap s;
    if (!thread_snapshot_pid(pid, &s)) return -1;

    if (idx < PROCFS_PID_LEAF_COUNT) {
        if (type_out) *type_out = 1;
        return copy_leaf_name(PROCFS_PID_LEAVES[idx], name, cap);
    }
    return -1;
}

int procfs_is_dir(const char *path)
{
    if (!path || !*path) return 1;          /* root */
    const char *end;
    int pid = parse_pid(path, &end);
    if (pid < 0) return 0;
    if (*end != '\0') return 0;             /* "<pid>/..." is a file */
    struct thread_snap s;
    return thread_snapshot_pid(pid, &s);
}
