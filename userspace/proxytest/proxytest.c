/*
 * userspace/proxytest/proxytest.c -- chapter 106b / M97 demo program.
 *
 * Demonstrates the chapter-106b architectural payoff in one
 * hermetic command: the browser dials its DEFAULT proxy
 * (http://127.0.0.1:8080/, set by `BR_DEFAULT_PROXY` in
 * userspace/browser/browser.c), which is the in-guest httpd,
 * which then forwards out via chapter-106a's serve_forward to
 * whatever HTTPD_UPSTREAM points at.
 *
 * The orchestration is a 3-process dance, all in userspace:
 *
 *   proxytest                                          (parent)
 *     |
 *     |--- spawn /bin/httpd 8080 --once                (child A)
 *     |       (HTTPD_UPSTREAM env inherited from us)
 *     |
 *     |--- sleep_ms(300)  -- give httpd time to listen
 *     |
 *     |--- spawn /bin/browser https://m97.test/path    (child B)
 *     |       Browser's canonicalize_url rewrites
 *     |       https://m97.test/path
 *     |    -> http://127.0.0.1:8080/m97.test/path
 *     |       (the proxy prefix + the path).
 *     |
 *     |--- waitpid(B): browser finishes plain-text
 *     |   render of whatever httpd returned.
 *     |
 *     '--- waitpid(A): httpd's --once exited after the
 *         single request was served.
 *
 * Why a dedicated orchestrator at all?  The shell doesn't have
 * `&` yet, so we can't background httpd from sh.c.  Pulling the
 * dance into a one-shot userspace binary keeps the test
 * deterministic (single `proxytest done` line marks success)
 * and gives us a hand-testable demo: a developer can just
 * type `proxytest` at the prompt and watch the chain work.
 *
 * Hand-test:
 *   $ export HTTPD_UPSTREAM=10.0.2.2:8080
 *   $ proxytest
 *   ... browser prints the rendered page ...
 *   $
 *
 * For the automated regression see scripts/test_browser_proxy.py.
 *
 * Naming note: this is "proxytest" rather than "browse" or
 * "fetch" because the THING under test is the proxy chain,
 * not the browser's rendering.  See chapter 106b for the
 * argument that "be a dumb pipe" architecture is what we are
 * exercising.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Helper: hand-rolled streq to avoid pulling in libc string.h. */
static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv)
{
    /* Default URL exercises the https:// -> proxy rewrite path
     * (case 3 in canonicalize_url).  The host part is a fake
     * domain so any test upstream can match on the path and
     * return a recognisable marker.  Callers can override. */
    const char *url = "https://m97.proxy.test/path";
    int viewport = 600;
    int verbose = 1;
    /* Chapter 106b: --repeat N reproduces the user-observed
     * "every refresh takes 30+ seconds" GUI-browser symptom by
     * running the browser N times back-to-back through a single
     * long-lived httpd (no --once).  Each iteration prints its
     * wall time so the harness can see whether the cost is per-
     * fetch (kernel/TCP bug -- amortise across N) or one-shot
     * (warm cache or first-fetch only).  Default 1 preserves
     * the original single-fetch demo. */
    int repeat = 1;
    /* Pass --timing through to the browser so the harness sees
     * per-stage [timing] lines on the serial console.  Off by
     * default to keep the demo output tidy. */
    int timing = 0;

    int i = 1;
    while (i < argc) {
        if (s_eq(argv[i], "--url")) {
            if (i + 1 >= argc) {
                printf("proxytest: --url needs a value\n");
                return 1;
            }
            url = argv[++i];
        } else if (s_eq(argv[i], "--repeat")) {
            if (i + 1 >= argc) {
                printf("proxytest: --repeat needs a value\n");
                return 1;
            }
            /* Cheap atoi -- arg is always 1..99 in practice. */
            const char *s = argv[++i];
            int n = 0;
            while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
            if (n < 1) n = 1;
            if (n > 99) n = 99;
            repeat = n;
        } else if (s_eq(argv[i], "--timing")) {
            timing = 1;
        } else if (s_eq(argv[i], "--quiet")) {
            verbose = 0;
        } else if (s_eq(argv[i], "-h") || s_eq(argv[i], "--help")) {
            printf("usage: proxytest [--url <url>] [--repeat N] "
                   "[--timing] [--quiet]\n");
            printf("  default url: https://m97.proxy.test/path\n");
            printf("  needs HTTPD_UPSTREAM set in env (chapter 106a).\n");
            printf("  --repeat N runs the browser N times against a\n");
            printf("    single long-lived httpd; default 1 (one-shot).\n");
            return 0;
        } else {
            printf("proxytest: unknown arg \"%s\"\n", argv[i]);
            return 1;
        }
        i++;
    }

    /* (1) Spawn the in-guest httpd.  HTTPD_UPSTREAM is inherited
     * via the kernel's env table; the parent (us) is supposed to
     * have it set already, either by the shell `export` builtin
     * (interactive hand-test) or by the harness (test_*.py).
     *
     * --once vs long-lived: with --repeat==1 we use --once so
     * httpd exits cleanly after serving the single request and
     * we can waitpid it.  With --repeat>1 we need it to keep
     * accepting; we kill+wait it after the last browser exits. */
    const char *httpd_args = (repeat == 1) ? "8080 --once" : "8080";
    if (verbose)
        printf("[proxytest] spawning /bin/httpd %s\n", httpd_args);
    int httpd_pid = spawn("/bin/httpd", httpd_args);
    if (httpd_pid < 0) {
        printf("proxytest: spawn httpd failed (%d)\n", httpd_pid);
        return 1;
    }

    /* (1b) Tell our browser child to use the 8080 httpd we just
     * spawned, NOT init's auto-spawned port-80 httpd (chapter
     * 106c).  The chapter-106c port split (init -> 80, tests
     * -> 8080) means BR_DEFAULT_PROXY now points at :80, and
     * init's httpd has no HTTPD_UPSTREAM set -- forwarding
     * requests to it would 502.  setenv writes the kernel's
     * env table for the current proc; the browser child picks
     * it up via the same inheritance path HTTPD_UPSTREAM uses. */
    if (setenv("BROWSER_PROXY", "http://127.0.0.1:8080/") < 0) {
        printf("proxytest: setenv BROWSER_PROXY failed\n");
        return 1;
    }
    if (verbose)
        printf("[proxytest] BROWSER_PROXY=http://127.0.0.1:8080/\n");

    /* (2) Wait for httpd to bind its listen socket.  Chapter 105
     * shows that httpd's listen line prints almost immediately
     * after spawn; 300 ms is generous (the kernel boot self-test
     * is the only thing that has ever taken longer).  We could
     * poll-connect to 127.0.0.1:8080 in a loop, but doing so would
     * consume the --once slot the browser needs. */
    sleep_ms(300);

    /* (3) Spawn the browser N times.  We deliberately use spawn()
     * (the chapter-9 fast path "fork + immediate exec") rather
     * than fork/exec by hand: the orchestrator owns no per-process
     * state the browser needs.  argv shape is
     * `[--timing] <url> <viewport>`, matching the chapter-71
     * plain-mode CLI.  No --gui flag -- we want the deterministic
     * plain-text render so the test can grep for the marker. */
    int br_failures = 0;
    for (int iter = 0; iter < repeat; iter++) {
        char args[200];
        {
            int n = 0;
            if (timing) {
                const char *t = "--timing ";
                while (*t) args[n++] = *t++;
            }
            const char *u = url;
            while (*u && n < (int)sizeof(args) - 8) args[n++] = *u++;
            args[n++] = ' ';
            /* Cheap itoa for the viewport (always 3-digit-ish). */
            int v = viewport;
            char digits[8]; int dn = 0;
            if (v <= 0) { digits[dn++] = '0'; }
            else { while (v && dn < 8) { digits[dn++] = (char)('0' + v % 10); v /= 10; } }
            while (dn--) args[n++] = digits[dn];
            args[n] = '\0';
        }
        if (verbose)
            printf("[proxytest] iter %d/%d: spawning /bin/browser %s\n",
                   iter + 1, repeat, args);
        unsigned long t0 = uptime_ms();
        int br_pid = spawn("/bin/browser", args);
        if (br_pid < 0) {
            printf("proxytest: spawn browser failed (%d)\n", br_pid);
            br_failures++;
            break;
        }
        int br_code = -1;
        int wbr = waitpid(br_pid, &br_code, 0);
        unsigned long elapsed = uptime_ms() - t0;
        if (verbose)
            printf("[proxytest] iter %d/%d browser exit code=%d "
                   "wall=%lu ms (waitpid=%d)\n",
                   iter + 1, repeat, br_code, elapsed, wbr);
        if (wbr < 0 || br_code != 0) {
            br_failures++;
            /* In --once mode we still need to reap httpd below. */
            break;
        }
    }

    /* (4) Reap httpd.  --once mode: it exits on its own after the
     * single served request.  Long-lived mode: SIGTERM it; the
     * accept loop exits with -EINTR which httpd's main treats as
     * an error code and returns 1 from, but that's the expected
     * shutdown path for repeat>1 (the test counts BROWSER exit
     * codes, not httpd's). */
    if (repeat > 1) {
        kill(httpd_pid, SIGTERM);
    }
    int hd_code = -1;
    int whd = waitpid(httpd_pid, &hd_code, 0);
    if (verbose)
        printf("[proxytest] httpd exit code=%d (waitpid=%d)\n",
               hd_code, whd);

    if (br_failures || whd < 0) {
        printf("proxytest: child failure (br_failures=%d hd=%d)\n",
               br_failures, hd_code);
        return 1;
    }

    printf("[proxytest] done\n");
    return 0;
}
