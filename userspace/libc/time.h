/*
 * userspace/libc/time.h — chapter 95: civil time helpers.
 *
 * Header-only.  All callers that want anything beyond
 * `time_t = seconds-since-1970` reach for these.
 *
 * The split between the two interfaces:
 *
 *   gettimeofday() / time()  — kernel syscall, returns scalars.
 *   gmtime_r()               — pure C, breaks scalar into fields.
 *   strftime_iso()           — pure C, formats fields into a
 *                              fixed ISO-8601 buffer.
 *
 * No timezone support: gmtime_r is UTC-only.  Userspace tools
 * that want local time apply a timezone offset themselves, e.g.
 * by reading /data/timezone (today nothing does — the chapter
 * 95 floor is UTC throughout, including the taskbar clock).
 *
 * Why no `localtime`?  Implementing it properly requires a tz
 * database (zoneinfo / posix tz strings + DST rules) which is a
 * milestone of its own.  We punt; future ch-95-polish can land
 * a `/data/timezone = "+10:00"`-style scalar offset that the
 * taskbar adds before calling gmtime_r.
 */

#ifndef OSDEV_LIBC_TIME_H
#define OSDEV_LIBC_TIME_H

#include <stdint.h>
#include "syscall.h"

/* Civil-time breakdown.  Field semantics match POSIX `struct tm`
 * except that we use direct names (sec/min/hour/mday/...) rather
 * than the historical tm_* prefix so the layout is obvious in
 * debugger output. */
struct civil_time {
    int year;       /* 4-digit, e.g. 2026                      */
    int month;      /* 1..12                                   */
    int mday;       /* day of month, 1..31                     */
    int hour;       /* 0..23                                   */
    int min;        /* 0..59                                   */
    int sec;        /* 0..59 (no leap seconds)                 */
    int wday;       /* 0..6 (Sun=0)                            */
    int yday;       /* 1..366                                  */
};

/* Return 1 if `y` is a Gregorian leap year, 0 otherwise.  Leap
 * year iff (y % 4 == 0 && y % 100 != 0) || y % 400 == 0. */
static inline int is_leap(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

/* Days in the n-th month (1..12) of year y. */
static inline int days_in_month(int y, int m)
{
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && is_leap(y)) return 29;
    return dim[m - 1];
}

/* Convert epoch seconds (UTC) into a civil_time.  Does NOT
 * apply any timezone offset — callers that want local time
 * should add their offset to `secs` before calling.
 *
 * Algorithm: walk years from 1970 up, then months, subtracting
 * seconds as we go.  O(years-since-1970) which is fine for a
 * civil-time helper called once per render frame.
 *
 * For 64-bit Y2038-safe input.  Negative inputs (pre-1970) are
 * not supported; caller will get garbage.  Tighten if/when we
 * grow a use-case for them.
 */
static inline void gmtime_r(time_t secs, struct civil_time *out)
{
    int64_t s = (int64_t)secs;

    /* Day-of-week first.  1970-01-01 was a Thursday (= 4 since
     * Sunday).  86400 seconds in a day. */
    int64_t days = s / 86400;
    out->wday = (int)((days + 4) % 7);

    /* Time-of-day. */
    int64_t day_secs = s - days * 86400;
    out->hour = (int)(day_secs / 3600);
    out->min  = (int)((day_secs % 3600) / 60);
    out->sec  = (int)(day_secs % 60);

    /* Walk years from 1970 forward, subtracting per-year days. */
    int year = 1970;
    while (1) {
        int yd = is_leap(year) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        year++;
    }
    out->year = year;

    /* Walk months. */
    int month = 1;
    while (month <= 12) {
        int md = days_in_month(year, month);
        if (days < md) break;
        days -= md;
        month++;
    }
    out->month = month;
    out->mday  = (int)(days + 1);

    /* Day of year — recompute the easy way so we don't have to
     * carry a running count through the month loop. */
    int yday = (int)days;       /* days remaining inside this month */
    for (int m = 1; m < month; m++)
        yday += days_in_month(year, m);
    out->yday = yday + 1;       /* 1-based as POSIX wants            */
}

/* Format `ct` into `buf` as "YYYY-MM-DD HH:MM:SS" (19 chars
 * plus a NUL = 20 bytes).  `cap` MUST be >= 20; if smaller, we
 * return -1 and write nothing.  Returns 19 on success.
 *
 * Deliberately NOT a full strftime — chapter 95 only needs one
 * timestamp shape.  A real strftime gets implemented when
 * something cares about another format. */
static inline int strftime_iso(char *buf, unsigned long cap,
                               const struct civil_time *ct)
{
    if (cap < 20) return -1;
    /* Manual decimal formatter to avoid pulling in printf for
     * the simplest possible /bin/date and taskbar.c. */
    static const char digits[] = "0123456789";
    int y = ct->year;
    buf[0] = digits[(y / 1000) % 10];
    buf[1] = digits[(y /  100) % 10];
    buf[2] = digits[(y /   10) % 10];
    buf[3] = digits[ y          % 10];
    buf[4] = '-';
    buf[5] = digits[(ct->month / 10) % 10];
    buf[6] = digits[ ct->month       % 10];
    buf[7] = '-';
    buf[8]  = digits[(ct->mday / 10) % 10];
    buf[9]  = digits[ ct->mday       % 10];
    buf[10] = ' ';
    buf[11] = digits[(ct->hour / 10) % 10];
    buf[12] = digits[ ct->hour       % 10];
    buf[13] = ':';
    buf[14] = digits[(ct->min  / 10) % 10];
    buf[15] = digits[ ct->min        % 10];
    buf[16] = ':';
    buf[17] = digits[(ct->sec  / 10) % 10];
    buf[18] = digits[ ct->sec        % 10];
    buf[19] = '\0';
    return 19;
}

#endif /* OSDEV_LIBC_TIME_H */
