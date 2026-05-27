/*
 * kernel/core/vfs.h — minimal virtual file system layer.
 *
 * The VFS is a thin indirection between syscalls and the
 * underlying file-system implementations.  Initially there
 * is exactly one mounted FS — `ramfs` — which serves a fixed set
 * of files baked into the kernel image at build time.  The
 * indirection still earns its keep: when chapter 17 lands a
 * disk-backed FS it slots into the same `struct vfs_ops` table
 * without touching the syscall layer.
 *
 * Files are read-only.  Opens that name a non-existent file fail
 * with -ENOENT.  Writes to fd > 2 fail with -EROFS.  Stdout (fd
 * 1) and stderr (fd 2) keep going to the kernel console via
 * SYS_WRITE as before.
 *
 * File descriptors are per-thread (later: per-process).  Each
 * thread carries an fd table of FD_TABLE_SIZE slots; descriptors
 * 0..FD_TABLE_SIZE-1 are valid.  Slots 0 (stdin), 1 (stdout), 2
 * (stderr) are reserved and pre-occupied; opens hand out the
 * lowest free slot from index 3 upwards, matching POSIX.
 */
#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>
#include "../arch/spinlock.h"

#define FD_TABLE_SIZE   16
#define VFS_MAX_NAME    64

/* Errno values returned from VFS calls. */
#define ENOENT_VFS  2
#define EINTR        4
#define EBADF       9
#define ENOMEM_VFS  12
#define EINVAL_VFS  22
#define EMFILE      24
#define ENOSPC      28
#define EROFS       30
#define EIO          5
/* Chapter 106: returned by SYS_SOCKET_LISTEN when another
 * conn already owns the requested port.  Matches POSIX. */
#define EADDRINUSE  98
/* Chapter 132 — returned by the mount-table dispatcher when a
 * caller writes to a MOUNT_RO mount.  Same numeric value as
 * the existing EROFS so userspace callers comparing against
 * either name keep working. */
#define EROFS_VFS   30
/* Chapter 132 — returned by the dispatcher when a driver does
 * not implement the requested op (e.g. mkdir on procfs).  Matches
 * POSIX ENOSYS. */
#define ENOSYS_VFS  38

/* Chapter 140 — additional errnos for userspace filesystem
 * servers (`SYS_MOUNT` / `SYS_UMOUNT` and the userfs vtable).
 * Names + values mirror Linux's <errno.h> so userspace callers
 * comparing against either header keep working. */
#ifndef EEXIST
#define EEXIST      17    /* prefix already mounted */
#endif
#ifndef EBUSY_VFS
#define EBUSY_VFS   16    /* umount with open fds against the mount */
#endif
#ifndef EDEADLK
#define EDEADLK     35    /* daemon recursed into its own mount */
#endif
#ifndef ETIMEDOUT_VFS
#define ETIMEDOUT_VFS 110 /* userfs request exceeded its deadline */
#endif

/* Chapter 150 — returned by vfs_lseek when the fd kind is not
 * seekable (pipes, sockets, ttys, the kernel console).  Matches
 * POSIX value 29 ("illegal seek"). */
#ifndef ESPIPE
#define ESPIPE      29
#endif

/* Forward decl — defined in thread.h, but kept opaque here. */
struct thread;

/* Forward decl — defined in pipe.h. */
struct pipe;

/* Forward decls — defined in srv.h (chapter 112). */
struct srv_listen;
struct srv_conn;

/* Forward decl — defined in userfs.h (chapter 140). */
struct userfs_channel;

/* Forward decl — defined later in this file.  Filesystem drivers
 * receive a pointer to the per-fd entry so they can stash their
 * own state in the per-kind fields (osfs2_ino, etc). */
struct fd_entry;

/* ------------------------------------------------------------------
 * Chapter 132 — mount table + struct fs_ops vtable.
 *
 * Replaces the per-syscall prefix ladders that had grown across
 * vfs.c and syscall.c (one branch per filesystem in vfs_open /
 * vfs_read / vfs_load / sys_listdir_at / sys_unlink / sys_mkdir).
 *
 * Each filesystem driver supplies a `struct fs_ops` whose methods
 * take a driver-private `cookie` plus the path *relative to the
 * mount point* (e.g. for mount prefix "/data" and path
 * "/data/foo", rel is "/foo").  The dispatcher in vfs.c does a
 * longest-prefix match on the mount table and forwards.
 *
 * Unimplemented methods are NULL; the dispatcher returns
 * -ENOSYS_VFS when a caller asks for an unimplemented op (e.g.
 * mkdir on procfs).
 *
 * Step 1 of the chapter-113 sequence introduces the types, the
 * resolver, and an empty mount table.  Drivers are wired in over
 * the following steps (procfs, tmpfs, OSFS-1, OSFS-2, ramfs).
 * Callers continue to use the legacy prefix ladders until each
 * filesystem is ported.
 * ------------------------------------------------------------------ */

#define MOUNT_MAX   16

/* Mount flags.  Bit field; OR together. */
#define MOUNT_RO    0x1u   /* writes return -EROFS_VFS */

struct fs_ops {
    /* Open `rel` against the mount.  Populates `out` (kind +
     * driver-private fields) on success.  Returns 0 or a
     * negative errno. */
    long (*open)    (void *cookie, const char *rel, int flags,
                     struct fd_entry *out);

    /* Read / write / close / lseek against an fd previously
     * populated by this driver's `open`.  These are reached only
     * after the dispatcher confirms the fd was opened through the
     * matching mount, so the driver may rely on its own per-fd
     * fields being valid. */
    long (*read)    (void *cookie, struct fd_entry *e,
                     void *buf, size_t n);
    long (*write)   (void *cookie, struct fd_entry *e,
                     const void *buf, size_t n);
    long (*close)   (void *cookie, struct fd_entry *e);
    long (*lseek)   (void *cookie, struct fd_entry *e,
                     int64_t off, int whence);

    /* Enumerate entries under `rel`.  `idx` walks 0..N-1; on
     * success returns the byte length written to `name` and sets
     * *type to a filesystem-specific tag (0 = file, 1 = dir).
     * Returns -ENOENT_VFS past the end.  `rel` of "" or "/" means
     * the mount root. */
    int  (*listdir) (void *cookie, const char *rel, int idx,
                     char *name, size_t cap, uint32_t *type);

    /* Mutating ops; may be NULL on read-only filesystems. */
    int  (*unlink)  (void *cookie, const char *rel);
    int  (*mkdir)   (void *cookie, const char *rel);

    /* Predicate: 1 if `rel` is a directory, 0 if a file, negative
     * errno on lookup failure. */
    int  (*is_dir)  (void *cookie, const char *rel);

    /* Load an entire file into a freshly kheap-allocated buffer.
     * Caller owns the returned buffer.  Used by the exec path
     * (sys_spawn / sys_execv) and by tools like /bin/cat that
     * want the whole file at once. */
    long (*load)    (void *cookie, const char *rel,
                     uint8_t **out_data, size_t *out_len);
};

struct mount {
    const char          *prefix;   /* e.g. "/data", "/proc", "/" */
    const struct fs_ops *ops;
    void                *cookie;   /* driver-private; passed to every method */
    uint32_t             flags;
};

/* Register a mount.  Returns 0 on success, -EINVAL_VFS for a
 * malformed prefix, -ENOSPC if the table is full.  Prefix
 * conventions:
 *   - Must start with '/'.
 *   - Must NOT have a trailing '/' (use "/data", not "/data/").
 *   - The single character "/" is the root mount; it matches
 *     anything that no longer prefix covers.
 * Idempotent registration is the caller's responsibility; the
 * dispatcher's longest-prefix logic will route to the most
 * recently registered duplicate. */
int vfs_mount_register(const char *prefix, const struct fs_ops *ops,
                       void *cookie, uint32_t flags);

/* Longest-prefix-match against the mount table.  Returns NULL
 * if no mount covers the path (which becomes impossible once
 * the root mount "/" has been registered in step 5).  On a
 * match, *rel_out is the suffix of `path` after the prefix —
 * still beginning with '/' (or "" if path == prefix exactly).
 * `rel_out` may be NULL if the caller doesn't need it. */
const struct mount *vfs_resolve(const char *path, const char **rel_out);

/* Mount-table enumeration, used by SYS_MOUNTS (step 6) and by
 * kernel-side diagnostics.  Returns the number of registered
 * mounts; vfs_mount_at(i) returns the mount or NULL if i is
 * out of range. */
int                  vfs_mount_count(void);
const struct mount  *vfs_mount_at(int idx);

/* Remove the mount at slot `idx`, compacting later slots
 * down by one.  Used by SYS_UMOUNT.  The caller is
 * responsible for any teardown of the cookie (e.g. freeing
 * a userfs_channel).  Returns 0 / -EINVAL. */
int                  vfs_mount_remove(int idx);

/* What kind of object an fd refers to.  Determines which read /
 * write / close path runs.  FD_CONSOLE covers fd 0/1/2 by
 * default; the file kinds were the legacy default and remain
 * implied when kind == FD_FILE.  FD_PIPE_R / FD_PIPE_W mean
 * the slot points to a pipe object via the `pipe` field. */
enum fd_kind {
    FD_CONSOLE = 0,
    FD_FILE,        /* osfs OR ramfs (distinguished by osfs_size != 0) */
    FD_PIPE_R,
    FD_PIPE_W,
    FD_TMPFS_RW,    /* read+write tmpfs file; tmpfs index in ramfs_index */
    FD_SOCKET,      /* TCP socket; tcp_cid in `socket_cid` */
    FD_SOCKET_LISTEN, /* chapter 106: TCP listening socket;
                       * tcp_cid in `socket_cid` (a TCP_LISTEN
                       * conn slot).  Read/write return -EINVAL;
                       * the only valid op besides close is
                       * SYS_SOCKET_ACCEPT. */
    FD_PTY_MASTER,  /* gui_term side of a pty (chapter 79) */
    FD_PTY_SLAVE,   /* /bin/sh side of a pty; goes on fd 0/1/2 */
    FD_OSFS2_FILE,  /* writable OSFS-2 file at /data/...; ino in `osfs2_ino` */
    FD_SRV_LISTEN,  /* chapter 112: named-IPC listener; srv_listen in `srv_l` */
    FD_SRV_CONN,    /* chapter 112: named-IPC connected fd; srv_conn in `srv_c`,
                     * `srv_is_service` distinguishes the service vs client end */
    FD_USERFS_FILE, /* chapter 140: file behind a userspace fs server;
                     * `userfs_ch` + `userfs_handle` carry the per-fd
                     * state.  Read/write dispatch through g_userfs_ops. */
};

/* Forward decl — defined in pty.h. */
struct pty;

/* Per-thread fd table entry.  When in_use is 0 the slot is free. */
struct fd_entry {
    int        in_use;
    enum fd_kind kind;
    uint64_t   offset;        /* current read position */
    /* Identifier of the underlying ramfs file.  Index into the
     * ramfs file table; -1 means "console" for fd 0/1/2. */
    int        ramfs_index;
    /* OSFS mount-backed file.  When osfs_size != 0 this fd reads
     * from the on-disk OSFS-1 mount at /mnt; ramfs_index is
     * ignored.  start_sector / size_bytes are the directory
     * entry's coordinates. */
    uint32_t   osfs_start;
    uint32_t   osfs_size;
    /* Pipe object the slot refers to when kind is FD_PIPE_R or
     * FD_PIPE_W.  Refcounted: each FD_PIPE_R holds one r_ref,
     * each FD_PIPE_W holds one w_ref.  Last close frees. */
    struct pipe *pipe;
    /* TCP connection id when kind == FD_SOCKET.  -1 otherwise. */
    int        socket_cid;
    /* Pty object the slot refers to when kind is FD_PTY_MASTER or
     * FD_PTY_SLAVE.  Closing the fd calls pty_close_master /
     * pty_close_slave which drop the underlying pipe references
     * and free the pty when both ends are closed. */
    struct pty *pty;
    /* OSFS-2 inode number when kind == FD_OSFS2_FILE.  0 means
     * "unset" since inode 0 is the on-disk null sentinel and
     * never refers to a real file.  Reads and writes route
     * through osfs2_read / osfs2_write at offset `offset`. */
    uint32_t   osfs2_ino;
    /* Chapter 112 — named-IPC service bus.  When kind is
     * FD_SRV_LISTEN, `srv_l` points at the registered
     * listener and `srv_c` / `srv_is_service` are unused.
     * When kind is FD_SRV_CONN, `srv_c` points at the
     * connected conn and `srv_is_service` is 1 for the
     * accepted (service) end, 0 for the connect (client)
     * end — that bit picks which queue read/write touches.
     * Both pointers carry one refcount per fd; vfs_close
     * routes through srv_unref_listen / srv_unref_conn. */
    struct srv_listen *srv_l;
    struct srv_conn   *srv_c;
    int                srv_is_service;
    /* Chapter 140 — userspace filesystem server fd state.  Only
     * valid when kind == FD_USERFS_FILE.  The channel pointer
     * carries one open-fd refcount; vfs_close decrements
     * userfs_ch->open_fds via g_userfs_ops.close. */
    struct userfs_channel *userfs_ch;
    uint32_t               userfs_handle;
};

/* Chapter 94 — refcounted fd table.
 *
 * Pre-chapter 94, every `struct thread` carried its own
 * `struct fd_entry fds[FD_TABLE_SIZE]` directly inline.  This
 * forced one private fd table per thread, which is wrong for
 * "threads inside one process" (POSIX-style): a thread that
 * opens a file expects its sibling thread to be able to read /
 * write the same descriptor, and a thread that exits should
 * NOT close descriptors its siblings still hold.
 *
 * Chapter 94 lifts the table into a separately-allocated
 * `struct fd_table` owned by reference count.  The default
 * (used by spawn / fork / SYS_CLONE without CLONE_FILES) is
 * still "one fresh table per thread, refcount = 1".  When a
 * caller passes CLONE_FILES (via SYS_CLONE3), the new thread
 * adopts the *parent's* fd_table and bumps its refcount.  The
 * existing `vfs_close_all(t)` becomes "drop t's reference;
 * close every fd only when the count hits zero".
 *
 * The lock field is held across multi-step fd lookups in
 * vfs_open / vfs_close / vfs_alloc_socket_fd whenever two
 * threads in the same process can race for the same slot.
 * Single-thread processes never contend (uncontested lock is
 * one LDADD + one STLR on AArch64 LSE).
 */
struct fd_table {
    spinlock_t        lock;
    volatile uint32_t refcount;
    struct fd_entry   fds[FD_TABLE_SIZE];
};

/* Allocate a fresh fd_table with refcount = 1 and slots 0/1/2
 * pre-populated as FD_CONSOLE.  Returns NULL on OOM.  Caller
 * owns the single reference and is responsible for eventually
 * calling fd_table_unref. */
struct fd_table *fd_table_create(void);

/* Bump the refcount.  Used by SYS_CLONE3 with CLONE_FILES so a
 * new thread can adopt an existing table without copying. */
void fd_table_share(struct fd_table *ft);

/* Decrement the refcount.  When it hits zero, closes every
 * still-open fd in the table (dropping pipe / pty / socket
 * refs) and frees the table itself.  Safe to call with NULL. */
void fd_table_unref(struct fd_table *ft);

/* Initialise the VFS.  Wires up ramfs.  Idempotent. */
void vfs_init(void);

/* Chapter 91 — expose a ramfs blob to in-kernel page-cache
 * loaders.  Returns 0 and fills *out_data + *out_size on
 * success; -1 if `idx` is out of range.  The returned pointer
 * stays valid for the lifetime of the kernel image (ramfs blobs
 * live in .rodata.embedded_user). */
int vfs_ramfs_blob(int idx, const uint8_t **out_data, uint64_t *out_size);

/* Number of ramfs entries.  For bounds-checking before calling
 * vfs_ramfs_blob. */
uint64_t vfs_ramfs_count(void);

/* Look up a ramfs file by path; returns the index or -1.  Used
 * by mmap callers (and tests) to translate a name into the
 * cache-key int that page_cache uses. */
int vfs_ramfs_lookup(const char *name);

/* Per-thread fd-table setup, called from thread_create /
 * user_thread_create.  Pre-fills slots 0, 1, 2 as console. */
void vfs_init_fdtable(struct thread *t);

/* Open flags accepted by vfs_open.  Today the writable paths
 * (/tmp/... in tmpfs, /data/... in OSFS-2) honour O_CREAT /
 * O_TRUNC / O_WRONLY; for read-only file systems they're parsed
 * but ignored.  Bit positions match POSIX so user code looks
 * familiar. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100      /* 64 — create if missing */
#define O_TRUNC   01000     /* 512 — truncate to zero on open */
#define O_APPEND  02000     /* 1024 — write at EOF (parsed; tmpfs always appends) */

/* Open `name` for reading.  Returns the new fd on success or a
 * negative errno.  `flags` is reserved for later write support. */
int vfs_open(const char *name, int flags);

/* Open `name` directly into thread `t`'s slot `fd`, overwriting
 * whatever was there.  Used by sys_spawn_redir to wire a child's
 * fd 0 to a file before the child runs.  Returns 0 on success or
 * a negative errno. */
int vfs_open_into(struct thread *t, int fd, const char *name, int flags);

/* Chapter 153 -- POSIX-shaped file metadata.  Returned by
 * vfs_stat_path / vfs_fstat (kernel) and sys_stat / sys_fstat
 * (the user surface).  Kept deliberately small: GCC / TCC /
 * make only need mode + size today; mtime is wired through to
 * the OSFS-2 inode for future `ls -lt` use but every other
 * filesystem reports 0 (acceptable for the read-only paths).
 *
 * `mode` is laid out the POSIX way:
 *   - High 4 bits encode the file kind (S_IFREG / S_IFDIR /
 *     S_IFCHR / S_IFIFO / S_IFSOCK).
 *   - Low 12 bits are the permission triplets (rwx user / group /
 *     other).  We don't enforce these yet -- there's no notion
 *     of users or groups -- but populating them lets `ls -l`
 *     render the familiar "-rw-r--r--" string from day one.
 *
 * Layout must match userspace/libc/sys/stat.h::struct stat
 * byte-for-byte.  Do not reorder.  The 4-byte _pad keeps `size`
 * 8-byte aligned.  Chapter 178 appended st_dev / st_ino;
 * chapter 179 renamed st_mtime_ms -> st_mtime (POSIX seconds;
 * still always 0) and appended st_uid / st_gid (always 0). */
struct kstat {
    uint32_t st_mode;
    uint32_t _pad;
    uint64_t st_size;
    uint64_t st_mtime;
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
};

/* File-kind bits (high nibble of st_mode). */
#define S_IFMT_K    0xF000u
#define S_IFREG_K   0x8000u
#define S_IFDIR_K   0x4000u
#define S_IFCHR_K   0x2000u
#define S_IFIFO_K   0x1000u
#define S_IFSOCK_K  0xC000u

/* Stat by path.  Walks the mount table, dispatches to the
 * resolved filesystem.  Returns 0 / -errno; on success fills
 * *out.  -ENOENT for a path that doesn't exist, -EFAULT if
 * either pointer is bad. */
long vfs_stat_path(const char *path, struct kstat *out);

/* Stat by fd.  Reads metadata directly from the fd_entry --
 * cheaper than re-walking the path and avoids races where the
 * file is unlinked between open and stat. */
long vfs_fstat(int fd, struct kstat *out);


/* Allocate a fresh fd in the current thread that points at the
 * TCP connection identified by `cid`.  Used by sys_socket
 * (which creates a fresh cid via tcp_connect / tcp_listen) to
 * hand the connection back to userspace as a normal fd that
 * read()/write()/close() understand.  Returns the new fd >= 3
 * on success, -EMFILE if the table is full. */
int vfs_alloc_socket_fd(int cid);

/* Chapter 106 -- allocate a fresh fd that wraps a TCP_LISTEN
 * cid.  Identical to vfs_alloc_socket_fd except for the kind
 * (FD_SOCKET_LISTEN), which the read/write paths reject and
 * which SYS_SOCKET_ACCEPT requires.  Returns the new fd >= 3,
 * or -EMFILE if the table is full. */
int vfs_alloc_listen_fd(int cid);

/* Chapter 112 — allocate a fresh fd in the current thread
 * pointing at a /srv/<name> listener object (returned by
 * srv_bind).  Read/write paths reject FD_SRV_LISTEN; only
 * SYS_SRV_ACCEPT and close are valid.  Returns the new fd
 * >= 3, or -EMFILE if the table is full. */
int vfs_alloc_srv_listen_fd(struct srv_listen *ls);

/* Chapter 112 — allocate a fresh fd in the current thread
 * pointing at a /srv connected conn.  `is_service_end` picks
 * which queue read/write touches: nonzero = accepted (service)
 * side; zero = connect (client) side.  Returns the new fd
 * >= 3, or -EMFILE if the table is full. */
int vfs_alloc_srv_conn_fd(struct srv_conn *c, int is_service_end);

/* Close every fd in thread `t`'s table.  Must be called when a
 * thread exits so pipe refcounts drop and the other side sees
 * EOF / -EPIPE.  Idempotent (a slot that's already closed is
 * skipped). */
void vfs_close_all(struct thread *t);

/* Read up to `len` bytes from `fd` into `buf`.  Returns the number
 * of bytes copied (0 = EOF) or a negative errno. */
long vfs_read(int fd, void *buf, size_t len);

/* Close `fd`.  Returns 0 or a negative errno.  Closing fd 0/1/2
 * is a no-op success (POSIX-style). */
int vfs_close(int fd);

/* Chapter 150 — POSIX lseek.  Re-position `fd`'s read/write
 * cursor.  `whence` is 0/1/2 for SEEK_SET/SEEK_CUR/SEEK_END.
 * Returns the new absolute offset on success, or a negative
 * errno (-EBADF, -ESPIPE on un-seekable kinds, -EINVAL for a
 * bad whence or a negative resulting offset, -ENOSYS for
 * SEEK_END on filesystems that don't expose size yet). */
long vfs_lseek(int fd, int64_t off, int whence);

/* Look up a ramfs file by path.  On success stores a pointer to
 * the file's backing bytes in *data and the byte count in *size,
 * and returns 0.  On a missing path returns -ENOENT_VFS.  Only
 * walks the embedded ramfs; for disk-backed files use vfs_load. */
int vfs_lookup(const char *path, const uint8_t **data, size_t *size);

/* Load an entire file (ramfs OR OSFS-mounted) into a freshly
 * allocated kheap buffer.  Caller owns the buffer and must kfree
 * it.  Returns 0 on success and stores the buffer pointer in
 * *out_buf and its byte count in *out_size, or a negative errno
 * on failure (-ENOENT_VFS if missing, -ENOMEM_VFS on OOM, -EIO on
 * a disk read error).  The path dispatch is the same as vfs_open:
 *   /mnt/<name>  -> OSFS-1 disk mount
 *   /bin/<name>  -> OSFS-1 disk mount (after the /bin/ prefix is
 *                   stripped) — this is where user binaries live.
 *   /data/<name> -> OSFS-2 writable disk mount (chapter 82+)
 *   <other>      -> embedded ramfs
 * Used by sys_spawn and the boot-time init loader. */
int vfs_load(const char *path, uint8_t **out_buf, size_t *out_size);

/* Enumerate the combined ramfs + OSFS-mount namespace.  `idx`
 * walks 0..N-1 across all mounts; the order is ramfs first then
 * OSFS.  On success fills `name` (NUL-terminated, up to `cap-1`
 * bytes) with the full path (e.g. "/motd" for ramfs, "/mnt/x"
 * for OSFS), stores the file size in *size_out, and returns the
 * length of the name written.  Returns -ENOENT_VFS if `idx` is
 * past the end and -EINVAL_VFS for invalid args (cap < 1, etc).
 * Used by SYS_LISTDIR and `/bin/ls`. */
long vfs_listdir(int idx, char *name, size_t cap, uint32_t *size_out);

#endif /* VFS_H */
