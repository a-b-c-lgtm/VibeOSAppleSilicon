/* userspace/strtest/strtest.c — chapter 167 regression.
 *
 * Exercises ctype.h and string.h end-to-end.  Each macro/function
 * gets at least one positive and one negative input; the test
 * keeps a running counter and prints "all checks passed" only
 * if zero failures.
 *
 * Does NOT call assert() -- assertion-failure behaviour is
 * covered by userspace/assertfail/assertfail.c instead, because
 * a failing assert() terminates the process.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/ctype.h"
#include "../libc/string.h"

static int g_fail;

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_fail++;                                                \
        }                                                            \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[strtest] starting\n");

    /* ctype.h */
    CHECK(isdigit('0') && isdigit('9'));
    CHECK(!isdigit('a') && !isdigit(' '));
    CHECK(isxdigit('0') && isxdigit('a') && isxdigit('F'));
    CHECK(!isxdigit('g'));
    CHECK(isalpha('a') && isalpha('Z'));
    CHECK(!isalpha('0'));
    CHECK(isalnum('a') && isalnum('0'));
    CHECK(!isalnum('!'));
    CHECK(isspace(' ') && isspace('\t') && isspace('\n'));
    CHECK(!isspace('a'));
    CHECK(isupper('A') && !isupper('a'));
    CHECK(islower('a') && !islower('A'));
    CHECK(toupper('a') == 'A');
    CHECK(toupper('A') == 'A');
    CHECK(tolower('Z') == 'z');
    CHECK(tolower('z') == 'z');
    CHECK(isprint(' ') && isprint('a'));
    CHECK(!isprint('\n'));
    CHECK(iscntrl('\n') && iscntrl('\t'));
    CHECK(!iscntrl('a'));
    CHECK(isblank(' ') && isblank('\t'));
    CHECK(!isblank('\n'));
    CHECK(ispunct('!') && ispunct(','));
    CHECK(!ispunct('a') && !ispunct(' '));

    /* string.h — strlen / strcmp / strncmp */
    CHECK(strlen("") == 0);
    CHECK(strlen("hello") == 5);
    CHECK(strcmp("a", "a") == 0);
    CHECK(strcmp("a", "b") < 0);
    CHECK(strcmp("b", "a") > 0);
    CHECK(strncmp("abcd", "abce", 3) == 0);
    CHECK(strncmp("abcd", "abce", 4) != 0);
    CHECK(strncmp("abcd", "abce", 0) == 0);

    /* strchr / strrchr / memchr */
    const char *hay = "hello, world";
    CHECK(strchr(hay, 'l') == hay + 2);
    CHECK(strchr(hay, 'z') == 0);
    CHECK(strchr(hay, '\0') == hay + 12);
    CHECK(strrchr(hay, 'l') == hay + 10);
    CHECK(strrchr(hay, 'z') == 0);
    CHECK(memchr("abcde", 'c', 5) == (void *)"abcde" + 2);
    CHECK(memchr("abcde", 'z', 5) == 0);

    /* strcpy / strncpy */
    char buf[16];
    strcpy(buf, "hello");
    CHECK(strcmp(buf, "hello") == 0);
    strncpy(buf, "abcdefgh", 4);
    /* strncpy: first 4 bytes only, no NUL added because src is
     * longer than n */
    CHECK(buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c' && buf[3] == 'd');

    /* strcat / strncat */
    strcpy(buf, "foo");
    strcat(buf, "bar");
    CHECK(strcmp(buf, "foobar") == 0);
    strcpy(buf, "x");
    strncat(buf, "yyyyyyyy", 3);
    CHECK(strcmp(buf, "xyyy") == 0);

    /* strspn / strcspn / strpbrk */
    CHECK(strspn("aaabbb", "ab") == 6);
    CHECK(strspn("aaabbb", "a") == 3);
    CHECK(strcspn("aaabbb", "b") == 3);
    CHECK(strpbrk("hello", "lo") == (char *)"hello" + 2);
    CHECK(strpbrk("hello", "xyz") == 0);

    /* strstr */
    CHECK(strstr("hello world", "world") == (char *)"hello world" + 6);
    CHECK(strstr("hello", "") == (char *)"hello");
    CHECK(strstr("abc", "abcdef") == 0);

    /* strlcpy / strlcat */
    char small[6];
    size_t n = strlcpy(small, "hello", sizeof small);
    CHECK(n == 5);
    CHECK(strcmp(small, "hello") == 0);
    /* truncation case */
    n = strlcpy(small, "helloworld", sizeof small);
    CHECK(n == 10);                  /* total source length */
    CHECK(strcmp(small, "hello") == 0);  /* truncated and NUL'd */
    n = strlcat(small, "XYZ", sizeof small);
    CHECK(n == 8);                   /* would-be combined length */
    CHECK(strcmp(small, "hello") == 0);  /* no room, unchanged */

    /* atoi */
    CHECK(atoi("0") == 0);
    CHECK(atoi("42") == 42);
    CHECK(atoi("-17") == -17);
    CHECK(atoi("  +99 trailing junk") == 99);
    CHECK(atoi("not a number") == 0);

    /* memcpy / memset / memmove / memcmp (from cstring.o) */
    char a[16], b[16];
    memset(a, 0xAB, sizeof a);
    for (int i = 0; i < (int)sizeof a; i++) CHECK(a[i] == (char)0xAB);
    memcpy(b, a, sizeof a);
    CHECK(memcmp(a, b, sizeof a) == 0);
    memset(b, 0xCD, sizeof b);
    CHECK(memcmp(a, b, sizeof a) != 0);
    /* memmove overlap (high to low) */
    char ov[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    memmove(ov + 2, ov, 6);
    CHECK(ov[0] == 1 && ov[1] == 2 && ov[2] == 1
       && ov[3] == 2 && ov[4] == 3 && ov[5] == 4
       && ov[6] == 5 && ov[7] == 6);

    if (g_fail == 0) {
        printf("[strtest] all checks passed\n");
        return 0;
    }
    printf("[strtest] FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
