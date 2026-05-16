/* userspace/echo/echo.c — print argv joined by spaces, then newline.
 *
 * Trivial test that the kernel laid out the argv blob correctly:
 *   $ /bin/echo hello argv world
 *   hello argv world
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? " " : "", argv[i] ? argv[i] : "");
    printf("\n");
    return 0;
}

