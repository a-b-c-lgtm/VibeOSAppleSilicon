/* userspace/chldtest/chldtest.c \u2014 chapter 77 SIGCHLD + waitpid test.
 *
 * Five checks, all gated on `[chldtest] FAIL` lines:
 *
 *   1. SIGCHLD fires asynchronously when a child exits.  Parent
 *      installs a SIGCHLD handler, forks a short-lived child,
 *      and yields.  The handler runs (sig=17).  The child is
 *      then reaped via waitpid(-1, ..., 0).
 *
 *   2. waitpid(specific_pid, ..., 0) reaps that specific child
 *      while another child (longer-lived) is still running.
 *
 *   3. waitpid(child, ..., WNOHANG) returns 0 while the child
 *      is alive, then returns the child's pid once it exits.
 *
 *   4. SIGCHLD's default action is "ignore".  A program that
 *      never installs a SIGCHLD handler must NOT be killed
 *      when its child exits.  We re-exec ourselves through a
 *      fresh fork to verify the no-handler case (after check 1
 *      we have a handler installed in this process).
 *
 *   5. wait() (the legacy any-child shape) still works after
 *      the SYS_WAIT \u2192 thread_waitpid(-1, ..., 0) refactor.
 *
 * Success line: `[chldtest] all checks passed`. */

#include "../libc/syscall.h"
#include "../libc/signal.h"

static volatile int g_chld_handler_runs;
static volatile int g_last_chld_sig;

static void on_chld(int sig)
{
    g_chld_handler_runs++;
    g_last_chld_sig = sig;
    puts("[chldtest] SIGCHLD handler ran sig=");
    putd(sig);
    puts("\n");
}

/* Spawn a child that immediately exits with `code`.  Returns
 * the child's pid. */
static int spawn_quick_child(int code)
{
    int p = fork();
    if (p == 0) exit(code);
    return p;
}

/* Spawn a child that yields N times then exits with `code`. */
static int spawn_slow_child(int spins, int code)
{
    int p = fork();
    if (p == 0) {
        for (int i = 0; i < spins; i++) yield();
        exit(code);
    }
    return p;
}

int main(int argc, char **argv)
{
    (void)argv;     /* only argc gates the re-entrant child path */
    /* Argv-driven re-entrant mode for check 4: when invoked as
     * "chldtest --no-handler-child", we are the freshly-fork'd
     * child that must NOT install a SIGCHLD handler.  Spawn a
     * grandchild and wait() for it.  If we survive, check 4
     * passes \u2014 if SIGCHLD's default action killed us, the parent
     * would see exit code (128 + SIGCHLD) = 145 instead of 0. */
    if (argc >= 2) {
        int gc = spawn_quick_child(0);
        if (gc < 0) return 11;
        int code = -1;
        int r = wait(&code);
        if (r != gc || code != 0) return 12;
        return 0;
    }

    puts("[chldtest] starting\n");

    /* ----- Check 1: SIGCHLD handler fires on child exit. ----- */
    g_chld_handler_runs = 0;
    g_last_chld_sig     = 0;
    if (signal(SIGCHLD, on_chld) == (sig_handler_t)-1) {
        puts("[chldtest] FAIL: signal(SIGCHLD) rejected\n");
        return 1;
    }
    int c1 = spawn_quick_child(7);
    if (c1 < 0) {
        puts("[chldtest] FAIL: check1 fork() failed\n");
        return 1;
    }
    /* Yield enough that the child runs and exits.  Each yield
     * traps into the kernel \u2014 at the next return tail any
     * pending SIGCHLD will be delivered. */
    for (int i = 0; i < 50 && g_chld_handler_runs == 0; i++) yield();
    if (g_chld_handler_runs == 0) {
        puts("[chldtest] FAIL: SIGCHLD handler never ran\n");
        return 1;
    }
    if (g_last_chld_sig != SIGCHLD) {
        puts("[chldtest] FAIL: handler saw wrong signum\n");
        return 1;
    }
    int code1 = -1;
    int r1 = waitpid(-1, &code1, 0);
    if (r1 != c1 || code1 != 7) {
        puts("[chldtest] FAIL: check1 waitpid mismatch\n");
        return 1;
    }
    puts("[chldtest] check 1 (SIGCHLD on exit) ok\n");

    /* ----- Check 2: waitpid(specific_pid). ----- */
    int slow = spawn_slow_child(200, 11);
    int fast = spawn_quick_child(22);
    if (slow < 0 || fast < 0) {
        puts("[chldtest] FAIL: check2 fork() failed\n");
        return 1;
    }
    /* Reap fast first by name, even though slow exists. */
    int code2 = -1;
    int r2    = waitpid(fast, &code2, 0);
    if (r2 != fast || code2 != 22) {
        puts("[chldtest] FAIL: check2 specific waitpid wrong pid/code pid=");
        putd(r2);
        puts(" code=");
        putd(code2);
        puts("\n");
        return 1;
    }
    /* Then reap slow with -1. */
    int code2b = -1;
    int r2b    = waitpid(-1, &code2b, 0);
    if (r2b != slow || code2b != 11) {
        puts("[chldtest] FAIL: check2 slow waitpid wrong\n");
        return 1;
    }
    puts("[chldtest] check 2 (waitpid by pid) ok\n");

    /* ----- Check 3: WNOHANG. ----- */
    int slow3 = spawn_slow_child(100, 33);
    if (slow3 < 0) {
        puts("[chldtest] FAIL: check3 fork() failed\n");
        return 1;
    }
    /* While the child is still running, WNOHANG must return 0. */
    int saw_zero = 0;
    for (int i = 0; i < 5; i++) {
        int code3 = -1;
        int r3    = waitpid(slow3, &code3, WNOHANG);
        if (r3 == 0) { saw_zero = 1; break; }
        if (r3 == slow3) {
            puts("[chldtest] FAIL: WNOHANG reaped immediately (race?)\n");
            return 1;
        }
        if (r3 < 0) {
            puts("[chldtest] FAIL: WNOHANG returned -1 with live child\n");
            return 1;
        }
    }
    if (!saw_zero) {
        puts("[chldtest] FAIL: WNOHANG never returned 0\n");
        return 1;
    }
    /* Now drain.  Yield until WNOHANG returns the pid. */
    int code3 = -1;
    int r3    = -1;
    for (int i = 0; i < 1000; i++) {
        r3 = waitpid(slow3, &code3, WNOHANG);
        if (r3 == slow3) break;
        if (r3 < 0) break;
        yield();
    }
    if (r3 != slow3 || code3 != 33) {
        puts("[chldtest] FAIL: WNOHANG never returned pid\n");
        return 1;
    }
    puts("[chldtest] check 3 (WNOHANG) ok\n");

    /* ----- Check 4: SIGCHLD's default action is ignore. ----- */
    /* Re-exec ourselves through a fresh fork, with --no-handler-child
     * arg.  In that branch we do NOT install a SIGCHLD handler; we
     * just spawn a grandchild and wait().  Survival proves that
     * SIGCHLD's default action did not terminate us. */
    int p4 = fork();
    if (p4 < 0) {
        puts("[chldtest] FAIL: check4 fork() failed\n");
        return 1;
    }
    if (p4 == 0) {
        char *argv2[] = { "chldtest", "--no-handler-child", 0 };
        execv("/bin/chldtest", argv2);
        exit(99);   /* exec failed */
    }
    int code4 = -1;
    int r4    = waitpid(p4, &code4, 0);
    if (r4 != p4 || code4 != 0) {
        puts("[chldtest] FAIL: check4 child died (code=");
        putd(code4);
        puts(")\n");
        return 1;
    }
    puts("[chldtest] check 4 (SIGCHLD default = ignore) ok\n");

    /* ----- Check 5: legacy wait() still works. ----- */
    int c5 = spawn_quick_child(55);
    if (c5 < 0) {
        puts("[chldtest] FAIL: check5 fork() failed\n");
        return 1;
    }
    int code5 = -1;
    int r5    = wait(&code5);
    if (r5 != c5 || code5 != 55) {
        puts("[chldtest] FAIL: legacy wait() broken\n");
        return 1;
    }
    puts("[chldtest] check 5 (legacy wait) ok\n");

    puts("[chldtest] all checks passed\n");
    return 0;
}
