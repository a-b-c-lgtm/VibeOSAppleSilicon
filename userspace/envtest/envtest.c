/* userspace/envtest/envtest.c -- chapter 151 env arena smoke test.
 *
 * Drives the new POSIX-shaped env API:
 *
 *   T1  getenv("PATH") returns non-NULL (init seeded it to /bin).
 *   T2  getenv("DOES_NOT_EXIST") returns NULL.
 *   T3  setenv("FOO","bar",1) -> 0; getenv("FOO") == "bar".
 *   T4  setenv("FOO","baz",0) is a no-op (overwrite=0); FOO=="bar".
 *   T5  setenv("FOO","baz",1) overwrites; FOO=="baz".
 *   T6  unsetenv("FOO") -> 0; getenv("FOO") == NULL.
 *   T7  putenv("MUTEX=42"); getenv("MUTEX")=="42".
 *   T8  environ[] iteration finds the live entries.
 *   T9  setenv with '=' in the name rejected with EINVAL.
 *   T10 kernel-side __sys_getenv sees the same value (write-through).
 *
 * Output: one tagged line per assertion; scripts/test_libc_env.py
 * greps for PASS/FAIL.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/printf.h"
#include "../libc/env.h"

static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int main(void)
{
    printf("[envtest] starting\n");
    int fails = 0;

    /* T1: PATH was seeded by init. */
    {
        const char *p = getenv("PATH");
        if (p && p[0]) {
            printf("[envtest] T1 PASS PATH=%s\n", p);
        } else {
            printf("[envtest] T1 FAIL PATH=%s\n", p ? p : "(null)");
            fails++;
        }
    }

    /* T2: missing key. */
    {
        const char *p = getenv("ZZZ_NEVER_SET");
        if (!p) printf("[envtest] T2 PASS\n");
        else    { printf("[envtest] T2 FAIL p=%s\n", p); fails++; }
    }

    /* T3: setenv + getenv roundtrip. */
    {
        errno = 0;
        int rc = setenv("FOO", "bar", 1);
        const char *v = getenv("FOO");
        if (rc == 0 && streq(v, "bar")) {
            printf("[envtest] T3 PASS\n");
        } else {
            printf("[envtest] T3 FAIL rc=%d v=%s errno=%d\n",
                   rc, v ? v : "(null)", errno);
            fails++;
        }
    }

    /* T4: overwrite=0 leaves the existing value. */
    {
        int rc = setenv("FOO", "baz", 0);
        const char *v = getenv("FOO");
        if (rc == 0 && streq(v, "bar")) {
            printf("[envtest] T4 PASS\n");
        } else {
            printf("[envtest] T4 FAIL rc=%d v=%s\n", rc, v ? v : "(null)");
            fails++;
        }
    }

    /* T5: overwrite=1 replaces. */
    {
        int rc = setenv("FOO", "baz", 1);
        const char *v = getenv("FOO");
        if (rc == 0 && streq(v, "baz")) {
            printf("[envtest] T5 PASS\n");
        } else {
            printf("[envtest] T5 FAIL rc=%d v=%s\n", rc, v ? v : "(null)");
            fails++;
        }
    }

    /* T6: unsetenv removes. */
    {
        int rc = unsetenv("FOO");
        const char *v = getenv("FOO");
        if (rc == 0 && v == (const char *)0) {
            printf("[envtest] T6 PASS\n");
        } else {
            printf("[envtest] T6 FAIL rc=%d v=%s\n", rc, v ? v : "(null)");
            fails++;
        }
    }

    /* T7: putenv. */
    {
        static char kv[] = "MUTEX=42";  /* writable, not stack */
        int rc = putenv(kv);
        const char *v = getenv("MUTEX");
        if (rc == 0 && streq(v, "42")) {
            printf("[envtest] T7 PASS\n");
        } else {
            printf("[envtest] T7 FAIL rc=%d v=%s\n", rc, v ? v : "(null)");
            fails++;
        }
    }

    /* T8: walk environ[].  Expect PATH and MUTEX to be present. */
    {
        int saw_path = 0, saw_mutex = 0, n = 0;
        for (char **p = environ; *p; p++) {
            n++;
            const char *e = *p;
            /* "PATH=" prefix */
            if (e[0] == 'P' && e[1] == 'A' && e[2] == 'T' &&
                e[3] == 'H' && e[4] == '=') saw_path = 1;
            if (e[0] == 'M' && e[1] == 'U' && e[2] == 'T' &&
                e[3] == 'E' && e[4] == 'X' && e[5] == '=') saw_mutex = 1;
        }
        if (saw_path && saw_mutex && n >= 2) {
            printf("[envtest] T8 PASS n=%d\n", n);
        } else {
            printf("[envtest] T8 FAIL n=%d path=%d mutex=%d\n",
                   n, saw_path, saw_mutex);
            fails++;
        }
    }

    /* T9: setenv with '=' in key is EINVAL. */
    {
        errno = 0;
        int rc = setenv("BAD=NAME", "x", 1);
        if (rc < 0 && errno == EINVAL) {
            printf("[envtest] T9 PASS\n");
        } else {
            printf("[envtest] T9 FAIL rc=%d errno=%d\n", rc, errno);
            fails++;
        }
    }

    /* T10: kernel-side reflects the write-through. */
    {
        char tmp[32];
        long n = __sys_getenv("MUTEX", tmp, sizeof(tmp));
        if (n > 0 && streq(tmp, "42")) {
            printf("[envtest] T10 PASS\n");
        } else {
            printf("[envtest] T10 FAIL n=%ld tmp=%s\n", n, tmp);
            fails++;
        }
    }

    if (fails == 0) printf("[envtest] ALL PASS\n");
    else            printf("[envtest] FAILS=%d\n", fails);
    return fails;
}
