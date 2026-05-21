/*
 * userspace/cookies/cookies.c -- chapter 110 cookie jar inspector.
 *
 * Reads the on-disk cookie jar that browser and httpget share at
 * /data/cookies/<host>.  This is the user-visible front for the
 * cookie machinery -- the equivalent of "find ~/Library/Cookies"
 * on a real OS, except plain text so a `cat` shows the same data.
 *
 * Usage:
 *
 *   cookies                # list every host that has cookies,
 *                          # then dump each jar.
 *
 *   cookies <host>         # dump just one host's jar.
 *
 *   cookies clear          # unlink every file under /data/cookies,
 *                          # effectively a log-out-of-everything.
 *
 *   cookies clear <host>   # remove just one host's jar.
 *
 * Output format mirrors the on-disk format (name<TAB>value<TAB>
 * expires<TAB>path), with a header line per host.  Expired
 * cookies are still listed but marked "(expired)" so the user
 * can see them before they fall off on the next write.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"

#define COOKIE_DIR "/data/cookies"

static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Print one jar file -- read it whole into memory and stream out
 * with light reformatting (TABs become spaces for readability,
 * expiry timestamps get an "(expired)" tag).  Returns 0 on
 * success, -1 if the file couldn't be opened (treated as "no
 * cookies for this host"). */
static int dump_one(const char *host, time_t now)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", COOKIE_DIR, host);

    int fd = open(path, 0 /* O_RDONLY */);
    if (fd < 0) return -1;

    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); printf("cookies: oom\n"); return -1; }
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)malloc(ncap);
            if (!nb) { free(buf); close(fd); printf("cookies: oom\n"); return -1; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf); buf = nb; cap = ncap;
        }
        long n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);

    printf("# %s (%lu bytes)\n", host, (unsigned long)len);

    size_t i = 0;
    while (i < len) {
        /* Find end of line. */
        size_t le = i;
        while (le < len && buf[le] != '\n') le++;

        /* Split on TAB into up to 4 fields. */
        size_t f[4]; int nf = 0;
        for (size_t k = i; k < le && nf < 3; k++) {
            if (buf[k] == '\t') f[nf++] = k;
        }
        if (nf == 3) {
            f[3] = le;
            /* Parse expiry to decide if expired. */
            long long exp = 0;
            int  neg = 0;
            size_t es = f[1] + 1;
            if (es < f[2] && buf[es] == '-') { neg = 1; es++; }
            for (size_t k = es; k < f[2]; k++) {
                if (buf[k] < '0' || buf[k] > '9') { exp = 0; break; }
                exp = exp * 10 + (buf[k] - '0');
            }
            if (neg) exp = -exp;

            /* name = value (path) [(expired)] */
            write(1, "  ", 2);
            write(1, buf + i, f[0] - i);
            write(1, " = ", 3);
            write(1, buf + f[0] + 1, f[1] - f[0] - 1);
            write(1, "  [path=", 8);
            write(1, buf + f[2] + 1, f[3] - f[2] - 1);
            write(1, "]", 1);
            if (exp != 0 && exp <= (long long)now) {
                write(1, " (expired)", 10);
            } else if (exp == 0) {
                write(1, " (session)", 10);
            }
            write(1, "\n", 1);
        }
        i = le + 1;
    }
    free(buf);
    return 0;
}

/* Walk /data/cookies/ via listdir_at and call dump_one() for
 * every file there.  Returns the number of jars dumped. */
static int dump_all(time_t now)
{
    int dumped = 0;
    for (int idx = 0; ; idx++) {
        char name[128];
        unsigned int sz = 0, ty = 0;
        long rc = listdir_at(COOKIE_DIR, idx, name, sizeof(name), &sz, &ty);
        if (rc < 0) break;
        if (ty != LISTDIR_TYPE_FILE) continue;
        if (dump_one(name, now) == 0) dumped++;
    }
    return dumped;
}

/* "clear" subcommand: unlink every regular file in /data/cookies/.
 * If a host argument is given, only that one file is removed. */
static int do_clear(const char *one_host)
{
    if (one_host && one_host[0]) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", COOKIE_DIR, one_host);
        int rc = unlink(path);
        if (rc < 0) {
            printf("cookies: cannot remove %s (errno=%d)\n", path, -rc);
            return 1;
        }
        printf("cookies: removed %s\n", path);
        return 0;
    }
    int removed = 0;
    for (;;) {
        /* listdir_at indices shift after unlink, so always read
         * index 0 until the directory is empty. */
        char name[128];
        unsigned int sz = 0, ty = 0;
        long rc = listdir_at(COOKIE_DIR, 0, name, sizeof(name), &sz, &ty);
        if (rc < 0) break;
        if (ty != LISTDIR_TYPE_FILE) {
            /* Non-file entry: skip by walking ahead one and giving up. */
            break;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", COOKIE_DIR, name);
        int urc = unlink(path);
        if (urc < 0) {
            printf("cookies: cannot remove %s (errno=%d)\n", path, -urc);
            return 1;
        }
        removed++;
    }
    printf("cookies: cleared %d jar(s)\n", removed);
    return 0;
}

static void usage(void)
{
    printf("usage: cookies                # list all\n");
    printf("       cookies <host>         # show one host\n");
    printf("       cookies clear          # remove every jar\n");
    printf("       cookies clear <host>   # remove one host\n");
}

int main(int argc, char **argv)
{
    time_t now = time(0);

    if (argc >= 2 && s_eq(argv[1], "clear")) {
        const char *one = (argc >= 3) ? argv[2] : 0;
        return do_clear(one);
    }
    if (argc >= 2 && (s_eq(argv[1], "-h") || s_eq(argv[1], "--help"))) {
        usage();
        return 0;
    }
    if (argc >= 2) {
        if (dump_one(argv[1], now) < 0) {
            printf("cookies: no jar for '%s'\n", argv[1]);
            return 1;
        }
        return 0;
    }

    int n = dump_all(now);
    if (n == 0) printf("cookies: no jars under %s\n", COOKIE_DIR);
    return 0;
}
