/* userspace/threadtest2/threadtest2.c — chapter 93 SMP smoke test.
 *
 * Spawns N_WORKERS clone'd threads sharing one address space and
 * pins them across both CPUs (half on CPU 0, half on CPU 1).
 * Every worker increments a shared counter ITERS times under a
 * mutex.  The expected total is N_WORKERS * ITERS; we verify the
 * count exactly matches.
 *
 * What this proves over chapter 92's threadtest:
 *
 *   - Per-CPU timer PPI is wired up on CPU 1 (without it, a
 *     CPU-1 worker stuck in mutex_lock's spin section would
 *     never preempt and the test would hang).
 *
 *   - Cross-CPU futex wake/wait work: a CPU-0 unlock fires
 *     while a CPU-1 thread is parked on the same address (and
 *     vice-versa).  Lost-wakeup races would leave threads
 *     sleeping forever and the test would hang.
 *
 *   - Cross-CPU AS sharing works under real concurrency: both
 *     CPUs read+write the same `counter` page-table mapping
 *     simultaneously, exercising the AS refcount + the
 *     atomic-store path the chapter-91 mutex relies on.
 *
 * We also call getcpu() inside each worker right after it
 * starts and once again right before it exits, then echo the
 * pair to stdout so the test harness can grep for the right CPU
 * id and verify pinning actually took effect.
 *
 * Markers we expect on stdout when everything works:
 *
 *   [thread2] start
 *   [thread2] worker N start cpu=C
 *   [thread2] worker N done  cpu=C   (printed N_WORKERS times,
 *                                     in some order)
 *   [thread2] OK
 *
 * Failure markers:
 *
 *   [thread2] FAIL spawn              clone2() returned -errno
 *   [thread2] FAIL join               waitpid() returned -1
 *   [thread2] FAIL cpu N got=X want=Y worker landed on the wrong CPU
 *   [thread2] FAIL count <hex>        race lost some increments
 */

#include "../libc/syscall.h"
#include "../libc/thread.h"

#define N_WORKERS  4
#define ITERS      1000

static volatile uint32_t counter = 0;
static mutex_t           lock    = MUTEX_INIT;

/* Serialise userspace writes to fd 1.  The kernel write() syscall
 * to the serial console isn't atomic across CPUs — it loops one
 * byte at a time and a CPU-1 worker preempted mid-write lets a
 * CPU-0 worker interleave its bytes into the middle of our line.
 * Holding `print_lock` while we call write() makes sure only one
 * worker is in the syscall at a time so the line lands intact. */
static mutex_t print_lock = MUTEX_INIT;

static void emit(const char *buf, size_t n)
{
    mutex_lock(&print_lock);
    write(1, buf, n);
    mutex_unlock(&print_lock);
}

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

/* Append the decimal representation of `v` (0..99) to `buf` at
 * offset *off; bumps *off.  Used to build a worker's whole status
 * line in one buffer so we can emit it with a SINGLE write() syscall
 * — concurrent workers on different CPUs would otherwise interleave
 * at byte boundaries (the chapter-92 stdout pipe is not a line-mode
 * device). */
static void buf_append_dec(char *buf, int *off, int v)
{
    if (v >= 10) buf[(*off)++] = (char)('0' + v / 10);
    buf[(*off)++] = (char)('0' + v % 10);
}

static void buf_append(char *buf, int *off, const char *s)
{
    while (*s) buf[(*off)++] = *s++;
}

/* Worker entry — `arg` is a packed (id<<8 | want_cpu) value so we
 * don't need a per-worker context struct. */
static void worker(void *arg)
{
    long packed   = (long)arg;
    int  id       = (int)((packed >> 8) & 0xFF);
    int  want_cpu = (int)(packed & 0xFF);

    int start_cpu = getcpu();

    /* Build the whole "start" line in one stack buffer and emit
     * it with ONE write() — see comment on buf_append_dec / emit. */
    {
        char line[64];
        int  n = 0;
        buf_append(line, &n, "[thread2] worker ");
        buf_append_dec(line, &n, id);
        buf_append(line, &n, " start cpu=");
        buf_append_dec(line, &n, start_cpu);
        line[n++] = '\n';
        emit(line, (size_t)n);
    }

    if (start_cpu != want_cpu) {
        char line[80];
        int  n = 0;
        buf_append(line, &n, "[thread2] FAIL cpu ");
        buf_append_dec(line, &n, id);
        buf_append(line, &n, " got=");
        buf_append_dec(line, &n, start_cpu);
        buf_append(line, &n, " want=");
        buf_append_dec(line, &n, want_cpu);
        line[n++] = '\n';
        emit(line, (size_t)n);
        exit(1);
    }

    for (int i = 0; i < ITERS; i++) {
        mutex_lock(&lock);
        counter++;
        mutex_unlock(&lock);
        if ((i & 0x3F) == 0) yield();
    }

    int end_cpu = getcpu();

    {
        char line[64];
        int  n = 0;
        buf_append(line, &n, "[thread2] worker ");
        buf_append_dec(line, &n, id);
        buf_append(line, &n, " done cpu=");
        buf_append_dec(line, &n, end_cpu);
        line[n++] = '\n';
        emit(line, (size_t)n);
    }

    if (end_cpu != want_cpu) {
        char line[64];
        int  n = 0;
        buf_append(line, &n, "[thread2] FAIL cpu ");
        buf_append_dec(line, &n, id);
        buf_append(line, &n, " end=");
        buf_append_dec(line, &n, end_cpu);
        line[n++] = '\n';
        emit(line, (size_t)n);
        exit(1);
    }

    exit(0);
}

int main(void)
{
    write(1, "[thread2] start\n", 16);

    /* Half the workers on CPU 0, half on CPU 1.  We assume
     * SMP_MAX_CPUS >= 2 (default `make run-graphical` config). */
    int tids[N_WORKERS];
    for (int i = 0; i < N_WORKERS; i++) {
        int want_cpu = (i & 1);     /* 0,1,0,1 */
        long packed  = ((long)i << 8) | (long)want_cpu;
        int t = thread_spawn_on(worker, (void *)packed, want_cpu);
        if (t < 0) {
            write(1, "[thread2] FAIL spawn\n", 21);
            return 1;
        }
        tids[i] = t;
    }

    for (int i = 0; i < N_WORKERS; i++) {
        int rc = thread_join(tids[i]);
        if (rc < 0) {
            write(1, "[thread2] FAIL join\n", 20);
            return 1;
        }
        /* If a worker died with non-zero exit code (e.g. CPU
         * mismatch), propagate the failure — but the worker
         * itself printed the diagnostic FAIL line already, so
         * we only need to abort the test. */
        if (rc != 0) {
            return 1;
        }
    }

    uint32_t want = (uint32_t)(N_WORKERS * ITERS);
    uint32_t got  = atomic_load32_u(&counter);
    if (got != want) {
        write(1, "[thread2] FAIL count ", 21);
        puthex_u32(got);
        write(1, " want ", 6);
        puthex_u32(want);
        write(1, "\n", 1);
        return 1;
    }

    write(1, "[thread2] OK\n", 13);
    return 0;
}
