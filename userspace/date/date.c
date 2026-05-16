/* userspace/date/date.c — chapter 95: print wall-clock time.
 *
 * Reads SYS_GETTIMEOFDAY and prints "YYYY-MM-DD HH:MM:SS UTC"
 * to stdout.  No flags today (no -u / -R / +%FORMAT).  When we
 * grow a real timezone scheme, this is the place that gains a
 * "TZ=Australia/Brisbane date" path.
 *
 * On a system where the kernel never found an RTC (e.g. PL031
 * absent, no DTB node), `time()` returns 0 + the uptime delta;
 * the formatted output starts at "1970-01-01 00:00:0X UTC".
 * Tests can use that as the "no real RTC" detector.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/time.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    struct timeval tv;
    int rc = gettimeofday(&tv);
    if (rc != 0) {
        printf("date: gettimeofday failed: %d\n", rc);
        return 1;
    }

    struct civil_time ct;
    gmtime_r((time_t)tv.tv_sec, &ct);

    char buf[24];
    strftime_iso(buf, sizeof(buf), &ct);
    printf("%s UTC\n", buf);
    return 0;
}
