/*
 * kernel/core/tmpfs.c — see tmpfs.h.  Single global table of
 * named in-memory buffers.  No locks (single CPU, no preemption
 * inside critical sections of vfs/syscall code paths).
 */

#include "tmpfs.h"
#include "heap.h"
#include "serial.h"

struct tmpfs_file {
    int       in_use;
    char      name[TMPFS_MAX_NAME];
    uint8_t  *data;
    uint32_t  size;       /* bytes currently held */
    uint32_t  cap;        /* allocated capacity   */
};

static struct tmpfs_file g_files[TMPFS_MAX_FILES];

void tmpfs_init(void)
{
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        g_files[i].in_use = 0;
        g_files[i].name[0] = '\0';
        g_files[i].data = NULL;
        g_files[i].size = 0;
        g_files[i].cap  = 0;
    }
    serial_puts("[tmpfs] mounted (writable, capacity ");
    /* Fixed-size message; exact byte sizes inline. */
    serial_puts("16 files x 256 KiB)\n");
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int tmpfs_lookup(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (g_files[i].in_use && name_eq(g_files[i].name, name))
            return i;
    }
    return -1;
}

static int copy_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < TMPFS_MAX_NAME - 1) {
        dst[i] = src[i];
        i++;
    }
    if (src[i]) return -1;     /* too long */
    dst[i] = '\0';
    return i;
}

int tmpfs_create_or_truncate(const char *name)
{
    if (!name || !name[0]) return -1;

    int existing = tmpfs_lookup(name);
    if (existing >= 0) {
        g_files[existing].size = 0;
        return existing;
    }

    /* Find a free slot. */
    int idx = -1;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!g_files[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return -1;     /* table full */

    if (copy_name(g_files[idx].name, name) < 0) return -1;

    g_files[idx].data = (uint8_t *)kmalloc(TMPFS_INITIAL_CAP);
    if (!g_files[idx].data) {
        g_files[idx].name[0] = '\0';
        return -1;
    }
    g_files[idx].cap    = TMPFS_INITIAL_CAP;
    g_files[idx].size   = 0;
    g_files[idx].in_use = 1;
    return idx;
}

long tmpfs_read(int idx, uint64_t offset, void *buf, size_t len)
{
    if (idx < 0 || idx >= TMPFS_MAX_FILES) return -9 /* EBADF */;
    struct tmpfs_file *f = &g_files[idx];
    if (!f->in_use) return -9;
    if (offset >= f->size) return 0;
    size_t avail = (size_t)(f->size - offset);
    size_t n = len < avail ? len : avail;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < n; i++) dst[i] = f->data[offset + i];
    return (long)n;
}

/* Grow `f` so it can hold at least `need` total bytes.  Doubles
 * cap until it fits or hits the cap; reallocates by kmalloc +
 * copy + kfree (no realloc primitive in our heap). */
static int tmpfs_grow_to(struct tmpfs_file *f, uint32_t need)
{
    if (need <= f->cap) return 0;
    if (need > TMPFS_MAX_FILE_SIZE) return -1;
    uint32_t newcap = f->cap;
    if (newcap == 0) newcap = TMPFS_INITIAL_CAP;
    while (newcap < need) {
        if (newcap >= TMPFS_MAX_FILE_SIZE / 2) {
            newcap = TMPFS_MAX_FILE_SIZE;
            break;
        }
        newcap *= 2;
    }
    if (newcap < need) return -1;
    uint8_t *nbuf = (uint8_t *)kmalloc(newcap);
    if (!nbuf) return -1;
    for (uint32_t i = 0; i < f->size; i++) nbuf[i] = f->data[i];
    if (f->data) kfree(f->data);
    f->data = nbuf;
    f->cap  = newcap;
    return 0;
}

long tmpfs_write(int idx, const void *buf, size_t len)
{
    if (idx < 0 || idx >= TMPFS_MAX_FILES) return -9;
    struct tmpfs_file *f = &g_files[idx];
    if (!f->in_use) return -9;
    if (len == 0) return 0;
    uint64_t need64 = (uint64_t)f->size + (uint64_t)len;
    if (need64 > TMPFS_MAX_FILE_SIZE) return -28 /* ENOSPC */;
    if (tmpfs_grow_to(f, (uint32_t)need64) != 0) return -12 /* ENOMEM */;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) f->data[f->size + i] = src[i];
    f->size += (uint32_t)len;
    return (long)len;
}

int tmpfs_count(void)
{
    int n = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++)
        if (g_files[i].in_use) n++;
    return n;
}

int tmpfs_listdir(int idx, char *out, size_t cap, uint32_t *size_out)
{
    /* idx is a "kth in-use file" index, not a slot index, so the
     * caller can iterate 0..N-1 without seeing holes. */
    if (idx < 0) return -1;
    int seen = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!g_files[i].in_use) continue;
        if (seen == idx) {
            int n = 0;
            while (g_files[i].name[n] && (size_t)(n + 1) < cap) {
                out[n] = g_files[i].name[n];
                n++;
            }
            out[n] = '\0';
            if (size_out) *size_out = g_files[i].size;
            return n;
        }
        seen++;
    }
    return -1;
}

uint32_t tmpfs_size_of(int idx)
{
    if (idx < 0 || idx >= TMPFS_MAX_FILES) return 0;
    if (!g_files[idx].in_use) return 0;
    return g_files[idx].size;
}

void tmpfs_seek_end(int idx)
{
    /* No-op: tmpfs_write currently always appends.  When we
     * grow tmpfs to support seek + overwrite we'll move the
     * write cursor here. */
    (void)idx;
}

int tmpfs_unlink(const char *name)
{
    int idx = tmpfs_lookup(name);
    if (idx < 0) return -2 /* ENOENT */;
    if (g_files[idx].data) {
        kfree(g_files[idx].data);
        g_files[idx].data = NULL;
    }
    g_files[idx].cap    = 0;
    g_files[idx].size   = 0;
    g_files[idx].in_use = 0;
    g_files[idx].name[0] = '\0';
    return 0;
}
