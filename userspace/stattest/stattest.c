/* userspace/stattest/stattest.c -- chapter 117 smoke test.
 *
 * Drives the new POSIX shape:
 *   1. stat("/mnt/hello.txt") returns S_IFREG with a real size.
 *   2. stat("/data") returns S_IFDIR.
 *   3. stat("/") returns S_IFDIR.
 *   4. stat("/does/not/exist") returns -1 with errno=ENOENT.
 *   5. fstat() on a freshly-opened file matches stat() on the
 *      same path.
 *   6. opendir("/mnt") + readdir() yields at least one entry
 *      whose d_type is DT_REG.
 *   7. access("/bin/cat", R_OK) succeeds; access("/nonexistent",
 *      F_OK) fails with ENOENT.
 *
 * Output format is one line per assertion; the harness in
 * scripts/test_libc_stat.py greps for each `PASS:` / `FAIL:`.
 */

#include "../libc/sys/stat.h"
#include "../libc/dirent.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/syscall.h"

static int passes = 0, fails = 0;

static void ok(int cond, const char *msg)
{
    if (cond) { printf("[stattest] PASS: %s\n", msg); passes++; }
    else      { printf("[stattest] FAIL: %s\n", msg); fails++; }
}

int main(void)
{
    printf("[stattest] starting\n");

    /* (1) Regular file. */
    {
        struct stat st;
        int rc = stat("/mnt/hello.txt", &st);
        ok(rc == 0, "stat(/mnt/hello.txt) returns 0");
        ok(S_ISREG(st.st_mode),
           "stat(/mnt/hello.txt) reports a regular file");
        ok(st.st_size > 0,
           "stat(/mnt/hello.txt) reports a non-zero size");
    }

    /* (2) Directory. */
    {
        struct stat st;
        int rc = stat("/data", &st);
        ok(rc == 0, "stat(/data) returns 0");
        ok(S_ISDIR(st.st_mode), "stat(/data) reports a directory");
    }

    /* (3) Root. */
    {
        struct stat st;
        int rc = stat("/", &st);
        ok(rc == 0, "stat(/) returns 0");
        ok(S_ISDIR(st.st_mode), "stat(/) reports a directory");
    }

    /* (4) Missing path. */
    {
        struct stat st;
        errno = 0;
        int rc = stat("/does/not/exist", &st);
        ok(rc == -1, "stat(missing) returns -1");
        ok(errno == 2 /*ENOENT*/,
           "stat(missing) sets errno=ENOENT");
    }

    /* (5) fstat matches stat. */
    {
        struct stat sp, sf;
        int rc = stat("/mnt/hello.txt", &sp);
        ok(rc == 0, "stat(/mnt/hello.txt) for fstat compare");
        int fd = open("/mnt/hello.txt", 0);
        ok(fd >= 0, "open(/mnt/hello.txt) for fstat compare");
        int rc2 = fstat(fd, &sf);
        ok(rc2 == 0, "fstat returns 0");
        ok(sf.st_size == sp.st_size,
           "fstat size matches stat size");
        ok(S_ISREG(sf.st_mode),
           "fstat reports a regular file");
        close(fd);
    }

    /* (6) opendir / readdir. */
    {
        DIR *d = opendir("/mnt");
        ok(d != (DIR *)0, "opendir(/mnt) succeeds");
        int saw_file = 0;
        struct dirent *de;
        while ((de = readdir(d)) != (struct dirent *)0) {
            if (de->d_type == DT_REG) { saw_file = 1; break; }
        }
        ok(saw_file, "readdir(/mnt) yields at least one DT_REG");
        closedir(d);
    }

    /* (7) access. */
    {
        int rc = access("/bin/cat", R_OK);
        ok(rc == 0, "access(/bin/cat, R_OK) succeeds");
        errno = 0;
        rc = access("/nonexistent/path", F_OK);
        ok(rc == -1, "access(missing, F_OK) returns -1");
        ok(errno == 2 /*ENOENT*/,
           "access(missing, F_OK) sets errno=ENOENT");
    }

    printf("[stattest] done  %d PASS / %d FAIL\n", passes, fails);
    if (fails == 0) printf("[stattest] ALL PASS\n");
    return fails ? 1 : 0;
}
