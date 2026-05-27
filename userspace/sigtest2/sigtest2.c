/* userspace/sigtest2/sigtest2.c — chapter 166 regression for
 * raise() and the expanded SIG* table.
 *
 * Exercises:
 *
 *   1. raise(SIGUSR1) -> installed handler runs, handler sees
 *      signum == SIGUSR1.
 *   2. raise(SIGUSR2) -> installed handler runs (separate
 *      handler), handler sees signum == SIGUSR2.
 *   3. signal(SIGINT, SIG_IGN) + raise(SIGINT) -> no crash, no
 *      handler runs (ignore disposition honoured).
 *   4. signal(SIGINT, SIG_DFL) restored after the IGN test;
 *      we do NOT raise(SIGINT) again because the default
 *      action terminates us with 130, and we'd like the test
 *      to print "all checks passed" first.
 *   5. raise() return value is 0 on success.
 *
 * Notes:
 *   - We do NOT test abort() here.  abort() always terminates
 *     the process with non-zero status; the test driver checks
 *     that case separately by running a small helper binary
 *     (see scripts/test_signal_raise.py).
 *   - Counters live in globals to avoid the "handlers can only
 *     touch sig_atomic_t" footgun.  We're single-threaded and
 *     the kernel delivers signals only at syscall return, so
 *     plain int is fine on AArch64 (8/16/32-bit loads and stores
 *     are atomic).
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/signal.h"

static volatile int g_usr1_count;
static volatile int g_usr1_seen_signum;
static volatile int g_usr2_count;
static volatile int g_usr2_seen_signum;

static void on_usr1(int s)
{
    g_usr1_count++;
    g_usr1_seen_signum = s;
}

static void on_usr2(int s)
{
    g_usr2_count++;
    g_usr2_seen_signum = s;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[sigtest2] starting (pid=%d)\n", getpid());

    /* Case 1: SIGUSR1. */
    if (signal(SIGUSR1, on_usr1) == (sig_handler_t)-1) {
        printf("  FAIL: signal(SIGUSR1, on_usr1) returned -1\n");
        return 1;
    }
    int r = raise(SIGUSR1);
    if (r != 0) {
        printf("  FAIL: raise(SIGUSR1) -> %d (want 0)\n", r);
        return 1;
    }
    if (g_usr1_count != 1 || g_usr1_seen_signum != SIGUSR1) {
        printf("  FAIL: SIGUSR1 not delivered (count=%d, signum=%d)\n",
               g_usr1_count, g_usr1_seen_signum);
        return 1;
    }
    printf("  SIGUSR1: handler ran, signum=%d (good)\n",
           g_usr1_seen_signum);

    /* Case 2: SIGUSR2. */
    if (signal(SIGUSR2, on_usr2) == (sig_handler_t)-1) {
        printf("  FAIL: signal(SIGUSR2, on_usr2) returned -1\n");
        return 1;
    }
    if (raise(SIGUSR2) != 0) {
        printf("  FAIL: raise(SIGUSR2) returned non-zero\n");
        return 1;
    }
    if (g_usr2_count != 1 || g_usr2_seen_signum != SIGUSR2) {
        printf("  FAIL: SIGUSR2 not delivered (count=%d, signum=%d)\n",
               g_usr2_count, g_usr2_seen_signum);
        return 1;
    }
    printf("  SIGUSR2: handler ran, signum=%d (good)\n",
           g_usr2_seen_signum);

    /* Confirm SIGUSR1 handler did NOT see the SIGUSR2 raise. */
    if (g_usr1_count != 1) {
        printf("  FAIL: SIGUSR1 handler re-entered (count=%d)\n",
               g_usr1_count);
        return 1;
    }

    /* Case 3: SIG_IGN suppresses delivery. */
    sig_handler_t prev = signal(SIGINT, SIG_IGN);
    if (prev == (sig_handler_t)-1) {
        printf("  FAIL: signal(SIGINT, SIG_IGN) returned -1\n");
        return 1;
    }
    if (raise(SIGINT) != 0) {
        printf("  FAIL: raise(SIGINT) under SIG_IGN failed\n");
        return 1;
    }
    /* If SIG_IGN weren't honoured the default action would
     * terminate us with 130 here and we'd never print the
     * "good" line. */
    printf("  SIGINT under SIG_IGN: process still alive (good)\n");
    /* Put SIGINT back to default; if we don't and an external
     * Ctrl-C arrived later, the shell user would think they
     * couldn't kill us. */
    signal(SIGINT, SIG_DFL);

    printf("[sigtest2] all checks passed\n");
    return 0;
}
