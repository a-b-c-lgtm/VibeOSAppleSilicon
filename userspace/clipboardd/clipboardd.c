/*
 * userspace/clipboardd/clipboardd.c — chapter 140 port.
 *
 * The system clipboard, now a chapter-114 userfs daemon
 * instead of a chapter-107 IPC service.  Mounts /clipboard
 * and exposes a single file:
 *
 *   /clipboard/text   — read returns the current payload,
 *                       write replaces it.
 *
 * The chapter-108 protocol (op codes, generation counter,
 * MIME tag, srv_bind/srv_accept loop) is gone.  Callers
 * just open the file and read or write.  cp, cat, echo,
 * grep, head — every existing shell tool now works on the
 * clipboard out of the box, which is the whole point of
 * the "everything is a file" rule.
 *
 * State
 * -----
 *
 *   g_data[CLIP_DATA_MAX]  payload bytes
 *   g_len                  live length (0 ≤ g_len ≤ CLIP_DATA_MAX)
 *
 * No generation counter — readers that want change notification
 * stat the file (mtime + size).  No MIME tag — the file IS
 * the type, and we only have one (text).  If we ever need a
 * second MIME, add /clipboard/png alongside /clipboard/text.
 *
 * Handle model
 * ------------
 *
 * Two distinct handles so on_read after on_write returns the
 * just-written bytes (a typical paste-after-copy sequence):
 *
 *   H_TEXT      reader/writer for /clipboard/text
 *
 * One file, one handle — every open() returns H_TEXT and the
 * read/write callbacks operate on the shared g_data array.
 *
 * Truncate semantics
 * ------------------
 *
 * O_TRUNC (passed by the kernel via the on_open `flags`
 * argument) resets g_len to 0 before the first write lands.
 * `echo foo > /clipboard/text` uses that path; a bare
 * `open(... O_WRONLY)` preserves the old payload, matching
 * conventional POSIX semantics.
 */

#include "../libfs/userfs.h"
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/clipboard.h"

enum { H_TEXT = 1 };

static uint8_t  g_data[CLIP_DATA_MAX];
static uint32_t g_len = 0;

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int on_open(void *ud, const char *path, int flags, uint32_t *h)
{
    (void)ud;
    if (!eq(path, "text")) return -2;  /* -ENOENT */
    /* O_TRUNC = 0x200 in syscall.h.  Reset before the first
     * write lands so `>` redirection replaces the payload
     * rather than appending to it. */
    if (flags & 0x200) {
        g_len = 0;
    }
    *h = H_TEXT;
    return 0;
}

static int on_read(void *ud, uint32_t h, uint64_t off,
                   void *buf, uint32_t cap)
{
    (void)ud;
    if (h != H_TEXT) return -9;       /* -EBADF */
    if (off >= g_len) return 0;        /* EOF */
    uint32_t avail = g_len - (uint32_t)off;
    uint32_t take  = avail < cap ? avail : cap;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < take; i++) dst[i] = g_data[off + i];
    return (int)take;
}

static int on_write(void *ud, uint32_t h, uint64_t off,
                    const void *buf, uint32_t n)
{
    (void)ud;
    if (h != H_TEXT) return -9;
    if (off > CLIP_DATA_MAX) return -22;   /* -EINVAL */
    uint32_t room = CLIP_DATA_MAX - (uint32_t)off;
    uint32_t take = n < room ? n : room;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < take; i++) g_data[off + i] = src[i];
    uint32_t endpos = (uint32_t)off + take;
    if (endpos > g_len) g_len = endpos;
    return (int)take;
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
    if (path[0] != '\0') return -2;
    if (idx != 0) return -2;
    name[0] = 't'; name[1] = 'e'; name[2] = 'x'; name[3] = 't';
    *type = 1;
    return 4;
}

static int on_is_dir(void *ud, const char *path)
{
    (void)ud;
    if (path[0] == '\0') return 1;
    if (eq(path, "text")) return 0;
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

    printf("[clipboardd] starting (chapter 140)\n");
    int r = userfs_serve("/clipboard", &h);
    printf("[clipboardd] serve returned %d\n", r);
    return r < 0 ? 1 : 0;
}
