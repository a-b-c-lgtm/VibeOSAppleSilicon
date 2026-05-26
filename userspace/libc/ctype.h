/* userspace/libc/ctype.h — chapter 128c.
 *
 * Header-only C99 <ctype.h>.  Every function is `static inline`
 * so each binary that includes the header gets a private copy
 * (matches the libc pattern from printf.h, malloc.h, etc.).
 *
 * Locale: we are C-locale-only.  No locale_t, no LC_CTYPE switch.
 * Every character class is the ASCII set.  Real upstream code
 * (Doom, GCC, BearSSL, ...) doesn't ever set up a non-C locale,
 * so this is the right scope.
 *
 * Argument convention (POSIX/C99 6.4.2.1): each function takes
 * an int, but its value MUST be representable as `unsigned char`
 * or be EOF.  Passing a signed char with the high bit set is
 * undefined behaviour because the macro implementations typically
 * index into a table; ours don't (we use range checks), so we
 * tolerate the broader input, but callers should still cast to
 * `(unsigned char)` for portability.
 */
#ifndef USERSPACE_LIBC_CTYPE_H
#define USERSPACE_LIBC_CTYPE_H

static inline int isascii(int c)  { return ((unsigned)c) < 128; }
static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
static inline int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n'
        || c == '\v' || c == '\f' || c == '\r';
}
static inline int isblank(int c)  { return c == ' ' || c == '\t'; }
static inline int iscntrl(int c)  { return (unsigned)c < 32 || c == 127; }
static inline int isprint(int c)  { return c >= 32 && c < 127; }
static inline int isgraph(int c)  { return c > 32 && c < 127; }
static inline int ispunct(int c)  { return isprint(c) && !isalnum(c) && c != ' '; }

static inline int tolower(int c)  { return isupper(c) ? c + ('a' - 'A') : c; }
static inline int toupper(int c)  { return islower(c) ? c - ('a' - 'A') : c; }

#endif /* USERSPACE_LIBC_CTYPE_H */
