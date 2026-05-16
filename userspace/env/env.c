/* userspace/env/env.c — print the environment, one var per line.
 *
 * Same idea as POSIX `env(1)` with no arguments.  Walks the env
 * blob (packed sequence of "KEY=VALUE\0" entries) and prints
 * each entry on its own line.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

#define ENV_BUF 512

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char buf[ENV_BUF];
    long n = getenv_all(buf, sizeof(buf));
    if (n <= 0) return 0;       /* empty env */

    /* Walk the blob: each entry is NUL-terminated; an empty
     * entry marks end-of-list. */
    const char *p = buf;
    while (*p) {
        printf("%s\n", p);
        /* Advance past this entry + its NUL. */
        while (*p) p++;
        p++;
    }
    return 0;
}
