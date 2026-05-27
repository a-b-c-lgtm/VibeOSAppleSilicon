/*
 * kernel/core/vfs.c — minimal ramfs-backed VFS.
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
#include "srv.h"
#include "userfs.h"
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
 * embedded blobs.  They are now stored on the
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

/* Chapter 91 — public ramfs blob accessor.  Used by the in-
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
 * Chapter 132 — struct fs_ops adapter for the embedded ramfs.
 *
 * The ramfs is the catchall root mount.  Every path that doesn't
 * match a more-specific mount (/tmp, /proc, /data, /mnt, /bin,
 * /srv, …) resolves here.  Misses (e.g. "/foo" with no entry
 * in g_ramfs) return -ENOENT_VFS exactly as the legacy
 * `ramfs_lookup(name) < 0` branch did.
 *
 * Read-only by design.  rel is the full path with leading slash
 * because vfs_resolve returns the unstripped path for root
 * mounts (see the special case in vfs_resolve).
 * ------------------------------------------------------------------ */
static long ramfs_op_open(void *cookie, const char *rel, int flags,
                          struct fd_entry *out)
{
    (void)cookie; (void)flags;
    if (!out || !rel) return -EINVAL_VFS;
    int idx = ramfs_lookup(rel);
    if (idx < 0) return -ENOENT_VFS;
    out->kind        = FD_FILE;
    out->offset      = 0;
    out->ramfs_index = idx;
    out->osfs_start  = 0;
    out->osfs_size   = 0;
    out->pipe        = NULL;
    out->socket_cid  = -1;
    out->pty         = NULL;
    out->osfs2_ino   = 0;
    out->srv_l       = NULL;
    out->srv_c       = NULL;
    out->srv_is_service = 0;
    out->userfs_ch     = NULL;
    out->userfs_handle = 0;
    return 0;
}

static long ramfs_op_read(void *cookie, struct fd_entry *e,
                          void *buf, size_t n)
{
    (void)cookie;
    if (!e || !buf) return -EINVAL_VFS;
    if (e->ramfs_index < 0 || (size_t)e->ramfs_index >= RAMFS_COUNT)
        return -EBADF;
    const struct ramfs_file *f = &g_ramfs[e->ramfs_index];
    size_t sz = ramfs_size(f);
    if (e->offset >= sz) return 0;
    size_t left = sz - (size_t)e->offset;
    if (n > left) n = left;
    const uint8_t *src = f->data + e->offset;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    e->offset += (uint64_t)n;
    return (long)n;
}

static long ramfs_op_close(void *cookie, struct fd_entry *e)
{
    (void)cookie; (void)e;
    return 0;
}

static int ramfs_op_listdir(void *cookie, const char *rel, int idx,
                            char *name, size_t cap, uint32_t *type)
{
    (void)cookie;
    /* Root mount: rel == "/" or rel == path-of-mountpoint.  We
     * only enumerate when the caller asked for "/" exactly; any
     * deeper path is a miss because ramfs has no subdirs. */
    if (!rel || (rel[0] != '/' || (rel[1] != '\0'))) return -1;
    if (idx < 0 || (size_t)idx >= RAMFS_COUNT) return -1;
    const char *src = g_ramfs[idx].name;
    /* Names are stored with a leading slash (e.g. "/motd"); the
     * listdir convention is bare names, so skip past the '/'. */
    if (*src == '/') src++;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) name[i] = src[i];
    name[i] = '\0';
    if (type) *type = 0;
    return (int)i;
}

static int ramfs_op_is_dir(void *cookie, const char *rel)
{
    (void)cookie;
    if (!rel || (rel[0] == '/' && rel[1] == '\0')) return 1;
    return 0;
}

static long ramfs_op_load(void *cookie, const char *rel,
                          uint8_t **out_buf, size_t *out_size)
{
    (void)cookie;
    if (!out_buf || !out_size) return -EINVAL_VFS;
    *out_buf = NULL; *out_size = 0;
    int idx = ramfs_lookup(rel);
    if (idx < 0) return -ENOENT_VFS;
    size_t sz = ramfs_size(&g_ramfs[idx]);
    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf && sz > 0) return -ENOMEM_VFS;
    const uint8_t *src = g_ramfs[idx].data;
    for (size_t i = 0; i < sz; i++) buf[i] = src[i];
    *out_buf = buf;
    *out_size = sz;
    return 0;
}

static const struct fs_ops g_ramfs_root_ops = {
    .open    = ramfs_op_open,
    .read    = ramfs_op_read,
    .write   = NULL,
    .close   = ramfs_op_close,
    .lseek   = NULL,
    .listdir = ramfs_op_listdir,
    .unlink  = NULL,
    .mkdir   = NULL,
    .is_dir  = ramfs_op_is_dir,
    .load    = ramfs_op_load,
};

static void ramfs_register_root_mount(void)
{
    (void)vfs_mount_register("/", &g_ramfs_root_ops, NULL, MOUNT_RO);
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
    /* Chapter 132 — register mount-table entries for every
     * kernel filesystem that has been ported to the fs_ops
     * vtable.  Each step of the section-16 refactor adds one
     * registration here and deletes the matching prefix branch
     * in vfs_open / syscall.c.  Chapter 145 dropped procfs
     * from this list — /proc is now served by /bin/procd via
     * a userfs mount installed at boot by init's supervisor. */
    tmpfs_register_mount();
    osfs1_register_mount();   /* /mnt + /bin (MOUNT_RO) */
    osfs2_register_mount();   /* /data (writable) */
    ramfs_register_root_mount();  /* "/" catchall (MOUNT_RO) */
}

void vfs_init_fdtable(struct thread *t)
{
    /* Chapter 94 — `t->fdt` is now a separately-allocated,
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
        ft->fds[i].srv_l       = NULL;
        ft->fds[i].srv_c       = NULL;
        ft->fds[i].srv_is_service = 0;
        ft->fds[i].userfs_ch     = NULL;
        ft->fds[i].userfs_handle = 0;
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
        ft->fds[i].srv_l       = NULL;
        ft->fds[i].srv_c       = NULL;
        ft->fds[i].srv_is_service = 0;
        ft->fds[i].userfs_ch     = NULL;
        ft->fds[i].userfs_handle = 0;
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
        else if ((e->kind == FD_SOCKET ||
                  e->kind == FD_SOCKET_LISTEN) && e->socket_cid >= 0)
            tcp_close(e->socket_cid);
        else if (e->kind == FD_PTY_MASTER && e->pty)
            pty_close_master(e->pty);
        else if (e->kind == FD_PTY_SLAVE && e->pty)
            pty_close_slave(e->pty);
        else if (e->kind == FD_SRV_LISTEN && e->srv_l)
            srv_unref_listen(e->srv_l);
        else if (e->kind == FD_SRV_CONN && e->srv_c)
            srv_unref_conn(e->srv_c, e->srv_is_service);
        else if (e->kind == FD_USERFS_FILE && e->userfs_ch)
            (void)g_userfs_ops.close(e->userfs_ch, e);
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

/* ------------------------------------------------------------------
 * Chapter 132 — mount table + vfs_resolve.
 *
 * Step 1 of the section-16 refactor: introduce the types and the
 * resolver.  No callers yet — the prefix ladders below this point
 * keep working unchanged.  Subsequent steps port one filesystem
 * driver at a time (procfs, tmpfs, OSFS-1, OSFS-2, ramfs) and
 * delete the corresponding branch from the ladder.
 * ------------------------------------------------------------------ */

static struct mount g_mounts[MOUNT_MAX];
static int          g_mounts_n;

static size_t k_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int vfs_mount_register(const char *prefix, const struct fs_ops *ops,
                       void *cookie, uint32_t flags)
{
    if (!prefix || !ops) return -EINVAL_VFS;
    if (prefix[0] != '/') return -EINVAL_VFS;
    size_t plen = k_strlen(prefix);
    /* "/" is the only allowed trailing-slash prefix; anything else
     * with a trailing slash is malformed (use "/data", not
     * "/data/").  This keeps vfs_resolve's boundary check simple. */
    if (plen > 1 && prefix[plen - 1] == '/') return -EINVAL_VFS;
    if (g_mounts_n >= MOUNT_MAX) return -ENOSPC;

    g_mounts[g_mounts_n].prefix = prefix;
    g_mounts[g_mounts_n].ops    = ops;
    g_mounts[g_mounts_n].cookie = cookie;
    g_mounts[g_mounts_n].flags  = flags;
    g_mounts_n++;
    return 0;
}

const struct mount *vfs_resolve(const char *path, const char **rel_out)
{
    if (!path) return NULL;
    const struct mount *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < g_mounts_n; i++) {
        const struct mount *m = &g_mounts[i];
        size_t plen = k_strlen(m->prefix);
        /* "/" matches anything but loses every tie to a longer
         * prefix.  For any other prefix we require the next char
         * in path to be '\0' or '/' so "/data" does not match
         * "/database". */
        if (plen == 1 && m->prefix[0] == '/') {
            if (best_len == 0) { best = m; best_len = 1; }
            continue;
        }
        if (!path_starts_with(path, m->prefix)) continue;
        char next = path[plen];
        if (next != '\0' && next != '/') continue;
        if (plen > best_len) { best = m; best_len = plen; }
    }

    if (!best) {
        if (rel_out) *rel_out = path;
        return NULL;
    }
    if (rel_out) {
        if (best_len == 1 && best->prefix[0] == '/') {
            /* Root mount: rel is the full path. */
            *rel_out = path;
        } else {
            /* Non-root mount: rel begins at the first char past
             * the prefix.  For path == prefix exactly this is the
             * trailing '\0' which makes rel an empty string. */
            *rel_out = path + best_len;
        }
    }
    return best;
}

int vfs_mount_count(void) { return g_mounts_n; }

const struct mount *vfs_mount_at(int idx)
{
    if (idx < 0 || idx >= g_mounts_n) return NULL;
    return &g_mounts[idx];
}

int vfs_mount_remove(int idx)
{
    if (idx < 0 || idx >= g_mounts_n) return -EINVAL_VFS;
    /* Compact: shift every later slot down by one.  The
     * mount-table is small (MOUNT_MAX=16) so an in-place
     * memmove is fine.  We deliberately do NOT preserve the
     * old indices — userspace only ever sees indices through
     * SYS_MOUNTS snapshots, so a concurrent shift is no
     * worse than two snapshots taken across an unrelated
     * mount/umount pair. */
    for (int i = idx; i + 1 < g_mounts_n; i++) {
        g_mounts[i] = g_mounts[i + 1];
    }
    g_mounts_n--;
    g_mounts[g_mounts_n].prefix = NULL;
    g_mounts[g_mounts_n].ops    = NULL;
    g_mounts[g_mounts_n].cookie = NULL;
    g_mounts[g_mounts_n].flags  = 0;
    return 0;
}

int vfs_open(const char *name, int flags)
{
    if (!name) return -EINVAL_VFS;

    /* Chapter 132 — vtable dispatch.  Resolve `name` against the
     * mount table; if a registered mount with an `open` method
     * covers it, hand the open off to the driver and the legacy
     * prefix ladder below is bypassed entirely.  Filesystems not
     * yet ported (today: tmpfs, OSFS-2, OSFS-1, root ramfs) fall
     * through.  Each subsequent chapter-113 step registers one
     * more mount here and deletes the matching branch below. */
    {
        const char *rel = NULL;
        const struct mount *m = vfs_resolve(name, &rel);
        if (m && m->ops && m->ops->open) {
            if ((m->flags & MOUNT_RO) &&
                ((flags & O_WRONLY) || (flags & O_RDWR) ||
                 (flags & O_CREAT)  || (flags & O_TRUNC)))
                return -EROFS_VFS;
            struct thread *t = thread_current();
            for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
                if (!t->fdt->fds[fd].in_use) {
                    long r = m->ops->open(m->cookie, rel, flags,
                                          &t->fdt->fds[fd]);
                    if (r < 0) return r;
                    t->fdt->fds[fd].in_use = 1;
                    return fd;
                }
            }
            /* Table full — the driver's open didn't run, so no
             * cleanup needed. */
            return -EMFILE;
        }
    }

    /* /tmp/<name>: handled by the chapter-113 vtable dispatch at
     * the top of this function (tmpfs_fs_ops, registered by
     * tmpfs_register_mount in vfs_init).  No legacy branch
     * remains here. */

    /* /data/<name>: handled by the chapter-113 vtable dispatch
     * at the top of this function (osfs2_fs_ops, registered by
     * osfs2_register_mount in vfs_init).  No legacy branch
     * remains here. */

    /* /srv/<name> -> chapter 112 named-IPC connect.
     * open("/srv/foo", O_RDWR) becomes srv_connect("/srv/foo")
     * internally so apps that don't know about IPC can still
     * talk to services through read/write/close.  Returns the
     * client-end fd; the service obtains its end via
     * SYS_SRV_ACCEPT after SYS_SRV_BIND.  We intentionally
     * route open() through srv_connect rather than srv_bind:
     * services use the syscall directly, while open() is the
     * client API.  The flag argument is parsed but only
     * O_RDWR makes sense (datagrams are bidirectional). */
    if (path_starts_with(name, "/srv/")) {
        int err = 0;
        struct srv_conn *c = srv_connect(name, &err);
        if (!c) return err ? err : -ENOENT_VFS;
        int fd = vfs_alloc_srv_conn_fd(c, /*is_service_end=*/0);
        if (fd < 0) {
            srv_unref_conn(c, /*is_service_end=*/0);
            return fd;
        }
        return fd;
    }

    /* /proc/<path>: handled by the chapter-113 vtable dispatch
     * at the top of this function (procfs_fs_ops, registered by
     * procfs_register_mount in vfs_init).  No legacy branch
     * remains here. */

    /* /mnt/<name> and /bin/<name>: handled by the chapter-113
     * vtable dispatch (osfs1_fs_ops, registered twice by
     * osfs1_register_mount in vfs_init — once per prefix).  No
     * legacy branch remains here. */

    /* Ramfs catchall ("/") is handled by the chapter-113 vtable
     * dispatch at the top of this function via g_ramfs_root_ops.
     * Any path that didn't match a more-specific mount AND
     * isn't in g_ramfs returns -ENOENT_VFS from ramfs_op_open. */
    return -ENOENT_VFS;
}

/* Open `name` directly into thread `t`'s slot `fd`, overwriting
 * whatever was there.  Used by sys_spawn_redir to wire a child's
 * fd 0 to a file before the child runs.  Same path resolution as
 * vfs_open (OSFS for /mnt|/bin, ramfs otherwise).  Returns 0 on
 * success or a negative errno; on failure `t->fdt->fds[fd]` is left
 * unchanged. */
int vfs_open_into(struct thread *t, int fd, const char *name, int flags)
{
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
        else if (slot->kind == FD_SRV_LISTEN && slot->srv_l) srv_unref_listen(slot->srv_l);
        else if (slot->kind == FD_SRV_CONN   && slot->srv_c) srv_unref_conn(slot->srv_c, slot->srv_is_service);
        slot->pipe = NULL;
        slot->pty  = NULL;
        slot->srv_l = NULL;
        slot->srv_c = NULL;
    }

    /* Chapter 132 \u2014 try the mount-table vtable first.  This
     * gives `vfs_open_into` the same dispatch as `vfs_open`, so
     * fork+exec-style redirections (sys_spawn_redir / dup2)
     * route through registered filesystems exactly as the public
     * open syscall does. */
    {
        const char *rel = NULL;
        const struct mount *m = vfs_resolve(name, &rel);
        if (m && m->ops && m->ops->open) {
            if ((m->flags & MOUNT_RO) &&
                ((flags & O_WRONLY) || (flags & O_RDWR) ||
                 (flags & O_CREAT)  || (flags & O_TRUNC)))
                return -EROFS_VFS;
            long r = m->ops->open(m->cookie, rel, flags, &t->fdt->fds[fd]);
            if (r < 0) return (int)r;
            t->fdt->fds[fd].in_use = 1;
            return 0;
        }
    }

    /* /mnt/, /bin/, /data/: handled by the chapter-113 vtable
     * dispatch above (osfs1_fs_ops / osfs2_fs_ops).  No legacy
     * branches remain here. */

    /* Ramfs catchall: routed via the chapter-113 vtable above. */
    return -ENOENT_VFS;
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

    /* Pty endpoints (chapter 79).  Both ends are read+write,
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
         * inbound bytes.
         *
         * Chapter 110: we tried a 1024-iter spin between yields
         * here as an "optimisation" (avoid scheduler ping-pong)
         * but it actively hurt the chapter-106b splice: browser
         * read() and httpd read() / write() are on different
         * threads, and the splice requires the OTHER thread to
         * run between every chunk to produce more bytes.  Holding
         * the CPU for 1024 polls starved httpd of scheduling
         * opportunities, turning a 600ms HN fetch into a 10s
         * stall.  Yield every iteration -- net_poll() is cheap
         * and the scheduler tax is much smaller than the
         * cross-thread latency.
         *
         * Order of the result checks matters:
         *   1. n > 0  -- bytes available, return them.
         *   2. tcp_eof  -- normal end-of-stream.  This branch
         *      ALSO catches the case where tcp_poll has already
         *      reaped the conn after a clean close (get_conn
         *      returns NULL, tcp_recv returns -1, tcp_eof
         *      returns 1).  Without this ordering, a userspace
         *      reader that loops read() past the final byte sees
         *      -EIO instead of 0; chapter 108 surfaced that
         *      because loopback often delivers the peer's FIN
         *      and our reap-pass in the SAME net_poll tick that
         *      hands the last bytes to the user.
         *   3. n < 0  -- genuine error (RST while conn still
         *      live).  EIO is the only thing we can offer here. */
        for (;;) {
            (void)net_poll();
            int n = tcp_recv(e->socket_cid, buf, len);
            if (n > 0) return n;
            if (tcp_eof(e->socket_cid)) return 0;
            if (n < 0) return -EIO;
            yield();
        }
    }

    /* Chapter 106: listening sockets are not readable.  The only
     * way to extract anything from them is SYS_SOCKET_ACCEPT.
     * Without this guard, a stray read() would fall through to
     * the ramfs path and dereference ramfs_index = -1. */
    if (e->kind == FD_SOCKET_LISTEN) return -EINVAL_VFS;

    /* Chapter 112 named-IPC.  Listen fds are read-only via
     * SYS_SRV_ACCEPT; only FD_SRV_CONN supports read(). */
    if (e->kind == FD_SRV_LISTEN) return -EINVAL_VFS;
    if (e->kind == FD_SRV_CONN) {
        if (!e->srv_c) return -EBADF;
        return srv_read(e->srv_c, e->srv_is_service, buf, len);
    }

    /* Chapter 140 — userspace filesystem-backed file.  All
     * read state (offset, daemon handle, channel) is on the
     * fd_entry; userfs_op_read marshals a P9_OP_READ across
     * the channel and returns the daemon's reply. */
    if (e->kind == FD_USERFS_FILE) {
        if (!e->userfs_ch) return -EBADF;
        return g_userfs_ops.read(e->userfs_ch, e, buf, len);
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
    /* TODO: copy_to_user with bounds-check. */
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
        else if ((e->kind == FD_SOCKET ||
                  e->kind == FD_SOCKET_LISTEN) && e->socket_cid >= 0)
            tcp_close(e->socket_cid);
        else if (e->kind == FD_PTY_MASTER && e->pty) pty_close_master(e->pty);
        else if (e->kind == FD_PTY_SLAVE  && e->pty) pty_close_slave(e->pty);
        else if (e->kind == FD_SRV_LISTEN && e->srv_l) srv_unref_listen(e->srv_l);
        else if (e->kind == FD_SRV_CONN   && e->srv_c) srv_unref_conn(e->srv_c, e->srv_is_service);
        else if (e->kind == FD_USERFS_FILE && e->userfs_ch) {
            /* Forward the close to the daemon so it can drop its
             * handle state; g_userfs_ops.close also decrements
             * the channel's open_fds counter. */
            (void)g_userfs_ops.close(e->userfs_ch, e);
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
        e->srv_l       = NULL;
        e->srv_c       = NULL;
        e->srv_is_service = 0;
        e->userfs_ch       = NULL;
        e->userfs_handle   = 0;
    }
    return 0;
}

/* ── Chapter 150 — POSIX lseek ─────────────────────────────
 *
 * Re-position `fd`'s offset.  Most fs drivers keep state in
 * `fd_entry->offset` (which vfs_read advances after each chunk),
 * so for them sys_lseek mutates the field directly.  Drivers
 * that own remote state -- today only userfs -- get a vtable
 * call via ops->lseek.
 *
 * SEEK_END requires knowing the file's size:
 *   - FD_FILE (OSFS-1)        : osfs_size already on the fd
 *   - FD_OSFS2_FILE           : osfs2_size(ino)
 *   - FD_TMPFS_RW             : tmpfs_size_of(idx)
 *   - FD_USERFS_FILE          : delegated to ops->lseek, which
 *                               today returns -EINVAL for SEEK_END
 *                               (will grow proper support when
 *                               the daemon protocol does)
 *
 * Pipes, ptys, sockets, the console, and IPC server fds are
 * not seekable -- return -ESPIPE per POSIX.
 */
long vfs_lseek(int fd, int64_t off, int whence)
{
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    struct thread *t = thread_current();
    struct fd_entry *e = &t->fdt->fds[fd];
    if (!e->in_use) return -EBADF;

    if (whence < 0 || whence > 2) return -EINVAL_VFS;

    switch (e->kind) {
    case FD_CONSOLE:
    case FD_PIPE_R:
    case FD_PIPE_W:
    case FD_SOCKET:
    case FD_SOCKET_LISTEN:
    case FD_PTY_MASTER:
    case FD_PTY_SLAVE:
    case FD_SRV_LISTEN:
    case FD_SRV_CONN:
        return -ESPIPE;

    case FD_USERFS_FILE:
        if (!e->userfs_ch) return -EBADF;
        return g_userfs_ops.lseek(e->userfs_ch, e, off, whence);

    case FD_FILE: {
        int64_t cur = (int64_t)e->offset;
        int64_t base = (whence == 0) ? 0
                     : (whence == 1) ? cur
                     : (int64_t)e->osfs_size;
        int64_t neu = base + off;
        if (neu < 0) return -EINVAL_VFS;
        e->offset = (uint64_t)neu;
        return (long)neu;
    }

    case FD_OSFS2_FILE: {
        int64_t cur = (int64_t)e->offset;
        int64_t base = (whence == 0) ? 0
                     : (whence == 1) ? cur
                     : (int64_t)osfs2_size(e->osfs2_ino);
        int64_t neu = base + off;
        if (neu < 0) return -EINVAL_VFS;
        e->offset = (uint64_t)neu;
        return (long)neu;
    }

    case FD_TMPFS_RW: {
        int64_t cur = (int64_t)e->offset;
        int64_t base = (whence == 0) ? 0
                     : (whence == 1) ? cur
                     : (int64_t)tmpfs_size_of(e->ramfs_index);
        int64_t neu = base + off;
        if (neu < 0) return -EINVAL_VFS;
        e->offset = (uint64_t)neu;
        return (long)neu;
    }
    }

    /* Unknown kind -- should not happen.  Be conservative. */
    return -EINVAL_VFS;
}

/* ── Chapter 153 — POSIX stat / fstat ───────────────────────
 *
 * Two operations:
 *   - vfs_stat_path(path, out)  reads metadata by name.  Uses
 *     ops->is_dir(rel) to distinguish directories from files
 *     without opening; for files, opens (via vfs_open) so the
 *     resulting fd_entry carries the size already-cached by
 *     the FS driver, then closes.
 *   - vfs_fstat(fd, out)  reads metadata straight off an open
 *     fd_entry.  No path resolution, no allocation, can't race
 *     with unlink.
 *
 * Size is read from the FS-specific field of fd_entry:
 *   FD_FILE        -> osfs_size            (OSFS-1, set at open)
 *   FD_OSFS2_FILE  -> osfs2_size(ino)      (writable disk fs)
 *   FD_TMPFS_RW    -> tmpfs_size_of(idx)   (writable in-mem fs)
 *   everything else (pipes, ptys, sockets, IPC, userfs, console)
 *                  -> size 0; mode reflects the kind below.
 *
 * mtime is sourced from the OSFS-2 inode when available; every
 * other path reports 0 (we don't track mtime for ramfs / tmpfs
 * yet -- adequate for the GCC / TCC / make use cases this
 * chapter unlocks).
 */
static void fill_size_from_fd_entry(const struct fd_entry *e,
                                    struct kstat *out)
{
    out->st_size = 0;
    out->st_mtime = 0;
    out->st_uid = 0;
    out->st_gid = 0;
    switch (e->kind) {
    case FD_FILE:
        out->st_size = e->osfs_size;
        break;
    case FD_OSFS2_FILE:
        out->st_size = osfs2_size(e->osfs2_ino);
        break;
    case FD_TMPFS_RW:
        out->st_size = tmpfs_size_of(e->ramfs_index);
        break;
    default:
        break;
    }
}

/* Chapter 178 — populate POSIX `st_dev` / `st_ino`.
 *
 * libiberty (fdmatch.c, getpwd.c) compares two stats for "same
 * file?" via `a.st_dev == b.st_dev && a.st_ino == b.st_ino`.
 * The pair has to be stable per file, but the actual values
 * are only meaningful relative to other stats on the same
 * filesystem.
 *
 * We assign one device number per fd_kind that backs a real
 * file, and a per-FS inode index:
 *   OSFS-1 -> dev=1, ino=osfs_start (directory-entry sector)
 *   OSFS-2 -> dev=2, ino=osfs2_ino
 *   tmpfs  -> dev=3, ino=ramfs_index + 1   (avoid 0)
 *   userfs -> dev=4, ino=userfs_handle
 *   pipes / sockets / consoles -> dev=0, ino=fd-table slot
 *
 * Path-based stat on directories (root / mount roots) leaves
 * dev/ino at 0; libiberty never compares directory stats. */
static void fill_dev_ino_from_fd_entry(const struct fd_entry *e,
                                       struct kstat *out)
{
    out->st_dev = 0;
    out->st_ino = 0;
    switch (e->kind) {
    case FD_FILE:
        out->st_dev = 1;
        out->st_ino = e->osfs_start;
        break;
    case FD_OSFS2_FILE:
        out->st_dev = 2;
        out->st_ino = e->osfs2_ino;
        break;
    case FD_TMPFS_RW:
        out->st_dev = 3;
        out->st_ino = (uint64_t)e->ramfs_index + 1u;
        break;
    case FD_USERFS_FILE:
        out->st_dev = 4;
        out->st_ino = e->userfs_handle;
        break;
    default:
        break;
    }
}

static uint32_t mode_for_fd_kind(enum fd_kind k)
{
    switch (k) {
    case FD_CONSOLE:        return S_IFCHR_K | 0666u;
    case FD_PIPE_R:
    case FD_PIPE_W:         return S_IFIFO_K | 0600u;
    case FD_SOCKET:
    case FD_SOCKET_LISTEN:  return S_IFSOCK_K | 0600u;
    case FD_PTY_MASTER:
    case FD_PTY_SLAVE:      return S_IFCHR_K | 0620u;
    case FD_SRV_LISTEN:
    case FD_SRV_CONN:       return S_IFSOCK_K | 0600u;
    case FD_FILE:           return S_IFREG_K | 0444u;
    case FD_OSFS2_FILE:     return S_IFREG_K | 0644u;
    case FD_TMPFS_RW:       return S_IFREG_K | 0644u;
    case FD_USERFS_FILE:    return S_IFREG_K | 0644u;
    default:                return S_IFREG_K | 0644u;
    }
}

long vfs_stat_path(const char *path, struct kstat *out)
{
    if (!path || !out) return -EINVAL_VFS;

    /* Root and bare mount prefixes are always directories. */
    if (path[0] == '/' && path[1] == '\0') {
        out->st_mode = S_IFDIR_K | 0755u;
        out->st_size = 0;
        out->st_mtime = 0;
        out->_pad = 0;
        out->st_dev = 0;
        out->st_ino = 0;
        out->st_uid = 0;
        out->st_gid = 0;
        return 0;
    }

    const char *rel = NULL;
    const struct mount *m = vfs_resolve(path, &rel);
    if (!m || !m->ops) return -ENOENT_VFS;

    /* Empty `rel` means the path WAS the mount prefix itself
     * (e.g. "/data" against the /data mount).  Treat as the
     * mount's root directory. */
    if (!rel || rel[0] == '\0' ||
        (rel[0] == '/' && rel[1] == '\0')) {
        out->st_mode = S_IFDIR_K | 0755u;
        out->st_size = 0;
        out->st_mtime = 0;
        out->_pad = 0;
        out->st_dev = 0;
        out->st_ino = 0;
        out->st_uid = 0;
        out->st_gid = 0;
        return 0;
    }

    /* Ask the filesystem whether it's a directory.  Drivers
     * that don't implement is_dir get treated as "file" and
     * fall through to the open-and-read-size path; if open
     * fails we propagate the error. */
    if (m->ops->is_dir) {
        int isd = m->ops->is_dir(m->cookie, rel);
        if (isd == 1) {
            out->st_mode = S_IFDIR_K | 0755u;
            out->st_size = 0;
            out->st_mtime = 0;
            out->_pad = 0;
            out->st_dev = 0;
            out->st_ino = 0;
            out->st_uid = 0;
            out->st_gid = 0;
            return 0;
        }
        /* isd == 0 means "file"; isd < 0 means lookup failure --
         * we still try open below, which will return the same
         * errno from a richer path. */
    }

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return fd;

    struct thread *t = thread_current();
    struct fd_entry *e = &t->fdt->fds[fd];
    out->st_mode = mode_for_fd_kind(e->kind);
    out->_pad = 0;
    fill_size_from_fd_entry(e, out);
    fill_dev_ino_from_fd_entry(e, out);
    vfs_close(fd);
    return 0;
}

long vfs_fstat(int fd, struct kstat *out)
{
    if (!out) return -EINVAL_VFS;
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    struct thread *t = thread_current();
    struct fd_entry *e = &t->fdt->fds[fd];
    if (!e->in_use) return -EBADF;

    out->st_mode = mode_for_fd_kind(e->kind);
    out->_pad = 0;
    fill_size_from_fd_entry(e, out);
    fill_dev_ino_from_fd_entry(e, out);
    return 0;
}

void vfs_close_all(struct thread *t)
{
    if (!t) return;
    /* Chapter 94 — drop our reference to the (possibly shared)
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

/* Chapter 106 -- allocate a fresh fd that wraps a TCP_LISTEN
 * conn slot.  Identical to vfs_alloc_socket_fd except for the
 * `kind`: the read/write paths reject FD_SOCKET_LISTEN with
 * -EINVAL, and SYS_SOCKET_ACCEPT requires this kind. */
int vfs_alloc_listen_fd(int cid)
{
    struct thread *t = thread_current();
    for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &t->fdt->fds[fd];
        if (!e->in_use) {
            e->in_use      = 1;
            e->kind        = FD_SOCKET_LISTEN;
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

/* Chapter 112 — allocate a fresh fd that wraps a /srv/<name>
 * listener.  Read/write reject FD_SRV_LISTEN; only
 * SYS_SRV_ACCEPT and close are valid.  Returns the new fd,
 * or -EMFILE if the table is full. */
int vfs_alloc_srv_listen_fd(struct srv_listen *ls)
{
    if (!ls) return -EINVAL_VFS;
    struct thread *t = thread_current();
    for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &t->fdt->fds[fd];
        if (!e->in_use) {
            e->in_use         = 1;
            e->kind           = FD_SRV_LISTEN;
            e->offset         = 0;
            e->ramfs_index    = -1;
            e->osfs_start     = 0;
            e->osfs_size      = 0;
            e->pipe           = NULL;
            e->socket_cid     = -1;
            e->pty            = NULL;
            e->osfs2_ino      = 0;
            e->srv_l          = ls;
            e->srv_c          = NULL;
            e->srv_is_service = 0;
            return fd;
        }
    }
    return -EMFILE;
}

/* Chapter 112 — allocate a fresh fd that wraps a /srv
 * connected conn.  `is_service_end` distinguishes the
 * accepted (service) side from the connect (client) side;
 * that bit picks which queue read/write touch.  Returns
 * the new fd, or -EMFILE. */
int vfs_alloc_srv_conn_fd(struct srv_conn *c, int is_service_end)
{
    if (!c) return -EINVAL_VFS;
    struct thread *t = thread_current();
    for (int fd = 3; fd < FD_TABLE_SIZE; fd++) {
        struct fd_entry *e = &t->fdt->fds[fd];
        if (!e->in_use) {
            e->in_use         = 1;
            e->kind           = FD_SRV_CONN;
            e->offset         = 0;
            e->ramfs_index    = -1;
            e->osfs_start     = 0;
            e->osfs_size      = 0;
            e->pipe           = NULL;
            e->socket_cid     = -1;
            e->pty            = NULL;
            e->osfs2_ino      = 0;
            e->srv_l          = NULL;
            e->srv_c          = c;
            e->srv_is_service = is_service_end ? 1 : 0;
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

    /* Chapter 132 — try the mount-table vtable first.  Any
     * filesystem that has been ported to fs_ops and implements
     * `load` handles the whole-file read here.  Drivers without
     * a `load` op (e.g. /srv) fall through to the legacy ramfs
     * branch below. */
    {
        const char *rel = NULL;
        const struct mount *m = vfs_resolve(path, &rel);
        if (m && m->ops && m->ops->load) {
            return m->ops->load(m->cookie, rel, out_buf, out_size);
        }
    }

    /* /mnt/<name>, /bin/<name>, /data/<name>: handled by the
     * chapter-113 vtable dispatch above. */

    /* Ramfs catchall: also routed via the chapter-113 vtable
     * above (g_ramfs_root_ops::load).  Nothing left to do here —
     * a miss already returned -ENOENT_VFS from ramfs_op_load. */
    return -ENOENT_VFS;
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
