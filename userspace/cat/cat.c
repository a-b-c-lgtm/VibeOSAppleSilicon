/* userspace/cat/cat.c — file-printing user program.
 *
 * Reads its target file path from argv[1] (laid out on the user
 * stack by the kernel ELF loader; see kernel/core/elf.c).
 * Defaults to "/motd" when called with no arguments.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    const char *target = "/motd";
    if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        target = argv[1];

    int fd = open(target, 0);
    if (fd < 0) {
        printf("cat: cannot open %s: errno=%d\n", target, -fd);
        return 1;
    }

    char buf[256];
    long total = 0;
    long n;
    int last_was_newline = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
        total += n;
        last_was_newline = (buf[n - 1] == '\n');
    }
    if (n < 0) {
        printf("\ncat: read failed: errno=%d\n", (int)-n);
        close(fd);
        return 2;
    }

    if (total > 0 && !last_was_newline)
        write(1, "\n", 1);

    close(fd);
    return 0;
}

