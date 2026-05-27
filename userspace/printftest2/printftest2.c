/* userspace/printftest2/printftest2.c — chapter 170 regression.
 *
 * Exercises the format-string surface added in 128f:
 *
 *   - %o (octal)
 *   - precision .N on integer + %s
 *   - flags + (force sign) and ' ' (space sign) on %d
 *   - flag # on %x, %X, %o
 *   - %n (running-count store)
 *   - %* width / .* precision (consume from va_list)
 *   - sscanf %d %i %u %o %x %s %c %[set] %n
 *
 * Uses snprintf into a fixed buffer for output testing so we can
 * compare against exact expected strings; uses sscanf against
 * literal strings for input testing.  Same CHECK pattern as
 * strtest / stdlibtest.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/string.h"
#include "../libc/scanf.h"

static int g_fail;

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_fail++;                                                \
        }                                                            \
    } while (0)

#define CHECK_STR(buf, expected)                                          \
    do {                                                                  \
        if (strcmp((buf), (expected)) != 0) {                             \
            printf("  FAIL %s:%d: got=\"%s\" want=\"%s\"\n",              \
                   __FILE__, __LINE__, (buf), (expected));                \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[printftest2] starting\n");

    char b[64];

    /* --- Octal -------------------------------------------------- */
    snprintf(b, sizeof b, "%o", 0);            CHECK_STR(b, "0");
    snprintf(b, sizeof b, "%o", 8);            CHECK_STR(b, "10");
    snprintf(b, sizeof b, "%o", 0777);         CHECK_STR(b, "777");
    snprintf(b, sizeof b, "%#o", 0777);        CHECK_STR(b, "0777");
    snprintf(b, sizeof b, "%#o", 0);           CHECK_STR(b, "0");  /* alt form harmless on 0 */

    /* --- Integer precision ------------------------------------- */
    snprintf(b, sizeof b, "%.5d", 42);         CHECK_STR(b, "00042");
    snprintf(b, sizeof b, "%.0d", 0);          CHECK_STR(b, "");    /* C99: nothing */
    snprintf(b, sizeof b, "%.3x", 0xa);        CHECK_STR(b, "00a");
    snprintf(b, sizeof b, "%8.5d", 42);        CHECK_STR(b, "   00042");
    snprintf(b, sizeof b, "%-8.5d|", 42);      CHECK_STR(b, "00042   |");
    /* '0' flag is overridden by explicit precision. */
    snprintf(b, sizeof b, "%08.3d", 42);       CHECK_STR(b, "     042");

    /* --- Plus / space ------------------------------------------ */
    snprintf(b, sizeof b, "%+d", 7);           CHECK_STR(b, "+7");
    snprintf(b, sizeof b, "%+d", -7);          CHECK_STR(b, "-7");
    snprintf(b, sizeof b, "% d", 7);           CHECK_STR(b, " 7");
    snprintf(b, sizeof b, "% d", -7);          CHECK_STR(b, "-7");
    /* '+' overrides ' '. */
    snprintf(b, sizeof b, "%+ d", 7);          CHECK_STR(b, "+7");
    /* Sign + zero-pad: sign comes first. */
    snprintf(b, sizeof b, "%+06d", 42);        CHECK_STR(b, "+00042");

    /* --- Alt form on hex --------------------------------------- */
    snprintf(b, sizeof b, "%#x", 0xff);        CHECK_STR(b, "0xff");
    snprintf(b, sizeof b, "%#X", 0xff);        CHECK_STR(b, "0XFF");
    snprintf(b, sizeof b, "%#x", 0);           CHECK_STR(b, "0");   /* C99: no prefix on zero */
    snprintf(b, sizeof b, "%#08x", 0xab);      CHECK_STR(b, "0x0000ab");

    /* --- String precision --------------------------------------- */
    snprintf(b, sizeof b, "%.3s", "hello");    CHECK_STR(b, "hel");
    snprintf(b, sizeof b, "%-6.3s|", "hello"); CHECK_STR(b, "hel   |");

    /* --- Variable width / precision --------------------------- */
    snprintf(b, sizeof b, "%*d", 6, 42);       CHECK_STR(b, "    42");
    snprintf(b, sizeof b, "%.*s", 4, "hello world"); CHECK_STR(b, "hell");
    /* Negative width via '*' => left-justify. */
    snprintf(b, sizeof b, "%*d|", -6, 42);     CHECK_STR(b, "42    |");

    /* --- %n: store running count ------------------------------- */
    int n_at = -1;
    snprintf(b, sizeof b, "abc%nXYZ", &n_at);
    CHECK(n_at == 3);
    CHECK_STR(b, "abcXYZ");

    /* --- Pre-existing behaviour still works -------------------- */
    snprintf(b, sizeof b, "hello %s %d", "world", 7);
    CHECK_STR(b, "hello world 7");
    snprintf(b, sizeof b, "%05d", -7);
    CHECK_STR(b, "-0007");
    snprintf(b, sizeof b, "%-5d|", 42);
    CHECK_STR(b, "42   |");

    /* =====================================================
     * sscanf
     * ===================================================== */

    /* Simple %d. */
    int a = -1; int n = sscanf("42", "%d", &a);
    CHECK(n == 1 && a == 42);

    /* Two ints, whitespace-delimited. */
    int x = 0, y = 0;
    n = sscanf("  100 -7", "%d %d", &x, &y);
    CHECK(n == 2 && x == 100 && y == -7);

    /* %u, %x, %o, %i. */
    unsigned u = 0;
    n = sscanf("12345", "%u", &u);   CHECK(n == 1 && u == 12345u);
    n = sscanf("0xdead", "%x", &u);  CHECK(n == 1 && u == 0xdeadu);
    n = sscanf("0777", "%o", &u);    CHECK(n == 1 && u == 0777u);
    /* %i: base auto-detect. */
    int iv = 0;
    n = sscanf("0xff",  "%i", &iv);  CHECK(n == 1 && iv == 0xff);
    n = sscanf("0777",  "%i", &iv);  CHECK(n == 1 && iv == 0777);
    n = sscanf("42",    "%i", &iv);  CHECK(n == 1 && iv == 42);

    /* %s: stops at whitespace. */
    char buf[32];
    n = sscanf("  hello world", "%s", buf);
    CHECK(n == 1 && strcmp(buf, "hello") == 0);

    /* %s with width: caps stored length. */
    n = sscanf("abcdefgh", "%4s", buf);
    CHECK(n == 1 && strcmp(buf, "abcd") == 0);

    /* %c: reads exactly one char (no skip-ws). */
    char ch = 'Z';
    n = sscanf(" X", "%c", &ch);
    CHECK(n == 1 && ch == ' ');

    /* Assignment suppression: %*d swallows a token without
     * storing.  Counts toward conversions performed but NOT
     * toward the return value. */
    a = -1;
    n = sscanf("100 200", "%*d %d", &a);
    CHECK(n == 1 && a == 200);

    /* Scanset: %[a-z] (but we don't support ranges; use literal). */
    n = sscanf("abc123", "%[abc]", buf);
    CHECK(n == 1 && strcmp(buf, "abc") == 0);
    /* Inverted scanset: %[^d] - stop at literal 'd'. */
    n = sscanf("foobardo", "%[^d]", buf);
    CHECK(n == 1 && strcmp(buf, "foobar") == 0);

    /* %n in scanf: store running input position. */
    int pos = -1;
    n = sscanf("abc 42", "%*s%n %d", &pos, &a);
    CHECK(n == 1 && a == 42 && pos == 3);

    /* Literal chars in fmt must match. */
    int hh = 0, mm = 0, ss = 0;
    n = sscanf("12:34:56", "%d:%d:%d", &hh, &mm, &ss);
    CHECK(n == 3 && hh == 12 && mm == 34 && ss == 56);

    /* sscanf returns EOF on empty input. */
    n = sscanf("", "%d", &a);
    CHECK(n == EOF);

    /* sscanf returns the count of successful conversions when
     * input runs out partway through. */
    n = sscanf("42", "%d %d", &x, &y);
    CHECK(n == 1);

    if (g_fail == 0) {
        printf("[printftest2] all checks passed\n");
        return 0;
    }
    printf("[printftest2] FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
