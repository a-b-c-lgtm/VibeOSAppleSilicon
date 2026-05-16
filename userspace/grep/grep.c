/* userspace/grep/grep.c — substring matcher.
 *
 *   grep PATTERN PATH
 *
 * Reads PATH line-by-line and prints lines whose content
 * contains PATTERN as a substring.  No regex, no -i/-v/-n
 * flags, no recursion.  Just the 90% case.
 *
 * Lines longer than LINE_MAX are truncated; the rest of the
 * line is consumed but not matched (nor printed).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

#define LINE_MAX 512

static int contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1]) {
        printf("usage: grep PATTERN [PATH]\n");
        return 1;
    }
    const char *pattern = argv[1];
    const char *path    = (argc >= 3 && argv[2]) ? argv[2] : 0;

    int fd;
    if (path) {
        fd = open(path, 0);
        if (fd < 0) {
            printf("grep: cannot open %s: errno=%d\n", path, -fd);
            return 1;
        }
    } else {
        fd = 0;   /* read from stdin (redirected file or console) */
    }

    char buf[256];
    char line[LINE_MAX];
    int  llen = 0;
    int  truncated = 0;
    long n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[llen] = '\0';
                if (!truncated && contains(line, pattern))
                    printf("%s\n", line);
                llen = 0;
                truncated = 0;
            } else if (llen < LINE_MAX - 1) {
                line[llen++] = c;
            } else {
                truncated = 1;
            }
        }
    }
    /* tail line without trailing newline */
    if (llen > 0 && !truncated) {
        line[llen] = '\0';
        if (contains(line, pattern))
            printf("%s\n", line);
    }
    if (fd != 0) close(fd);
    return 0;
}
