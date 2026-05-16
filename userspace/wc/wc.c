/* userspace/wc/wc.c — line / word / byte counter.
 *
 * Reads PATH (defaults to no input — must be given) and prints
 *   <lines> <words> <bytes> <path>
 * on a single line, like POSIX wc.
 *
 * No -l/-w/-c flags yet; no stdin path (we have no pipes).  When
 * pipes land, this picks up "wc <stdin>" for free if argc==1.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    int fd;
    const char *path;
    if (argc < 2 || !argv[1]) {
        fd = 0;
        path = "";
    } else {
        path = argv[1];
        fd = open(path, 0);
        if (fd < 0) {
            printf("wc: cannot open %s: errno=%d\n", path, -fd);
            return 1;
        }
    }

    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += (unsigned long)n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            int is_space = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (is_space) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    if (fd != 0) close(fd);
    if (n < 0) {
        printf("wc: read failed: errno=%d\n", (int)-n);
        return 2;
    }

    if (path[0])
        printf("%lu %lu %lu %s\n", lines, words, bytes, path);
    else
        printf("%lu %lu %lu\n", lines, words, bytes);
    return 0;
}
