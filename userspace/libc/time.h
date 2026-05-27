/*
 * userspace/libc/time.h — chapter 168 (POSIX shape).
 *
 * Header-only.  Started life in chapter 96 with a non-POSIX
 * `struct civil_time` + `gmtime_r(time_t, struct civil_time *)`
 * shape; chapter 168 swaps in the C99 / POSIX shape so real
 * upstream code (Doom, GCC, BearSSL) can include this header
 * unmodified.
 *
 * The split between the two interfaces:
 *
 *   gettimeofday() / time()  — kernel syscall, returns scalars
 *                              (defined in syscall.h, not here).
 *   gmtime_r() / gmtime()    — pure C, break scalar into POSIX
 *                              `struct tm` fields.
 *   localtime_r() / localtime() — alias for gmtime; we are
 *                              UTC-only (no tzdata).
 *   mktime() / timegm()      — pure C inverse.
 *   strftime()               — minimal C99 subset:
 *                              %Y %y %m %d %H %M %S %j %p %%.
 *   asctime() / ctime()      — fixed 26-char "Wed Jun 30
 *                              21:49:08 1993\n\0" shape.
 *   clock_gettime()          — CLOCK_REALTIME, CLOCK_MONOTONIC
 *                              (both alias gettimeofday today).
 *   clock()                  — monotonic ms since first call.
 *   difftime()               — end - start as double.
 *
 * No timezone support: localtime == gmtime.  We run in UTC.
 * Userspace tools that want local time apply a timezone offset
 * themselves; nothing in tree does today.
 */

#ifndef OSDEV_LIBC_TIME_H
#define OSDEV_LIBC_TIME_H

#include <stddef.h>
#include <stdint.h>
#include "syscall.h"        /* time_t, struct timeval,
                             * gettimeofday() */

#ifdef __cplusplus
extern "C" {
#endif

typedef long clock_t;
typedef int  clockid_t;

#ifndef CLOCKS_PER_SEC
# define CLOCKS_PER_SEC 1000L   /* we report clock() in ms */
#endif

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec;     /* 0..60 (60 for leap second)            */
    int tm_min;     /* 0..59                                 */
    int tm_hour;    /* 0..23                                 */
    int tm_mday;    /* 1..31                                 */
    int tm_mon;     /* 0..11 (Jan = 0)                       */
    int tm_year;    /* years since 1900                      */
    int tm_wday;    /* 0..6  (Sun = 0)                       */
    int tm_yday;    /* 0..365                                */
    int tm_isdst;   /* always 0 -- we don't model DST        */
};

/* ── small helpers (kept from chapter 96) ────────────────────── */

/* Return 1 if `y` is a Gregorian leap year, 0 otherwise. */
static inline int __osdev_is_leap(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

/* Days in the n-th month (1..12) of year y. */
static inline int __osdev_days_in_month(int y, int m)
{
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && __osdev_is_leap(y)) return 29;
    return dim[m - 1];
}

/* ── difftime ────────────────────────────────────────────────── */
/* C99 says difftime returns double.  Pre-chapter-129 we don't
 * have FP at EL0; callers that actually consume the result will
 * fail to compile then.  Doom calls difftime exactly once and
 * the value is fed straight back into time arithmetic, so we
 * can re-target it to `long` in chapter 172 if needed. */
static inline double difftime(time_t end, time_t start)
{
    return (double)(end - start);
}

/* ── clock_gettime / clock ───────────────────────────────────── */
static inline int clock_gettime(clockid_t clk, struct timespec *ts)
{
    if (!ts) return -1;
    (void)clk;
    struct timeval tv;
    if (gettimeofday(&tv, (void *)0) != 0) return -1;
    ts->tv_sec  = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000L;
    return 0;
}

/* clock(): "best approximation of processor time used by the
 * program".  We approximate as wall time since the first call.
 * Good enough for Doom's RNG seeding and `time make`. */
static inline clock_t clock(void)
{
    static time_t origin = 0;
    static int    inited = 0;
    struct timeval tv;
    if (gettimeofday(&tv, (void *)0) != 0) return (clock_t)-1;
    if (!inited) { origin = tv.tv_sec; inited = 1; }
    return (clock_t)((tv.tv_sec - origin) * 1000L
                   + (long)(tv.tv_usec / 1000));
}

/* ── gmtime_r / gmtime ───────────────────────────────────────── */
/* Walks years forward from 1970; O(years-since-epoch).  Good
 * enough for any plausible runtime length on this OS. */
static inline struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    if (!t || !out) return (struct tm *)0;
    int64_t s = (int64_t)*t;

    /* Day-of-week first.  1970-01-01 was a Thursday (= 4). */
    int64_t days = s / 86400;
    int64_t day_secs = s - days * 86400;
    /* Handle negative inputs (rare; pre-1970 timestamps). */
    if (day_secs < 0) { day_secs += 86400; days--; }
    out->tm_wday = (int)(((days % 7) + 4 + 7) % 7);

    out->tm_hour = (int)(day_secs / 3600);
    out->tm_min  = (int)((day_secs % 3600) / 60);
    out->tm_sec  = (int)(day_secs % 60);

    /* Walk years from 1970 forward, subtracting per-year days. */
    int year = 1970;
    while (1) {
        int yd = __osdev_is_leap(year) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        year++;
    }

    /* Walk months. */
    int month = 1;
    while (month <= 12) {
        int md = __osdev_days_in_month(year, month);
        if (days < md) break;
        days -= md;
        month++;
    }

    out->tm_year  = year - 1900;
    out->tm_mon   = month - 1;
    out->tm_mday  = (int)(days + 1);

    /* Day-of-year: walk months earlier in the same year. */
    int yday = (int)days;
    for (int m = 1; m < month; m++)
        yday += __osdev_days_in_month(year, m);
    out->tm_yday  = yday;            /* 0-based, per POSIX */
    out->tm_isdst = 0;
    return out;
}

static inline struct tm *gmtime(const time_t *t)
{
    static struct tm buf;
    return gmtime_r(t, &buf);
}

/* localtime: alias for gmtime (we don't model DST or TZ). */
static inline struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return gmtime_r(t, out);
}

static inline struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}

/* ── mktime / timegm ─────────────────────────────────────────── */
/* Sum up days for the calendar fields, then re-derive the tm
 * struct from the canonical time_t to normalize wday/yday/etc. */
static inline time_t timegm(struct tm *tm)
{
    if (!tm) return (time_t)-1;
    int year = tm->tm_year + 1900;
    int mon  = tm->tm_mon + 1;
    /* Days from 1970-01-01 to year/mon/mday. */
    int64_t days = 0;
    for (int y = 1970; y < year; y++)
        days += __osdev_is_leap(y) ? 366 : 365;
    for (int m = 1; m < mon; m++)
        days += __osdev_days_in_month(year, m);
    days += tm->tm_mday - 1;
    time_t t = (time_t)(days * 86400
                      + tm->tm_hour * 3600
                      + tm->tm_min  * 60
                      + tm->tm_sec);
    gmtime_r(&t, tm);
    return t;
}

static inline time_t mktime(struct tm *tm)
{
    /* No TZ -> mktime == timegm. */
    return timegm(tm);
}

/* ── strftime: minimal C99 subset ─────────────────────────────
 * Supports %Y %y %m %d %H %M %S %j %p %% and literal passthrough.
 * No widths, no padding flags beyond default zero-pad of numeric
 * fields.  Returns the number of characters written (excluding
 * the trailing NUL), or 0 if the buffer was too small.
 *
 * Covers Doom (demo timestamps) and GCC diagnostics
 * ("%Y-%m-%d %H:%M:%S").  Extend on demand. */
static inline size_t strftime(char *dst, size_t cap,
                              const char *fmt,
                              const struct tm *tm)
{
    if (!dst || cap == 0 || !fmt || !tm) return 0;
    size_t w = 0;
    #define __OSDEV_PUT(c) do { \
        if (w + 1 >= cap) { dst[w] = '\0'; return 0; } \
        dst[w++] = (char)(c); \
    } while (0)
    #define __OSDEV_PUTN(val, width) do { \
        char tmpb[8]; int ti = 0; long vv = (val); \
        if (vv < 0) vv = -vv; \
        if (vv == 0) tmpb[ti++] = '0'; \
        while (vv > 0) { tmpb[ti++] = (char)('0' + vv % 10); vv /= 10; } \
        while (ti < (width)) tmpb[ti++] = '0'; \
        while (ti--) __OSDEV_PUT(tmpb[ti]); \
    } while (0)
    while (*fmt) {
        if (*fmt != '%') { __OSDEV_PUT(*fmt); fmt++; continue; }
        fmt++;
        switch (*fmt) {
        case 'Y': __OSDEV_PUTN(tm->tm_year + 1900, 4); break;
        case 'y': __OSDEV_PUTN((tm->tm_year + 1900) % 100, 2); break;
        case 'm': __OSDEV_PUTN(tm->tm_mon + 1, 2); break;
        case 'd': __OSDEV_PUTN(tm->tm_mday, 2); break;
        case 'H': __OSDEV_PUTN(tm->tm_hour, 2); break;
        case 'M': __OSDEV_PUTN(tm->tm_min, 2); break;
        case 'S': __OSDEV_PUTN(tm->tm_sec, 2); break;
        case 'j': __OSDEV_PUTN(tm->tm_yday + 1, 3); break;
        case 'p': {
            const char *s = tm->tm_hour < 12 ? "AM" : "PM";
            while (*s) __OSDEV_PUT(*s++);
            break;
        }
        case '%': __OSDEV_PUT('%'); break;
        case '\0': dst[w] = '\0'; return w;
        default:  __OSDEV_PUT('%'); __OSDEV_PUT(*fmt); break;
        }
        fmt++;
    }
    dst[w] = '\0';
    return w;
    #undef __OSDEV_PUT
    #undef __OSDEV_PUTN
}

/* ── asctime / ctime ─────────────────────────────────────────── */
static inline char *asctime_r(const struct tm *tm, char *buf)
{
    static const char *const wday[7] = {
        "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
    };
    static const char *const mon[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    if (!tm || !buf) return (char *)0;
    int wd = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    int mo = (tm->tm_mon  >= 0 && tm->tm_mon < 12) ? tm->tm_mon  : 0;
    int yr = tm->tm_year + 1900;
    buf[ 0] = wday[wd][0]; buf[ 1] = wday[wd][1]; buf[ 2] = wday[wd][2];
    buf[ 3] = ' ';
    buf[ 4] = mon[mo][0];  buf[ 5] = mon[mo][1];  buf[ 6] = mon[mo][2];
    buf[ 7] = ' ';
    buf[ 8] = (char)('0' + (tm->tm_mday / 10));
    buf[ 9] = (char)('0' + (tm->tm_mday % 10));
    buf[10] = ' ';
    buf[11] = (char)('0' + (tm->tm_hour / 10));
    buf[12] = (char)('0' + (tm->tm_hour % 10));
    buf[13] = ':';
    buf[14] = (char)('0' + (tm->tm_min / 10));
    buf[15] = (char)('0' + (tm->tm_min % 10));
    buf[16] = ':';
    buf[17] = (char)('0' + (tm->tm_sec / 10));
    buf[18] = (char)('0' + (tm->tm_sec % 10));
    buf[19] = ' ';
    buf[20] = (char)('0' + ((yr / 1000) % 10));
    buf[21] = (char)('0' + ((yr / 100)  % 10));
    buf[22] = (char)('0' + ((yr / 10)   % 10));
    buf[23] = (char)('0' + ( yr         % 10));
    buf[24] = '\n';
    buf[25] = '\0';
    return buf;
}

static inline char *asctime(const struct tm *tm)
{
    static char buf[26];
    return asctime_r(tm, buf);
}

static inline char *ctime_r(const time_t *t, char *buf)
{
    struct tm tm;
    if (!gmtime_r(t, &tm)) return (char *)0;
    return asctime_r(&tm, buf);
}

static inline char *ctime(const time_t *t)
{
    static char buf[26];
    return ctime_r(t, buf);
}

#ifdef __cplusplus
}
#endif

#endif /* OSDEV_LIBC_TIME_H */

