/* userspace/errnotest/errnotest.c — chapter 116a smoke test.
 *
 * Drives the new errno plumbing:
 *
 *   1. open() a path that doesn't exist → return value is -1
 *      (POSIX convention as of 116d), and the global `errno`
 *      carries the specific code (ENOENT).
 *   2. close(-1)              → return -1, errno=EBADF.
 *   3. read(-1, ...)          → return -1, errno=EBADF.
 *   4. A successful syscall (getpid()) does NOT clobber errno.
 *
 * Output format is one line per assertion; the test harness in
 * scripts/test_libc_errno.py greps for each.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"

static void show(const char *tag, long rc, int err)
{
    printf("[errnotest] %s rc=%ld errno=%d\n", tag, rc, err);
}

int main(void)
{
    printf("[errnotest] starting\n");

    /* (1) open() of a nonexistent path. */
    errno = 0;
    long r = open("/this/path/does/not/exist", 0);
    show("open_missing", r, errno);

    /* (2) close() of a bogus fd. */
    errno = 0;
    r = close(-1);
    show("close_badfd", r, errno);

    /* (3) read() from a bogus fd. */
    errno = 0;
    char buf[16];
    r = read(-1, buf, sizeof(buf));
    show("read_badfd", r, errno);

    /* (4) Successful syscall must not stomp errno. */
    errno = 42;
    r = getpid();
    show("getpid_ok", r, errno);

    printf("[errnotest] done\n");
    return 0;
}
