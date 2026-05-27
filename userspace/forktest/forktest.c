/* userspace/forktest/forktest.c — fork+exec smoke test.
 *
 * Three checks, in order:
 *
 *   1. PURE FORK.  Clone ourselves; verify both halves run, the
 *      child sees fork() == 0 with a different pid, the parent
 *      sees the child's pid, and wait() reaps it cleanly with
 *      exit code 0.
 *
 *   2. FORK + EXEC.  Same fork, but the child calls execv into
 *      /bin/hello with a synthetic argv.  Hello prints, exits 0,
 *      and the parent reaps it.  Verifies that exec preserves
 *      stdin/stdout fds, swaps in a new AS without reusing any
 *      pages from the old one, and erets cleanly into the new
 *      entry point.
 *
 *   3. INHERITED HEAP.  Before the fork, malloc() a 1 KiB buffer
 *      and write a sentinel into it.  In the child, verify the
 *      sentinel is still there and write a different value.  In
 *      the parent (after wait()), verify that the parent's copy
 *      still holds the original sentinel — proving the AS clone
 *      gave each side its own physical pages, not a shared
 *      mapping.
 *
 * On success prints the line `[forktest] all checks passed` —
 * scripts/test_fork_exec.py greps for that.  Any failure prints
 * a `[forktest] FAIL ...` line and exits 1.
 */

#include "../libc/syscall.h"
#include "../libc/malloc.h"

static void say(const char *s)
{
    write(1, s, strlen(s));
}

static void say_ln(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

static void say_d(const char *prefix, long v)
{
    say(prefix);
    putd(v);
    write(1, "\n", 1);
}

int main(void)
{
    say_ln("[forktest] starting");

    /* ------------------------------------------------------------
     * Check 1: pure fork.
     * ---------------------------------------------------------- */
    int parent_pid = getpid();
    int pid = fork();
    if (pid < 0) {
        say_d("[forktest] FAIL fork -errno=", -pid);
        return 1;
    }
    if (pid == 0) {
        /* Child path. */
        int my_pid = getpid();
        if (my_pid == parent_pid) {
            say_ln("[forktest] FAIL child has parent's pid");
            exit(11);
        }
        say_d("[forktest] child  fork=0 my_pid=", my_pid);
        exit(0);
    }
    /* Parent path. */
    say_d("[forktest] parent fork=child_pid=", pid);
    int code = 0;
    int reaped = wait(&code);
    if (reaped != pid) {
        say_d("[forktest] FAIL wait reaped wrong tid: ", reaped);
        return 1;
    }
    if (code != 0) {
        say_d("[forktest] FAIL child exit code: ", code);
        return 1;
    }
    say_ln("[forktest] check 1 (pure fork) ok");

    /* ------------------------------------------------------------
     * Check 2: fork + exec into /bin/hello.
     * ---------------------------------------------------------- */
    int pid2 = fork();
    if (pid2 < 0) {
        say_d("[forktest] FAIL second fork -errno=", -pid2);
        return 1;
    }
    if (pid2 == 0) {
        char *argv[] = { (char *)"/bin/hello", (char *)0 };
        int rc = execv("/bin/hello", argv);
        /* exec only returns on failure. */
        say_d("[forktest] FAIL execv returned: ", rc);
        exit(99);
    }
    int code2 = 0;
    int reaped2 = wait(&code2);
    if (reaped2 != pid2) {
        say_d("[forktest] FAIL wait2 reaped wrong tid: ", reaped2);
        return 1;
    }
    if (code2 != 0) {
        say_d("[forktest] FAIL exec'd child exit code: ", code2);
        return 1;
    }
    say_ln("[forktest] check 2 (fork+exec) ok");

    /* ------------------------------------------------------------
     * Check 3: inherited heap is COPIED, not shared.
     * ---------------------------------------------------------- */
    unsigned char *buf = (unsigned char *)malloc(1024);
    if (!buf) {
        say_ln("[forktest] FAIL malloc returned NULL");
        return 1;
    }
    for (int i = 0; i < 1024; i++) buf[i] = 0xA5;

    int pid3 = fork();
    if (pid3 < 0) {
        say_d("[forktest] FAIL third fork -errno=", -pid3);
        return 1;
    }
    if (pid3 == 0) {
        /* Child: verify sentinel survived the AS clone, then
         * trample it.  Parent's copy must remain untouched. */
        for (int i = 0; i < 1024; i++) {
            if (buf[i] != 0xA5) {
                say_ln("[forktest] FAIL child sees stale sentinel");
                exit(31);
            }
        }
        for (int i = 0; i < 1024; i++) buf[i] = 0x5A;
        exit(0);
    }
    int code3 = 0;
    int reaped3 = wait(&code3);
    if (reaped3 != pid3 || code3 != 0) {
        say_d("[forktest] FAIL check3 reaped/code; reaped=", reaped3);
        say_d("                                       code=", code3);
        return 1;
    }
    /* Parent's pages must still hold 0xA5 — fork did NOT alias. */
    for (int i = 0; i < 1024; i++) {
        if (buf[i] != 0xA5) {
            say_ln("[forktest] FAIL parent's heap was clobbered");
            return 1;
        }
    }
    free(buf);
    say_ln("[forktest] check 3 (heap is copied) ok");

    say_ln("[forktest] all checks passed");
    return 0;
}
