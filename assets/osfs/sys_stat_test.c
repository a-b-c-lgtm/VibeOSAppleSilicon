/* assets/osfs/sys_stat_test.c -- chapter 132j subdir-headers smoke.
 *
 * Exercises every angle of the sys/ + unistd.h shipping:
 *
 *   - #include <sys/stat.h>   tests cpp's -isystem /bin search
 *                             for a literal "sys/stat.h" entry.
 *   - #include <sys/types.h>  same, second sys/ header.
 *   - #include <unistd.h>     top-level header that itself does
 *                             #include "sys/stat.h" (quoted) --
 *                             resolves relative to /bin/unistd.h's
 *                             dir = /bin/, finds /bin/sys/stat.h
 *                             via literal name match.
 *
 * Body uses real stat() + access() syscalls so a missing symbol
 * at link time also fails the test.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
    struct stat st;
    if (stat("/bin/stdio_test.c", &st) != 0) {
        puts("stat /bin/stdio_test.c FAILED");
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        puts("stdio_test.c is not a regular file?");
        return 2;
    }
    if (access("/bin/stdio_test.c", F_OK) != 0) {
        puts("access /bin/stdio_test.c FAILED");
        return 3;
    }
    printf("stdio_test.c size=%lld mode=0x%x\n",
           (long long)st.st_size, (unsigned)st.st_mode);
    puts("sys_stat_test OK");
    return 0;
}
