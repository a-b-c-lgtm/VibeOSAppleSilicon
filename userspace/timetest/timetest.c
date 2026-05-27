/* userspace/timetest/timetest.c — chapter 168 regression.
 *
 * Exercises the POSIX <time.h> surface:
 *   - clock_gettime(CLOCK_REALTIME)
 *   - gmtime_r round-trip via mktime
 *   - strftime("%Y-%m-%d %H:%M:%S", ...)
 *   - asctime() shape
 *   - difftime() simple subtraction
 *
 * Most checks are byte-exact against hand-computed values so
 * that an algorithm regression (off-by-one in the year loop,
 * wrong leap-day handling, etc.) surfaces as a specific FAIL
 * rather than a fuzzy "looks wrong".
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/string.h"
#include "../libc/time.h"

static int g_fail;

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_fail++;                                                \
        }                                                            \
    } while (0)

#define CHECK_EQ_STR(a, b)                                           \
    do {                                                             \
        if (strcmp((a), (b)) != 0) {                                 \
            printf("  FAIL %s:%d: \"%s\" != \"%s\"\n",               \
                   __FILE__, __LINE__, (a), (b));                    \
            g_fail++;                                                \
        }                                                            \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[timetest] starting\n");

    /* clock_gettime smoke -- just check the call succeeds and
     * yields a plausible value. */
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 0;
    CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    CHECK(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L);

    /* Epoch 0 -> 1970-01-01 00:00:00 Thu. */
    time_t t = 0;
    struct tm tm;
    CHECK(gmtime_r(&t, &tm) == &tm);
    CHECK(tm.tm_year == 70);          /* 1970 - 1900 */
    CHECK(tm.tm_mon  == 0);
    CHECK(tm.tm_mday == 1);
    CHECK(tm.tm_hour == 0);
    CHECK(tm.tm_min  == 0);
    CHECK(tm.tm_sec  == 0);
    CHECK(tm.tm_wday == 4);           /* Thursday */
    CHECK(tm.tm_yday == 0);

    /* Known reference: t = 1234567890 -> 2009-02-13 23:31:30 UTC.
     * This is the famous unix-time-billennium check. */
    t = 1234567890;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 109);
    CHECK(tm.tm_mon  == 1);
    CHECK(tm.tm_mday == 13);
    CHECK(tm.tm_hour == 23);
    CHECK(tm.tm_min  == 31);
    CHECK(tm.tm_sec  == 30);

    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    CHECK_EQ_STR(buf, "2009-02-13 23:31:30");

    strftime(buf, sizeof buf, "%y/%m/%d %H:%M:%S %p", &tm);
    CHECK_EQ_STR(buf, "09/02/13 23:31:30 PM");

    strftime(buf, sizeof buf, "100%% %j", &tm);
    /* Feb 13 is day 31 + 13 = day 44 in 2009 (not a leap year). */
    CHECK_EQ_STR(buf, "100% 044");

    /* Leap-year boundary: 2020-02-29 00:00:00 UTC. */
    struct tm leap = {
        .tm_year = 120, .tm_mon = 1, .tm_mday = 29,
        .tm_hour = 0,   .tm_min = 0, .tm_sec = 0
    };
    time_t leap_t = timegm(&leap);
    /* Round-trip: gmtime_r should give us the same field set. */
    struct tm back;
    gmtime_r(&leap_t, &back);
    CHECK(back.tm_year == 120);
    CHECK(back.tm_mon  == 1);
    CHECK(back.tm_mday == 29);
    CHECK(back.tm_wday == 6);  /* Saturday */
    /* Day-of-year for 2020-02-29: 31 (Jan) + 29 - 1 = 59. */
    CHECK(back.tm_yday == 59);

    /* mktime round-trip: pick t = 1700000000, round-trip via
     * mktime, expect identity. */
    t = 1700000000;
    gmtime_r(&t, &tm);
    time_t t2 = mktime(&tm);
    CHECK(t == t2);

    /* difftime: returns double, needs FP at EL0 (chapter 171);
     * not exercised here.  Test will get a difftime() check
     * when chapter 171 lifts -mgeneral-regs-only. */

    /* asctime shape -- always exactly 25 chars + NUL. */
    struct tm wed = {
        .tm_year = 93, .tm_mon = 5, .tm_mday = 30,
        .tm_hour = 21, .tm_min = 49, .tm_sec = 8,
        .tm_wday = 3
    };
    char *as = asctime(&wed);
    CHECK_EQ_STR(as, "Wed Jun 30 21:49:08 1993\n");

    /* localtime is gmtime (no TZ). */
    t = 1234567890;
    struct tm lt;
    localtime_r(&t, &lt);
    struct tm gt;
    gmtime_r(&t, &gt);
    CHECK(lt.tm_year == gt.tm_year);
    CHECK(lt.tm_yday == gt.tm_yday);
    CHECK(lt.tm_sec  == gt.tm_sec);

    if (g_fail == 0) {
        printf("[timetest] all checks passed\n");
        return 0;
    }
    printf("[timetest] FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
