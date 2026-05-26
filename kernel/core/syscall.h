/*
 * kernel/core/syscall.h — kernel-side syscall ABI.
 *
 * Calling convention (matches the AArch64 Linux ABI):
 *   x8       syscall number       (selects which handler)
 *   x0..x5   up to six arguments
 *   x0       return value         (negative errno on error)
 *
 * The syscall number lives in x8 rather than x0 so that the
 * dispatcher does not have to shuffle arguments before calling the
 * handler — the C signature `long handler(long, long, long, long,
 * long, long)` already maps 1:1 to x0..x5.
 *
 * The numeric values are intentionally NOT the Linux ABI numbers.
 * We pick small dense numbers and grow the table as needed; user
 * code talks to this kernel via a tiny userspace libc that hides
 * the numbers behind named wrappers (write, exit, ...).
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "exception.h"

/* Stable syscall numbers.  Append-only — never renumber. */
enum {
    SYS_WRITE  = 1,   /* (int fd, const void *buf, size_t len) -> ssize_t  */
    SYS_EXIT   = 2,   /* (int code) -> noreturn                            */
    SYS_GETPID = 3,   /* (void) -> int                                     */
    SYS_YIELD  = 4,   /* (void) -> int (always 0)                          */
    SYS_OPEN   = 5,   /* (const char *name, int flags) -> int fd           */
    SYS_READ   = 6,   /* (int fd, void *buf, size_t len) -> ssize_t        */
    SYS_CLOSE  = 7,   /* (int fd) -> int                                   */
    SYS_SPAWN  = 8,   /* (const char *path, const char *args) -> int tid   */
    SYS_WAIT   = 9,   /* (int *code_out) -> int tid (or -1 if no children) */
    SYS_GETARGS= 10,  /* (char *buf, size_t len) -> ssize_t bytes copied   */
    SYS_SBRK   = 11,  /* (intptr_t inc) -> void *prev_brk (-1 on failure)  */
    SYS_LISTDIR= 12,  /* (int idx, char *name, size_t cap, uint32_t *size_out)
                       *   -> ssize_t name length (or -ENOENT past end)    */
    SYS_UPTIME_MS = 13, /* (void) -> uint64 monotonic milliseconds since boot */
    SYS_CHDIR  = 14,  /* (const char *path) -> int (0 / -errno)            */
    SYS_GETCWD = 15,  /* (char *buf, size_t cap) -> ssize_t bytes incl NUL */
    SYS_GETENV = 16,  /* (const char *key, char *buf, size_t cap)
                       *   -> ssize_t bytes incl NUL, or -ENOENT           */
    SYS_SETENV = 17,  /* (const char *key, const char *val) -> int         */
    SYS_UNSETENV = 18,/* (const char *key) -> int                          */
    SYS_GETENV_ALL = 19, /* (char *buf, size_t cap) -> ssize_t bytes used   */
    SYS_SPAWN_REDIR = 20, /* (const char *path, const char *args,
                           *  const char *stdin_path) -> int tid             */
    SYS_SLEEP_MS = 21,    /* (uint64_t ms) -> int (always 0)                  */
    SYS_PIPE     = 22,    /* (int fds[2]) -> int (0 ok, -errno fail)          */
    SYS_DUP2     = 23,    /* (int oldfd, int newfd) -> int (newfd)            */
    SYS_SPAWN_PIPE = 24,  /* (path, args, stdin_fd, stdout_fd) -> int tid     */
    SYS_UNLINK   = 25,    /* (const char *path) -> int (0 ok, -errno fail)    */
    SYS_TTY_RAW  = 26,    /* (int enable) -> int (previous mode, 0/1)         */
    SYS_KILL     = 27,    /* (int pid, int sig) -> int (0 ok, -errno fail)    */
    SYS_SET_FG_PID = 28,  /* (int pid) -> int (previous fg pid)               */

    /* Milestone 65 — fork + exec.  Together these supplant the
     * old SYS_SPAWN family for callers that want POSIX semantics
     * (fork to set up redirections / inherit fds / ..., then exec).
     * SYS_SPAWN is preserved as a fast-path "fork-then-immediate-
     * exec" primitive for everything that doesn't need it. */
    SYS_FORK     = 29,    /* (void) -> int  (parent: child pid; child: 0)     */
    SYS_EXEC     = 30,    /* (const char *path, char *const argv[]) -> -errno
                           *   on failure; never returns on success.          */

    /* Chapter 77 — Catchable signals (sigaction + sigreturn).
     *   sys_sigaction(sig, handler, restorer) -> uint64_t old_handler
     *     handler == 0 ⇒ SIG_DFL (terminate with 128+sig).
     *     handler == 1 ⇒ SIG_IGN (drop silently).
     *     otherwise   ⇒ EL0 user function pointer; called with
     *                   x0 = signum and LR = restorer.
     *   restorer is a tiny user-mode stub (typically in libc)
     *   that issues SYS_SIGRETURN with x0 = the sigframe pointer
     *   that the kernel placed at SP_EL0 just before the
     *   handler started running.  The stub is shared across all
     *   handlers in this thread; pass its address on the FIRST
     *   sys_sigaction call (subsequent calls may pass 0 to keep
     *   the existing one).
     *   Returns the previous handler value, or (uint64_t)-EINVAL
     *   for a bad signum.
     *
     *   sys_sigreturn(uptr) -> noreturn
     *     uptr points at a struct sigframe written by the kernel
     *     when it diverted the thread into the handler.  Restores
     *     gpr[0..30], elr, spsr, sp_el0 from that frame and erets
     *     to wherever the program was when the signal arrived. */
    SYS_SIGACTION = 31,
    SYS_SIGRETURN = 32,

    /* Chapter 78 — SIGCHLD + waitpid.  Generalises SYS_WAIT:
     *   pid > 0 → wait for that specific child only.
     *   pid <= 0 → wait for any child (legacy SYS_WAIT).
     *   options & WNOHANG (1) → do not block; return 0 if a
     *     matching child exists but has not yet exited.
     * Returns the reaped child's pid (and writes exit code via
     * code_out_ptr if non-NULL), 0 for the WNOHANG no-exit case,
     * or -1 if no child matches the filter at all. */
    SYS_WAITPID = 33,

    /* Chapter 79b — pseudo-terminal allocation.
     *   sys_openpty(int *master_out, int *slave_out) -> 0 or -errno
     * Allocates a pty (two pipes + a foreground-pid field) and
     * installs two new fds in the calling thread:
     *   - *master_out: FD_PTY_MASTER, intended for gui_term.
     *     Reads drain the slave-to-master ring (non-blocking;
     *     returns 0 if no data).  Writes enqueue to the
     *     master-to-slave ring after scanning each byte: a
     *     literal Ctrl-C (0x03) is translated to SIGINT for the
     *     pty's fg_pid (set via sys_set_fg_pid by the shell)
     *     and not forwarded.
     *   - *slave_out: FD_PTY_SLAVE, intended to be dup2'd onto
     *     fds 0/1/2 of the child shell after fork().  Reads
     *     block on the master-to-slave ring; writes enqueue to
     *     slave-to-master.
     * Errors: -EFAULT (bad pointer), -EMFILE (no fd slots),
     * -ENOMEM (pty/pipe alloc). */
    SYS_OPENPTY = 34,

    /* Chapter 82 — durability for OSFS-2.
     *   sys_fsync(int fd) -> 0 or -errno
     * For an OSFS-2 file fd, flushes every dirty block in the
     * write-back cache to disk synchronously and returns 0 only
     * after every virtio-blk write has acked.  No-op (returns 0)
     * for fds backed by storage that's already synchronous or
     * volatile by design: console, ramfs, OSFS-1 (read-only),
     * tmpfs, pipes, ptys, sockets.
     * Errors: -EBADF (bad fd), -EIO (a writeback failed; the
     * cache slot is left dirty so a retry can succeed). */
    SYS_FSYNC = 35,

    /* Chapter 85 \u2014 directory namespace.
     *   sys_mkdir(const char *path) -> 0 or -errno
     * Creates a directory at `path` (must start with /data/ \u2014
     * the only writable mount with subdirectory support).  All
     * parent components must already exist.  -EEXIST if the leaf
     * already exists, -EINVAL if the prefix isn't /data/, -ENOSPC
     * if we ran out of inodes/blocks. */
    SYS_MKDIR = 36,

    /*   sys_listdir_at(const char *path, int idx, char *name,
     *                  size_t cap, uint32_t *size_out,
     *                  uint32_t *type_out) -> bytes-of-name or -errno
     * Iterates the directory at `path` (currently /data/<dir>)
     * by index, returning leaf names rather than full paths.
     * `type_out` receives 1 (file) or 2 (dir) so callers can\n     * tag entries without a follow-up stat. */
    SYS_LISTDIR_AT = 37,

    /* Milestone 40 — minimal in-kernel window manager. */
    SYS_GUI_CREATE_WINDOW  = 40,  /* (uint32_t w, uint32_t h, const char *t) -> id */
    SYS_GUI_DESTROY_WINDOW = 41,  /* (int id) -> 0/-errno                          */
    SYS_GUI_PRESENT        = 42,  /* (struct gui_present_args *)   -> 0/-errno     */
    SYS_GUI_FILL_RECT      = 43,  /* (struct gui_fill_rect_args *) -> 0/-errno     */
    SYS_GUI_DRAW_TEXT      = 44,  /* (struct gui_draw_text_args *) -> 0/-errno     */
    SYS_GUI_FLUSH          = 45,  /* (int id) -> 0/-errno                          */
    SYS_GUI_POLL_EVENT     = 46,  /* (struct gui_event *out) -> 1 if ev, 0 if none */

    /* Milestone 47 — taskbar / always-on-top windows + window list. */
    SYS_GUI_CREATE_WINDOW_EX = 47, /* (struct gui_create_window_ex_args *) -> id   */
    SYS_GUI_LIST_WINDOWS   = 48,  /* (struct gui_window_info *out, int max) -> n  */
    SYS_GUI_RAISE_WINDOW   = 49,  /* (int id) -> 0/-errno                         */

    /* Milestone 50 — userspace desktop environment. */
    SYS_GUI_GET_SCREEN_SIZE = 50, /* (uint32_t *w, uint32_t *h) -> 0/-errno       */

    /* Milestone 51 — minimize / restore windows. */
    SYS_GUI_SET_MINIMIZED   = 51, /* (int id, int on) -> 0/-errno                 */

    /* Chapter 102 -- measure a string's rendered width in pixels
     * using the kernel's default font. Lets userspace centre
     * labels, position carets, and truncate-to-fit without
     * assuming a fixed glyph pitch (which no longer holds with
     * the variable-width TTF font). Returns width in pixels, or
     * -EFAULT if the string pointer isn't readable. */
    SYS_GUI_MEASURE_TEXT    = 52, /* (const char *s) -> uint32_t px width        */

    /* Chapter 108a -- userspace access to window pixel buffers.
     * The WM-owned pixel buffer for a window is exposed at a
     * fresh user VA range (RW, EL0, page-aligned, tagged with
     * DESC_SW_WM_WINDOW so AS teardown skips it and fork()
     * doesn't inherit it).  Apps that want pixel-level control
     * (the chapter-108a pixapp demo, the chapter-108b TTF
     * rasteriser, any future SVG / video / GL renderer) write
     * directly into the mapping and call SYS_GUI_DAMAGE to tell
     * the WM which rect changed.  Apps that just want the
     * existing primitives keep calling SYS_GUI_FILL_RECT etc.
     *
     * SYS_GUI_MAP_WINDOW(struct gui_map_window_args *a) -> 0/-errno
     *   On success, writes the user VA into *a->va_out, the
     *   row stride (bytes) into *a->stride_out, and the current
     *   window dimensions into *a->w_out / *a->h_out.  Output
     *   pointers may be NULL ("don't care").  Idempotent: a
     *   second call from the same owner returns the same VA.
     *   -EPERM if not the window owner; -EINVAL if the window
     *   is RESIZABLE (108a defers resize coherence); -ENOMEM
     *   if pmem can't supply the page frames; -EFAULT on a
     *   bad output pointer (mapping still installed).
     *
     * SYS_GUI_UNMAP_WINDOW(int id) -> 0/-errno
     *   Tears the mapping down and returns the frames to pmem.
     *   Implicit on SYS_GUI_DESTROY_WINDOW / process exit, so
     *   well-behaved apps don't need to call this; useful for
     *   tests that want to verify the mapping actually went
     *   away.  Idempotent on an unmapped window.
     *
     * SYS_GUI_DAMAGE(struct gui_damage_args *a) -> 0/-errno
     *   Tells the WM that the rect (x,y,w,h) of the mapping
     *   has been written by the app.  The WM copies that rect
     *   from the user-visible pages into the compositor's
     *   authoritative buffer and triggers a recompose.  Rect
     *   coordinates are window-content relative; over-large
     *   rects are silently clipped to the window so a
     *   "damage everything" call works without preflight.
     *   -ENOENT if the window has no mapping yet. */
    SYS_GUI_MAP_WINDOW      = 53, /* (struct gui_map_window_args *) -> 0/-errno */
    SYS_GUI_UNMAP_WINDOW    = 54, /* (int id)                       -> 0/-errno */
    SYS_GUI_DAMAGE          = 55, /* (struct gui_damage_args *)     -> 0/-errno */

    /* Milestone 56 — sockets (active-open client side). */
    SYS_SOCKET_CONNECT  = 60, /* (uint32_t ip4_be, uint16_t port) -> fd / -errno */
    SYS_SOCKET_STATE    = 61, /* (int fd) -> int state (enum tcp_state)          */
    SYS_SOCKET_SHUTDOWN = 62, /* (int fd) -> 0 / -errno; FIN, fd still readable  */

    /* Milestone 57 — DNS resolver. */
    SYS_RESOLVE         = 63, /* (const char *name, uint32_t *out_ip4_be) -> 0/-errno */

    /* Chapter 104 / M93 — sockets (passive-open server side).
     *
     * SYS_SOCKET_LISTEN(port, backlog) -> fd / -errno
     *   Create a TCP_LISTEN slot bound to `port` and return a
     *   new fd of kind FD_SOCKET_LISTEN.  read/write on this fd
     *   return -EINVAL; SYS_SOCKET_ACCEPT is the only way to
     *   extract a peer connection.  `backlog` is currently
     *   advisory (the kernel uses a fixed TCP_ACCEPT_QCAP = 8).
     *   Returns -EADDRINUSE if another listener owns `port`.
     *
     * SYS_SOCKET_ACCEPT(listen_fd, peer_ip_out, peer_port_out) -> fd / -errno
     *   Block until the listener's accept queue has a fully-
     *   handshaken child, then pop it and return a fresh
     *   FD_SOCKET fd ready for read/write.  Optionally writes
     *   the peer's IPv4 address (BE-packed uint32) and TCP port
     *   to the caller-supplied output pointers; pass NULL for
     *   "don't care".  Polls the NIC + yields between checks.
     */
    SYS_SOCKET_LISTEN   = 64, /* (uint16_t port, int backlog) -> fd / -errno     */
    SYS_SOCKET_ACCEPT   = 65, /* (int listen_fd, uint32_t *peer_ip_out,
                                  uint16_t *peer_port_out) -> fd / -errno        */

    /* Chapter 107 — named-IPC service bus.
     *
     * SYS_SRV_BIND(const char *path) -> fd / -errno
     *   Register the calling thread as the listener for
     *   `/srv/<name>`.  Returns a FD_SRV_LISTEN fd; the only
     *   valid ops on it are SYS_SRV_ACCEPT and close.  Path
     *   must start with "/srv/" and contain no further '/'.
     *   Errors: -EINVAL bad shape, -EADDRINUSE name taken,
     *   -ENOMEM SRV_MAX_LISTENERS exhausted.
     *
     * SYS_SRV_ACCEPT(int listen_fd) -> fd / -errno
     *   Block until a client connects, then return a fresh
     *   FD_SRV_CONN fd (service end).  Mirrors
     *   SYS_SOCKET_ACCEPT shape; -EBADF on a non-listener fd,
     *   -EINTR on signal.
     *
     * SYS_SRV_CONNECT(const char *path) -> fd / -errno
     *   Open a client-side connection.  Equivalent to
     *   open(path, O_RDWR), which is the same code path
     *   under the hood.  -ENOENT if no service is bound,
     *   -ENOMEM if the listener's backlog is full, -EINTR
     *   on signal, -EPIPE if the service vanished mid-
     *   handshake.
     *
     * Message framing: each read returns exactly one
     * datagram, each write enqueues exactly one.  Cap is
     * SRV_MSG_MAX (64 KiB).  -EMSGSIZE if the caller's
     * read buffer is too small (message stays queued).
     */
    SYS_SRV_BIND        = 81, /* (const char *path) -> fd / -errno  */
    SYS_SRV_ACCEPT      = 82, /* (int listen_fd) -> fd / -errno     */
    SYS_SRV_CONNECT     = 83, /* (const char *path) -> fd / -errno  */

    /* Chapter 108d — userspace window server foundation.
     *
     * SYS_FB_MAP_SCANOUT(struct fb_map_args *out) -> 0 / -errno
     *   Maps the active virtio-gpu scanout buffer's physical
     *   pages into the caller's address space, RW from EL0,
     *   non-executable, tagged so AS teardown skips them (the
     *   FB stays alive for the kernel's continued use).  Fills
     *   *out with the user VA, dimensions, and byte stride.
     *
     *   The mapping uses the same page-install primitive that
     *   chapter 108a's gui_window_fb uses — the framebuffer's
     *   physical pages are contiguous (allocated via
     *   pmem_alloc_contig at fb_init), so we synthesise a
     *   trivial PA-array and reuse address_space_install_wm_window.
     *
     *   Access control: scoped by simple first-caller-wins.
     *   The kernel records the calling pid on first call; later
     *   callers from other pids get -EBUSY.  When the holding
     *   process exits, the slot is released (see
     *   fb_release_owner in main.c's exit path) so a respawned
     *   wsd can re-claim.  -EAGAIN if the FB is not yet ready.
     *
     *   Idempotent for the holding pid: repeat calls return
     *   the cached descriptors. */
    SYS_FB_MAP_SCANOUT  = 84, /* (struct fb_map_args *out) -> 0 / -errno */

    /* Chapter 108d — server-allocated, client-mappable
     * per-window framebuffer.  See kernel/core/win_fb.h for the
     * full design.  Three syscalls all take a user struct pointer
     * (or a small int for FREE) and return 0/-errno:
     *
     *   SYS_WIN_FB_ALLOC(struct win_fb_alloc_args *) — caller
     *     becomes the owner of a fresh BGRA backing buffer of
     *     w*h*4 bytes (rounded up to a page).  Kernel maps the
     *     pages RW into caller's AS and fills in {id, va,
     *     stride, size}.
     *
     *   SYS_WIN_FB_MAP(struct win_fb_map_args *) — caller
     *     passes a known id; kernel maps the same physical
     *     pages into caller's AS at a fresh user VA and fills
     *     in {va, w, h, stride, size}.  Idempotent per AS.
     *     Chapter 108d is permissive (anyone who knows the id
     *     may map); the chapter-107 IPC channel between wsd
     *     and the client is the de-facto ACL until a future
     *     capability layer tightens it.
     *
     *   SYS_WIN_FB_FREE(uint32_t id) — owner only.  Uninstalls
     *     every mapper's user-VA range first, then frees the
     *     backing pages, then clears the slot.  -EPERM if
     *     caller isn't owner; -ENOENT if id is unknown. */
    SYS_WIN_FB_ALLOC    = 85, /* (struct win_fb_alloc_args *) -> 0 / -errno */
    SYS_WIN_FB_MAP      = 86, /* (struct win_fb_map_args   *) -> 0 / -errno */
    SYS_WIN_FB_FREE     = 87, /* (uint32_t id)               -> 0 / -errno */
    /* chapter 108e — resize an existing win_fb in place.
     *   SYS_WIN_FB_RESIZE(uint32_t id,
     *                     uint32_t new_w, uint32_t new_h)
     *     -> 0 / -errno
     * Owner-only.  Allocates a fresh backing of new_w*new_h*4
     * bytes (page-rounded), copies as much of the old buffer
     * as fits (top-left preserved), uninstalls EVERY existing
     * mapping (owner + mappers) from its AS, frees the old
     * pages, then re-installs for the owner only.  The owner's
     * VA may change; the new VA is NOT returned by this
     * syscall (owner re-discovers it via a subsequent
     * SYS_WIN_FB_MAP, which now returns the post-resize geom +
     * a fresh VA install since the old mapping slot was
     * cleared).  Mappers must also re-call SYS_WIN_FB_MAP
     * after seeing GUI_EVENT_RESIZE; until they do, their
     * old VA returns translation-fault. */
    SYS_WIN_FB_RESIZE   = 93, /* (uint32_t id, uint32_t w, uint32_t h) -> 0/-errno */

    /* Chapter 112 — entropy source.  Backed by the virtio-rng
     * device (driver in kernel/device/virtio_rng.c) and stretched
     * by the ChaCha20 CSPRNG in kernel/core/random.c.
     *
     *   sys_getrandom(void *buf, size_t len, unsigned flags)
     *       -> bytes_written  on success
     *       -> -EFAULT        if `buf` isn't writable for `len`
     *       -> -EINVAL        if any unknown flag bit is set
     *
     * `flags` is reserved (must be zero) so the kernel keeps the
     * ability to add GRND_NONBLOCK / GRND_RANDOM-style modifiers
     * without breaking existing callers; passing unknown bits is
     * an error, NOT silently ignored.
     *
     * The call may block briefly while the virtio-rng device
     * serves the request (typically < 1 ms under HVF).  It NEVER
     * partial-fills: on success the full `len` bytes are present
     * in `buf`; on failure nothing was copied. */
    SYS_GETRANDOM       = 94,

    /* Chapter 113 — mount-table introspection.
     *
     *   sys_mounts(struct mount_info *out, int max)
     *       -> count_written  on success (0..max, never > MOUNT_MAX)
     *       -> -EFAULT        if `out` isn't writable for `max * sizeof(struct mount_info)` bytes
     *       -> -EINVAL        if `max` is negative
     *
     * Each `mount_info` is a fixed-layout snapshot of one entry
     * in the kernel's mount table:
     *
     *     struct mount_info {
     *         char     prefix[32];   // NUL-terminated, e.g. "/data"
     *         uint32_t flags;        // bit 0 = read-only (MOUNT_RO)
     *     };
     *
     * Userspace uses this to discover where the writable
     * filesystems live without hard-coding "/data".  The "Save
     * As" dialog calls it to enumerate destinations; future
     * apps that want to back up state can pick the
     * largest-flags-zero mount that exists.  The table is small
     * (MOUNT_MAX = 16 today) so the syscall is cheap.  No
     * pagination or cursor needed. */
    SYS_MOUNTS          = 95,

    /* Chapter 114 — userspace filesystem servers.
     *
     *   sys_mount(const char *prefix, int fds_out[2])
     *       -> mount_id (>= 0) on success
     *       -> -EEXIST     if `prefix` is already in the mount table
     *       -> -EINVAL     if `prefix` is malformed (chapter-113 rules)
     *       -> -ENOSPC     if the mount table is full (MOUNT_MAX)
     *       -> -ENOMEM     if pipe / channel allocation fails
     *       -> -EFAULT     if `fds_out` is not writable
     *
     * The kernel allocates a pair of anonymous pipes and a
     * fresh userfs_channel.  Two fds are installed in the
     * CALLING thread's fd table:
     *   fds_out[0] : FD_PIPE_R   — daemon reads requests here
     *   fds_out[1] : FD_PIPE_W   — daemon writes replies here
     * The kernel-internal ends are kept on the channel.
     * Registration uses `g_userfs_ops` with the channel as
     * cookie; `vfs_resolve` will route every path under
     * `prefix` through the channel from this point on.
     *
     *   sys_umount(int mount_id)
     *       -> 0           on success
     *       -> -EINVAL     if `mount_id` is out of range
     *       -> -ENOENT     if the slot is not a userfs mount
     *       -> -EBUSY      if any FD_USERFS_FILE fd still references
     *                      this mount (caller must close those first)
     *
     * v1 deletes the mount-table entry, marks the channel dead
     * (so any in-flight request returns -EIO), then frees the
     * channel.  Any future SYS_MOUNT for the same prefix will
     * succeed because the slot is now free.
     */
    SYS_MOUNT           = 96,
    SYS_UMOUNT          = 97,

    /* Chapter 114e — kernel state snapshots for /bin/procd.
     *
     * procd is the userspace daemon that serves /proc.  It used
     * to live in kernel/core/procfs.c; chapter 114e replaces
     * that file with a userfs daemon and these three syscalls
     * are the kernel's way of exposing the bits of state procd
     * needs to render each /proc file.
     *
     *   SYS_KSTAT(struct kstat_pub *user_out) -> 0 / -errno
     *     Fills the caller's `struct kstat_pub` with uptime,
     *     pmem totals, kernel-heap usage, CPU count, live
     *     thread count, and per-CPU runqueue lengths.  This is
     *     the data underlying /proc/uptime, /proc/meminfo,
     *     /proc/cpuinfo and /proc/sched.  Layout in
     *     userspace/libc/proc_stat.h.
     *
     *   SYS_THREAD_SNAPSHOT(int pid, struct thread_snap_pub *out, int max)
     *       -> n_written / -errno
     *     If pid == -1, fills up to `max` entries (capped by
     *     the kernel's live thread count) and returns n.  If
     *     pid >= 0, fills exactly one entry; returns 1 on hit,
     *     0 if no such pid.  `max` is the number of struct
     *     slots in `out`, not bytes.  -EFAULT if `out` isn't
     *     writable for `max * sizeof(struct thread_snap_pub)`.
     *
     *   SYS_STRACE_RENDER(int pid, char *user_buf, size_t cap)
     *       -> bytes_written / -errno
     *     Drains the target thread's strace ring into
     *     `user_buf` (which procd then serves as
     *     /proc/<pid>/trace).  -ENOENT_VFS if no such pid;
     *     returns 0 (with a one-line "(not traced)\n" banner)
     *     for live threads without an attached tracer.
     */
    SYS_KSTAT           = 98,
    SYS_THREAD_SNAPSHOT = 99,
    SYS_STRACE_RENDER   = 100,

    /* Chapter 116b — POSIX-shaped lseek for the new stdio FILE *
     * layer.  Lets fseek / ftell / rewind sit on top of the
     * existing fd_entry->offset that vfs_read already maintains.
     *
     *   sys_lseek(int fd, int64_t off, int whence) -> new_off / -errno
     *     whence: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END
     *     -ESPIPE on pipes / sockets / consoles / ptys (anything
     *      where "current position" is meaningless).
     *     -EINVAL for an unknown whence or a negative resulting
     *      offset.
     *     -ENOSYS for SEEK_END on filesystems that can't report
     *      file size (today: tmpfs and osfs2 — they'll grow size
     *      reporting in chapter 117 when stat lands).
     */
    SYS_LSEEK           = 101,

    /* Chapter 117 — POSIX-shaped stat / fstat.  Returns a
     * `struct kstat` (defined in vfs.h) by path or fd.  The
     * underlying mode bits use S_IFREG_K / S_IFDIR_K / S_IFCHR_K
     * / S_IFIFO_K / S_IFSOCK_K (vfs.h); the userspace surface
     * in `userspace/libc/sys/stat.h` aliases those to the POSIX
     * S_IF* names and provides the S_IS{REG,DIR,...} macros.
     * size and mtime_ms are 64-bit; mtime_ms is 0 for
     * filesystems that don't track it yet (everything except
     * OSFS-2 — see vfs.c::fill_size_from_fd_entry).
     *
     *   sys_stat(const char *path, struct kstat *out) -> 0 / -errno
     *   sys_fstat(int fd, struct kstat *out)          -> 0 / -errno
     */
    SYS_STAT            = 102,
    SYS_FSTAT           = 103,

    /* Chapter 108d — userspace-driven GPU flush.  Wsd
     * holds a writable mapping of the scanout (via
     * SYS_FB_MAP_SCANOUT) and now also owns composition end to
     * end, so it needs to drive virtio-gpu's flush itself
     * rather than relying on the kernel WM's `compose_all` to
     * stamp TRANSFER_TO_HOST_2D + RESOURCE_FLUSH on every event.
     * After chapter 108d the kernel WM never paints, so without
     * this syscall wsd's pixel stores would sit in scanout RAM
     * forever and never reach the GPU surface.
     *
     *   SYS_FB_PRESENT(uint32_t x, uint32_t y,
     *                  uint32_t w, uint32_t h) -> 0 / -errno
     *     Flush the rect [x,x+w) x [y,y+h) of the scanout to
     *     the GPU.  Passing w=h=0 means "entire scanout".
     *     Out-of-range rects are clipped, not rejected.  The
     *     four args are passed as plain registers so no user
     *     struct copy is needed.  Caller must already hold a
     *     scanout mapping via SYS_FB_MAP_SCANOUT (we don't
     *     gate by pid here \u2014 the syscall just calls
     *     fb_present() and that's a single virtio-gpu submit;
     *     a caller with no FB to read still ends up flushing
     *     whatever bytes are in scanout RAM, which is fine).
     */
    SYS_FB_PRESENT      = 88, /* (x, y, w, h)                -> 0 / -errno */

    /* chapter 108e -- userspace decorations + cursor.  wsd uses
     * these three syscalls to:
     *   - SYS_POINTER_STATE: snapshot the cursor (x, y, btn
     *     bitmap) so it can paint the cursor sprite and hit-
     *     test title-bar / close-button clicks itself, X-server
     *     style.  Any output pointer may be NULL.
     *   - SYS_GUI_MOVE_WINDOW: relocate a kernel-WM window (a
     *     wsd "input shadow") to a new scanout (x,y) after wsd
     *     dragged the title bar.  No event delivery; wsd
     *     handles GUI_EVENT_MOVE-style notifications itself if
     *     needed.
     *   - SYS_GUI_DELIVER_EVENT: inject a gui_event into a
     *     kernel-WM window's event ring so the owner's next
     *     gui_poll_event returns it.  wsd uses this to deliver
     *     GUI_EVENT_CLOSE on a close-button click and (later)
     *     synthesised pointer events.  ev.window_id is
     *     overwritten by the kernel with the syscall's id arg.
     */
    SYS_POINTER_STATE     = 89, /* (int32_t*, int32_t*, uint32_t*) -> 0/-errno */
    SYS_GUI_MOVE_WINDOW   = 90, /* (int id, int32_t x, int32_t y) -> 0/-errno */
    SYS_GUI_DELIVER_EVENT = 91, /* (int id, const gui_event*)      -> 0/-errno */

    /* chapter 108e -- toggle wsd-routed pointer-passthrough on
     * a kernel WM shadow.  When on, kernel hit-test pretends the
     * window isn't there; wsd becomes the sole authority on
     * input routing for that window's pixels.  See struct
     * wm_window.input_passthrough in kernel/core/wm.c. */
    SYS_GUI_SET_INPUT_PASSTHROUGH = 92, /* (int id, int on) -> 0/-errno */

    /* Chapter 90 — mmap + unified page cache.
     *   sys_mmap(addr, len, prot, flags, fd, offset) -> VA / -errno
     *     Supports MAP_PRIVATE | MAP_ANONYMOUS (anonymous, lazy
     *     zero-fill) and MAP_PRIVATE on a ramfs file fd at
     *     PROT_READ.  See mmap_uapi.h for the full list of
     *     accepted flags + protections.  `addr` is ignored;
     *     the kernel always picks a fresh range from the per-AS
     *     mmap bump pointer.  `len`, `offset` must be page-
     *     aligned; `len` must be > 0.
     *   sys_munmap(addr, len) -> 0 / -errno
     *     `addr` must be the exact start of an existing mmap;
     *     partial unmaps return -EINVAL.  `len` is currently
     *     ignored \u2014 the whole vma is removed.
     */
    SYS_MMAP    = 70,
    SYS_MUNMAP  = 71,

    /* Chapter 91 — userspace threads (clone-shaped) + futex.
     *
     *   sys_clone(entry, arg, stack_top, tls) -> tid / -errno
     *     Spawns a new thread that shares the calling thread's
     *     address space.  The new thread starts at user VA
     *     `entry` with x0 = arg, SP_EL0 = stack_top, and
     *     TPIDR_EL0 = tls.  When `entry` returns or calls exit,
     *     the thread terminates like any other.  Both `entry`
     *     and `stack_top` must lie in the user range; `tls` is
     *     opaque to the kernel.  Returns the child's tid in
     *     the parent on success, or -EINVAL/-ENOMEM on failure.
     *     File descriptors are NOT shared — each thread gets a
     *     fresh empty fd table.  Pre-chapter-91 fork copied the
     *     parent's fds; chapter 91 deliberately doesn't because
     *     the floor target (a thread doing CPU work under a
     *     mutex) doesn't need shared fds and per-thread tables
     *     are simpler than refcounted ones.
     *
     *   sys_futex_wait(uaddr, expected) -> 0 / -EAGAIN / -EFAULT
     *     If *uaddr == expected, block on a wait queue keyed by
     *     `uaddr` (per-AS — the same VA in different processes
     *     would alias today, but cross-process futex isn't a
     *     thing in chapter 91 because we have no shared memory).
     *     If *uaddr != expected, return -EAGAIN immediately.
     *     Spurious wakeups are allowed; callers must re-check
     *     the predicate after waking.
     *
     *   sys_futex_wake(uaddr, n) -> count_woken / -EFAULT
     *     Wake up to `n` threads currently blocked on this AS's
     *     `uaddr` queue.  Returns the number actually woken
     *     (may be 0 if no waiter has parked yet).  Pass INT_MAX
     *     for "wake all".
     */
    SYS_CLONE       = 72,
    SYS_FUTEX_WAIT  = 73,
    SYS_FUTEX_WAKE  = 74,
    /* Chapter 92 — clone with explicit CPU placement.  Same
     * contract as SYS_CLONE plus a 5th `cpu_id` argument:
     *
     *   sys_clone2(entry, arg, stack_top, tls, cpu_id) -> tid
     *
     *   cpu_id == -1   : place on the calling thread's CPU
     *                    (identical behaviour to SYS_CLONE).
     *   cpu_id >= 0    : pin the new thread to absolute CPU
     *                    cpu_id (must be < SMP_MAX_CPUS).
     *
     * The new thread inherits its home_cpu from cpu_id and stays
     * there for life — chapter 92 doesn't migrate.  Misplacing a
     * thread on a CPU that doesn't have user-thread support
     * (today: any cpu_id >= 2 in the default 2-CPU build) is
     * caught by the SMP_MAX_CPUS bound; cpu_id of an unbooted
     * but in-range CPU returns -EINVAL via the secondary-CPU
     * boot status. */
    SYS_CLONE2      = 75,
    /* Chapter 92 — return the CPU the calling thread is currently
     * running on.  Used by tests to verify pinning works.  The
     * value is a snapshot — if the caller is preempted between
     * the syscall and using the result, it might end up on a
     * different CPU.  Threads with home_cpu set (i.e. ALL chapter
     * 92 user threads) don't migrate, so for them the snapshot
     * is stable. */
    SYS_GETCPU      = 76,
    /* Chapter 93 — clone with extended argument struct (POSIX
     * pthread_create-style interface, in spirit) and per-clone
     * "what to share" flags.  Argument is a pointer to a
     * `struct clone_args` in user memory; kernel copies it into
     * its own buffer before validating.  Returns the new tid or
     * -errno.
     *
     *   sys_clone3(struct clone_args *uargs) -> tid
     *
     * struct clone_args carries flags / entry / arg / stack_top
     * / tls / cpu_id.  The only flag bit defined today is
     * CLONE_FILES (bit 0) — when set, the new thread shares its
     * parent's fd_table by reference (CLONE_FILES semantics).
     * When clear, the new thread gets a fresh private fd_table
     * (same as SYS_CLONE / SYS_CLONE2).
     *
     * Future flag bits will be rejected as -EINVAL until they
     * are explicitly defined; this keeps userspace from
     * accidentally relying on undefined-bit behaviour. */
    SYS_CLONE3      = 77,

    /* Chapter 95 — wall-clock time.  Pairs with the PL031 RTC
     * boot snapshot in kernel/core/walltime.c.
     *
     *   sys_gettimeofday(struct timeval *out) -> 0 / -EFAULT
     *
     * Writes (tv_sec, tv_usec) where tv_sec is 64-bit signed
     * seconds since 1970-01-01 UTC and tv_usec is the sub-
     * second residual in microseconds (0..999_999).  The 64-
     * bit width is deliberate Y2038-safety: the underlying
     * PL031 wraps in 2038 but the kernel promotes the value
     * before exporting it, so the ABI itself is good past 2038.
     *
     * No timezone information is conveyed — that's a userspace
     * concern.  See /bin/date for an example consumer.
     *
     * `out` must be a writable user pointer; the kernel uses
     * uaccess to copy the struct out, returning -EFAULT if the
     * page is missing or read-only. */
    SYS_GETTIMEOFDAY = 78,

    /* Chapter 96 — synthesise a square wave through the
     * virtio-snd PCM stream.
     *
     *   sys_beep(uint32_t freq_hz, uint32_t duration_ms)
     *       -> 0 on success
     *       -> -ENODEV if no virtio-sound device is present
     *
     * `freq_hz` is clipped to [20, 22050] (Nyquist for our
     * 44_100 Hz sampling rate).  `duration_ms` is clipped to
     * [1, 5000] to bound how long the call blocks.
     *
     * The call BLOCKS the calling thread for approximately
     * `duration_ms` while the device consumes the synthesised
     * samples; it does NOT return early.  Callers that want
     * fire-and-forget audio should spawn a child first.
     *
     * No mixing: concurrent SYS_BEEP calls serialise inside the
     * driver and only the most recent caller's audio is heard
     * cleanly. */
    SYS_BEEP        = 79,

    /* Chapter 100 — per-thread syscall tracer.
     *
     *   sys_trace_me(void) -> 0 / -ENOMEM
     *
     * Allocates a STRACE_RING_CAP-entry ring on the calling
     * thread (see strace.h).  After this returns, every SVC
     * that thread issues is recorded into the ring, viewable
     * as text via `/proc/<pid>/trace`.  Drained on read.
     *
     * Idempotent: a second call is a no-op success.  No way to
     * disable today — the ring is freed when the thread exits.
     *
     * Privilege model: ANY thread may trace itself; there is no
     * way to attach to another pid yet (`ptrace_attach` is a
     * future chapter).  This means /bin/strace works by
     * forking, calling sys_trace_me in the child, then exec'ing
     * the target program; the trace ring survives exec because
     * exec rebuilds the AS but preserves struct thread fields. */
    SYS_TRACE_ME    = 80,
};

/* Chapter 95 — POSIX-shaped wall-clock value.  Layout is part
 * of the userspace ABI (mirrored byte-for-byte in
 * userspace/libc/syscall.h).  The 4-byte _pad keeps the total
 * struct size a multiple of 8 so further fields could be
 * appended without changing alignment of existing ones. */
struct timeval {
    int64_t  tv_sec;     /* seconds since 1970-01-01 UTC      */
    uint32_t tv_usec;    /* microseconds, 0..999_999          */
    uint32_t _pad;
};

/* Chapter 93 — clone3 argument struct.  Layout is part of the
 * userspace ABI; do not reorder.  The 4-byte _pad keeps the
 * struct's total size a multiple of 8 so further additions can
 * append fields without changing alignment of existing ones. */
struct clone_args {
    uint64_t flags;
    uint64_t entry;
    uint64_t arg;
    uint64_t stack_top;
    uint64_t tls;
    int32_t  cpu_id;
    uint32_t _pad;
};

/* Chapter 93 — clone3 flag bits.  Only bit 0 (CLONE_FILES) is
 * defined today; sys_clone3 rejects unknown bits with -EINVAL. */
#define CLONE_FILES  0x01ULL

/* Errno values.  Returned as negatives from syscalls. */
#define ENOSYS    38
#define ENODEV    19   /* matches POSIX ENODEV; see SYS_BEEP */

/* Called from svc_entry in vectors.S after save_context. */
void svc_dispatch(struct exception_frame *frame);

#endif
