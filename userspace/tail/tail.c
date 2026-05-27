/* userspace/tail/tail.c — print last N lines of a file (default 10).
 *
 *   tail PATH         # last 10 lines
 *   tail -N PATH      # last N lines
 *
 * Strategy: stream-read the whole file once, keeping a circular
 * buffer of the last N line-start offsets, then fseek back to
 * the earliest one and stream from there.  O(file size) work,
 * O(N) memory.  No -f option (would require a growing file).
 *
 * Chapter 152: drives the FILE * layer and uses fseek (chapter
 * 116b's new SYS_LSEEK) to jump back instead of the old
 * read-and-discard loop.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/stdio.h"

#define MAX_N 64

static int parse_int(const char *s, long *out)
{
    long v = 0;
    if (!s || !*s) return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    long count = 10;
    const char *path = 0;

    int ai = 1;
    if (ai < argc && argv[ai] && argv[ai][0] == '-') {
        if (parse_int(argv[ai] + 1, &count) != 0) {
            printf("tail: bad count %s\n", argv[ai]);
            return 1;
        }
        ai++;
    }
    if (ai < argc) path = argv[ai];

    if (!path) {
        printf("usage: tail [-N] PATH\n");
        return 1;
    }
    if (count <= 0) return 0;
    if (count > MAX_N) count = MAX_N;

    /* Pass 1: count lines + remember the byte offset of the
     * (count+1)-th newline from the end.  We track a ring of
     * line-end byte positions; once we know totals we know where
     * the desired tail starts. */
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("tail: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }

    long ring[MAX_N + 1];        /* file offset just AFTER each '\n' */
    int  rhead = 0;              /* next slot to write */
    int  rcount = 0;             /* number of valid entries (<= count+1) */
    long pos = 0;
    long total_lines = 0;
    int  ends_with_nl = 0;
    long file_size = 0;

    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                long after = pos + (long)i + 1;
                ring[rhead] = after;
                rhead = (rhead + 1) % (MAX_N + 1);
                if (rcount < MAX_N + 1) rcount++;
                total_lines++;
                ends_with_nl = 1;
            } else {
                ends_with_nl = 0;
            }
        }
        pos += (long)n;
        file_size = pos;
    }
    if (ferror(f)) {
        printf("tail: read failed: %s\n", strerror(errno));
        fclose(f);
        return 2;
    }

    long effective_lines = total_lines + (ends_with_nl ? 0 : 1);
    if (effective_lines == 0) { fclose(f); return 0; }

    long start_off;
    if (effective_lines <= count) {
        start_off = 0;
    } else {
        long need = count;       /* how many \n-anchored offsets back from rhead */
        int  slot = (rhead - need - 1 + (MAX_N + 1) * 2) % (MAX_N + 1);
        if (need < total_lines && (count + 1) <= rcount) {
            start_off = ring[slot];
        } else {
            start_off = ring[(rhead - rcount + (MAX_N + 1)) % (MAX_N + 1)];
        }
    }

    /* Pass 2: fseek to start_off, stream the rest. */
    if (fseek(f, start_off, SEEK_SET) != 0) {
        printf("tail: fseek %ld failed: %s\n", start_off, strerror(errno));
        fclose(f);
        return 2;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);

    if (file_size > 0 && !ends_with_nl)
        fputc('\n', stdout);
    return 0;
}
