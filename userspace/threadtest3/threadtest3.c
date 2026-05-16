/* userspace/threadtest3/threadtest3.c — chapter 93 CLONE_FILES smoke
 * test.
 *
 * Two cases, run back-to-back:
 *
 * Case A — CLONE_FILES SHARES the fd table:
 *   - main opens /tmp/clone_files_a as fd N (some N > 2).
 *   - main spawns a worker via thread_spawn_files(...) on CPU 1.
 *   - worker writes "FROM_WORKER" to that exact fd N (it
 *     inherited the entire fd table by reference, so fd N is
 *     valid in the worker too).
 *   - main joins, then close(N), reopens for read, reads back,
 *     and asserts the bytes are present.
 *
 * Case B — plain clone DOES NOT share the fd table:
 *   - main opens /tmp/clone_files_b as fd M.
 *   - main spawns a worker via thread_spawn_on(...) (no
 *     CLONE_FILES) on CPU 1.
 *   - worker calls write(M, ...).  M is not in_use in the
 *     worker's freshly-allocated fd_table, so the kernel
 *     returns -EBADF.  Worker exits with the negation of the
 *     errno so main can verify.
 *
 * Markers we expect on stdout when everything works:
 *
 *   [thread3] start
 *   [thread3] case A: spawned worker tid=...
 *   [thread3] case A: worker wrote N bytes
 *   [thread3] case A: read back: FROM_WORKER
 *   [thread3] case A: OK
 *   [thread3] case B: spawned worker tid=...
 *   [thread3] case B: worker write returned -9 (EBADF)
 *   [thread3] case B: OK
 *   [thread3] OK
 *
 * Failure markers:
 *
 *   [thread3] FAIL <where>            — anything not expected
 */

#include "../libc/syscall.h"
#include "../libc/thread.h"

/* Open flags — match kernel/core/vfs.h.  We only need the bits
 * the kernel's sys_open recognises for tmpfs paths. */
#define OPEN_RDONLY  0
#define OPEN_WRONLY  1
#define OPEN_RDWR    2
#define OPEN_CREAT   0100   /* 64 */
#define OPEN_TRUNC   01000  /* 512 */

static const char path_a[] = "/tmp/clone_files_a";
static const char path_b[] = "/tmp/clone_files_b";

/* Worker arg packs the (fd, want_cpu) pair into a single long so
 * we don't need a per-worker context struct. */
struct worker_arg {
    int  fd;
    int  want_cpu;
    long write_rc;     /* set by worker; read by main after join */
};

static const char payload[] = "FROM_WORKER";

/* ---- Case A worker: shared fd table.  Writes to inherited fd. ---- */
static void worker_shared(void *arg)
{
    struct worker_arg *wa = (struct worker_arg *)arg;
    int cpu = getcpu();
    if (cpu != wa->want_cpu) {
        /* Pinning failed.  Bail with a distinct exit code so
         * main can tell apart "wrong CPU" from "EBADF leak". */
        exit(2);
    }
    /* Write the payload.  fd was opened by main *in the same
     * fd_table*; CLONE_FILES means we see it as in_use. */
    long n = write(wa->fd, payload, sizeof(payload) - 1);
    wa->write_rc = n;
    exit(n == (long)(sizeof(payload) - 1) ? 0 : 1);
}

/* ---- Case B worker: private fd table.  Expects write to fail. ---- */
static void worker_private(void *arg)
{
    struct worker_arg *wa = (struct worker_arg *)arg;
    int cpu = getcpu();
    if (cpu != wa->want_cpu) exit(2);
    /* This write SHOULD fail because the worker was clone'd
     * without CLONE_FILES — its private fd_table has slots 0/1/2
     * as console and everything else closed.  Any negative
     * return is acceptable; the kernel returns -EBADF for some
     * fd kinds and -ENOSYS for others (write() falls through to
     * "unknown fd, assume console-only stdout" for slots that
     * aren't in_use), and either is enough to prove the worker
     * has its own private table. */
    long n = write(wa->fd, payload, sizeof(payload) - 1);
    wa->write_rc = n;
    /* Encode "the write failed" as exit code 0xFB (251), so main
     * can distinguish from "write happened to succeed because
     * the fd_table was actually shared" (would be exit 0). */
    if (n >= 0) exit(0);              /* unexpected success */
    exit(0xFB);
}

static void emit(const char *s)
{
    write(1, s, strlen(s));
}

static void emit_dec(long v)
{
    char buf[24];
    int  i = 0;
    int  neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) buf[i++] = '-';
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char tmp = buf[a]; buf[a] = buf[b]; buf[b] = tmp;
    }
    write(1, buf, (size_t)i);
}

int main(void)
{
    emit("[thread3] start\n");

    /* ---------------- Case A: CLONE_FILES shares fd table ---------------- */

    /* Pre-clean.  unlink may return -ENOENT on first run; ignore. */
    (void)unlink(path_a);
    int fd_a = open(path_a, OPEN_CREAT | OPEN_RDWR | OPEN_TRUNC);
    if (fd_a < 0) { emit("[thread3] FAIL open A rc="); emit_dec(fd_a); emit("\n"); return 1; }

    struct worker_arg wa = { .fd = fd_a, .want_cpu = 1, .write_rc = -999 };
    int tid = thread_spawn_files(worker_shared, &wa, /*cpu_id=*/1);
    if (tid < 0) { emit("[thread3] FAIL spawn A rc="); emit_dec(tid); emit("\n"); return 1; }
    emit("[thread3] case A: spawned worker tid="); emit_dec(tid); emit("\n");

    int rc = thread_join(tid);
    if (rc != 0) {
        emit("[thread3] FAIL join A rc="); emit_dec(rc);
        emit(" write_rc="); emit_dec(wa.write_rc); emit("\n");
        return 1;
    }
    emit("[thread3] case A: worker wrote "); emit_dec(wa.write_rc); emit(" bytes\n");

    /* Close the writer fd, reopen as reader, read back. */
    close(fd_a);
    int fd_a_r = open(path_a, OPEN_RDONLY);
    if (fd_a_r < 0) { emit("[thread3] FAIL reopen A rc="); emit_dec(fd_a_r); emit("\n"); return 1; }

    char buf[64];
    long n = read(fd_a_r, buf, sizeof(buf) - 1);
    close(fd_a_r);
    if (n < 0) { emit("[thread3] FAIL read A rc="); emit_dec(n); emit("\n"); return 1; }
    buf[n] = '\0';
    emit("[thread3] case A: read back: "); emit(buf); emit("\n");

    /* Compare. */
    if (n != (long)(sizeof(payload) - 1)) {
        emit("[thread3] FAIL A length mismatch got="); emit_dec(n);
        emit(" want="); emit_dec((long)(sizeof(payload) - 1)); emit("\n");
        return 1;
    }
    for (long i = 0; i < n; i++) {
        if (buf[i] != payload[i]) { emit("[thread3] FAIL A content mismatch\n"); return 1; }
    }
    emit("[thread3] case A: OK\n");

    /* ---------------- Case B: plain clone (no CLONE_FILES) ---------------- */

    (void)unlink(path_b);
    int fd_b = open(path_b, OPEN_CREAT | OPEN_RDWR | OPEN_TRUNC);
    if (fd_b < 0) { emit("[thread3] FAIL open B rc="); emit_dec(fd_b); emit("\n"); return 1; }

    struct worker_arg wb = { .fd = fd_b, .want_cpu = 1, .write_rc = -999 };
    int tid_b = thread_spawn_on(worker_private, &wb, /*cpu_id=*/1);
    if (tid_b < 0) { emit("[thread3] FAIL spawn B rc="); emit_dec(tid_b); emit("\n"); return 1; }
    emit("[thread3] case B: spawned worker tid="); emit_dec(tid_b); emit("\n");

    int rc_b = thread_join(tid_b);
    /* worker_private exits with 0xFB if its write failed (ANY
     * negative errno).  Anything else (including 0 = success)
     * means the worker's fd_table was NOT actually private — a
     * chapter-93 regression. */
    if (rc_b != 0xFB) {
        emit("[thread3] FAIL B exit_code="); emit_dec(rc_b);
        emit(" write_rc="); emit_dec(wb.write_rc); emit("\n");
        return 1;
    }
    emit("[thread3] case B: worker write returned ");
    emit_dec(wb.write_rc); emit(" (private fd_table OK)\n");

    /* Sanity: file should be EMPTY because the worker's write
     * failed and main never wrote anything. */
    close(fd_b);
    int fd_b_r = open(path_b, OPEN_RDONLY);
    if (fd_b_r < 0) { emit("[thread3] FAIL reopen B rc="); emit_dec(fd_b_r); emit("\n"); return 1; }
    long m = read(fd_b_r, buf, sizeof(buf));
    close(fd_b_r);
    if (m != 0) { emit("[thread3] FAIL B file not empty len="); emit_dec(m); emit("\n"); return 1; }
    emit("[thread3] case B: OK\n");

    emit("[thread3] OK\n");
    return 0;
}
