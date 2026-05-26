/* userspace/head/head.c — print first N lines of a file (default 10).
 *
 *   head PATH         # first 10 lines
 *   head -N PATH      # first N lines
 *
 * Stops reading once N newlines have been emitted; doesn't slurp
 * the rest of the file.
 *
 * Chapter 116d: drives the FILE * layer.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/stdio.h"

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

    FILE *f;
    if (path) {
        f = fopen(path, "r");
        if (!f) {
            printf("head: cannot open %s: %s\n", path, strerror(errno));
            return 1;
        }
    } else {
        f = stdin;
    }

    char buf[256];
    long emitted_lines = 0;
    size_t n;
    int done = 0;
    while (!done && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t start = 0;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                fwrite(buf + start, 1, (i - start + 1), stdout);
                start = i + 1;
                emitted_lines++;
                if (emitted_lines >= count) {
                    done = 1;
                    break;
                }
            }
        }
        if (!done && start < n)
            fwrite(buf + start, 1, (n - start), stdout);
    }
    if (f != stdin) fclose(f);
    return 0;
}
