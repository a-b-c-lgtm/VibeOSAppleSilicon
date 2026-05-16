/*
 * kernel/core/vfs.c — minimal ramfs-backed VFS for milestone 8.
 *
 * The ramfs is a static array of {name, data pointer, size}
 * triples.  The data lives in the kernel image (under
 * .rodata.embedded_user — same section we use for embedded user
 * binaries), populated by the same `objcopy -I binary` pipeline.
 *
 * `vfs_open` walks the array linearly and returns the first
 * matching fd.  `vfs_read` copies bytes from the underlying blob
 * into the user buffer, advancing the per-fd offset.  No write
 * support: the entire FS is read-only by construction.
 *
 * Per-thread fd tables live inside `struct thread` (added in
 * thread.h alongside this file).  `vfs_init_fdtable` is called
 * from thread_init / thread_create / user_thread_create to set
 * up slots 0/1/2 as the console.
 */

#include "vfs.h"
#include "thread.h"
#include "serial.h"
#include "console_in.h"
#include "osfs.h"
#include "heap.h"
#include "pipe.h"
#include "pty.h"
#include "tmpfs.h"
#include "osfs2.h"
#include "tcp.h"
#include "net.h"
#include "procfs.h"
#include "../arch/atomic.h"
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------
 * Embedded ramfs files.  Each one is wrapped via `objcopy -I binary`
 * by the Makefile and exposes _binary_<name>_start/_end symbols.
 * Add new files by extending the table at the bottom of this file
 * and declaring the symbols here.
 * ------------------------------------------------------------------ */

extern char _binary_motd_txt_start[];
extern char _binary_motd_txt_end[];
extern char _binary_README_txt_start[];
extern char _binary_README_txt_end[];
/* User binaries (init, sh, cat, hello) used to live here as
 * embedded blobs.  Since milestone 13 they are stored on the
 * OSFS-1 disk under /mnt/bin/<name> and reached via the /bin/
 * dispatch in vfs_open / vfs_load.  Editing a user program no
 * longer relinks the kernel. */

struct ramfs_file {
    const char    *name;
    const uint8_t *data;
    const uint8_t *end;     /* `_end` symbol; size = end - data, computed at use */
};

#define RAMFS_FILE(symbol_base, fname) \
    { .name = (fname), \
      .data = (const uint8_t *)_binary_##symbol_base##_start, \
      .end  = (const uint8_t *)_binary_##symbol_base##_end }

static struct ramfs_file g_ramfs[] = {
    RAMFS_FILE(motd_txt,     "/motd"),
    RAMFS_FILE(README_txt,   "/README"),
};

#define RAMFS_COUNT (sizeof(g_ramfs) / sizeof(g_ramfs[0]))

static inline size_t ramfs_size(const struct ramfs_file *f)
{
    return (size_t)((uintptr_t)f->end - (uintptr_t)f->data);
}

/* ------------------------------------------------------------------
 * Helpers.
 * ------------------------------------------------------------------ */
static int strings_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ramfs_lookup(const char *name)
{
    for (size_t i = 0; i < RAMFS_COUNT; i++) {
        if (strings_equal(name, g_ramfs[i].name))
            return (int)i;
    }
    return -1;
}

/* Chapter 90 — public ramfs blob accessor.  Used by the in-
 * kernel mmap path to feed the page cache from ramfs.  Returns
 * 0 on success, -1 on out-of-range index.  No bounds-check on
 * the returned span beyond ramfs_size; the caller is responsible
 * for clamping their reads. */
int vfs_ramfs_blob(int idx, const uint8_t **out_data, uint64_t *out_size)
{
    if (idx < 0 || (size_t)idx >= RAMFS_COUNT) return -1;
    if (out_data) *out_data = g_ramfs[idx].data;
    if (out_size) *out_size = (uint64_t)ramfs_size(&g_ramfs[idx]);
    return 0;
}

uint64_t vfs_ramfs_count(void) { return (uint64_t)RAMFS_COUNT; }

int vfs_ramfs_lookup(const char *name)
{
    return ramfs_lookup(name);
}

/* ------------------------------------------------------------------
 * Public VFS API.
 * ------------------------------------------------------------------ */
void vfs_init(void)
{
    serial_puts("[vfs] ramfs files:\n");
    for (size_t i = 0; i < RAMFS_COUNT; i++) {
        serial_puts("       ");
        serial_puts(g_ramfs[i].name);
        serial_puts(" (");
        serial_puthex((uint64_t)ramfs_size(&g_ramfs[i]));
        serial_puts(" bytes)\n");
    }
    tmpfs_init();
}

void vfs_init_fdtable(struct thread *t)
{
    /* Chapter 93 — `t->fdt` is now a separately-allocated,
     * refcounted table.  This function is the "give the new
     * thread a private table" path; CLONE_FILES users instead
     * call fd_table_share on an existing table and bypass this
     * function entirely.
     *
     * Defensive: if a table is already attached (e.g. shared
     * by a clone(CLONE_FILES) caller), do NOT overwrite — the
     * caller chose to inherit and we'd double-allocate. */
    if (!t) return;
    if (t->fdt) return;

    t->fdt = fd_table_create();
    /* OOM here would surface as a NULL fdt at the first
     * read/write — every syscall checks `e->in_use` and
     * touching a NULL fdt would null-deref well before user
     * code runs.  thread_create already returns NULL on the
     * preceding kmalloc(stack) so the caller has an early
     * abort path; we keep this function void-returning to
     * preserve the chapter-8 signature, and the few in-tree
     * callers (boot thread, idle threads) all run before any
     * I/O so a NULL here is effectively boot-fatal anyway. */
}

struct fd_table *fd_table_create(void)
{
    struct fd_table *ft = (struct fd_table *)kmalloc(sizeof(*ft));
    if (!ft) return NULL;

    ft->lock.locked = 0;
    ft->refcount = 1;

    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        ft->fds[i].in_use      = 0;
        ft->fds[i].kind        = FD_CONSOLE;
        ft->fds[i].offset      = 0;
        ft->fds[i].ramfs_index = -1;
        ft->fds[i].osfs_start  = 0;
        ft->fds[i].osfs_size   = 0;
        ft->fds[i].pipe        = NULL;
        ft->fds[i].socket_cid  = -1;
        ft->fds[i].pty         = NULL;
        ft->fds[i].osfs2_ino   = 0;
    }
    /* fds 0, 1, 2 = console (ramfs_index = -1 sentinel, osfs_size = 0). */
    for (int i = 0; i < 3; i++) {
        ft->fds[i].in_use      = 1;
        ft->fds[i].kind        = FD_CONSOLE;
        ft->fds[i].offset      = 0;
        ft->fds[i].ramfs_index = -1;
        ft->fds[i].osfs_start  = 0;
        ft->fds[i].osfs_size   = 0;
        ft->fds[i].pipe        = NULL;
        ft->fds[i].socket_cid  = -1;
        ft->fds[i].pty         = NULL;
        ft->fds[i].osfs2_ino   = 0;
    }
    return ft;
}

void fd_table_share(struct fd_table *ft)
{
    if (!ft) return;
    /* Bump under the LSE atomic so concurrent shares from
     * other CPUs (e.g. two clone3 calls in flight) compose
     * correctly with a concurrent unref-to-zero. */
    (void)atomic_add_return32(&ft->refcount, 1);
}

void fd_table_unref(struct fd_table *ft)
{
    if (!ft) return;
    /* Drop our reference.  If others still hold a ref, leave
     * every fd in place — those threads still need them. */
    if (atomic_sub_return32(&ft->refcount, 1) > 0) return;

    /* Last reference — close every still-open slot so pipe /
     * pty / socket refcounts drop and the matching peer sees
     * EOF / -EPIPE.  We do not need the lock here: by
     * definition no other thread holds a reference and so
     * none can be touching the table concurrently. */
    for (int fd = 0; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &ft->fds[fd];
        if (!e->in_use) continue;
        if (e->kind == FD_PIPE_R && e->pipe)
            pipe_unref(e->pipe, PIPE_REF_R);
        else if (e->kind == FD_PIPE_W && e->pipe)
            pipe_unref(e->pipe, PIPE_REF_W);
        else if (e->kind == FD_SOCKET && e->socket_cid >= 0)
            tcp_close(e->socket_cid);
        else if (e->kind == FD_PTY_MASTER && e->pty)
            pty_close_master(e->pty);
        else if (e->kind == FD_PTY_SLAVE && e->pty)
            pty_close_slave(e->pty);
        e->in_use = 0;
    }
    kfree(ft);
}

/* True if `path` starts with the literal prefix `prefix`. */
static int path_starts_with(const char *path, const char *prefix)
{
    while (*prefix) {
        if (*path != *prefix) return 0;
        path++; prefix++;
    }
    return 1;
}

int vfs_open(const char *name, int flags)
{
    if (!name) return -EINVAL_VFS;

    /* /tmp/<name> -> writable in-memory tmpfs.  O_CREAT means
     * "create if missing".  O_TRUNC (implied for `>` redirects)
     * truncates an existing file; O_APPEND skips truncation so
     * subsequent writes accumulate.  Without O_CREAT, a missing
     * file -> -ENOENT. */
    if (path_starts_with(name, "/tmp/")) {
        const char *bare = name + 5;
        int tidx;
        if (flags & O_CREAT) {
            int found = tmpfs_lookup(bare);
            if (found >= 0) {
                tidx = found;
                if (!(flags & O_APPEND)) {
                    /* Truncate existing.  create_or_truncate on a
                     * known-extant slot is a no-fail truncation. */
                    (void)tmpfs_create_or_truncate(bare);
                }
            } else {
                tidx = tmpfs_create_or_truncate(bare);
                if (tidx < 0) return -ENOMEM_VFS;
            }
        } else {
            tidx = tmpfs_lookup(bare);
            if (tidx < 0) return -ENOENT_VFS;
        }
        struct thread *t = thread_current();
        for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
            if (!t->fdt->fds[fd].in_use) {
                t->fdt->fds[fd].in_use      = 1;
                t->fdt->fds[fd].kind        = FD_TMPFS_RW;
                t->fdt->fds[fd].offset      = 0;
                t->fdt->fds[fd].ramfs_index = tidx;
                t->fdt->fds[fd].osfs_start  = 0;
                t->fdt->fds[fd].osfs_size   = 0;
                t->fdt->fds[fd].pipe        = NULL;
                t->fdt->fds[fd].osfs2_ino   = 0;
                return fd;
            }
        }
        return -EMFILE;
    }

    /* /data/<name> -> writable OSFS-2 (chapter 81+).  O_CREAT means
     * "create if missing".  O_TRUNC truncates an existing file to
     * zero bytes; O_APPEND keeps existing content but read/write
     * still start at offset 0 (sys_write may seek to EOF when the
     * O_APPEND-equivalent semantics get added).  Without O_CREAT,
     * a missing file -> -ENOENT. */
    if (path_starts_with(name, "/data/")) {
        const char *bare = name + 6;
        if (!osfs2_present()) return -ENOENT_VFS;
        uint32_t ino = osfs2_lookup(bare);
        if (ino == 0) {
            if (!(flags & O_CREAT)) return -ENOENT_VFS;
            ino = osfs2_create(bare);
            if (ino == 0) return -ENOMEM_VFS;
        } else if ((flags & O_TRUNC) && !(flags & O_APPEND)) {
            if (osfs2_truncate(ino, 0) != 0) return -EIO;
        }
        struct thread *t = thread_current();
        for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
            if (!t->fdt->fds[fd].in_use) {
                t->fdt->fds[fd].in_use      = 1;
                t->fdt->fds[fd].kind        = FD_OSFS2_FILE;
                t->fdt->fds[fd].offset      = 0;
                t->fdt->fds[fd].ramfs_index = -1;
                t->fdt->fds[fd].osfs_start  = 0;
                t->fdt->fds[fd].osfs_size   = 0;
                t->fdt->fds[fd].pipe        = NULL;
                t->fdt->fds[fd].osfs2_ino   = ino;
                return fd;
            }
        }
        return -EMFILE;
    }

    /* /proc/<path> -> chapter 99 read-only snapshot pseudo-FS.
     * Snapshots the rendered content at open time into a
     * kheap buffer so subsequent reads see consistent data
     * even if a thread spawns / exits mid-read.  vfs_close
     * frees the buffer.  Also accept "/proc" with no trailing
     * slash — that maps to the empty suffix, which procfs_render
     * turns into a directory listing. */
    {
        const char *rel = NULL;
        if (path_starts_with(name, "/proc/"))
            rel = name + 6;
        else if (name[0] == '/' && name[1] == 'p' && name[2] == 'r' &&
                 name[3] == 'o' && name[4] == 'c' && name[5] == '\0')
            rel = "";
        if (rel) {
            char *buf = (char *)kmalloc(PROCFS_MAX_FILE);
            if (!buf) return -ENOMEM_VFS;
            long n = procfs_render(rel, buf, PROCFS_MAX_FILE);
            if (n < 0) { kfree(buf); return -ENOENT_VFS; }
            struct thread *t = thread_current();
            for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
                if (!t->fdt->fds[fd].in_use) {
                    t->fdt->fds[fd].in_use      = 1;
                    t->fdt->fds[fd].kind        = FD_PROCFS;
                    t->fdt->fds[fd].offset      = 0;
                    t->fdt->fds[fd].ramfs_index = -1;
                    t->fdt->fds[fd].osfs_start  = 0;
                    t->fdt->fds[fd].osfs_size   = 0;
                    t->fdt->fds[fd].pipe        = NULL;
                    t->fdt->fds[fd].osfs2_ino   = 0;
                    t->fdt->fds[fd].procfs_buf  = buf;
                    t->fdt->fds[fd].procfs_len  = (uint32_t)n;
                    return fd;
                }
            }
            kfree(buf);
            return -EMFILE;
        }
    }

    /* /mnt/<name> -> OSFS-1 disk mount.
     * /bin/<name> -> OSFS-1 disk mount (binaries live on disk
     *               since milestone 13; the prefix is stripped
     *               before the OSFS lookup). */
    const char *bare = NULL;
    if (path_starts_with(name, "/mnt/")) bare = name + 5;
    else if (path_starts_with(name, "/bin/")) bare = name + 5;
    if (bare) {
        uint32_t start = 0, size = 0;
        if (osfs_lookup(bare, &start, &size) != 0)
            return -ENOENT_VFS;
        struct thread *t = thread_current();
        for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
            if (!t->fdt->fds[fd].in_use) {
                t->fdt->fds[fd].in_use      = 1;
                t->fdt->fds[fd].kind        = FD_FILE;
                t->fdt->fds[fd].offset      = 0;
                t->fdt->fds[fd].ramfs_index = -1;
                t->fdt->fds[fd].osfs_start  = start;
                t->fdt->fds[fd].osfs_size   = size;
                t->fdt->fds[fd].pipe        = NULL;
                t->fdt->fds[fd].osfs2_ino   = 0;
                return fd;
            }
        }
        return -EMFILE;
    }

    int idx = ramfs_lookup(name);
    if (idx < 0) return -ENOENT_VFS;

    struct thread *t = thread_current();
    /* Lowest free slot at or above 3 (POSIX semantics). */
    for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
        if (!t->fdt->fds[fd].in_use) {
            t->fdt->fds[fd].in_use      = 1;
            t->fdt->fds[fd].kind        = FD_FILE;
            t->fdt->fds[fd].offset      = 0;
            t->fdt->fds[fd].ramfs_index = idx;
            t->fdt->fds[fd].osfs_start  = 0;
            t->fdt->fds[fd].osfs_size   = 0;
            t->fdt->fds[fd].pipe        = NULL;
            t->fdt->fds[fd].osfs2_ino   = 0;
            return fd;
        }
    }
    return -EMFILE;
}

/* Open `name` directly into thread `t`'s slot `fd`, overwriting
 * whatever was there.  Used by sys_spawn_redir to wire a child's
 * fd 0 to a file before the child runs.  Same path resolution as
 * vfs_open (OSFS for /mnt|/bin, ramfs otherwise).  Returns 0 on
 * success or a negative errno; on failure `t->fdt->fds[fd]` is left
 * unchanged. */
int vfs_open_into(struct thread *t, int fd, const char *name, int flags)
{
    (void)flags;
    if (!t || !name) return -EINVAL_VFS;
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;

    /* If the slot is already wired to a pipe / pty / tmpfs file
     * (e.g. because sys_spawn_redir's thread_inherit_fds bumped
     * a pty slave into fd 0 a moment ago), drop those references
     * cleanly before we overwrite — otherwise the underlying
     * pipe / pty leaks and never finalises when the spawned
     * process exits. */
    struct fd_entry *slot = &t->fdt->fds[fd];
    if (slot->in_use) {
        if      (slot->kind == FD_PIPE_R && slot->pipe) pipe_unref(slot->pipe, PIPE_REF_R);
        else if (slot->kind == FD_PIPE_W && slot->pipe) pipe_unref(slot->pipe, PIPE_REF_W);
        else if (slot->kind == FD_PTY_MASTER && slot->pty) pty_close_master(slot->pty);
        else if (slot->kind == FD_PTY_SLAVE  && slot->pty) pty_close_slave(slot->pty);
        slot->pipe = NULL;
        slot->pty  = NULL;
    }

    const char *bare = NULL;
    if (path_starts_with(name, "/mnt/")) bare = name + 5;
    else if (path_starts_with(name, "/bin/")) bare = name + 5;

    if (bare) {
        uint32_t start = 0, size = 0;
        if (osfs_lookup(bare, &start, &size) != 0)
            return -ENOENT_VFS;
        t->fdt->fds[fd].in_use      = 1;
        t->fdt->fds[fd].kind        = FD_FILE;
        t->fdt->fds[fd].offset      = 0;
        t->fdt->fds[fd].ramfs_index = -1;
        t->fdt->fds[fd].osfs_start  = start;
        t->fdt->fds[fd].osfs_size   = size;
        t->fdt->fds[fd].pipe        = NULL;
        t->fdt->fds[fd].osfs2_ino   = 0;
        return 0;
    }

    /* /data/<name> -> writable OSFS-2.  Same flags semantics as
     * the /tmp/ branch in vfs_open: O_CREAT creates on miss,
     * O_TRUNC zeros an existing file unless O_APPEND is set. */
    if (path_starts_with(name, "/data/")) {
        const char *bare2 = name + 6;
        if (!osfs2_present()) return -ENOENT_VFS;
        uint32_t ino = osfs2_lookup(bare2);
        if (ino == 0) {
            if (!(flags & O_CREAT)) return -ENOENT_VFS;
            ino = osfs2_create(bare2);
            if (ino == 0) return -ENOMEM_VFS;
        } else if ((flags & O_TRUNC) && !(flags & O_APPEND)) {
            if (osfs2_truncate(ino, 0) != 0) return -EIO;
        }
        t->fdt->fds[fd].in_use      = 1;
        t->fdt->fds[fd].kind        = FD_OSFS2_FILE;
        t->fdt->fds[fd].offset      = 0;
        t->fdt->fds[fd].ramfs_index = -1;
        t->fdt->fds[fd].osfs_start  = 0;
        t->fdt->fds[fd].osfs_size   = 0;
        t->fdt->fds[fd].pipe        = NULL;
        t->fdt->fds[fd].osfs2_ino   = ino;
        return 0;
    }

    int idx = ramfs_lookup(name);
    if (idx < 0) return -ENOENT_VFS;
    t->fdt->fds[fd].in_use      = 1;
    t->fdt->fds[fd].kind        = FD_FILE;
    t->fdt->fds[fd].offset      = 0;
    t->fdt->fds[fd].ramfs_index = idx;
    t->fdt->fds[fd].osfs_start  = 0;
    t->fdt->fds[fd].osfs_size   = 0;
    t->fdt->fds[fd].pipe        = NULL;
    t->fdt->fds[fd].osfs2_ino   = 0;
    return 0;
}

long vfs_read(int fd, void *buf, size_t len)
{
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    if (!buf) return -EINVAL_VFS;

    struct thread *t = thread_current();
    struct fd_entry *e = &t->fdt->fds[fd];
    if (!e->in_use) return -EBADF;

    /* Pipe read end. */
    if (e->kind == FD_PIPE_R) return pipe_read(e->pipe, buf, len);
    /* Pipe write end is not readable. */
    if (e->kind == FD_PIPE_W) return -EBADF;

    /* Pty endpoints (chapter 79b).  Both ends are read+write,
     * so they get their own dispatch in vfs_read / sys_write
     * rather than being limited to a single direction. */
    if (e->kind == FD_PTY_MASTER) return pty_master_read(e->pty, buf, len);
    if (e->kind == FD_PTY_SLAVE)  return pty_slave_read(e->pty, buf, len);

    /* TCP socket. */
    if (e->kind == FD_SOCKET) {
        if (e->socket_cid < 0) return -EBADF;
        /* Spin-yield until data shows up or peer closes.  We have
         * no scheduler-blocking primitives wired into TCP yet, so
         * userspace gets a polling read with periodic yield().
         * We must drive net_poll() ourselves — yield() doesn't
         * pump the NIC, so without this read would never see
         * inbound bytes. */
        for (;;) {
            (void)net_poll();
            int n = tcp_recv(e->socket_cid, buf, len);
            if (n > 0) return n;
            if (n < 0) return -EIO;
            if (tcp_eof(e->socket_cid)) return 0;
            yield();
        }
    }

    /* Writable tmpfs file (read side). */
    if (e->kind == FD_TMPFS_RW) {
        long got = tmpfs_read(e->ramfs_index, e->offset, buf, len);
        if (got > 0) e->offset += (uint64_t)got;
        return got;
    }

    /* Writable OSFS-2 file (read side). */
    if (e->kind == FD_OSFS2_FILE) {
        long got = osfs2_read(e->osfs2_ino, e->offset, buf, len);
        if (got > 0) e->offset += (uint64_t)got;
        return got;
    }

    /* Chapter 99 — /proc snapshot.  Slice the pre-rendered
     * buffer at the current offset; EOF when offset reaches
     * procfs_len. */
    if (e->kind == FD_PROCFS) {
        if (!e->procfs_buf) return 0;
        if (e->offset >= e->procfs_len) return 0;
        size_t remaining = (size_t)(e->procfs_len - e->offset);
        size_t to_copy   = len < remaining ? len : remaining;
        uint8_t *dst = (uint8_t *)buf;
        const uint8_t *src = (const uint8_t *)e->procfs_buf + e->offset;
        for (size_t i = 0; i < to_copy; i++) dst[i] = src[i];
        e->offset += (uint64_t)to_copy;
        return (long)to_copy;
    }

    /* OSFS-1 disk-backed file. */
    if (e->osfs_size != 0) {
        long got = osfs_read(e->osfs_start, e->osfs_size,
                             e->offset, buf, len);
        if (got > 0) e->offset += (uint64_t)got;
        return got;
    }

    /* Console fd 0 = keyboard read.  Two modes:
     *   COOKED (default): line-buffered with local echo and
     *     local backspace handling.  Returns when Enter is
     *     pressed (newline appended to buffer) or when the user
     *     buffer fills, whichever comes first.
     *   RAW (per-thread tty_raw flag, see SYS_TTY_RAW): one byte
     *     at a time, no echo, no buffering.  Returns 1 byte as
     *     soon as the PL011 RX FIFO has one available.  Used by
     *     the shell line editor to do its own ESC-sequence
     *     parsing for arrow-key history.
     * fd 1/2 (stdout/stderr) reads return 0 (EOF). */
    if (e->ramfs_index < 0) {
        if (fd != 0) return 0;
        char *dst = (char *)buf;

        if (t->tty_raw) {
            /* Raw single-byte read.  Always returns exactly 1
             * byte (we never wake spuriously without one). */
            char c;
            while (!console_try_getc(&c)) yield();
            dst[0] = c;
            return 1;
        }

        size_t n = 0;
        while (n < len) {
            char c;
            while (!console_try_getc(&c)) yield();

            /* Ctrl-C in cooked mode: deliver SIGINT to the
             * foreground PID (set by the shell around wait())
             * and short-circuit the read with -EINTR.  The
             * thread that's actually doing the read may BE the
             * foreground PID (e.g. cat reading from stdin while
             * shell waits) — in which case the syscall-return
             * signal check fires next and the thread exits.
             * If g_fg_pid is unset (no foreground), we still
             * consume the byte but don't deliver anything. */
            if (c == 0x03) {
                int fg = thread_get_fg_pid();
                if (fg > 0) thread_signal_pid(fg, SIGINT);
                return -EINTR;
            }

            if (c == '\r' || c == '\n') {
                serial_putc('\r');
                serial_putc('\n');
                dst[n++] = '\n';
                return (long)n;
            }
            if (c == 0x7f || c == 0x08) {
                if (n > 0) {
                    n--;
                    serial_putc('\b');
                    serial_putc(' ');
                    serial_putc('\b');
                }
                continue;
            }
            serial_putc(c);
            dst[n++] = c;
        }
        return (long)n;
    }

    const struct ramfs_file *f = &g_ramfs[e->ramfs_index];
    size_t f_size = ramfs_size(f);
    if (e->offset >= f_size) return 0;

    size_t remaining = f_size - (size_t)e->offset;
    size_t to_copy   = len < remaining ? len : remaining;
    /* TODO(milestone 11): copy_to_user with bounds-check. */
    uint8_t *dst = (uint8_t *)buf;
    const uint8_t *src = f->data + e->offset;
    for (size_t i = 0; i < to_copy; i++) dst[i] = src[i];

    e->offset += to_copy;
    return (long)to_copy;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    struct thread *t = thread_current();
    struct fd_entry *e = &t->fdt->fds[fd];
    if (!e->in_use) return -EBADF;

    /* Closing fd 0/1/2 is a no-op success (matches POSIX). */
    if (fd >= 3) {
        /* Drop pipe refcount before clearing the slot. */
        if (e->kind == FD_PIPE_R && e->pipe) pipe_unref(e->pipe, PIPE_REF_R);
        else if (e->kind == FD_PIPE_W && e->pipe) pipe_unref(e->pipe, PIPE_REF_W);
        else if (e->kind == FD_SOCKET && e->socket_cid >= 0)
            tcp_close(e->socket_cid);
        else if (e->kind == FD_PTY_MASTER && e->pty) pty_close_master(e->pty);
        else if (e->kind == FD_PTY_SLAVE  && e->pty) pty_close_slave(e->pty);
        else if (e->kind == FD_PROCFS && e->procfs_buf) {
            /* Free the snapshot allocated at vfs_open time. */
            kfree(e->procfs_buf);
        }
        e->in_use      = 0;
        e->kind        = FD_CONSOLE;
        e->offset      = 0;
        e->ramfs_index = -1;
        e->osfs_start  = 0;
        e->osfs_size   = 0;
        e->pipe        = NULL;
        e->socket_cid  = -1;
        e->pty         = NULL;
        e->osfs2_ino   = 0;
        e->procfs_buf  = NULL;
        e->procfs_len  = 0;
    }
    return 0;
}

void vfs_close_all(struct thread *t)
{
    if (!t) return;
    /* Chapter 93 — drop our reference to the (possibly shared)
     * fd_table.  fd_table_unref does the actual close work
     * only if we held the LAST reference; threads that share
     * the table via CLONE_FILES still need their fds open and
     * MUST observe identical behaviour to fork+exit (where
     * vfs_close_all dropped one of N copies).
     *
     * After this call t->fdt is stale — we deliberately leave
     * the field non-NULL so accidental post-exit use shows up
     * as a use-after-free in kasan-style debug builds rather
     * than a NULL deref.  Live code never touches t->fdt
     * after thread_exit -> reaper. */
    fd_table_unref(t->fdt);
}

int vfs_alloc_socket_fd(int cid)
{
    struct thread *t = thread_current();
    for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &t->fdt->fds[fd];
        if (!e->in_use) {
            e->in_use      = 1;
            e->kind        = FD_SOCKET;
            e->offset      = 0;
            e->ramfs_index = -1;
            e->osfs_start  = 0;
            e->osfs_size   = 0;
            e->pipe        = NULL;
            e->socket_cid  = cid;
            e->osfs2_ino   = 0;
            return fd;
        }
    }
    return -EMFILE;
}

int vfs_lookup(const char *path, const uint8_t **data, size_t *size)
{
    if (!path || !data || !size) return -EINVAL_VFS;
    int idx = ramfs_lookup(path);
    if (idx < 0) return -ENOENT_VFS;
    *data = g_ramfs[idx].data;
    *size = ramfs_size(&g_ramfs[idx]);
    return 0;
}

/* Whole-file read into a freshly-allocated kheap buffer.  Handles
 * both the embedded ramfs and OSFS-1 disk mounts.  Caller kfree's. */
int vfs_load(const char *path, uint8_t **out_buf, size_t *out_size)
{
    if (!path || !out_buf || !out_size) return -EINVAL_VFS;
    *out_buf  = NULL;
    *out_size = 0;

    /* OSFS dispatch: /mnt/<name> or /bin/<name>. */
    const char *bare = NULL;
    if (path_starts_with(path, "/mnt/")) bare = path + 5;
    else if (path_starts_with(path, "/bin/")) bare = path + 5;

    if (bare) {
        uint32_t start = 0, sz = 0;
        if (osfs_lookup(bare, &start, &sz) != 0)
            return -ENOENT_VFS;
        uint8_t *buf = (uint8_t *)kmalloc((size_t)sz);
        if (!buf) return -ENOMEM_VFS;
        long got = osfs_read(start, sz, 0, buf, (size_t)sz);
        if (got < 0 || (uint32_t)got != sz) {
            kfree(buf);
            return -EIO;
        }
        *out_buf  = buf;
        *out_size = (size_t)sz;
        return 0;
    }

    /* /data/<name> -> writable OSFS-2 disk mount.  Whole-file
     * load mirrors the OSFS-1 path: allocate a kheap buffer
     * sized to the inode's `size`, then call osfs2_read at
     * offset 0. */
    if (path_starts_with(path, "/data/")) {
        if (!osfs2_present()) return -ENOENT_VFS;
        const char *bare2 = path + 6;
        uint32_t ino = osfs2_lookup(bare2);
        if (ino == 0) return -ENOENT_VFS;
        uint32_t sz = osfs2_size(ino);
        uint8_t *buf = (uint8_t *)kmalloc((size_t)sz);
        if (!buf && sz > 0) return -ENOMEM_VFS;
        if (sz > 0) {
            long got = osfs2_read(ino, 0, buf, (size_t)sz);
            if (got < 0 || (uint32_t)got != sz) {
                kfree(buf);
                return -EIO;
            }
        }
        *out_buf  = buf;
        *out_size = (size_t)sz;
        return 0;
    }

    /* Ramfs path: copy out of the embedded blob into a fresh
     * buffer so the caller's kfree() story is uniform. */
    int idx = ramfs_lookup(path);
    if (idx < 0) return -ENOENT_VFS;
    size_t sz = ramfs_size(&g_ramfs[idx]);
    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf) return -ENOMEM_VFS;
    const uint8_t *src = g_ramfs[idx].data;
    for (size_t i = 0; i < sz; i++) buf[i] = src[i];
    *out_buf  = buf;
    *out_size = sz;
    return 0;
}

/* ------------------------------------------------------------------
 * Directory enumeration.
 *
 * We expose a single flat namespace: ramfs entries first (with
 * their full path, e.g. "/motd"), followed by OSFS entries (with
 * the "/mnt/" prefix prepended -- /bin/<name> is just an alias for
 * the same OSFS file, not a separate mount).  Walked by `/bin/ls`
 * via SYS_LISTDIR.
 * ------------------------------------------------------------------ */
long vfs_listdir(int idx, char *name, size_t cap, uint32_t *size_out)
{
    if (!name || !size_out || cap < 1) return -EINVAL_VFS;
    if (idx < 0) return -EINVAL_VFS;

    /* Ramfs slot. */
    if ((size_t)idx < RAMFS_COUNT) {
        const struct ramfs_file *f = &g_ramfs[idx];
        const char *src = f->name;
        size_t i = 0;
        while (src[i] && i + 1 < cap) { name[i] = src[i]; i++; }
        name[i] = '\0';
        *size_out = (uint32_t)ramfs_size(f);
        return (long)i;
    }

    /* OSFS slot — index 0 starts at offset RAMFS_COUNT. */
    size_t osfs_idx = (size_t)idx - RAMFS_COUNT;
    size_t osfs_n   = osfs_present() ? osfs_file_count() : 0;
    if (osfs_idx < osfs_n) {
        const struct osfs_dirent *e = osfs_dirent_at(osfs_idx);
        if (!e) return -ENOENT_VFS;
        static const char prefix[] = "/mnt/";
        size_t i = 0;
        for (size_t k = 0; prefix[k] && i + 1 < cap; k++) name[i++] = prefix[k];
        for (size_t k = 0; k < OSFS_NAME_MAX && e->name[k] && i + 1 < cap; k++)
            name[i++] = e->name[k];
        name[i] = '\0';
        *size_out = e->size_bytes;
        return (long)i;
    }

    /* OSFS-2 slot — walks the root directory of the writable
     * mount.  Index counts only non-empty dirents, but we need
     * a stable mapping from the linear `idx` SYS_LISTDIR uses to
     * the dirent index inside OSFS-2 (which may have holes left
     * by unlinks).  We rebuild the mapping each call: cheap
     * since the root directory is a single 4 KiB block. */
    size_t after_osfs = (size_t)idx - RAMFS_COUNT - osfs_n;
    if (osfs2_present()) {
        uint32_t walk = 0;
        size_t   skipped = 0;
        char     name2[OSFS2_NAME_MAX];
        uint32_t size2 = 0;
        while (osfs2_listdir(&walk, name2, sizeof(name2), &size2)) {
            if (skipped == after_osfs) {
                static const char dprefix[] = "/data/";
                size_t j = 0;
                for (size_t k = 0; dprefix[k] && j + 1 < cap; k++)
                    name[j++] = dprefix[k];
                for (size_t k = 0; name2[k] && j + 1 < cap; k++)
                    name[j++] = name2[k];
                name[j] = '\0';
                *size_out = size2;
                return (long)j;
            }
            skipped++;
        }
        /* Fall through into tmpfs after exhausting OSFS-2 dirents. */
        after_osfs -= skipped;
    }

    /* Tmpfs slot — `after_osfs` has already been decremented past
     * any OSFS-2 entries we walked above, so it indexes directly
     * into the tmpfs file table. */
    size_t tmp_idx = after_osfs;
    int tcount = tmpfs_count();
    if (tmp_idx >= (size_t)tcount) return -ENOENT_VFS;
    char tname[TMPFS_MAX_NAME];
    uint32_t tsize = 0;
    int tn = tmpfs_listdir((int)tmp_idx, tname, sizeof(tname), &tsize);
    if (tn < 0) return -ENOENT_VFS;
    static const char tprefix[] = "/tmp/";
    size_t j = 0;
    for (size_t k = 0; tprefix[k] && j + 1 < cap; k++) name[j++] = tprefix[k];
    for (int k = 0; k < tn && j + 1 < cap; k++) name[j++] = tname[k];
    name[j] = '\0';
    *size_out = tsize;
    return (long)j;
}
