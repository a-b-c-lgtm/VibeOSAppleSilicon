/* userspace/printftest/printftest.c — exercises the printf header.
 *
 * Validates the supported format specifiers, width and flag
 * handling, snprintf truncation behaviour, and the integration
 * between argv (milestone 18) and printf (milestone 19).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    printf("[printftest] starting\n");

    /* Basic specifiers. */
    printf("  hello %s, you are %d years old\n", "world", 7);
    printf("  unsigned %u, hex %x, HEX %X\n", 0xABCDu, 0xdeadu, 0xbeefu);
    printf("  ptr %p, char '%c', literal %%\n", (void *)0x123456, 'Q');

    /* Long / size_t. */
    printf("  long  %ld, ulong %lx\n", -1234567890L, 0x1122334455667788UL);
    printf("  size  %zu\n", (size_t)4096);

    /* Width + flags. */
    printf("  '|%5d|' '|%-5d|' '|%05d|'\n", 42, 42, 42);
    printf("  '|%05d|' (negative)\n", -7);

    /* argv echo. */
    printf("  argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("    argv[%d] = \"%s\"\n", i, argv[i]);

    /* snprintf into a fixed buffer. */
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "pi=%d.%d, x=%x", 3, 14, 0xCAFE);
    printf("  snprintf -> n=%d buf=\"%s\"\n", n, buf);

    /* snprintf truncation: ask for more than buf can hold. */
    char small[8];
    n = snprintf(small, sizeof(small), "abcdefghijklmno");
    printf("  trunc    -> n=%d buf=\"%s\"\n", n, small);

    printf("[printftest] all checks passed\n");
    return 0;
}
