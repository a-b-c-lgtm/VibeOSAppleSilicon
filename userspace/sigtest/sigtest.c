/* userspace/sigtest/sigtest.c — chapter 76 catchable-signal test.
 *
 * Three checks:
 *
 *   1. signal-then-kill-self.  Install a SIGUSR-style handler
 *      (we use SIGTERM since SIGUSR isn't defined yet), then
 *      kill(getpid(), SIGTERM).  Expect the handler to run and
 *      execution to resume cleanly past the kill().
 *
 *   2. SIG_IGN.  Install SIG_IGN for SIGTERM, kill ourselves,
 *      verify we keep running and the handler did NOT run.
 *
 *   3. fork-then-signal-child.  Parent forks; child installs
 *      a handler and busy-loops.  Parent sleeps a tick, then
 *      kill(child_pid, SIGINT).  Child's handler runs, prints
 *      a marker, and exits.  Parent reaps and verifies exit
 *      code == 0 (handler exited cleanly, did NOT inherit
 *      the 128+sig default).
 *
 * Success line: `[sigtest] all checks passed`.
 *
 * Failure: any line beginning with `[sigtest] FAIL` and the
 * test exits non-zero. */

#include "../libc/syscall.h"
#include "../libc/signal.h"

static volatile int g_handler_ran;
static volatile int g_last_sig;

static void on_signal(int sig)
{
    g_handler_ran = 1;
    g_last_sig    = sig;
    puts("[sigtest] handler ran sig=");
    putd(sig);
    puts("\n");
}

/* The child of check 3 runs this.  Installs a handler, marks
 * itself the foreground process, then busy-loops with yield()
 * until the handler fires.  Exits 0 on a clean handler return,
 * or 1 if it ever sees its own pid as bogus. */
static int child_main(void)
{
    if (signal(SIGINT, on_signal) == (sig_handler_t)-1) {
        puts("[sigtest] child FAIL: signal(SIGINT) rejected\n");
        return 1;
    }
    puts("[sigtest] child waiting for SIGINT pid=");
    putd(getpid());
    puts("\n");
    /* Spin (with yield) until the handler ran.  We need to
     * call SOMETHING that traps into the kernel for the signal
     * to be delivered — yield is convenient. */
    int spins = 0;
    while (!g_handler_ran && spins < 5000) {
        yield();
        spins++;
    }
    if (!g_handler_ran) {
        puts("[sigtest] child FAIL: handler never ran\n");
        return 1;
    }
    if (g_last_sig != SIGINT) {
        puts("[sigtest] child FAIL: wrong signum delivered\n");
        return 1;
    }
    puts("[sigtest] child resumed cleanly after handler\n");
    return 0;
}

int main(void)
{
    puts("[sigtest] starting\n");

    /* Check 1: catch SIGTERM in this process. */
    g_handler_ran = 0;
    g_last_sig    = 0;
    if (signal(SIGTERM, on_signal) == (sig_handler_t)-1) {
        puts("[sigtest] FAIL: signal(SIGTERM) rejected\n");
        return 1;
    }
    if (kill(getpid(), SIGTERM) != 0) {
        puts("[sigtest] FAIL: kill(self, SIGTERM) failed\n");
        return 1;
    }
    /* The signal is delivered at the syscall return tail of the
     * kill() above.  When we get here, the handler has already
     * run and SYS_SIGRETURN has restored our state. */
    if (!g_handler_ran || g_last_sig != SIGTERM) {
        puts("[sigtest] FAIL: handler did not run for SIGTERM\n");
        return 1;
    }
    puts("[sigtest] check 1 (catch + sigreturn) ok\n");

    /* Check 2: SIG_IGN. */
    g_handler_ran = 0;
    g_last_sig    = 0;
    signal(SIGTERM, SIG_IGN);
    kill(getpid(), SIGTERM);
    /* yield once so any pending delivery would actually happen. */
    yield();
    if (g_handler_ran) {
        puts("[sigtest] FAIL: SIG_IGN still ran handler\n");
        return 1;
    }
    puts("[sigtest] check 2 (SIG_IGN) ok\n");

    /* Restore SIG_DFL for SIGTERM so it doesn't bleed into
     * future tests (defensive — exec/exit will reset anyway). */
    signal(SIGTERM, SIG_DFL);

    /* Check 3: fork, signal the child. */
    int p = fork();
    if (p < 0) {
        puts("[sigtest] FAIL: fork failed\n");
        return 1;
    }
    if (p == 0) {
        /* Reset child-local state — fork inherited the parent's
         * run flags from globals, but those are .bss and were
         * already zero by the time we hit this branch.  Reset
         * defensively. */
        g_handler_ran = 0;
        g_last_sig    = 0;
        int rc = child_main();
        exit(rc);
    }
    /* Parent: sleep briefly so the child has time to install
     * its handler and reach yield(), then signal it. */
    for (int i = 0; i < 100; i++) yield();
    if (kill(p, SIGINT) != 0) {
        puts("[sigtest] FAIL: kill(child, SIGINT) failed\n");
        return 1;
    }
    int code = -1;
    int reaped = wait(&code);
    if (reaped != p) {
        puts("[sigtest] FAIL: wait reaped wrong pid\n");
        return 1;
    }
    if (code != 0) {
        puts("[sigtest] FAIL: child exited non-zero code=");
        putd(code);
        puts("\n");
        return 1;
    }
    puts("[sigtest] check 3 (fork + cross-pid signal) ok\n");

    puts("[sigtest] all checks passed\n");
    return 0;
}
