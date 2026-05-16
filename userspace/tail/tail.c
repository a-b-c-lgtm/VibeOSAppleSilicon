/* userspace/tail/tail.c — print last N lines of a file (default 10).
 *
 *   tail PATH         # last 10 lines
 *   tail -N PATH      # last N lines
 *
 * Strategy: the file system is read-only and small.  We seek to
 * the end (well — we don't have lseek; we just stream-read and
 * keep a circular buffer of the last N line-start offsets, then
 * re-read from the earliest one).  Simple, O(file size) work,
 * O(N) memory.  No -f option (would require a growing file).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

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
    int fd = open(path, 0);
    if (fd < 0) {
        printf("tail: cannot open %s: errno=%d\n", path, -fd);
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
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                long after = pos + i + 1;
                ring[rhead] = after;
                rhead = (rhead + 1) % (MAX_N + 1);
                if (rcount < MAX_N + 1) rcount++;
                total_lines++;
                ends_with_nl = 1;
            } else {
                ends_with_nl = 0;
            }
        }
        pos += n;
        file_size = pos;
    }
    close(fd);
    if (n < 0) {
        printf("tail: read failed: errno=%d\n", (int)-n);
        return 2;
    }

    long effective_lines = total_lines + (ends_with_nl ? 0 : 1);
    if (effective_lines == 0) return 0;

    long start_off;
    if (effective_lines <= count) {
        start_off = 0;
    } else {
        long need = count;       /* how many \n-anchored offsets back from rhead */
        int  slot = (rhead - need + (MAX_N + 1) * 2) % (MAX_N + 1);
        /* The "start of the kept tail" is the offset AFTER the
         * (count)-th-from-last newline — i.e. at slot (rhead - count - 1)
         * + 1.  But ring[i] already stores the offset AFTER newline i,
         * so the line that follows newline (rhead-count-1) starts at
         * ring[(rhead-count-1)] which equals ring[(rhead-count-1+N+1)%(N+1)]. */
        slot = (rhead - need - 1 + (MAX_N + 1) * 2) % (MAX_N + 1);
        if (need < total_lines && (count + 1) <= rcount) {
            start_off = ring[slot];
        } else {
            start_off = ring[(rhead - rcount + (MAX_N + 1)) % (MAX_N + 1)];
        }
    }

    /* Pass 2: re-read the file, skip until start_off, then echo. */
    fd = open(path, 0);
    if (fd < 0) {
        printf("tail: cannot reopen %s: errno=%d\n", path, -fd);
        return 1;
    }
    long skipped = 0;
    while (skipped < start_off) {
        long want = start_off - skipped;
        if ((unsigned long)want > sizeof(buf)) want = (long)sizeof(buf);
        long got = read(fd, buf, (size_t)want);
        if (got <= 0) break;
        skipped += got;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);

    if (file_size > 0 && !ends_with_nl)
        write(1, "\n", 1);
    return 0;
}
