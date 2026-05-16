/* userspace/pipetest/pipetest.c — kernel pipe self-test.
 *
 * Single-process exerciser for SYS_PIPE / SYS_DUP2 / pipe_read /
 * pipe_write / pipe_unref.  No fork yet (milestone 30 only adds
 * the kernel primitives), so all writes/reads happen in the same
 * thread.  Proves:
 *
 *   1. pipe() returns two valid fds.
 *   2. write to wfd is buffered, read from rfd returns the bytes.
 *   3. close(wfd) before all bytes are drained still lets reader
 *      drain the remaining bytes, then sees EOF (read returns 0).
 *   4. dup2(wfd, 5) produces a usable second writer; closing the
 *      original wfd alone doesn't EOF the reader.
 *
 * Output: a series of [PASS]/[FAIL] lines and a final summary.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        write(1, "[PASS] ", 7);
    } else {
        write(1, "[FAIL] ", 7);
        failures++;
    }
    write(1, what, strlen(what));
    write(1, "\n", 1);
}

static int streq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Test 1: basic pipe. */
    int fds[2];
    int rc = pipe(fds);
    check(rc == 0, "pipe() returned 0");
    check(fds[0] >= 3 && fds[1] >= 3 && fds[0] != fds[1],
          "pipe() gave two distinct fds >= 3");

    const char *msg = "hello pipe!";
    long w = write(fds[1], msg, 11);
    check(w == 11, "write 11 bytes to pipe wfd");

    char buf[32];
    long r = read(fds[0], buf, sizeof(buf));
    check(r == 11, "read returned 11 bytes");
    check(streq(buf, msg, 11), "read bytes match what was written");

    /* Test 2: close writer -> reader sees EOF. */
    w = write(fds[1], "x", 1);
    check(w == 1, "second write of 1 byte");
    close(fds[1]);
    /* Drain the leftover byte. */
    r = read(fds[0], buf, sizeof(buf));
    check(r == 1 && buf[0] == 'x', "after close(wfd), still drain pending byte");
    /* Now buffer empty + no writers -> EOF. */
    r = read(fds[0], buf, sizeof(buf));
    check(r == 0, "read on drained+closed pipe returns 0 (EOF)");
    close(fds[0]);

    /* Test 3: dup2 keeps pipe alive across one writer close. */
    rc = pipe(fds);
    check(rc == 0, "second pipe() returned 0");
    int wdup = 5;
    rc = dup2(fds[1], wdup);
    check(rc == wdup, "dup2(wfd, 5) returned 5");
    close(fds[1]);  /* original writer gone, but dup'd writer alive */
    w = write(wdup, "abc", 3);
    check(w == 3, "write to duplicated wfd works after closing original");
    r = read(fds[0], buf, sizeof(buf));
    check(r == 3 && streq(buf, "abc", 3),
          "read from rfd after dup'd write returns those bytes");
    close(wdup);
    r = read(fds[0], buf, sizeof(buf));
    check(r == 0, "after closing duplicated writer too, reader sees EOF");
    close(fds[0]);

    if (failures == 0)
        printf("[pipetest] all checks passed\n");
    else
        printf("[pipetest] %d FAILURE(S)\n", failures);
    return failures;
}
