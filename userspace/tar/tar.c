/* userspace/tar/tar.c -- minimal ustar archive reader.
 *
 * Chapter 133a.  Supports two modes:
 *
 *     tar tf <archive>            list contents
 *     tar xf <archive> [-C dir]   extract to dir (default cwd)
 *
 * Only the ustar format is recognised.  Only regular files
 * (typeflag '0' or '\0') and directories ('5') are honoured;
 * symlinks, hardlinks, device nodes, and the GNU long-name
 * extensions are skipped (printed with a 'skipped:' note).
 *
 * Read-only: we never write archives.  In-guest packaging
 * happens at the build host via scripts/mktar.py.
 *
 * Implementation notes:
 *
 *   * The ustar header is 512 bytes.  Each file's data follows
 *     the header, padded out to the next 512-byte boundary.
 *     A pair of zero blocks marks end-of-archive.
 *
 *   * The size field is 11 octal digits + NUL/space.  We parse
 *     it manually because /bin/tar runs before atoi() in real
 *     /bin would be reliable on weird strings.
 *
 *   * Path components are created on demand via mkdir().  Our
 *     SYS_MKDIR has no "-p" semantics so we walk the path
 *     prefix-by-prefix, swallowing -EEXIST.
 *
 *   * We deliberately do NOT clear S_IXUSR or apply file modes:
 *     OSFS-2 ignores the mode argument today, so we pass 0755
 *     and call it a day.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/fcntl.h"
#include "../libc/printf.h"
#include "../libc/string.h"
#include "../libc/stdlib.h"

#define BLOCK 512

struct tar_hdr {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

_Static_assert(sizeof(struct tar_hdr) == BLOCK,
               "ustar header must be exactly one block");

static int is_zero_block(const unsigned char *b)
{
    for (int i = 0; i < BLOCK; i++) if (b[i]) return 0;
    return 1;
}

/* Parse a NUL/space-terminated octal field.  Up to `max` chars;
 * stops at the first NUL or space.  Returns -1 on malformed
 * input (any digit outside 0..7). */
static long parse_octal(const char *s, int max)
{
    long v = 0;
    int seen = 0;
    for (int i = 0; i < max; i++) {
        char c = s[i];
        if (c == 0 || c == ' ') {
            if (!seen) continue;
            break;
        }
        if (c < '0' || c > '7') return -1;
        v = (v << 3) | (c - '0');
        seen = 1;
    }
    if (!seen) return -1;
    return v;
}

/* Read exactly `n` bytes or return -1 on short/EOF. */
static long read_n(int fd, void *buf, long n)
{
    char *p = buf;
    long left = n;
    while (left > 0) {
        long got = read(fd, p, (size_t)left);
        if (got <= 0) return -1;
        p    += got;
        left -= got;
    }
    return n;
}

/* Concatenate prefix + "/" + name into out, sized cap.  If the
 * prefix is empty the slash is omitted.  Returns 0 on success
 * or -1 on overflow. */
static int join_path(char *out, int cap, const char *prefix, const char *name)
{
    int i = 0;
    for (int j = 0; prefix[j] && j < 155; j++) {
        if (i + 1 >= cap) return -1;
        out[i++] = prefix[j];
    }
    if (i > 0) {
        if (i + 1 >= cap) return -1;
        out[i++] = '/';
    }
    for (int j = 0; name[j] && j < 100; j++) {
        if (i + 1 >= cap) return -1;
        out[i++] = name[j];
    }
    out[i] = 0;
    return 0;
}

/* mkdir -p, walking the path prefix-by-prefix.  EEXIST is
 * tolerated; any other failure prints a warning and continues.
 * Skips the basename if `with_basename == 0`. */
static void mkdir_p(const char *path, int with_basename)
{
    char buf[512];
    int  len = 0;
    while (path[len] && len < (int)sizeof(buf) - 1) {
        buf[len] = path[len];
        len++;
    }
    buf[len] = 0;

    int  last = -1;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '/' && i > 0) {
            buf[i] = 0;
            if (mkdir(buf, 0755) < 0 && errno != EEXIST) {
                /* swallow: directory might already exist via a
                 * parent created earlier in this archive */
            }
            buf[i] = '/';
            last = i;
        }
    }
    if (with_basename && len > 0 && buf[len - 1] != '/') {
        if (mkdir(buf, 0755) < 0 && errno != EEXIST) {
            printf("tar: warning: mkdir %s: %s\n",
                   buf, strerror(errno));
        }
    }
    (void)last;
}

/* Prepend `chroot/` to `path` if chroot is non-empty.  Result
 * goes into out (cap bytes).  Returns 0 on success or -1 on
 * overflow. */
static int rebase(char *out, int cap, const char *chroot, const char *path)
{
    int i = 0;
    if (chroot && chroot[0]) {
        for (int j = 0; chroot[j]; j++) {
            if (i + 1 >= cap) return -1;
            out[i++] = chroot[j];
        }
        if (i == 0 || out[i - 1] != '/') {
            if (i + 1 >= cap) return -1;
            out[i++] = '/';
        }
    }
    for (int j = 0; path[j]; j++) {
        if (i + 1 >= cap) return -1;
        out[i++] = path[j];
    }
    out[i] = 0;
    return 0;
}

static int walk(int fd, int extract, const char *chroot)
{
    unsigned char block[BLOCK];
    int zero_run = 0;
    int count = 0;

    for (;;) {
        if (read_n(fd, block, BLOCK) < 0) {
            printf("tar: archive ended without end-of-archive marker\n");
            return -1;
        }
        if (is_zero_block(block)) {
            zero_run++;
            if (zero_run >= 2) break;     /* clean EOA */
            continue;
        }
        zero_run = 0;

        struct tar_hdr *h = (struct tar_hdr *)block;
        long sz = parse_octal(h->size, 12);
        if (sz < 0) {
            printf("tar: malformed size field; aborting\n");
            return -1;
        }

        char fullname[300];
        if (join_path(fullname, sizeof(fullname), h->prefix, h->name) < 0) {
            printf("tar: path too long; aborting\n");
            return -1;
        }

        char tf = h->typeflag;
        if (tf == 0) tf = '0';        /* old-style "regular file" */

        if (tf == '0') {
            printf("%s\n", fullname);
            count++;
            if (extract) {
                char dst[512];
                if (rebase(dst, sizeof(dst), chroot, fullname) < 0) {
                    printf("tar: rebased path too long; skipping\n");
                } else {
                    mkdir_p(dst, 0);
                    int ofd = open(dst, O_CREAT | O_WRONLY | O_TRUNC);
                    if (ofd < 0) {
                        printf("tar: cannot create %s: %s\n",
                               dst, strerror(errno));
                    } else {
                        char buf[4096];
                        long left = sz;
                        while (left > 0) {
                            long want = left > (long)sizeof(buf)
                                        ? (long)sizeof(buf) : left;
                            if (read_n(fd, buf, want) < 0) {
                                printf("tar: short read on %s\n", fullname);
                                close(ofd);
                                return -1;
                            }
                            long wrote = write(ofd, buf, (size_t)want);
                            if (wrote != want) {
                                printf("tar: short write on %s (%ld/%ld)\n",
                                       dst, wrote, want);
                                close(ofd);
                                return -1;
                            }
                            left -= want;
                        }
                        close(ofd);
                        /* skip remainder of padding */
                        long pad = (BLOCK - (sz % BLOCK)) % BLOCK;
                        if (pad > 0) {
                            char skipbuf[BLOCK];
                            if (read_n(fd, skipbuf, pad) < 0) {
                                printf("tar: short read on pad after %s\n",
                                       fullname);
                                return -1;
                            }
                        }
                        continue;     /* already consumed data + pad */
                    }
                }
            }
            /* list mode (or extract that fell through to skip):
             * still need to advance past the file body */
            long total = sz + ((BLOCK - (sz % BLOCK)) % BLOCK);
            while (total > 0) {
                char skipbuf[BLOCK];
                long want = total > BLOCK ? BLOCK : total;
                if (read_n(fd, skipbuf, want) < 0) {
                    printf("tar: short read while skipping body of %s\n",
                           fullname);
                    return -1;
                }
                total -= want;
            }
        } else if (tf == '5') {
            printf("%s/\n", fullname);
            count++;
            if (extract) {
                char dst[512];
                if (rebase(dst, sizeof(dst), chroot, fullname) < 0) {
                    printf("tar: rebased path too long; skipping\n");
                } else {
                    mkdir_p(dst, 1);
                }
            }
        } else {
            printf("tar: skipping %s (typeflag '%c' not supported)\n",
                   fullname, tf);
            /* still need to skip the body if any */
            long total = sz + ((BLOCK - (sz % BLOCK)) % BLOCK);
            while (total > 0) {
                char skipbuf[BLOCK];
                long want = total > BLOCK ? BLOCK : total;
                if (read_n(fd, skipbuf, want) < 0) return -1;
                total -= want;
            }
        }
    }

    printf("tar: %d entries\n", count);
    return 0;
}

static void usage(void)
{
    printf("usage: tar tf <archive>\n");
    printf("       tar xf <archive> [-C dir]\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(); return 1; }

    const char *mode    = argv[1];
    const char *archive = argv[2];
    const char *chroot  = "";

    int list, extract;
    if (mode[0] == 't' && mode[1] == 'f' && mode[2] == 0) {
        list = 1; extract = 0;
    } else if (mode[0] == 'x' && mode[1] == 'f' && mode[2] == 0) {
        list = 0; extract = 1;
    } else {
        usage(); return 1;
    }
    (void)list;

    /* -C dir: change the extraction root.  Only relevant in
     * extract mode but parsed unconditionally so the caller can
     * issue the same argv either way. */
    for (int i = 3; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'C' && argv[i][2] == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            chroot = argv[++i];
        } else {
            printf("tar: unknown flag %s\n", argv[i]);
            return 1;
        }
    }

    int fd = open(archive, O_RDONLY);
    if (fd < 0) {
        printf("tar: cannot open %s: %s\n", archive, strerror(errno));
        return 1;
    }

    int rc = walk(fd, extract, chroot);
    close(fd);
    return rc < 0 ? 2 : 0;
}
