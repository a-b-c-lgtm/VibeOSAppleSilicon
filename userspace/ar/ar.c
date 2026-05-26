/*
 * userspace/ar/ar.c — minimal /bin/ar archive tool.
 *
 * Produces SysV-style ar archives in the simplest possible
 * shape:
 *
 *   "!<arch>\n"
 *   per-member header (60 bytes ASCII):
 *      name padded to 16 with spaces, '/' terminator if short
 *      mtime padded to 12 spaces ("0" used here)
 *      uid   padded to 6 spaces ("0")
 *      gid   padded to 6 spaces ("0")
 *      mode  padded to 8 spaces ("100644")
 *      size  padded to 10 spaces (decimal size of member bytes)
 *      end-marker "`\n" (backtick + newline)
 *   member bytes
 *   pad to 2-byte boundary with '\n' if needed
 *
 * Long file names use BSD's "#1/NN" extension where NN is the
 * length of the real name which is then written at the start
 * of the member payload.  We use this for any name > 15 bytes.
 *
 * Usage:
 *   ar rc out.a file1.o file2.o ...   — create / replace
 *   ar t  out.a                       — list contents
 *
 * Out of scope: random-access symbol index ("/" SYMDEF member),
 * incremental update, member deletion.  Our /bin/ld doesn't
 * consume archives yet so the lack of an index is fine.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/sys/stat.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t s_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t wrote = 0;
    while (wrote < n) {
        long w = write(fd, b + wrote, n - wrote);
        if (w <= 0) return;
        wrote += (size_t)w;
    }
}

static void pad_field(char *dst, int width, const char *src, char fill)
{
    int i = 0;
    while (src[i] && i < width) { dst[i] = src[i]; i++; }
    while (i < width) { dst[i] = fill; i++; }
}

static void dec_str(unsigned long v, char *out)
{
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

/* Just the basename portion of a path. */
static const char *basename_of(const char *path)
{
    const char *r = path;
    for (const char *p = path; *p; p++) if (*p == '/') r = p + 1;
    return r;
}

static int read_whole(const char *path, uint8_t **buf, size_t *out_sz)
{
    int fd = open(path, 0);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t sz = (size_t)st.st_size;
    uint8_t *p = (uint8_t *)malloc(sz ? sz : 1);
    if (!p) { close(fd); return -1; }
    size_t got = 0;
    while (got < sz) {
        long n = read(fd, p + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != sz) { free(p); return -1; }
    *buf = p; *out_sz = sz; return 0;
}

static int write_header(int fd, const char *name,
                        unsigned long size, int long_name)
{
    char hdr[60];
    for (int i = 0; i < 60; i++) hdr[i] = ' ';
    if (long_name) {
        /* "#1/NN" then real bytes appended to payload. */
        char tmp[24]; dec_str(s_len(name), tmp);
        char nbuf[20]; nbuf[0] = '#'; nbuf[1] = '1'; nbuf[2] = '/';
        size_t ti = 0; while (tmp[ti]) { nbuf[3 + ti] = tmp[ti]; ti++; }
        nbuf[3 + ti] = '\0';
        pad_field(hdr, 16, nbuf, ' ');
    } else {
        /* short name terminated by '/'. */
        char nbuf[18];
        size_t i = 0;
        while (name[i] && i < 15) { nbuf[i] = name[i]; i++; }
        nbuf[i] = '/'; nbuf[i + 1] = '\0';
        pad_field(hdr, 16, nbuf, ' ');
    }
    pad_field(hdr + 16, 12, "0", ' ');         /* mtime */
    pad_field(hdr + 28, 6,  "0", ' ');         /* uid */
    pad_field(hdr + 34, 6,  "0", ' ');         /* gid */
    pad_field(hdr + 40, 8,  "100644", ' ');    /* mode */
    char szbuf[24]; dec_str(size, szbuf);
    pad_field(hdr + 48, 10, szbuf, ' ');       /* size */
    hdr[58] = '`'; hdr[59] = '\n';
    write_all(fd, hdr, 60);
    return 0;
}

static int cmd_rc(const char *out_path, int argc_in, char **inputs)
{
    int fd = open(out_path, 0101 /* O_WRONLY|O_CREAT */);
    if (fd < 0) {
        printf("ar: cannot open %s: errno=%d\n", out_path, errno);
        return 1;
    }
    write_all(fd, "!<arch>\n", 8);
    for (int i = 0; i < argc_in; i++) {
        const char *path = inputs[i];
        uint8_t *data = 0; size_t dsize = 0;
        if (read_whole(path, &data, &dsize) < 0) {
            printf("ar: cannot read %s: errno=%d\n", path, errno);
            close(fd); return 1;
        }
        const char *nm = basename_of(path);
        size_t nlen = s_len(nm);
        int long_name = (nlen > 15);
        unsigned long total = (unsigned long)dsize +
                              (long_name ? nlen : 0);
        write_header(fd, nm, total, long_name);
        if (long_name) write_all(fd, nm, nlen);
        write_all(fd, data, dsize);
        free(data);
        /* Pad to 2-byte boundary with '\n' if odd. */
        if (total & 1) {
            char nl = '\n';
            write_all(fd, &nl, 1);
        }
    }
    close(fd);
    printf("ar: wrote %s (%d members)\n", out_path, argc_in);
    return 0;
}

static int cmd_t(const char *ar_path)
{
    uint8_t *data; size_t sz;
    if (read_whole(ar_path, &data, &sz) < 0) {
        printf("ar: cannot read %s: errno=%d\n", ar_path, errno);
        return 1;
    }
    if (sz < 8 ||
        data[0] != '!' || data[1] != '<' || data[2] != 'a' ||
        data[3] != 'r' || data[4] != 'c' || data[5] != 'h' ||
        data[6] != '>' || data[7] != '\n') {
        printf("ar: %s: not an ar archive\n", ar_path);
        free(data); return 1;
    }
    size_t off = 8;
    int members = 0;
    while (off + 60 <= sz) {
        /* Decode size (10-byte field at +48, ASCII decimal). */
        char szbuf[11];
        for (int j = 0; j < 10; j++) szbuf[j] = (char)data[off + 48 + j];
        szbuf[10] = '\0';
        unsigned long msize = 0;
        for (int j = 0; j < 10; j++) {
            if (szbuf[j] == ' ') break;
            msize = msize * 10UL + (unsigned long)(szbuf[j] - '0');
        }
        /* Member name. */
        char nbuf[17];
        for (int j = 0; j < 16; j++) nbuf[j] = (char)data[off + j];
        nbuf[16] = '\0';
        /* trim trailing spaces */
        for (int j = 15; j >= 0; j--) {
            if (nbuf[j] == ' ') nbuf[j] = '\0';
            else break;
        }
        /* short-form names end with '/' */
        size_t nlen = s_len(nbuf);
        if (nlen && nbuf[nlen - 1] == '/') nbuf[nlen - 1] = '\0';
        if (nbuf[0] == '#' && nbuf[1] == '1' && nbuf[2] == '/') {
            unsigned long real_nlen = 0;
            for (int j = 3; nbuf[j]; j++)
                real_nlen = real_nlen * 10UL +
                            (unsigned long)(nbuf[j] - '0');
            /* Real name lives at payload[0..real_nlen). */
            char tmp[256];
            unsigned long copy = real_nlen < 255 ? real_nlen : 255;
            for (unsigned long j = 0; j < copy; j++)
                tmp[j] = (char)data[off + 60 + j];
            tmp[copy] = '\0';
            printf("%s\n", tmp);
        } else {
            printf("%s\n", nbuf);
        }
        members++;
        off += 60 + msize;
        if (off & 1) off++;
    }
    printf("ar: %d members\n", members);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: ar rc out.a file1.o [...]\n"
               "       ar t  archive.a\n");
        return 1;
    }
    const char *cmd = argv[1];
    if (s_eq(cmd, "rc") || s_eq(cmd, "cr") || s_eq(cmd, "r"))
        return cmd_rc(argv[2], argc - 3, argv + 3);
    if (s_eq(cmd, "t")) return cmd_t(argv[2]);
    printf("ar: unknown command '%s'\n", cmd);
    return 1;
}
