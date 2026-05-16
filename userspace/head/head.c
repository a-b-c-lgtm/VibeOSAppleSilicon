/* userspace/head/head.c — print first N lines of a file (default 10).
 *
 *   head PATH         # first 10 lines
 *   head -N PATH      # first N lines
 *
 * Stops reading once N newlines have been emitted; doesn't slurp
 * the rest of the file.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

static int parse_int(const char *s, long *out)
{
    long v = 0;
    if (!s || !*s) return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    long count = 10;
    const char *path = 0;

    int ai = 1;
    if (ai < argc && argv[ai] && argv[ai][0] == '-') {
        if (parse_int(argv[ai] + 1, &count) != 0) {
            printf("head: bad count %s\n", argv[ai]);
            return 1;
        }
        ai++;
    }
    if (ai < argc) path = argv[ai];

    int fd;
    if (path) {
        fd = open(path, 0);
        if (fd < 0) {
            printf("head: cannot open %s: errno=%d\n", path, -fd);
            return 1;
        }
    } else {
        fd = 0;   /* stdin */
    }

    char buf[256];
    long emitted_lines = 0;
    long n;
    int done = 0;
    while (!done && (n = read(fd, buf, sizeof(buf))) > 0) {
        long start = 0;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                write(1, buf + start, (size_t)(i - start + 1));
                start = i + 1;
                emitted_lines++;
                if (emitted_lines >= count) {
                    done = 1;
                    break;
                }
            }
        }
        if (!done && start < n)
            write(1, buf + start, (size_t)(n - start));
    }
    if (fd != 0) close(fd);
    return 0;
}
