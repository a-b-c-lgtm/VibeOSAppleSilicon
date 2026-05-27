/* userspace/stdlibtest/stdlibtest.c — chapter 169 regression.
 *
 * Exercises the new <stdlib.h> surface:
 *   - qsort + bsearch
 *   - strtol / strtoul / strtoll / strtoull (incl. base 0, 16,
 *     overflow -> errno = ERANGE)
 *   - atol / atoll
 *   - abs / labs / llabs / div / ldiv / lldiv
 *   - getopt over a synthesised argv that mimics what Doom's
 *     command line looks like ("-iwad DOOM1.WAD -warp 1 -nomusic
 *     -monsters 4")
 *
 * Uses the same CHECK() macro pattern as strtest / timetest so a
 * single "all checks passed" marker fires on success.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/string.h"
#include "../libc/errno.h"
#include "../libc/stdlib.h"

static int g_fail;

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_fail++;                                                \
        }                                                            \
    } while (0)

/* --- qsort + bsearch -------------------------------------------------- */

static int cmp_int(const void *a, const void *b)
{
    int ai = *(const int *)a;
    int bi = *(const int *)b;
    return (ai > bi) - (ai < bi);
}

static int cmp_str(const void *a, const void *b)
{
    /* Caller passes `char **`s into qsort/bsearch over a
     * `char *[]` array. */
    const char *as = *(const char *const *)a;
    const char *bs = *(const char *const *)b;
    return strcmp(as, bs);
}

/* --- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[stdlibtest] starting\n");

    /* abs/labs/llabs, div/ldiv/lldiv */
    CHECK(abs(-5)         == 5);
    CHECK(abs( 0)         == 0);
    CHECK(labs(-1234567L) == 1234567L);
    CHECK(llabs(-1LL)     == 1LL);
    div_t  d  = div(17, 5);
    CHECK(d.quot == 3 && d.rem == 2);
    ldiv_t dl = ldiv(-17L, 5L);
    /* C99 division truncates toward zero: -17/5 = -3 r -2. */
    CHECK(dl.quot == -3L && dl.rem == -2L);

    /* strtol — basic + base 0 + base 16 + sign */
    char *ep;
    errno = 0;
    CHECK(strtol("0",    &ep, 10) == 0    && *ep == '\0');
    CHECK(strtol("42",   &ep, 10) == 42   && *ep == '\0');
    CHECK(strtol("-7",   &ep, 10) == -7   && *ep == '\0');
    CHECK(strtol(" +99trail", &ep, 10) == 99 && ep && *ep == 't');
    CHECK(strtol("0xFF", &ep, 0)  == 255  && *ep == '\0');
    CHECK(strtol("0xff", &ep, 16) == 255  && *ep == '\0');
    CHECK(strtol("011",  &ep, 0)  == 9    && *ep == '\0'); /* octal */
    CHECK(strtol("11",   &ep, 0)  == 11   && *ep == '\0');
    CHECK(strtol("zzz",  &ep, 36) == (35L * 36 + 35) * 36 + 35);

    /* Invalid input: returns 0, endptr == nptr. */
    const char *junk = "abc";
    CHECK(strtol(junk, &ep, 10) == 0);
    CHECK(ep == junk);

    /* Overflow on positive side. */
    errno = 0;
    long ov = strtol("99999999999999999999", &ep, 10);
    CHECK(ov == LONG_MAX);
    CHECK(errno == ERANGE);

    /* Overflow on negative side. */
    errno = 0;
    ov = strtol("-99999999999999999999", &ep, 10);
    CHECK(ov == LONG_MIN);
    CHECK(errno == ERANGE);

    /* strtoul — base 16 with explicit prefix, base 2 binary. */
    errno = 0;
    CHECK(strtoul("0xdeadbeef", &ep, 16) == 0xdeadbeefUL);
    CHECK(strtoul("1010",       &ep, 2)  == 10UL);

    /* strtoll / strtoull just delegate; spot-check. */
    CHECK(strtoll("-9223372036854775807", &ep, 10) == -9223372036854775807LL);
    CHECK(strtoull("18446744073709551615", &ep, 10) == 18446744073709551615ULL);

    /* atol / atoll */
    CHECK(atol("12345")      == 12345L);
    CHECK(atoll("-67890")    == -67890LL);

    /* qsort + bsearch on ints. */
    int arr[]   = { 9, 4, 7, 1, 8, 2, 6, 3, 5, 0, 11, 10, 13, 12 };
    int sorted[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    const size_t N = sizeof arr / sizeof arr[0];
    qsort(arr, N, sizeof(int), cmp_int);
    for (size_t i = 0; i < N; i++) CHECK(arr[i] == sorted[i]);

    int key = 7;
    int *hit = bsearch(&key, arr, N, sizeof(int), cmp_int);
    CHECK(hit != 0 && *hit == 7);
    key = 99;
    hit = bsearch(&key, arr, N, sizeof(int), cmp_int);
    CHECK(hit == 0);

    /* qsort + bsearch on strings (this is the binutils symbol-
     * table use case in miniature). */
    const char *strs[] = {
        "vorpal", "apple", "mango", "zeta", "bravo", "delta",
        "alpha", "yankee",
    };
    const size_t S = sizeof strs / sizeof strs[0];
    qsort(strs, S, sizeof(char *), cmp_str);
    CHECK(strcmp(strs[0], "alpha")  == 0);
    CHECK(strcmp(strs[1], "apple")  == 0);
    CHECK(strcmp(strs[S - 1], "zeta") == 0);

    const char *key_s = "mango";
    const char **hits = bsearch(&key_s, strs, S, sizeof(char *), cmp_str);
    CHECK(hits != 0 && strcmp(*hits, "mango") == 0);

    /* qsort edge cases — empty + single. */
    qsort(0, 0, sizeof(int), cmp_int);              /* no crash */
    int one[1] = { 42 };
    qsort(one, 1, sizeof(int), cmp_int);
    CHECK(one[0] == 42);

    /* getopt — simulate "doom -iwad DOOM1.WAD -warp 1 -nomusic -m4"
     * style invocation.  Optstring "i:w:nm:" => -i and -w take an
     * argument, -n doesn't, -m takes an argument. */
    char *gargv[] = {
        (char *)"doom",
        (char *)"-i", (char *)"DOOM1.WAD",
        (char *)"-w", (char *)"1",
        (char *)"-n",
        (char *)"-m4",
        (char *)"trailing.wad",
        (char *)0,
    };
    int gargc = 7;
    /* Reset getopt state. */
    optind = 1; opterr = 0;

    int c, count = 0;
    int saw_i = 0, saw_w = 0, saw_n = 0, saw_m = 0;
    while ((c = getopt(gargc, gargv, "i:w:nm:")) != -1) {
        count++;
        if (c == 'i') { saw_i = 1; CHECK(strcmp(optarg, "DOOM1.WAD") == 0); }
        else if (c == 'w') { saw_w = 1; CHECK(strcmp(optarg, "1") == 0); }
        else if (c == 'n') { saw_n = 1; }
        else if (c == 'm') { saw_m = 1; CHECK(strcmp(optarg, "4") == 0); }
        else { printf("  FAIL: unexpected getopt return %d\n", c); g_fail++; }
    }
    CHECK(count == 4);
    CHECK(saw_i && saw_w && saw_n && saw_m);
    /* optind should now point at "trailing.wad". */
    CHECK(optind == 7);
    CHECK(strcmp(gargv[optind], "trailing.wad") == 0);

    /* getopt — clustered short options "-abc" => -a -b -c. */
    char *cargv[] = {
        (char *)"prog", (char *)"-abc", (char *)"hello", (char *)0,
    };
    optind = 1; opterr = 0;
    int got_a = 0, got_b = 0, got_c = 0;
    char *c_arg = (char *)0;
    while ((c = getopt(3, cargv, "abc:")) != -1) {
        if (c == 'a') got_a = 1;
        else if (c == 'b') got_b = 1;
        else if (c == 'c') { got_c = 1; c_arg = optarg; }
    }
    CHECK(got_a && got_b && got_c);
    CHECK(c_arg && strcmp(c_arg, "hello") == 0);

    /* getopt — unknown option returns '?' and sets optopt. */
    char *uargv[] = {
        (char *)"prog", (char *)"-X", (char *)0,
    };
    optind = 1; opterr = 0; optopt = 0;
    c = getopt(2, uargv, "ab");
    CHECK(c == '?');
    CHECK(optopt == 'X');

    /* getopt — missing required arg, silent mode (":ab:") => returns
     * ':' instead of '?'. */
    char *margv[] = {
        (char *)"prog", (char *)"-b", (char *)0,
    };
    optind = 1; opterr = 0;
    c = getopt(2, margv, ":b:");
    CHECK(c == ':');
    CHECK(optopt == 'b');

    if (g_fail == 0) {
        printf("[stdlibtest] all checks passed\n");
        return 0;
    }
    printf("[stdlibtest] FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
