/* userspace/strace/strace.c — chapter 102 syscall tracer driver.
 *
 *   strace <program> [args...]
 *
 * Fork a child, turn on per-thread tracing in the child, then
 * exec the target program.  In the parent, poll
 * `/proc/<child>/trace` and write whatever the kernel renders
 * straight to stderr (fd 2) so it stays separate from the
 * traced program's stdout.
 *
 * Pattern fits the trace ring's snapshot-and-drain semantics:
 * each open() of the trace file returns whatever entries have
 * accumulated since the last read, then drains them.  We loop
 * until the child has exited AND the trace file renders empty.
 *
 * Deliberately minimal: no -p (attach), no -e (filter), no -o
 * (output file).  Those are user-facing knobs the kernel
 * tracer is already powerful enough to back when someone wants
 * them.  See the chapter for the design.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

static void put_err(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    write(2, s, (size_t)n);
}

/* Format a small int into base 10 and write to fd 2.  No printf
 * dependency on this hot path so the tracer itself contributes
 * fewer syscalls to any trace of itself. */
static void put_dec(int v)
{
    char buf[12];
    int  i = 0;
    int  neg = (v < 0);
    unsigned u = (unsigned)(neg ? -v : v);
    if (u == 0) buf[i++] = '0';
    while (u) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[i++] = '-';
    /* Reverse. */
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    write(2, buf, (size_t)i);
}

/* Drain /proc/<pid>/trace into stderr.  Returns the number of
 * bytes pulled (0 means the ring was empty this round, or that
 * the kernel rendered only its "(not traced)" banner — see the
 * banner-suppression note below). */
static long pump_trace_once(int pid)
{
    /* Build "/proc/<pid>/trace" by hand to avoid pulling in
     * snprintf for one path. */
    char path[40];
    int  i = 0;
    const char *p = "/proc/";
    while (*p) path[i++] = *p++;
    /* Itoa(pid). */
    char ds[12];
    int  dn = 0;
    int  v  = pid;
    if (v == 0) ds[dn++] = '0';
    while (v > 0) { ds[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn--) path[i++] = ds[dn];
    p = "/trace";
    while (*p) path[i++] = *p++;
    path[i] = '\0';

    int fd = open(path, 0);
    if (fd < 0) return -1;

    long total = 0;
    char buf[1024];
    /* Banner suppression: when the target hasn't called
     * trace_me yet (e.g. between fork and exec, or for the
     * brief window before the child is scheduled), the kernel
     * renders the literal string "(not traced)\n".  That's
     * helpful for `cat /proc/<pid>/trace`, but here it would
     * flood our stderr until the child finally enables
     * tracing.  Detect a single-read banner and drop it on
     * the floor — return 0 so the caller's idle-sleep kicks
     * in and we don't busy-loop. */
    static const char BANNER[] = "(not traced)\n";
    enum { BANNER_LEN = sizeof(BANNER) - 1 };
    long got = read(fd, buf, sizeof(buf));
    if (got == BANNER_LEN) {
        int eq = 1;
        for (int k = 0; k < BANNER_LEN; k++)
            if (buf[k] != BANNER[k]) { eq = 0; break; }
        if (eq) {
            /* Eat the rest (procfs is one-shot, so this should
             * be EOF, but be defensive). */
            while (read(fd, buf, sizeof(buf)) > 0) {}
            close(fd);
            return 0;
        }
    }
    if (got > 0) {
        write(2, buf, (size_t)got);
        total += got;
    }
    for (;;) {
        got = read(fd, buf, sizeof(buf));
        if (got <= 0) break;
        write(2, buf, (size_t)got);
        total += got;
    }
    close(fd);
    return total;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        put_err("usage: strace <program> [args...]\n");
        return 2;
    }

    int kid = fork();
    if (kid < 0) {
        put_err("strace: fork failed\n");
        return 1;
    }
    if (kid == 0) {
        /* Child — enable tracing on self, then exec target.
         * The trace ring lives on this thread and survives the
         * exec (struct thread fields are preserved across the
         * AS swap in sys_exec).  trace_me failures are
         * non-fatal: we exec anyway so the user at least gets
         * the program's output. */
        (void)trace_me();
        execv(argv[1], &argv[1]);
        /* exec returned — must have failed. */
        put_err("strace: execv failed for ");
        put_err(argv[1]);
        put_err("\n");
        exit(127);
    }

    /* Parent — poll the trace file until the child exits, then
     * pump one more time to flush any final entries that landed
     * between our last open and the child's exit. */
    for (;;) {
        long n = pump_trace_once(kid);
        if (n < 0) {
            /* Open failed.  Most likely the child has been
             * reaped already (zombie removed) — bail. */
            break;
        }
        int code = 0;
        int reaped = waitpid(kid, &code, WNOHANG);
        if (reaped == kid) {
            /* Final drain after exit — captures any syscalls
             * the kernel recorded between our last read and
             * the exit() that terminated the child. */
            (void)pump_trace_once(kid);
            put_err("strace: + exited with code ");
            put_dec(code);
            put_err("\n");
            return code;
        }
        if (n == 0) {
            /* Ring was empty this round; back off briefly so
             * we don't spin a CPU watching an idle child. */
            sleep_ms(20);
        }
    }
    return 0;
}
