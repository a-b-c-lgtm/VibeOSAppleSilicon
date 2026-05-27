/* userspace/cat/cat.c — file-printing user program.
 *
 * Reads its target file path from argv[1] (laid out on the user
 * stack by the kernel ELF loader; see kernel/core/elf.c).
 * Defaults to "/motd" when called with no arguments.
 *
 * Chapter 152: rewritten to drive the FILE * layer.  The body
 * is now identical to a POSIX cat — fopen + fread + fwrite, no
 * raw fd syscalls, and error reporting via strerror(errno).
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/stdio.h"

int main(int argc, char **argv)
{
    const char *target = "/motd";
    if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        target = argv[1];

    FILE *f = fopen(target, "r");
    if (!f) {
        printf("cat: cannot open %s: %s\n", target, strerror(errno));
        return 1;
    }

    char buf[256];
    long total = 0;
    size_t n;
    int last_was_newline = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
        total += (long)n;
        last_was_newline = (buf[n - 1] == '\n');
    }
    if (ferror(f)) {
        printf("\ncat: read failed: %s\n", strerror(errno));
        fclose(f);
        return 2;
    }

    if (total > 0 && !last_was_newline)
        fputc('\n', stdout);

    fclose(f);
    return 0;
}

