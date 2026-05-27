/* userspace/stdiotest/stdiotest.c — chapter 150 smoke test.
 *
 * Drives the new FILE * layer end-to-end:
 *
 *   T1: fopen("/mnt/poem.txt","r") + fread loop in 13-byte chunks,
 *       sum bytes, count newlines.
 *   T2: fopen("/data/stdio_out","w") + fwrite of a known pattern;
 *       fclose drives the buffer flush.
 *   T3: re-open + fread back the pattern, compare byte-for-byte.
 *   T4: fseek(SEEK_SET=100), ftell == 100, read 4 bytes and
 *       confirm they match the pattern at that offset.
 *   T5: fseek(SEEK_END=0) + ftell == filesize; rewind, ftell==0.
 *   T6: fopen("/does/not/exist","r") → NULL, errno == ENOENT.
 *   T7: fputc / fgetc round trip through /data/stdio_chars.
 *   T8: fprintf(stderr, ...) emits to fd 2 (visible in serial).
 *
 * Output: one tagged line per assertion; scripts/test_libc_stdio.py
 * greps for PASS/FAIL.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/printf.h"
#include "../libc/stdio.h"

#define PATTERN_LEN 200
static unsigned char g_pattern[PATTERN_LEN];

static void build_pattern(void)
{
    for (int i = 0; i < PATTERN_LEN; i++) {
        g_pattern[i] = (unsigned char)((i * 17 + 31) & 0xff);
    }
}

static int t1_read_poem(void)
{
    FILE *f = fopen("/mnt/poem.txt", "r");
    if (!f) {
        printf("[stdiotest] T1 FAIL fopen errno=%d\n", errno);
        return 1;
    }
    unsigned long sum = 0;
    int nls = 0;
    unsigned char buf[13];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            sum += buf[i];
            if (buf[i] == '\n') nls++;
        }
    }
    int eof = feof(f);
    int err = ferror(f);
    fclose(f);
    printf("[stdiotest] T1 sum=%lu nls=%d eof=%d err=%d\n",
           sum, nls, eof, err);
    if (sum == 0 || nls == 0 || !eof || err) {
        printf("[stdiotest] T1 FAIL\n");
        return 1;
    }
    printf("[stdiotest] T1 PASS\n");
    return 0;
}

static int t2_write_pattern(void)
{
    FILE *f = fopen("/data/stdio_out", "w");
    if (!f) {
        printf("[stdiotest] T2 FAIL fopen errno=%d\n", errno);
        return 1;
    }
    /* Write the pattern 30 times (6000 bytes -- crosses the
     * 4096-byte buffer boundary so we exercise the buffered
     * + direct paths). */
    for (int rep = 0; rep < 30; rep++) {
        size_t w = fwrite(g_pattern, 1, PATTERN_LEN, f);
        if (w != PATTERN_LEN) {
            printf("[stdiotest] T2 FAIL fwrite rep=%d w=%lu errno=%d\n",
                   rep, (unsigned long)w, errno);
            fclose(f);
            return 1;
        }
    }
    if (fclose(f) != 0) {
        printf("[stdiotest] T2 FAIL fclose errno=%d\n", errno);
        return 1;
    }
    printf("[stdiotest] T2 PASS\n");
    return 0;
}

static int t3_read_back(void)
{
    FILE *f = fopen("/data/stdio_out", "r");
    if (!f) {
        printf("[stdiotest] T3 FAIL fopen errno=%d\n", errno);
        return 1;
    }
    unsigned char buf[PATTERN_LEN];
    int bad = 0;
    int reps = 0;
    size_t n;
    while ((n = fread(buf, 1, PATTERN_LEN, f)) > 0) {
        if (n != PATTERN_LEN) {
            printf("[stdiotest] T3 partial short=%lu\n", (unsigned long)n);
            bad = 1;
            break;
        }
        for (size_t i = 0; i < PATTERN_LEN; i++) {
            if (buf[i] != g_pattern[i]) { bad = 1; break; }
        }
        reps++;
        if (bad) break;
    }
    fclose(f);
    if (bad || reps != 30) {
        printf("[stdiotest] T3 FAIL bad=%d reps=%d\n", bad, reps);
        return 1;
    }
    printf("[stdiotest] T3 PASS reps=%d\n", reps);
    return 0;
}

static int t4_seek_mid(void)
{
    FILE *f = fopen("/data/stdio_out", "r");
    if (!f) {
        printf("[stdiotest] T4 FAIL fopen errno=%d\n", errno);
        return 1;
    }
    if (fseek(f, 100, SEEK_SET) != 0) {
        printf("[stdiotest] T4 FAIL fseek errno=%d\n", errno);
        fclose(f);
        return 1;
    }
    long pos = ftell(f);
    if (pos != 100) {
        printf("[stdiotest] T4 FAIL ftell=%ld\n", pos);
        fclose(f);
        return 1;
    }
    unsigned char four[4];
    size_t n = fread(four, 1, 4, f);
    fclose(f);
    if (n != 4) {
        printf("[stdiotest] T4 FAIL fread n=%lu\n", (unsigned long)n);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        if (four[i] != g_pattern[100 + i]) {
            printf("[stdiotest] T4 FAIL mismatch i=%d got=%u exp=%u\n",
                   i, four[i], g_pattern[100 + i]);
            return 1;
        }
    }
    printf("[stdiotest] T4 PASS pos=%ld\n", pos);
    return 0;
}

static int t5_seek_end(void)
{
    FILE *f = fopen("/data/stdio_out", "r");
    if (!f) {
        printf("[stdiotest] T5 FAIL fopen errno=%d\n", errno);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        printf("[stdiotest] T5 FAIL fseek-end errno=%d\n", errno);
        fclose(f);
        return 1;
    }
    long end = ftell(f);
    rewind(f);
    long zero = ftell(f);
    fclose(f);
    if (end != PATTERN_LEN * 30 || zero != 0) {
        printf("[stdiotest] T5 FAIL end=%ld zero=%ld\n", end, zero);
        return 1;
    }
    printf("[stdiotest] T5 PASS end=%ld\n", end);
    return 0;
}

static int t6_missing(void)
{
    errno = 0;
    FILE *f = fopen("/no/such/path/xyz", "r");
    if (f) {
        printf("[stdiotest] T6 FAIL got non-NULL\n");
        fclose(f);
        return 1;
    }
    int e = errno;
    printf("[stdiotest] T6 errno=%d\n", e);
    /* ENOENT == 2 per Linux/our convention */
    if (e != 2) {
        printf("[stdiotest] T6 FAIL want errno=2 got=%d\n", e);
        return 1;
    }
    printf("[stdiotest] T6 PASS\n");
    return 0;
}

static int t7_char_io(void)
{
    FILE *f = fopen("/data/stdio_chars", "w");
    if (!f) {
        printf("[stdiotest] T7 FAIL fopen-w errno=%d\n", errno);
        return 1;
    }
    const char *msg = "FILE-roundtrip-via-fputc\n";
    for (const char *p = msg; *p; p++) {
        if (fputc(*p, f) == EOF) {
            printf("[stdiotest] T7 FAIL fputc errno=%d\n", errno);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    f = fopen("/data/stdio_chars", "r");
    if (!f) {
        printf("[stdiotest] T7 FAIL fopen-r errno=%d\n", errno);
        return 1;
    }
    char got[64];
    int i = 0;
    int c;
    while ((c = fgetc(f)) != EOF && i < 63) got[i++] = (char)c;
    got[i] = '\0';
    fclose(f);

    int ok = 1;
    for (int j = 0; msg[j] || got[j]; j++) {
        if (msg[j] != got[j]) { ok = 0; break; }
    }
    if (!ok) {
        printf("[stdiotest] T7 FAIL got=%s\n", got);
        return 1;
    }
    printf("[stdiotest] T7 PASS\n");
    return 0;
}

static int t8_stderr(void)
{
    /* fprintf(stderr,...) must produce visible serial output;
     * it is unbuffered so there is no flush dance. */
    int n = fprintf(stderr, "[stdiotest] T8 stderr printf n=%d\n", 42);
    if (n <= 0) {
        printf("[stdiotest] T8 FAIL n=%d\n", n);
        return 1;
    }
    printf("[stdiotest] T8 PASS\n");
    return 0;
}

int main(void)
{
    printf("[stdiotest] starting\n");
    build_pattern();

    int fails = 0;
    fails += t1_read_poem();
    fails += t2_write_pattern();
    fails += t3_read_back();
    fails += t4_seek_mid();
    fails += t5_seek_end();
    fails += t6_missing();
    fails += t7_char_io();
    fails += t8_stderr();

    if (fails == 0) printf("[stdiotest] ALL PASS\n");
    else            printf("[stdiotest] FAILS=%d\n", fails);
    return fails;
}
