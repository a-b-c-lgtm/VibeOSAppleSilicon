/* userspace/mmaptest/mmaptest.c — chapter 90 mmap smoke test.
 *
 * Exercises both flavours that chapter 90 supports:
 *
 *   1. Anonymous MAP_PRIVATE | MAP_ANONYMOUS, PROT_READ | PROT_WRITE
 *      \u2014 the malloc-replacement use case.  Pages are allocated
 *      lazily on first touch, zero-filled.  We write a pattern
 *      and read it back to prove the lazy fault-in worked.
 *
 *   2. File-backed MAP_PRIVATE on /motd at PROT_READ \u2014 the
 *      "treat a file as a string" use case.  We open /motd, mmap
 *      one page, and verify the first few bytes match what a
 *      regular read() would return.  Doing this twice in a row
 *      proves the page cache (the second mmap should hit).
 *
 * On success we print exactly:
 *
 *   [mmap] anon OK
 *   [mmap] file OK
 *   [mmap] OK
 *
 * The boot-time test scaffolding looks for `[mmap] OK` to declare
 * pass.  Failures print `[mmap] FAIL <where>` and exit non-zero.
 *
 * Notes:
 *   - We rely on /motd being present in ramfs (it has been since
 *     chapter 8).  If the chapter-90 floor ever extends to
 *     /mnt/<file> mmaps, swap the file path here.
 *   - We deliberately do NOT call fork().  Chapter 90 floor:
 *     mmaps don't survive fork.  See chapter doc for the
 *     rationale and the fix sketch for chapter 91+.
 */

#include "../libc/syscall.h"

static size_t s_len(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int  s_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void say(const char *s) { write(1, s, s_len(s)); }

static int test_anon(void)
{
    /* 4 pages = 16 KiB.  Big enough that a stride loop touches
     * each page exactly once and proves they're independent. */
    void *p = mmap(0, 4 * 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { say("[mmap] FAIL anon-mmap\n"); return 1; }

    unsigned char *b = (unsigned char *)p;

    /* Read first \u2014 anonymous pages must zero-fill on lazy fault. */
    for (int pg = 0; pg < 4; pg++) {
        if (b[pg * 4096] != 0) {
            say("[mmap] FAIL anon-not-zero\n");
            return 1;
        }
    }

    /* Now write a pattern.  Each page gets a different byte so
     * cross-page contamination would show up as a mismatch. */
    for (int pg = 0; pg < 4; pg++) {
        for (int off = 0; off < 4096; off++) {
            b[pg * 4096 + off] = (unsigned char)(0xA0 + pg);
        }
    }

    /* Verify. */
    for (int pg = 0; pg < 4; pg++) {
        for (int off = 0; off < 4096; off++) {
            if (b[pg * 4096 + off] != (unsigned char)(0xA0 + pg)) {
                say("[mmap] FAIL anon-pattern\n");
                return 1;
            }
        }
    }

    if (munmap(p, 4 * 4096) != 0) {
        say("[mmap] FAIL anon-munmap\n");
        return 1;
    }

    say("[mmap] anon OK\n");
    return 0;
}

static int test_file(void)
{
    /* Reference read of /motd via the legacy read path. */
    int fd = open("/motd", 0);
    if (fd < 0) { say("[mmap] FAIL motd-open\n"); return 1; }

    char ref[64];
    long n = read(fd, ref, sizeof(ref));
    if (n <= 0) { say("[mmap] FAIL motd-read\n"); return 1; }
    if (n > (long)sizeof(ref)) n = (long)sizeof(ref);

    /* mmap the same file, page 0, RO. */
    void *p = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { say("[mmap] FAIL file-mmap\n"); return 1; }

    const char *m = (const char *)p;
    if (!s_eq(m, ref, (size_t)n)) {
        say("[mmap] FAIL file-mismatch\n");
        return 1;
    }

    /* Second mmap of the same file should be a page-cache HIT.
     * From userspace we can't observe the hit count directly,
     * but the second mmap must succeed and yield the same
     * payload bytes. */
    void *q = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (q == MAP_FAILED) { say("[mmap] FAIL file-mmap-2nd\n"); return 1; }
    if (q == p)          { say("[mmap] FAIL same-va\n"); return 1; }
    const char *m2 = (const char *)q;
    if (!s_eq(m2, ref, (size_t)n)) {
        say("[mmap] FAIL file-mismatch-2nd\n");
        return 1;
    }

    /* PROT_WRITE on the file mmap must be rejected by the kernel. */
    void *bad = mmap(0, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE, fd, 0);
    if (bad != MAP_FAILED) {
        say("[mmap] FAIL file-rw-allowed\n");
        return 1;
    }

    /* Clean up. */
    if (munmap(p, 4096) != 0) { say("[mmap] FAIL file-munmap\n");  return 1; }
    if (munmap(q, 4096) != 0) { say("[mmap] FAIL file-munmap2\n"); return 1; }
    close(fd);

    say("[mmap] file OK\n");
    return 0;
}

int main(void)
{
    say("[mmap] start\n");
    if (test_anon() != 0) return 1;
    if (test_file() != 0) return 1;
    say("[mmap] OK\n");
    return 0;
}
