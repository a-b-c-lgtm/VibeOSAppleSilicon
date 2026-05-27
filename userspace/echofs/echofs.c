/*
 * userspace/echofs/echofs.c — chapter 140 demo daemon.
 *
 * The smallest interesting user-space filesystem.  Mounts
 * `/echo/` via SYS_MOUNT, then serves three children:
 *
 *   /echo/hello        read-only, returns the string
 *                      "hello from echofs\n"
 *   /echo/buf          read+write, in-memory scratch buffer
 *                      (up to 4 KiB).  Writes overwrite from
 *                      offset 0 by default; reads return the
 *                      bytes most recently written.
 *   /echo/echo         write-only: any bytes written are
 *                      printed to the daemon's stdout.  Useful
 *                      for tests that want to inject text into
 *                      the serial console via `echo > /echo/echo`.
 *
 * The daemon is intentionally minimal — it exists to prove the
 * libfs serve loop works.  Real production users (clipboardd,
 * procd) will follow in chapters 143/114d.
 */

#include "../libfs/userfs.h"
#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Handles allocated by on_open.  We hand out an integer that
 * encodes "which file" so on_read/on_write know what to do.
 * Handle 0 is reserved as "invalid". */
enum {
    H_HELLO = 1,
    H_BUF   = 2,
    H_ECHO  = 3,
};

static const char k_hello[] = "hello from echofs\n";
#define HELLO_LEN ((uint32_t)(sizeof(k_hello) - 1))

#define BUF_CAP 4096u
static uint8_t  g_buf[BUF_CAP];
static uint32_t g_buf_len = 0;

/* Tiny strcmp — avoids pulling in libc/cstring. */
static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int on_open(void *ud, const char *path, int flags, uint32_t *h)
{
    (void)ud; (void)flags;
    if (eq(path, "hello")) { *h = H_HELLO; return 0; }
    if (eq(path, "buf"))   { *h = H_BUF;   return 0; }
    if (eq(path, "echo"))  { *h = H_ECHO;  return 0; }
    return -2;  /* -ENOENT */
}

static int on_read(void *ud, uint32_t h, uint64_t off, void *buf, uint32_t cap)
{
    (void)ud;
    const uint8_t *src = NULL;
    uint32_t len = 0;

    switch (h) {
    case H_HELLO: src = (const uint8_t *)k_hello; len = HELLO_LEN; break;
    case H_BUF:   src = g_buf;                    len = g_buf_len; break;
    case H_ECHO:  return 0;   /* write-only file → EOF on read */
    default:      return -9;  /* -EBADF */
    }
    if (off >= len) return 0;
    uint32_t avail = len - (uint32_t)off;
    uint32_t take  = avail < cap ? avail : cap;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < take; i++) dst[i] = src[off + i];
    return (int)take;
}

static int on_write(void *ud, uint32_t h, uint64_t off,
                    const void *buf, uint32_t n)
{
    (void)ud;
    if (h == H_HELLO) return -13;        /* -EACCES */
    if (h == H_BUF) {
        if (off > BUF_CAP) return -22;   /* -EINVAL */
        uint32_t room = BUF_CAP - (uint32_t)off;
        uint32_t take = n < room ? n : room;
        const uint8_t *src = (const uint8_t *)buf;
        for (uint32_t i = 0; i < take; i++) g_buf[off + i] = src[i];
        if ((uint32_t)off + take > g_buf_len) g_buf_len = (uint32_t)off + take;
        return (int)take;
    }
    if (h == H_ECHO) {
        const char *s = (const char *)buf;
        /* Cap the printed slice to avoid swamping serial. */
        uint32_t take = n < 256 ? n : 256;
        for (uint32_t i = 0; i < take; i++) {
            char c = s[i];
            if (c == '\0') c = '?';
            printf("%c", c);
        }
        return (int)n;
    }
    return -9;
}

static int on_close(void *ud, uint32_t h)
{
    (void)ud; (void)h;
    return 0;
}

static int on_listdir(void *ud, const char *path, int idx,
                      char *name, uint32_t cap, uint32_t *type)
{
    (void)ud; (void)cap;
    /* Only the root of the mount has entries. */
    if (path[0] != '\0') return -2;
    static const char * const names[] = { "hello", "buf", "echo" };
    if (idx < 0 || idx >= 3) return -2;
    const char *n = names[idx];
    uint32_t len = 0;
    while (n[len]) { name[len] = n[len]; len++; }
    *type = 1;
    return (int)len;
}

static int on_is_dir(void *ud, const char *path)
{
    (void)ud;
    if (path[0] == '\0') return 1;
    if (eq(path, "hello") || eq(path, "buf") || eq(path, "echo")) return 0;
    return -2;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct userfs_handler h;
    h.on_open    = on_open;
    h.on_read    = on_read;
    h.on_write   = on_write;
    h.on_close   = on_close;
    h.on_listdir = on_listdir;
    h.on_unlink  = (int (*)(void *, const char *))0;
    h.on_mkdir   = (int (*)(void *, const char *))0;
    h.on_is_dir  = on_is_dir;
    h.userdata   = (void *)0;

    int r = userfs_serve("/echo", &h);
    printf("echofs: serve returned %d\n", r);
    return r < 0 ? 1 : 0;
}
