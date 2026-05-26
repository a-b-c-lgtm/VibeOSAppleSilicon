/* userspace/libc/locale.h — chapter 131e minimal POSIX <locale.h>.
 *
 * binutils 2.44 bfd/sysdep.h includes this unconditionally even with
 * --disable-nls, and ld/ldmain.c calls setlocale().  We don't have a
 * locale system; setlocale() is a no-op stub returning "C" and the
 * LC_* category constants are the standard POSIX values.
 *
 * localeconv() and struct lconv are intentionally NOT defined yet —
 * binutils never calls localeconv().  Add them in a later chapter
 * when something demands them.
 */
#ifndef _LOCALE_H
#define _LOCALE_H 1

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

static inline char *setlocale(int category, const char *locale)
{
    (void)category;
    (void)locale;
    return (char *)(unsigned long)"C";
}

#endif /* _LOCALE_H */
