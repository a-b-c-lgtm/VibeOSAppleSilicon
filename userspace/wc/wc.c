/* userspace/wc/wc.c — line / word / byte counter.
 *
 * Reads PATH (defaults to no input — must be given) and prints
 *   <lines> <words> <bytes> <path>
 * on a single line, like POSIX wc.
 *
 * No -l/-w/-c flags yet; no stdin path (we have no pipes).  When
 * pipes land, this picks up "wc <stdin>" for free if argc==1.
 *
 * Chapter 116d: drives the FILE * layer.  stdin is wired to
 * fd 0 by stdio.h's default table, so the argc==1 path will
 * pick up pipes for free.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/stdio.h"

int main(int argc, char **argv)
{
    FILE *f;
    const char *path;
    if (argc < 2 || !argv[1]) {
        f = stdin;
        path = "";
    } else {
        path = argv[1];
        f = fopen(path, "r");
        if (!f) {
            printf("wc: cannot open %s: %s\n", path, strerror(errno));
            return 1;
        }
    }

    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes += (unsigned long)n;
        for (size_t i = 0; i < n; i++) {
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
    int err = ferror(f);
    if (f != stdin) fclose(f);
    if (err) {
        printf("wc: read failed: %s\n", strerror(errno));
        return 2;
    }

    if (path[0])
        printf("%lu %lu %lu %s\n", lines, words, bytes, path);
    else
        printf("%lu %lu %lu\n", lines, words, bytes);
    return 0;
}
