/* userspace/threadtest/threadtest.c — chapter 91 smoke test.
 *
 * Spawns N_WORKERS threads that share an address space, each
 * incrementing a shared counter ITERS times under a mutex.
 * The single-threaded equivalent total is N_WORKERS * ITERS;
 * we verify the final value matches.  If a race lost any
 * increments, the count comes up short.
 *
 * Markers we expect on stdout when everything works:
 *
 *   [thread] start
 *   [thread] worker N done           (printed N_WORKERS times,
 *                                     in some order)
 *   [thread] OK                      (final summary)
 *
 * Failure markers (any of these means the run is broken):
 *
 *   [thread] FAIL spawn              clone() returned -errno
 *   [thread] FAIL join               waitpid() returned -1
 *   [thread] FAIL count <hex>        counter != N_WORKERS * ITERS
 */

#include "../libc/syscall.h"
#include "../libc/thread.h"

#define N_WORKERS  4
#define ITERS      1000

static volatile uint32_t counter = 0;
static mutex_t           lock    = MUTEX_INIT;

/* Tiny puthex (no libc.h dependency).  Used only on the
 * failure path so the test still gives an actionable hint
 * without bringing in the full printf machinery. */
static void puthex_u32(uint32_t v)
{
    char buf[10];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned nib = (v >> (28 - i * 4)) & 0xF;
        buf[2 + i] = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
    }
    write(1, buf, 10);
}

/* ASCII decimal printer for the worker id in "worker N done". */
static void putdec_small(int v)
{
    char buf[4];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0 && n < 4) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    /* reverse */
    for (int a = 0, b = n - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    write(1, buf, (size_t)n);
}

static void worker(void *arg)
{
    int id = (int)(long)arg;

    for (int i = 0; i < ITERS; i++) {
        mutex_lock(&lock);
        counter++;
        mutex_unlock(&lock);
        /* Every so often, yield voluntarily.  This is not
         * required for correctness — the timer preempts us
         * anyway — but it produces more interleaving in a
         * shorter wall-clock window, increasing the chance
         * that a missing-wake race (if one slipped through)
         * will be caught by the regression test. */
        if ((i & 0x3F) == 0) yield();
    }

    write(1, "[thread] worker ", 16);
    putdec_small(id);
    write(1, " done\n", 6);

    exit(0);
}

int main(void)
{
    write(1, "[thread] start\n", 15);

    int tids[N_WORKERS];
    for (int i = 0; i < N_WORKERS; i++) {
        int t = thread_spawn(worker, (void *)(long)i);
        if (t < 0) {
            write(1, "[thread] FAIL spawn\n", 20);
            return 1;
        }
        tids[i] = t;
    }

    /* Reap.  thread_join blocks on a specific tid via waitpid,
     * so we don't have to worry about reap order vs exit order. */
    for (int i = 0; i < N_WORKERS; i++) {
        int rc = thread_join(tids[i]);
        if (rc < 0) {
            write(1, "[thread] FAIL join\n", 19);
            return 1;
        }
    }

    /* Counter check.  Read once, atomically, after every
     * worker has been reaped — at which point no further
     * mutator exists, so a plain volatile read is sufficient. */
    uint32_t want = (uint32_t)(N_WORKERS * ITERS);
    uint32_t got  = atomic_load32_u(&counter);
    if (got != want) {
        write(1, "[thread] FAIL count ", 20);
        puthex_u32(got);
        write(1, " want ", 6);
        puthex_u32(want);
        write(1, "\n", 1);
        return 1;
    }

    write(1, "[thread] OK\n", 12);
    return 0;
}
