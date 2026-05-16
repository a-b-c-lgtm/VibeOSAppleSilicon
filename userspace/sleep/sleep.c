/* userspace/sleep/sleep.c — pause for N seconds (or N.MMM).
 *
 *   sleep N      # block for N seconds
 *
 * Accepts an integer or a fixed-point N.MMM (millisecond
 * precision).  Default 1 if no arg.  Sleep granularity at the
 * kernel is one scheduler tick (100 ms today), so very short
 * sleeps round up.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

static unsigned long parse_ms(const char *s)
{
    unsigned long whole = 0;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (unsigned long)(*s - '0');
        s++;
    }
    unsigned long frac = 0;
    int frac_digits = 0;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && frac_digits < 3) {
            frac = frac * 10 + (unsigned long)(*s - '0');
            s++;
            frac_digits++;
        }
        while (frac_digits < 3) { frac *= 10; frac_digits++; }
        /* drop further digits */
        while (*s >= '0' && *s <= '9') s++;
    }
    if (*s) return 0;        /* trailing garbage -> 0 (caller errors) */
    return whole * 1000UL + frac;
}

int main(int argc, char **argv)
{
    unsigned long ms = 1000UL;
    if (argc >= 2 && argv[1] && argv[1][0]) {
        ms = parse_ms(argv[1]);
        if (ms == 0 && (argv[1][0] != '0' || argv[1][1])) {
            printf("sleep: bad duration: %s\n", argv[1]);
            return 1;
        }
    }
    sleep_ms(ms);
    return 0;
}
