/*
 * kernel/core/procfs.h — chapter 99 read-only /proc pseudo-FS.
 *
 * Three things make this fundamentally different from osfs /
 * osfs2 / tmpfs:
 *
 *   1. No blocks anywhere.  Every file's content is generated
 *      on demand from live kernel state — the runqueue, the
 *      heap allocator's counters, the timer's tick count, the
 *      thread list.
 *
 *   2. Snapshot at open time.  When the user opens
 *      `/proc/meminfo`, the entire file is rendered into a
 *      bounded kmalloc'd buffer right then.  Subsequent
 *      `read()`s return slices of that buffer.  The buffer
 *      lives until `close()`.
 *
 *      We could re-render on every read (the way Linux 0.x did
 *      it), but a single read() that walks an enormous thread
 *      list while another thread spawns / exits would be racy
 *      and frustrating to test.  Snapshotting is cheap (every
 *      file is a few hundred bytes) and gives every read of a
 *      given fd a consistent view.
 *
 *   3. Directories are virtual.  `/proc/` enumerates a small
 *      set of well-known names PLUS every live pid.  Each pid
 *      directory exposes a fixed set of leaf files.  No
 *      `dir`-like on-disk structure is needed.
 *
 * The on-disk equivalent would be:
 *
 *   /proc/uptime              ms since boot, seconds.fraction
 *   /proc/meminfo             pmem total/free pages, heap used
 *   /proc/cpuinfo             SMP_MAX_CPUS, smp_cpu_count, etc.
 *   /proc/sched               per-CPU runqueue lengths
 *   /proc/<pid>/status        name, state, parent, home_cpu, cwd
 *   /proc/<pid>/cmdline       NUL-separated argv as stored in
 *                             struct thread::args
 *
 * Used by:
 *   - /bin/ps    walks /proc/, opens each <pid>/status
 *   - /bin/top   ditto + scheduler counters + sleep loop
 *   - future chapters (strace, /proc/<pid>/maps, ...) bolt
 *     more files in here without touching the FS plumbing.
 *
 * Permissions / namespaces are deliberately absent: every file
 * is world-readable, every pid is visible to every thread.  In
 * a real system you'd want at least uid checks here.
 */
#ifndef PROCFS_H
#define PROCFS_H

#include <stddef.h>
#include <stdint.h>

/* Cap on the rendered size of any single /proc file.  Picked
 * to comfortably hold the longest text we generate today (per-
 * thread status = ~150 bytes; a 64-thread sched dump = ~1 KiB)
 * with a generous safety margin.  kmalloc'd once at open, freed
 * at close — no recycle pool. */
#define PROCFS_MAX_FILE  (8 * 1024)

/* Render the contents of `path` (which must be a path under
 * /proc/, with the /proc/ prefix already stripped — e.g.
 * "uptime", "meminfo", "12/status").  Writes up to `cap-1`
 * bytes plus a NUL into `out`.  Returns the byte length
 * written (excluding NUL) on success, or -1 if no such file
 * exists.
 *
 * Caller owns `out`.  Typical caller: vfs_open allocates a
 * PROCFS_MAX_FILE buffer, fills it via procfs_render, stuffs
 * the buffer pointer into the fd entry.  Subsequent reads slice
 * the buffer; close frees it.
 *
 * Safe to call from any thread context — only takes the
 * g_all_lock briefly (via thread_snapshot) and otherwise reads
 * lock-free counters. */
long procfs_render(const char *path, char *out, size_t cap);

/* Enumerate the entries of a /proc directory by index.  Used by
 * sys_listdir_at when the caller asks to list "/proc" or
 * "/proc/<pid>".  Writes the leaf name into `name` (up to cap-1
 * bytes plus NUL), sets `*type_out` to 1 (file) or 2 (dir), and
 * returns the leaf-name length on success.  Returns -1 when idx
 * is past the end of the directory or `path` isn't a procfs
 * directory.
 *
 * `subdir` may be NULL (root) or a pointer to the part of the
 * path after `/proc/` (e.g. "12" for `/proc/12`).  Anything
 * else is unrecognised. */
int procfs_listdir(const char *subdir, int idx,
                    char *name, size_t cap, uint32_t *type_out);

/* True iff `path` (with the /proc/ prefix already stripped)
 * names a directory in /proc — either the root ("") or a live
 * pid ("12").  Used by sys_listdir_at to validate a request
 * before dispatching to procfs_listdir. */
int procfs_is_dir(const char *path);

/* Chapter 113 — vtable adapter for the mount table.  The fs_ops
 * forwards every method to the chapter-99 functions above; it
 * exists so the dispatcher in vfs.c / syscall.c can route /proc
 * through `vfs_resolve` instead of a hand-rolled prefix branch.
 * `cookie` is always NULL (procfs has no per-mount state). */
struct fs_ops;   /* forward decl from vfs.h */
extern const struct fs_ops procfs_fs_ops;

/* Idempotent: registers /proc in the mount table with
 * `procfs_fs_ops` + MOUNT_RO.  Called from vfs_init. */
void procfs_register_mount(void);

#endif /* PROCFS_H */
