/*
 * userspace/pngdec/pngdec.c — chapter 98 PNG decoder test harness.
 *
 * Reads a PNG file, decodes it via libc/png.h, and prints
 *   <path>: <w>x<h>, sum=<sum>, opaque=<count>
 * where `sum` is a 32-bit add-and-fold of every BGRA byte and
 * `opaque` is the count of pixels whose alpha == 0xFF.  These
 * two numbers together give a stable "did the decoder produce
 * the same bytes I expected" signature that the test harness
 * can pattern-match against.
 *
 * Usage: pngdec <path>
 *
 * Exit status: 0 on successful decode, 1 on any failure
 * (file-not-found, decoder error).
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/png.h"

static uint8_t *slurp_png(const char *path, size_t *out_len)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("pngdec: open %s: %s\n", path, strerror(errno));
        return 0;
    }
    size_t cap = 4096, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { close(fd); return 0; }
    uint8_t tmp[1024];
    long n;
    while ((n = read(fd, (char *)tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            size_t nc = cap * 2;
            while (nc < len + (size_t)n) nc *= 2;
            uint8_t *nb = (uint8_t *)malloc(nc);
            if (!nb) { free(buf); close(fd); return 0; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf); buf = nb; cap = nc;
        }
        for (long i = 0; i < n; i++) buf[len + (size_t)i] = tmp[i];
        len += (size_t)n;
    }
    close(fd);
    *out_len = len;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: pngdec <path>\n");
        return 1;
    }
    const char *path = argv[1];

    size_t plen = 0;
    uint8_t *pdata = slurp_png(path, &plen);
    if (!pdata) return 1;

    uint8_t *bgra = 0;
    int w = 0, h = 0;
    int rc = png_decode(pdata, plen, &bgra, &w, &h);
    free(pdata);
    if (rc < 0) {
        printf("pngdec: decode failed for %s\n", path);
        return 1;
    }

    uint32_t sum = 0;
    uint32_t opaque = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t b = bgra[(y * w + x) * 4 + 0];
            uint8_t g = bgra[(y * w + x) * 4 + 1];
            uint8_t r = bgra[(y * w + x) * 4 + 2];
            uint8_t a = bgra[(y * w + x) * 4 + 3];
            sum += (uint32_t)b + (uint32_t)g + (uint32_t)r + (uint32_t)a;
            if (a == 0xFF) opaque++;
        }
    }

    printf("%s: %dx%d, sum=%u, opaque=%u\n",
           path, w, h, (unsigned)sum, (unsigned)opaque);
    png_free(bgra);
    return 0;
}
