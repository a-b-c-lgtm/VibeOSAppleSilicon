/* userspace/env/env.c — print the environment, one var per line.
 *
 * Same idea as POSIX `env(1)` with no arguments.  Walks the
 * environ[] array (chapter 151) and prints each entry on its
 * own line.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/env.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* Touch any env-getter to force the lazy init that populates
     * environ[] from the kernel's blob; getenv("") returns NULL
     * cheaply and triggers _env_init() as a side effect. */
    (void)getenv("PATH");
    for (char **p = environ; *p; p++) {
        printf("%s\n", *p);
    }
    return 0;
}
