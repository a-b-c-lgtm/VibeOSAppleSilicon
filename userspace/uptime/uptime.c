/* userspace/uptime/uptime.c — print kernel uptime.
 *
 * Reads SYS_UPTIME_MS and prints it as "Hh Mm S.SSSs".  Useful
 * sanity check that the timer keeps ticking and that the syscall
 * really is monotonic.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    unsigned long ms = uptime_ms();
    unsigned long s  = ms / 1000UL;
    unsigned long m  = s  / 60UL;
    unsigned long h  = m  / 60UL;
    unsigned long ms_part = ms % 1000UL;
    unsigned long s_part  = s  % 60UL;
    unsigned long m_part  = m  % 60UL;

    printf("uptime: %luh %02lum %02lu.%03lus  (%lu ms)\n",
           h, m_part, s_part, ms_part, ms);
    return 0;
}
