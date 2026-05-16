/*
 * kernel/core/syscall.c — syscall dispatcher and minimal handlers.
 *
 * The dispatcher reads ESR_EL1 to confirm we got here via SVC (and
 * not, say, an unhandled data abort routed through the same vector
 * slot), then peels x8 and the argument registers out of the saved
 * frame, dispatches to a handler, and writes the return value back
 * into the frame's x0 slot so restore_context picks it up on the
 * way out via eret.
 *
 * Handlers are deliberately tiny.  SYS_WRITE only supports fd 1 (a
 * synonym for "the kernel console") and copies the bytes verbatim
 * with serial_putc.  When milestone 8 introduces a real VFS,
 * SYS_WRITE will start dispatching by descriptor.
 *
 * Security note: SYS_WRITE blindly trusts the user pointer.  In a
 * real OS we would copy_from_user with bounds-checking against the
 * process's address space.  Until milestone 7 introduces real
 * per-process address spaces this is a known shortcut and the
 * comments below flag the spots that need to grow validation.
 */

#include "syscall.h"
#include "serial.h"
#include "thread.h"
#include "exception.h"
#include "vfs.h"
#include "elf.h"
#include "heap.h"
#include "uaccess.h"
#include "timer.h"
#include "walltime.h"
#include "osfs.h"
#include "osfs2.h"
#include "procfs.h"
#include "pipe.h"
#include "pty.h"
#include "tmpfs.h"
#include "tcp.h"
#include "net.h"
#include "dns.h"
#include "wm.h"
#include "console_in.h"
#include "strace.h"
#include "../device/virtio_input.h"
#include "../device/virtio_tablet.h"
#include "../device/virtio_snd.h"
#include "../device/fb.h"
#include "../arch/address_space.h"
#include "pmem.h"
#include "mmap_uapi.h"
#include "../arch/spinlock.h"
#include "../arch/cpu.h"
#include <stdint.h>

/* ESR_EL1.EC field — bits [31:26].  EC=0x15 is "SVC instruction
 * execution in AArch64 state". */
#define ESR_EC_SHIFT  26
#define ESR_EC_SVC64  0x15u

static long sys_write(long fd, long buf_ptr, long len)
{
    if (len < 0) return -EINVAL_VFS;
    if (len == 0) return 0;

    /* Pipe write end?  Bounce through a small chunk buffer (so we
     * don't need a 4 KiB stack alloc) and forward to pipe_write,
     * which may block if the pipe is full. */
    if (fd >= 0 && fd < FD_TABLE_SIZE) {
        struct fd_entry *e = &thread_current()->fdt->fds[fd];
        if (e->in_use && e->kind == FD_PIPE_W) {
            char chunk[256];
            long total = 0;
            while (total < len) {
                size_t n = (size_t)(len - total);
                if (n > sizeof(chunk)) n = sizeof(chunk);
                if (copy_from_user(chunk, (uint64_t)buf_ptr + (uint64_t)total, n) < 0)
                    return -EFAULT;
                long w = pipe_write(e->pipe, chunk, n);
                if (w < 0) return total > 0 ? total : w;
                total += w;
                if ((size_t)w < n) {
                    /* Short write — pipe_write only returns short
                     * when readers vanished mid-write; loop will
                     * notice on next iteration and bail. */
                }
            }
            return total;
        }
        if (e->in_use && e->kind == FD_TMPFS_RW) {
            char chunk[256];
            long total = 0;
            while (total < len) {
                size_t n = (size_t)(len - total);
                if (n > sizeof(chunk)) n = sizeof(chunk);
                if (copy_from_user(chunk, (uint64_t)buf_ptr + (uint64_t)total, n) < 0)
                    return -EFAULT;
                long w = tmpfs_write(e->ramfs_index, chunk, n);
                if (w < 0) return total > 0 ? total : w;
                total += w;
            }
            return total;
        }
        if (e->in_use && e->kind == FD_OSFS2_FILE) {
            /* OSFS-2 file write: copy each chunk through a
             * stack buffer, hand it to osfs2_write at the
             * fd's current offset, advance the offset by
             * however many bytes the FS accepted. */
            char chunk[256];
            long total = 0;
            while (total < len) {
                size_t n = (size_t)(len - total);
                if (n > sizeof(chunk)) n = sizeof(chunk);
                if (copy_from_user(chunk, (uint64_t)buf_ptr + (uint64_t)total, n) < 0)
                    return -EFAULT;
                long w = osfs2_write(e->osfs2_ino, e->offset, chunk, n);
                if (w < 0) return total > 0 ? total : w;
                e->offset += (uint64_t)w;
                total += w;
                if ((size_t)w < n) break;   /* out of space */
            }
            return total;
        }
        if (e->in_use && e->kind == FD_SOCKET) {
            if (e->socket_cid < 0) return -EBADF;
            char chunk[512];
            long total = 0;
            while (total < len) {
                size_t n = (size_t)(len - total);
                if (n > sizeof(chunk)) n = sizeof(chunk);
                if (copy_from_user(chunk, (uint64_t)buf_ptr + (uint64_t)total, n) < 0)
                    return -EFAULT;
                /* tcp_send accepts what fits in the TX buffer; if
                 * it returns short we yield so the segmenter
                 * drains and try again. */
                long w = tcp_send(e->socket_cid, chunk, (uint32_t)n);
                if (w < 0) return total > 0 ? total : -EIO;
                if (w == 0) { (void)net_poll(); yield(); continue; }
                total += w;
            }
            return total;
        }
        if (e->in_use && (e->kind == FD_PTY_MASTER ||
                          e->kind == FD_PTY_SLAVE)) {
            char chunk[256];
            long total = 0;
            while (total < len) {
                size_t n = (size_t)(len - total);
                if (n > sizeof(chunk)) n = sizeof(chunk);
                if (copy_from_user(chunk, (uint64_t)buf_ptr + (uint64_t)total, n) < 0)
                    return -EFAULT;
                long w = (e->kind == FD_PTY_MASTER)
                    ? pty_master_write(e->pty, chunk, n)
                    : pty_slave_write(e->pty, chunk, n);
                if (w < 0) return total > 0 ? total : w;
                total += w;
                if ((size_t)w < n) {
                    /* Master-side short writes can happen because
                     * Ctrl-C bytes are eaten silently — but we
                     * report them as accepted, so this branch
                     * shouldn't fire today.  Defensive only. */
                }
            }
            return total;
        }
    }

    if (fd != 1 && fd != 2) return -ENOSYS;

    /* Stream-copy in chunks so we don't need a huge bounce buffer
     * for large writes. */
    char chunk[256];
    long total = 0;
    while (total < len) {
        size_t n = (size_t)(len - total);
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (copy_from_user(chunk, (uint64_t)buf_ptr + (uint64_t)total, n) < 0)
            return -EFAULT;
        for (size_t i = 0; i < n; i++)
            serial_putc(chunk[i]);
        total += (long)n;
    }
    return len;
}

static long sys_exit(long code)
{
    serial_puts("[sys_exit] thread '");
    serial_puts(thread_current()->name);
    serial_puts("' exited with code ");
    serial_puthex((uint64_t)code);
    serial_puts("\n");
    thread_exit((int)code);   /* never returns */
    return 0;                 /* unreachable */
}

static long sys_getpid(void)
{
    return thread_current()->id;
}

/* Drain any pending virtio-input events into the WM ring.  Without
 * this hook GUI apps that never call read() (only yield + poll the
 * GUI event queue) would never see keystrokes — virtio_input_poll
 * is otherwise only invoked from console_try_getc on a stdin read.
 *
 * NOTE: do NOT gate this on wm_has_windows().  The cursor sprite
 * is owned by the WM and lives even when no app has a window open
 * (e.g. after the user closes the last one), and the tablet's
 * used-ring fills up whether or not anyone is listening.  An older
 * version of this function early-returned when the window list
 * was empty, which left the cursor advancing only at whatever
 * rate non-GUI processes happened to yield() — visible as choppy
 * mouse motion the moment the last window closes.  The keyboard
 * helpers below are already no-ops when no window has focus, so
 * always running them is free in the empty-WM case. */
static void pump_input_into_wm(void)
{
    if (virtio_input_present()) {
        char c;
        while (virtio_input_try_getc(&c))
            (void)wm_keyboard_byte(c);
    }
    /* End-of-batch boundary for the WM's CSI parser: any partial
     * "ESC ..." sequence held back during the drain above is now
     * flushed as a bare ESC keypress.  See wm_flush_pending_keys. */
    wm_flush_pending_keys();
    if (virtio_tablet_present())
        virtio_tablet_poll();
}

static long sys_yield(void)
{
    pump_input_into_wm();
    yield();
    return 0;
}

static long sys_open(long name_ptr, long flags)
{
    char path[128];
    long n = copy_string_from_user(path, (uint64_t)name_ptr, sizeof(path));
    if (n < 0) return n;
    return vfs_open(path, (int)flags);
}

static long sys_read(long fd, long buf_ptr, long len)
{
    if (len < 0) return -EINVAL_VFS;
    if (len == 0) return 0;

    /* Bounds check the user buffer first so we don't trash kernel
     * memory if vfs_read writes more than expected. */
    if (uaccess_check((uint64_t)buf_ptr, (size_t)len) < 0)
        return -EFAULT;

    /* The active TTBR0 IS the caller's AS, so vfs_read can write
     * directly to the user buffer.  This is safe because we just
     * proved the buffer is in user range — even if vfs_read
     * walks the pointer arithmetic, it can't escape into kernel
     * memory. */
    return vfs_read((int)fd, (void *)(uintptr_t)buf_ptr, (size_t)len);
}

static long sys_close(long fd)
{
    return vfs_close((int)fd);
}

/*
 * sys_spawn(const char *path, const char *args) — load a ramfs file
 * as ELF and launch it as a new user thread.  The optional `args`
 * string (NUL-terminated, may be NULL or "") is split on whitespace
 * into individual argv tokens; the path is prepended as argv[0]
 * and the resulting array (with a NULL terminator) is laid out on
 * the new thread's user stack by the ELF loader.  The child sees
 * them as `int main(int argc, char **argv)`.
 *
 * For backward compat the entire (un-split) args string is also
 * stashed in thread.args so SYS_GETARGS still works.  Programs
 * are expected to migrate to argv at their leisure.
 *
 * Returns the new thread id, or a negative errno on failure.
 */
#define MAX_SPAWN_ARGV 16

static long sys_spawn(long name_ptr, long args_ptr)
{
    /* Copy path and args into kernel-owned buffers so that we
     * don't accidentally dereference user pointers from a stale
     * AS later (we destroy the parent's AS on context-switch). */
    char path[128];
    long pn = copy_string_from_user(path, (uint64_t)name_ptr, sizeof(path));
    if (pn < 0) return pn;

    char args[THREAD_ARGS_MAX];
    args[0] = '\0';
    if (args_ptr) {
        long an = copy_string_from_user(args, (uint64_t)args_ptr, sizeof(args));
        if (an < 0) return an;
    }

    /* Split args into argv tokens.  The argv pointers index into
     * a working copy (`args_split`) so we can NUL-terminate each
     * token without trampling the original `args` (which is also
     * stashed in thread.args for SYS_GETARGS). */
    char args_split[THREAD_ARGS_MAX];
    for (size_t i = 0; i < THREAD_ARGS_MAX; i++) args_split[i] = args[i];

    const char *argv[MAX_SPAWN_ARGV + 1];
    int argc = 0;
    argv[argc++] = path;
    {
        char *p = args_split;
        while (*p && argc < MAX_SPAWN_ARGV) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    argv[argc] = NULL;

    /* vfs_load handles ramfs OR OSFS-mounted (/mnt/, /bin/) paths
     * uniformly and hands us a kheap-owned buffer. */
    uint8_t *data; size_t size;
    int rc = vfs_load(path, &data, &size);
    if (rc < 0) return rc;

    /* Per-process address space.  Owns the user pages we are about
     * to allocate; freed when the thread is reaped. */
    struct address_space *as = address_space_create();
    if (!as) { kfree(data); return -ENOMEM_VFS; }

    struct user_image img;
    int loaded = elf_load_user(data, size, as, argv, &img);
    kfree(data);
    if (loaded != 0) { address_space_destroy(as); return -EINVAL_VFS; }

    struct thread *t = user_thread_create(img.entry_va, img.stack_top_va, path, as);
    if (!t) { address_space_destroy(as); return -ENOMEM_VFS; }

    /* Inherit the parent's fd table into the child (POSIX
     * fork+exec semantics).  Chapter 79b: when /bin/sh is itself
     * running with fd 0/1/2 wired to a pty slave (because it was
     * spawned by gui_term), every command the shell runs must see
     * the same pty — otherwise `uptime` writes its bytes to the
     * raw serial UART and they never reach the gui_term window.
     * Pre-79b the default-console placeholders happened to be
     * correct because the only sh in the system ran on the serial
     * UART; this assumption now needs to be paid for. */
    thread_inherit_fds(t, thread_current());

    /* Args were already copied into a kernel buffer above. */
    size_t i = 0;
    for (; i + 1 < THREAD_ARGS_MAX && args[i] != '\0'; i++)
        t->args[i] = args[i];
    t->args[i] = '\0';

    return (long)t->id;
}

/*
 * sys_spawn_redir(path, args, stdin_path) — like sys_spawn but
 * pre-opens stdin_path as fd 0 in the new thread before it runs.
 * If stdin_path is NULL or empty the call degenerates to plain
 * sys_spawn.  Path resolution mirrors vfs_open (OSFS for /mnt|/bin,
 * ramfs otherwise).  Returns the new tid or -errno.
 */
static long sys_spawn_redir(long name_ptr, long args_ptr, long stdin_ptr)
{
    char path[128];
    long pn = copy_string_from_user(path, (uint64_t)name_ptr, sizeof(path));
    if (pn < 0) return pn;

    char args[THREAD_ARGS_MAX];
    args[0] = '\0';
    if (args_ptr) {
        long an = copy_string_from_user(args, (uint64_t)args_ptr, sizeof(args));
        if (an < 0) return an;
    }

    char stdin_path[128];
    stdin_path[0] = '\0';
    if (stdin_ptr) {
        long sn = copy_string_from_user(stdin_path, (uint64_t)stdin_ptr, sizeof(stdin_path));
        if (sn < 0) return sn;
    }

    /* If no redirect requested, behave exactly like sys_spawn. */
    if (stdin_path[0] == '\0')
        return sys_spawn(name_ptr, args_ptr);

    /* Validate stdin_path BEFORE we burn the cost of an ELF
     * load + thread create.  The actual fd-table installation
     * happens after thread_create using the same resolution. */
    {
        const char *bare = NULL;
        if (stdin_path[0] == '/' && stdin_path[1] == 'm' &&
            stdin_path[2] == 'n' && stdin_path[3] == 't' &&
            stdin_path[4] == '/')
            bare = stdin_path + 5;
        else if (stdin_path[0] == '/' && stdin_path[1] == 'b' &&
                 stdin_path[2] == 'i' && stdin_path[3] == 'n' &&
                 stdin_path[4] == '/')
            bare = stdin_path + 5;
        if (bare) {
            uint32_t s, z;
            if (osfs_lookup(bare, &s, &z) != 0) return -ENOENT_VFS;
        } else {
            const uint8_t *d; size_t z;
            if (vfs_lookup(stdin_path, &d, &z) != 0) return -ENOENT_VFS;
        }
    }

    /* Same body as sys_spawn from here, except for the fd-0
     * install at the end. */
    char args_split[THREAD_ARGS_MAX];
    for (size_t i = 0; i < THREAD_ARGS_MAX; i++) args_split[i] = args[i];

    const char *argv[MAX_SPAWN_ARGV + 1];
    int argc = 0;
    argv[argc++] = path;
    {
        char *p = args_split;
        while (*p && argc < MAX_SPAWN_ARGV) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    argv[argc] = NULL;

    uint8_t *data; size_t size;
    int rc = vfs_load(path, &data, &size);
    if (rc < 0) return rc;

    struct address_space *as = address_space_create();
    if (!as) { kfree(data); return -ENOMEM_VFS; }

    struct user_image img;
    int loaded = elf_load_user(data, size, as, argv, &img);
    kfree(data);
    if (loaded != 0) { address_space_destroy(as); return -EINVAL_VFS; }

    struct thread *t = user_thread_create(img.entry_va, img.stack_top_va, path, as);
    if (!t) { address_space_destroy(as); return -ENOMEM_VFS; }

    /* Inherit parent's fd 1/2 (and any other slots) before
     * overriding fd 0; the redirect only touches stdin.  Same
     * pty-propagation rationale as sys_spawn — see comment
     * there. */
    thread_inherit_fds(t, thread_current());

    size_t i = 0;
    for (; i + 1 < THREAD_ARGS_MAX && args[i] != '\0'; i++)
        t->args[i] = args[i];
    t->args[i] = '\0';

    /* Install fd 0 from the (already-validated) path.  The
     * lookup tables are global / monotonic — having validated a
     * moment ago, this cannot fail.  vfs_open_into closes any
     * pre-existing fd in the slot (including a pty slave we just
     * inherited) so no refcount leak. */
    (void)vfs_open_into(t, 0, stdin_path, 0);

    return (long)t->id;
}

/*
 * sys_spawn_pipe(path, args, stdin_fd, stdout_fd) — like sys_spawn
 * but optionally inherits two of the parent's fds as the new
 * thread's fd 0 (stdin) and fd 1 (stdout).  A fd of -1 means
 * "leave the default console in that slot".  This is the
 * primitive the shell uses to wire up `cmd1 | cmd2` pipelines.
 *
 * fds are interpreted in the *calling* (parent) thread's fd
 * table.  The parent retains its references; a successful
 * spawn_pipe bumps the underlying object's refcount (for pipes)
 * so that the child holds an independent reference.
 */
static int dup_parent_fd_into_child(struct thread *parent, int pfd,
                                    struct thread *child, int cfd)
{
    if (pfd < 0 || pfd >= FD_TABLE_SIZE) return -EBADF;
    struct fd_entry *src = &parent->fdt->fds[pfd];
    if (!src->in_use) return -EBADF;

    /* Tear down any pre-existing object in the child's target
     * slot before we overwrite — chapter 79b made sys_spawn_pipe
     * call thread_inherit_fds first, which may have planted a
     * pty slave (or another pipe end) at cfd that we must not
     * leak. */
    struct fd_entry *dst = &child->fdt->fds[cfd];
    if (dst->in_use) {
        if      (dst->kind == FD_PIPE_R && dst->pipe) pipe_unref(dst->pipe, PIPE_REF_R);
        else if (dst->kind == FD_PIPE_W && dst->pipe) pipe_unref(dst->pipe, PIPE_REF_W);
        else if (dst->kind == FD_PTY_MASTER && dst->pty) pty_close_master(dst->pty);
        else if (dst->kind == FD_PTY_SLAVE  && dst->pty) pty_close_slave(dst->pty);
    }
    *dst = *src;
    dst->in_use = 1;
    /* Independent reference for the child. */
    if (dst->kind == FD_PIPE_R && dst->pipe)
        dst->pipe->r_refs++;
    else if (dst->kind == FD_PIPE_W && dst->pipe)
        dst->pipe->w_refs++;
    else if (dst->kind == FD_PTY_MASTER && dst->pty) {
        dst->pty->refs++;
        dst->pty->s2m->r_refs++;
        dst->pty->m2s->w_refs++;
    }
    else if (dst->kind == FD_PTY_SLAVE && dst->pty) {
        dst->pty->refs++;
        dst->pty->m2s->r_refs++;
        dst->pty->s2m->w_refs++;
    }
    return 0;
}

static long sys_spawn_pipe(long name_ptr, long args_ptr,
                           long stdin_fd_signed, long stdout_fd_signed)
{
    int sin_fd  = (int)stdin_fd_signed;
    int sout_fd = (int)stdout_fd_signed;

    /* Degenerate to plain spawn if no redirection is requested. */
    if (sin_fd < 0 && sout_fd < 0)
        return sys_spawn(name_ptr, args_ptr);

    /* Validate the parent's fds before spending an ELF load. */
    struct thread *parent = thread_current();
    if (sin_fd >= 0) {
        if (sin_fd >= FD_TABLE_SIZE || !parent->fdt->fds[sin_fd].in_use)
            return -EBADF;
    }
    if (sout_fd >= 0) {
        if (sout_fd >= FD_TABLE_SIZE || !parent->fdt->fds[sout_fd].in_use)
            return -EBADF;
    }

    /* Same body as sys_spawn. */
    char path[128];
    long pn = copy_string_from_user(path, (uint64_t)name_ptr, sizeof(path));
    if (pn < 0) return pn;

    char args[THREAD_ARGS_MAX];
    args[0] = '\0';
    if (args_ptr) {
        long an = copy_string_from_user(args, (uint64_t)args_ptr, sizeof(args));
        if (an < 0) return an;
    }

    char args_split[THREAD_ARGS_MAX];
    for (size_t i = 0; i < THREAD_ARGS_MAX; i++) args_split[i] = args[i];

    const char *argv[MAX_SPAWN_ARGV + 1];
    int argc = 0;
    argv[argc++] = path;
    {
        char *p = args_split;
        while (*p && argc < MAX_SPAWN_ARGV) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    argv[argc] = NULL;

    uint8_t *data; size_t size;
    int rc = vfs_load(path, &data, &size);
    if (rc < 0) return rc;

    struct address_space *as = address_space_create();
    if (!as) { kfree(data); return -ENOMEM_VFS; }

    struct user_image img;
    int loaded = elf_load_user(data, size, as, argv, &img);
    kfree(data);
    if (loaded != 0) { address_space_destroy(as); return -EINVAL_VFS; }

    struct thread *t = user_thread_create(img.entry_va, img.stack_top_va, path, as);
    if (!t) { address_space_destroy(as); return -ENOMEM_VFS; }

    /* Inherit parent's fd table first (so fd 2 / fd 3+ flow into
     * the child).  Then override fd 0 / fd 1 with the requested
     * pipe ends.  Chapter 79b: without the inherit step a piped
     * command run under gui_term would write its diagnostics
     * (stderr) to the raw serial UART instead of the pty
     * slave. */
    thread_inherit_fds(t, parent);

    size_t i = 0;
    for (; i + 1 < THREAD_ARGS_MAX && args[i] != '\0'; i++)
        t->args[i] = args[i];
    t->args[i] = '\0';

    /* Wire stdin / stdout from the parent's fds.  Parent still
     * holds its references; cleanup is the caller's job (the
     * shell will close its own pipe ends after spawning).  The
     * dup helper releases any inherited slot before installing
     * the pipe end, so the inheritance above doesn't leak. */
    if (sin_fd  >= 0) (void)dup_parent_fd_into_child(parent, sin_fd,  t, 0);
    if (sout_fd >= 0) (void)dup_parent_fd_into_child(parent, sout_fd, t, 1);

    return (long)t->id;
}

/*
 * sys_getargs(char *buf, size_t len) — copy the calling thread's
 * spawn-time args string into `buf`.  Returns the number of bytes
 * copied (excluding the NUL), or 0 if there are no args.  Always
 * NUL-terminates if len > 0.
 */
static long sys_getargs(long buf_ptr, long len)
{
    if (!buf_ptr || len <= 0) return 0;
    size_t cap = (size_t)len;
    const char *src = thread_current()->args;
    /* Determine source length so we can size the copy precisely. */
    size_t srclen = 0;
    while (srclen + 1 < cap && src[srclen] != '\0') srclen++;
    /* Build a kernel-side staging buffer with the trailing NUL. */
    char tmp[THREAD_ARGS_MAX];
    size_t i = 0;
    for (; i < srclen; i++) tmp[i] = src[i];
    tmp[i] = '\0';
    if (copy_to_user((uint64_t)buf_ptr, tmp, srclen + 1) < 0)
        return -EFAULT;
    return (long)srclen;
}

/*
 * sys_wait(int *code_out) — block until any child of the caller
 * exits, then return its tid and (optionally) store its exit code
 * in *code_out.  Returns -1 if the caller has no children.
 */
static long sys_wait(long code_out_ptr)
{
    int code = 0;
    int tid  = thread_wait(&code);
    if (code_out_ptr) {
        if (copy_to_user((uint64_t)code_out_ptr, &code, sizeof(code)) < 0)
            return -EFAULT;
    }
    return (long)tid;
}

/*
 * sys_waitpid(int pid, int *code_out, int options) — chapter 78.
 * Generalised reaper: filter by specific pid, optional WNOHANG.
 * See SYS_WAITPID doc in syscall.h for the contract.  We only
 * write *code_out when we actually reap (return > 0), to keep
 * the WNOHANG=poll case from clobbering caller storage with a
 * stale value.
 */
static long sys_waitpid(long pid, long code_out_ptr, long options)
{
    int code = 0;
    int tid  = thread_waitpid((int)pid, &code, (int)options);
    if (tid > 0 && code_out_ptr) {
        if (copy_to_user((uint64_t)code_out_ptr, &code, sizeof(code)) < 0)
            return -EFAULT;
    }
    return (long)tid;
}

/*
 * sys_sbrk(intptr_t inc) — adjust the calling process's program
 * break by `inc` bytes (positive grows, negative shrinks).  Returns
 * the *previous* break (so the caller's malloc can use it as the
 * start of the freshly-allocated region).  Returns -1 on failure
 * (OOM, brk would leave the heap range, or caller is a kernel
 * thread without an AS).
 *
 * Concurrency note: this mutates the per-process page tables.
 * It's safe today because we're single-CPU and never preempt
 * inside a syscall — the only thing that can run between map
 * operations is an IRQ, and our IRQ handlers don't touch user
 * page tables.  Once we have multiple cores or in-syscall
 * preemption this'll need a per-AS lock.
 */
static long sys_sbrk(long inc)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -1;

    uint64_t old_brk = t->as->heap_brk;

    /* Compute the requested new break with overflow / signed-shrink
     * carefully.  inc may be negative. */
    int64_t  signed_inc = (int64_t)inc;
    uint64_t new_brk;
    if (signed_inc >= 0) {
        new_brk = old_brk + (uint64_t)signed_inc;
        if (new_brk < old_brk) return -1;            /* wrap */
    } else {
        uint64_t mag = (uint64_t)(-signed_inc);
        if (mag > old_brk - USER_HEAP_BASE) {
            new_brk = USER_HEAP_BASE;                /* clamp at base */
        } else {
            new_brk = old_brk - mag;
        }
    }

    if (address_space_set_brk(t->as, new_brk) != 0)
        return -1;

    return (long)old_brk;
}

/*
 * sys_mmap(addr, len, prot, flags, fd, offset) — chapter 90.
 *
 * Supports the floor described in mmap_uapi.h:
 *
 *   MAP_PRIVATE | MAP_ANONYMOUS               (writable iff PROT_WRITE)
 *   MAP_PRIVATE on a ramfs file fd, PROT_READ only
 *
 * Anything else returns -EINVAL.  `addr` is ignored (no
 * MAP_FIXED yet); the kernel always picks a fresh range from
 * each AS's mmap bump pointer.
 *
 * Errors:
 *   -EINVAL  bad/unsupported flags or prot, len == 0,
 *            non-page-aligned len/offset, MAP_FIXED requested,
 *            MAP_SHARED requested, file mmap with PROT_WRITE
 *   -EBADF   file mmap with an invalid/non-ramfs fd
 *   -ENOMEM  out of mmap VA space, OOM in the vma allocator,
 *            or page cache full at fault time (deferred)
 */
static long sys_mmap(long addr_unused, long len, long prot, long flags,
                     long fd, long offset)
{
    (void)addr_unused;       /* MAP_FIXED would honour this */
    struct thread *t = thread_current();
    if (!t || !t->as) return -EINVAL_VFS;

    if (len <= 0) return -EINVAL_VFS;
    if (((uint64_t)len) & (PAGE_SIZE - 1)) return -EINVAL_VFS;
    uint64_t pages = ((uint64_t)len) / PAGE_SIZE;

    /* Reject every flag we don't support. */
    if (flags & MAP_FIXED) return -EINVAL_VFS;
    if ((flags & MAP_PRIVATE) == 0) return -EINVAL_VFS;
    /* MAP_SHARED would land here too \u2014 we just don't define it
     * in mmap_uapi.h, but defensive: reject any flag bits we
     * don't recognise. */
    long known = MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS;
    if (flags & ~known) return -EINVAL_VFS;

    /* prot: only PROT_READ and PROT_WRITE matter; PROT_EXEC is
     * accepted-but-ignored for now (chapter-90 mmap is data-only).
     * PROT_NONE (no read, no write) is rejected because we don't
     * have guard-page support yet. */
    long known_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    if (prot & ~known_prot) return -EINVAL_VFS;
    if ((prot & (PROT_READ | PROT_WRITE)) == 0) return -EINVAL_VFS;

    if (flags & MAP_ANONYMOUS) {
        uint64_t va = address_space_mmap_anon(t->as, pages, (uint32_t)prot);
        if (!va) return -ENOMEM_VFS;
        return (long)va;
    }

    /* File-backed.  Chapter 90: PROT_READ only, ramfs only. */
    if (prot & PROT_WRITE) return -EINVAL_VFS;
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    struct fd_entry *e = &t->fdt->fds[fd];
    if (!e->in_use) return -EBADF;
    if (e->kind != FD_FILE) return -EINVAL_VFS;
    /* FD_FILE distinguishes ramfs vs osfs by osfs_size != 0.
     * Chapter 90 supports ramfs only. */
    if (e->osfs_size != 0) return -EINVAL_VFS;
    if (e->ramfs_index < 0) return -EBADF;

    if (((uint64_t)offset) & (PAGE_SIZE - 1)) return -EINVAL_VFS;

    uint64_t va = address_space_mmap_ramfs(t->as, pages,
                                           (uint32_t)e->ramfs_index,
                                           (uint64_t)offset);
    if (!va) return -ENOMEM_VFS;
    return (long)va;
}

static long sys_munmap(long addr, long len)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -EINVAL_VFS;
    (void)len;       /* chapter 90 only supports whole-vma unmap */
    if (((uint64_t)addr) & (PAGE_SIZE - 1)) return -EINVAL_VFS;
    if (address_space_munmap(t->as, (uint64_t)addr) != 0)
        return -EINVAL_VFS;
    return 0;
}

/* ============================================================
 * Chapter 91 — userspace threads (SYS_CLONE) and futex.
 *
 * Mental model:
 *
 *   - SYS_CLONE: spawn a fresh thread that shares the calling
 *     thread's address space.  The kernel-level
 *     user_thread_create_shared bumps the AS refcount; the
 *     refcount-aware address_space_destroy makes sure the page
 *     tables only go away when the LAST thread referencing
 *     them exits.
 *
 *   - SYS_FUTEX_WAIT / SYS_FUTEX_WAKE: kernel-blocking sync
 *     primitives that user-space mutexes layer on top of with
 *     a fast atomic_cmpxchg path.  The wait queue is keyed on
 *     the user VA itself: same-AS threads see the same VA so
 *     they hash to the same key, while cross-AS aliasing
 *     causes only spurious wakes (the awoken thread re-checks
 *     the predicate and goes back to sleep).
 *
 * Race story: with chapter 89 SMP, user threads are pinned to
 * CPU 0.  The only thing that can preempt a futex syscall is a
 * timer IRQ on the same CPU 0.  We close the wait/wake race by
 * masking IRQs from "predicate check" through "state =
 * BLOCKED" — yield() itself manages DAIF from there on (and
 * cswitch_to's eret restores IRQs-on for the next thread, so
 * we never strand the scheduler in IRQs-off state).
 * ============================================================ */

#define EAGAIN_FUTEX  11    /* matches POSIX EAGAIN; futex-only here */

/*
 * sys_clone(entry, arg, stack_top, tls) -> tid / -errno
 *
 * Spawns a new thread sharing the calling thread's AS.  See the
 * kernel/core/syscall.h block for the contract.
 */
static long sys_clone(long entry, long arg, long stack_top, long tls)
{
    struct thread *parent = thread_current();
    if (!parent || !parent->as) return -EINVAL_VFS;

    /* Both `entry` and `stack_top` must point into the user
     * range.  We don't check that they're actually mapped — the
     * page-fault handler will kill the new thread cleanly if
     * the caller passed garbage, which is the same failure mode
     * as a bogus user-space jump. */
    uint64_t entry_va = (uint64_t)entry;
    uint64_t sp_top   = (uint64_t)stack_top;
    if (entry_va < USER_VA_BASE || entry_va >= USER_VA_END)
        return -EINVAL_VFS;
    if (sp_top   <= USER_VA_BASE || sp_top   >  USER_VA_END)
        return -EINVAL_VFS;
    /* Stacks grow down; require 16-byte alignment of the
     * initial SP_EL0 to satisfy AAPCS at the first call. */
    if (sp_top & 0xFULL) return -EINVAL_VFS;

    struct thread *child = user_thread_create_shared(entry_va,
                                                     sp_top,
                                                     "clone",
                                                     parent->as,
                                                     (uint64_t)arg,
                                                     (uint64_t)tls);
    if (!child) return -ENOMEM_VFS;

    return (long)child->id;
}

/*
 * sys_futex_wait(uaddr, expected) -> 0 / -EAGAIN / -EFAULT
 *
 * Block on `uaddr` if *uaddr == expected; otherwise return
 * -EAGAIN immediately.  Spurious wakes allowed; the user-space
 * caller (e.g. mutex_lock) is expected to loop and re-check.
 *
 * Chapter 92 update: the chapter 91 implementation closed the
 * wait/wake race by masking IRQs around (predicate, state-set).
 * That worked because user threads were pinned to CPU 0 (chapter
 * 89 invariant) so the only thing that could preempt was a
 * same-CPU timer IRQ.  Once chapter 92 puts user threads on CPU
 * 1, an unlocker on the OTHER CPU is no longer blocked by our
 * IRQ disable — it can race the gap freely.  We close it with a
 * spinlock instead: thread_global_lock takes g_all_lock, which
 * is the same lock thread_wake_blocked walks under, so wait and
 * wake are now mutually-exclusive.  The window between
 * thread_block_on_held releasing g_all_lock and yield's actual
 * cswitch is benign (see thread_block_on_held banner).
 */
static long sys_futex_wait(long uaddr, long expected)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -EINVAL_VFS;
    if ((uint64_t)uaddr < USER_VA_BASE || (uint64_t)uaddr >= USER_VA_END)
        return -EFAULT;
    if (uaddr & 0x3) return -EINVAL_VFS;     /* 32-bit aligned */

    uint64_t flags = thread_global_lock();

    /* Read the predicate.  We're in t->as so the user VA is
     * directly addressable; copy_from_user adds bounds-check
     * (we already validated the range above).  Page-fault risk
     * inside the lock: the user mutex_t is in normal data
     * memory that the calling thread already touched
     * (atomic_cmpxchg on the same word right before the
     * syscall), so the page is mapped and we won't fault. */
    uint32_t cur;
    if (copy_from_user(&cur, (uint64_t)uaddr, sizeof(cur)) < 0) {
        thread_global_unlock(flags);
        return -EFAULT;
    }
    if (cur != (uint32_t)expected) {
        thread_global_unlock(flags);
        return -EAGAIN_FUTEX;
    }

    /* Block on the user VA as the wait-queue key.  Same-AS
     * threads see the same VA → same key → wake works.  Cross-
     * AS aliasing causes only spurious wakes (the post-wake
     * predicate re-read will mismatch and the caller will
     * re-block).  thread_block_on_held releases g_all_lock +
     * IRQs before yielding so a wake from the other CPU can
     * proceed promptly. */
    thread_block_on_held((void *)(uintptr_t)uaddr, flags);

    /* Returned from yield — IRQs are on (cswitch_to's eret
     * restored saved SPSR with I=0). */
    return 0;
}

/*
 * sys_futex_wake(uaddr, n) -> count_woken / -EFAULT
 *
 * Wake threads currently blocked on this AS's `uaddr` queue.
 *
 * Chapter 91 floor: `n` is currently treated as "wake all"
 * regardless of value (for n >= 1).  thread_wake_blocked is the
 * existing pipe-style wake primitive — it walks g_all_head and
 * marks every BLOCKED thread with a matching token READY.
 * Returns 1 if at least one wake was attempted, 0 if n == 0.
 *
 * The thundering-herd cost from "wake all" only matters once
 * a futex has many waiters; the chapter 91 mutex with 4
 * threads is fine.  Bounding `n` honestly would require
 * exposing g_all_head walk through a new helper; deferred.
 */
static long sys_futex_wake(long uaddr, long n)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -EINVAL_VFS;
    if ((uint64_t)uaddr < USER_VA_BASE || (uint64_t)uaddr >= USER_VA_END)
        return -EFAULT;
    if (uaddr & 0x3) return -EINVAL_VFS;
    if (n < 0) return -EINVAL_VFS;
    if (n == 0) return 0;

    thread_wake_blocked((void *)(uintptr_t)uaddr);
    return 1;
}

/*
 * sys_clone2(entry, arg, stack_top, tls, cpu_id) -> tid / -errno
 *
 * Chapter 92 — same contract as sys_clone but with explicit CPU
 * placement.  cpu_id == -1 means "current CPU" (identical to
 * SYS_CLONE).  cpu_id in [0, SMP_MAX_CPUS) pins the new thread
 * to that absolute CPU; out-of-range values are -EINVAL.
 *
 * Implementation just delegates to user_thread_create_shared_on
 * which knows about home_cpu and runs runq_push_to so the new
 * thread is enqueued on its target CPU's runqueue (with an
 * IPI_RESCHED if cross-CPU).  All the chapter-91 validation is
 * shared with sys_clone.
 */
static long sys_clone2(long entry, long arg, long stack_top,
                       long tls, long cpu_id)
{
    struct thread *parent = thread_current();
    if (!parent || !parent->as) return -EINVAL_VFS;

    uint64_t entry_va = (uint64_t)entry;
    uint64_t sp_top   = (uint64_t)stack_top;
    if (entry_va < USER_VA_BASE || entry_va >= USER_VA_END)
        return -EINVAL_VFS;
    if (sp_top   <= USER_VA_BASE || sp_top   >  USER_VA_END)
        return -EINVAL_VFS;
    if (sp_top & 0xFULL) return -EINVAL_VFS;

    /* cpu_id is signed long because we accept -1 as "current".
     * Reject values that don't fit a CPU index. */
    if (cpu_id < -1 || cpu_id >= (long)SMP_MAX_CPUS)
        return -EINVAL_VFS;

    struct thread *child = user_thread_create_shared_on(entry_va,
                                                        sp_top,
                                                        "clone",
                                                        parent->as,
                                                        (uint64_t)arg,
                                                        (uint64_t)tls,
                                                        (int)cpu_id);
    if (!child) return -ENOMEM_VFS;

    return (long)child->id;
}

/*
 * sys_clone3(struct clone_args *uargs) -> tid / -errno
 *
 * Chapter 93 — clone with extended argument struct and per-clone
 * "what to share" flags.  Argument is a USER pointer to a
 * struct clone_args; we copy_from_user into a kernel-side
 * buffer before validating any field, so a hostile user
 * can't race the validation by mutating the struct mid-syscall.
 *
 * Today the only flag bit defined is CLONE_FILES (bit 0).
 * Other bits will eventually carry CLONE_VM, CLONE_SIGHAND,
 * etc; for now any unknown bit is rejected with -EINVAL so
 * userspace can't accidentally rely on undefined-bit behaviour.
 */
static long sys_clone3(long uargs)
{
    struct thread *parent = thread_current();
    if (!parent || !parent->as) return -EINVAL_VFS;

    struct clone_args a;
    if (copy_from_user(&a, (uint64_t)uargs, sizeof(a)) < 0)
        return -EINVAL_VFS;

    /* Reject any flag bit we don't know about yet.  This is the
     * "extensibility safety" — userspace that links against an
     * older kernel must not silently get the future fancier
     * semantics it didn't ask for. */
    const uint64_t known_flags = CLONE_FILES;
    if (a.flags & ~known_flags) return -EINVAL_VFS;

    /* Validate entry / stack the same way sys_clone /
     * sys_clone2 do.  All EL0 user mappings live in the
     * [USER_VA_BASE, USER_VA_END) range. */
    if (a.entry < USER_VA_BASE || a.entry >= USER_VA_END)
        return -EINVAL_VFS;
    if (a.stack_top <= USER_VA_BASE || a.stack_top > USER_VA_END)
        return -EINVAL_VFS;
    if (a.stack_top & 0xFULL) return -EINVAL_VFS;

    /* cpu_id is signed: -1 means "current CPU", otherwise must
     * fit a CPU index. */
    if (a.cpu_id < -1 || a.cpu_id >= (int32_t)SMP_MAX_CPUS)
        return -EINVAL_VFS;

    int share_fdt = (a.flags & CLONE_FILES) ? 1 : 0;

    struct thread *child = user_thread_create_shared_files_on(
        a.entry,
        a.stack_top,
        share_fdt ? "clone3+files" : "clone3",
        parent->as,
        a.arg,
        a.tls,
        (int)a.cpu_id,
        share_fdt);
    if (!child) return -ENOMEM_VFS;

    return (long)child->id;
}

/*
 * sys_getcpu() -> cpu_id
 *
 * Chapter 92 — return the CPU id the calling thread is currently
 * running on.  For chapter-92 user threads, home_cpu pinning
 * means this value is stable across the syscall return — the
 * caller will land back on the same CPU.  Used by tests to
 * verify clone2's cpu_id placement actually happens.
 */
static long sys_getcpu(void)
{
    return (long)cpu_current_id();
}

/*
 * sys_listdir(int idx, char *name, size_t cap, uint32_t *size_out)
 * — copy the idx-th directory entry's name + size to userspace.
 * Returns the number of bytes written to `name` (excluding NUL),
 * -ENOENT past the end of the namespace, -EFAULT on bad pointers.
 *
 * Names returned are full paths (e.g. "/motd", "/mnt/hello.txt").
 * Used by `/bin/ls` to print the FS catalogue.
 */
static long sys_listdir(long idx, long name_ptr, long cap, long size_out_ptr)
{
    if (cap <= 0) return -EINVAL_VFS;
    if (cap > 256) cap = 256;            /* cap kernel-side staging buffer */
    if (uaccess_check((uint64_t)name_ptr, (size_t)cap) != 0) return -EFAULT;
    if (uaccess_check((uint64_t)size_out_ptr, sizeof(uint32_t)) != 0)
        return -EFAULT;

    char     name[256];
    uint32_t size = 0;
    long     n = vfs_listdir((int)idx, name, (size_t)cap, &size);
    if (n < 0) return n;

    if (copy_to_user((uint64_t)name_ptr, name, (size_t)n + 1) < 0)
        return -EFAULT;
    if (copy_to_user((uint64_t)size_out_ptr, &size, sizeof(size)) < 0)
        return -EFAULT;
    return n;
}

/*
 * sys_uptime_ms() — return the number of milliseconds since
 * timer_init.  Wraps timer_ticks() multiplied by TICK_INTERVAL_MS;
 * monotonic, never decreases.  64-bit so we won't wrap until ~584
 * million years at 100 ms ticks (more than enough for any program
 * that finishes before the heat death of the universe).
 */
static long sys_uptime_ms(void)
{
    return (long)(timer_ticks() * (uint64_t)TICK_INTERVAL_MS);
}

/*
 * sys_gettimeofday(struct timeval *out) — chapter 95.
 *
 * Pulls the current wall-clock time from walltime_now_us (which
 * derives from a single boot-time PL031 RTC read plus the live
 * tick counter; see kernel/core/walltime.c) and copies it into
 * the user's `out` buffer.
 *
 * Returns 0 on success or -EFAULT if `out` is unmapped or
 * read-only at EL0.
 *
 * The split-into-(secs, usecs) shape comes from POSIX `struct
 * timeval`.  We keep the same byte-for-byte layout in the
 * userspace ABI; struct timeval is defined in syscall.h and
 * mirrored verbatim in userspace/libc/syscall.h.
 */
static long sys_gettimeofday(uintptr_t out_ptr)
{
    struct timeval tv;
    int64_t  secs  = 0;
    uint32_t usecs = 0;
    walltime_now_us(&secs, &usecs);

    tv.tv_sec  = secs;
    tv.tv_usec = usecs;
    tv._pad    = 0;

    if (copy_to_user(out_ptr, &tv, sizeof(tv)) < 0)
        return -EFAULT;
    return 0;
}

/*
 * sys_beep(uint32_t freq_hz, uint32_t duration_ms) — chapter 96.
 *
 * Synthesises a square wave at `freq_hz` for `duration_ms` and
 * streams it through the virtio-sound driver.  Blocks the
 * calling thread for approximately `duration_ms`.
 *
 * Returns 0 on success or -ENODEV if no virtio-sound device
 * was present at boot.  Bounds-checks the inputs inside the
 * driver, not here, so a single out-of-range arg can't escape
 * to MMIO.
 */
static long sys_beep(uintptr_t freq_hz, uintptr_t duration_ms)
{
    if (!virtio_snd_present())
        return -ENODEV;
    int rc = virtio_snd_play_square((uint32_t)freq_hz,
                                    (uint32_t)duration_ms);
    return (rc == 0) ? 0 : -EIO;
}

/*
 * Tiny string helpers used by chdir/getcwd.  Kept local so they
 * don't pollute kstring or accidentally end up linked into a
 * userspace artifact.
 */
static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static size_t s_len(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * sys_chdir(const char *path) — change current working directory.
 *
 * Our namespace is flat (chapter 12 ramfs + chapter 13 OSFS), so
 * the only "real" directories are:
 *   "/"      — root (motd, README)
 *   "/mnt"   — OSFS mount point
 *   "/bin"   — alias for OSFS (chapter 13)
 *
 * Anything else returns -ENOENT.  Trailing slashes are stripped
 * and "//" → "/" collapse is applied via a single normalization
 * pass before the comparison.
 *
 * On success the per-thread cwd buffer is overwritten and 0 is
 * returned.  Children spawned after this call inherit the new cwd
 * (chapter-23 thread initializer copies g_current->cwd).
 */
static long sys_chdir(long path_uptr)
{
    /* Stage in a kernel buffer so user can't observe partial
     * mutation of cwd if the source string is malicious. */
    char tmp[THREAD_CWD_MAX];
    long n = copy_string_from_user(tmp, (uint64_t)path_uptr, sizeof(tmp));
    if (n < 0) return n;        /* -EFAULT or -ENAMETOOLONG */
    if (tmp[0] != '/') return -EINVAL_VFS;

    /* Normalize: strip trailing slashes (but keep the leading
     * one for the root case). */
    size_t len = (size_t)n;
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    /* Validate against the (very short) directory list. */
    if (!s_eq(tmp, "/") &&
        !s_eq(tmp, "/mnt") &&
        !s_eq(tmp, "/bin"))
        return -ENOENT_VFS;

    struct thread *t = thread_current();
    for (size_t i = 0; i <= len; i++) t->cwd[i] = tmp[i];
    return 0;
}

/*
 * sys_getcwd(char *buf, size_t cap) — copy the current cwd
 * (NUL-terminated) into user buf.  Returns the number of bytes
 * written including the terminating NUL, or -ERANGE / -EFAULT.
 *
 * Mirrors the POSIX `getcwd(3)` shape closely enough that future
 * libc compatibility code won't have to translate.
 */
static long sys_getcwd(long buf_uptr, long cap)
{
    if (cap <= 0) return -EINVAL_VFS;
    if (cap > (long)THREAD_CWD_MAX) cap = (long)THREAD_CWD_MAX;

    struct thread *t = thread_current();
    size_t need = s_len(t->cwd) + 1;        /* include NUL */
    if (need > (size_t)cap) return -EINVAL_VFS;

    if (uaccess_check((uint64_t)buf_uptr, need) != 0) return -EFAULT;
    if (copy_to_user((uint64_t)buf_uptr, t->cwd, need) < 0)
        return -EFAULT;
    return (long)need;
}

/*
 * Environment-block helpers.  All operate on the calling
 * thread's `env[]` blob, which is laid out as a packed sequence
 * of NUL-terminated "KEY=VALUE" entries with a trailing extra
 * NUL marking end-of-list.  The blob is fixed-size
 * (THREAD_ENV_MAX); setenv returns -ENOMEM if a new entry
 * wouldn't fit.
 *
 * Walk pattern:
 *
 *   for (size_t off = 0; t->env[off]; ) {
 *       const char *e = &t->env[off];
 *       size_t len = s_len(e);
 *       ... use e ...
 *       off += len + 1;
 *   }
 */

/* Length in bytes of every entry plus trailing zeros (does NOT
 * include the final extra \0 marker; that's reported as
 * env_used + 1 if you want the entire blob). */
static size_t env_used(const char *blob)
{
    size_t off = 0;
    while (blob[off]) {
        size_t len = s_len(&blob[off]);
        off += len + 1;
    }
    return off;
}

/* Find the offset of the entry whose key matches `key`, or -1. */
static long env_find(const char *blob, const char *key)
{
    size_t klen = s_len(key);
    for (size_t off = 0; blob[off]; ) {
        const char *e = &blob[off];
        size_t len   = s_len(e);
        /* Match if e[0..klen) == key && e[klen] == '=' */
        int match = 1;
        for (size_t i = 0; i < klen; i++) {
            if (e[i] != key[i]) { match = 0; break; }
        }
        if (match && e[klen] == '=') return (long)off;
        off += len + 1;
    }
    return -1;
}

/* Remove the entry at offset `off` from the blob in place. */
static void env_delete_at(char *blob, size_t off)
{
    size_t len = s_len(&blob[off]) + 1;     /* incl NUL */
    size_t used = env_used(blob);
    /* memmove tail down. */
    for (size_t i = off; i + len < used; i++) {
        blob[i] = blob[i + len];
    }
    /* Zero the now-vacated tail bytes (preserves the end-of-list
     * marker AND ensures any stale bytes don't leak via getenv_all). */
    for (size_t i = used - len; i < THREAD_ENV_MAX; i++) {
        blob[i] = '\0';
    }
}

/*
 * sys_getenv(const char *key, char *buf, size_t cap)
 * — look up `key` in the calling thread's env, copy the value
 * (NUL-terminated) into user `buf`, return bytes written incl
 * NUL.  -ENOENT if not found, -EINVAL if cap too small.
 */
static long sys_getenv(long key_uptr, long buf_uptr, long cap)
{
    if (cap <= 0) return -EINVAL_VFS;
    char key[THREAD_ENV_MAX];
    long n = copy_string_from_user(key, (uint64_t)key_uptr, sizeof(key));
    if (n < 0) return n;

    struct thread *t = thread_current();
    long off = env_find(t->env, key);
    if (off < 0) return -ENOENT_VFS;

    const char *val = &t->env[off + s_len(key) + 1];   /* skip "KEY=" */
    size_t need = s_len(val) + 1;
    if (need > (size_t)cap) return -EINVAL_VFS;
    if (uaccess_check((uint64_t)buf_uptr, need) != 0) return -EFAULT;
    if (copy_to_user((uint64_t)buf_uptr, val, need) < 0) return -EFAULT;
    return (long)need;
}

/*
 * sys_setenv(const char *key, const char *val) — replace or
 * append.  Returns 0, -EINVAL on empty/bad key, -ENOMEM on
 * blob overflow, -EFAULT on bad pointers.
 */
static long sys_setenv(long key_uptr, long val_uptr)
{
    char key[128];
    char val[THREAD_ENV_MAX];
    long kn = copy_string_from_user(key, (uint64_t)key_uptr, sizeof(key));
    if (kn < 0) return kn;
    long vn = copy_string_from_user(val, (uint64_t)val_uptr, sizeof(val));
    if (vn < 0) return vn;

    /* Reject empty key or key containing '='. */
    if (key[0] == '\0') return -EINVAL_VFS;
    for (long i = 0; i < kn; i++) if (key[i] == '=') return -EINVAL_VFS;

    struct thread *t = thread_current();
    long off = env_find(t->env, key);
    if (off >= 0) env_delete_at(t->env, (size_t)off);

    /* Append "key=val\0" at end-of-list (which is at env_used). */
    size_t used    = env_used(t->env);
    size_t need    = (size_t)kn + 1 + (size_t)vn + 1;   /* key + '=' + val + '\0' */
    /* Need to keep an extra trailing \0 as end-of-list marker,
     * so total available = THREAD_ENV_MAX - 1. */
    if (used + need > THREAD_ENV_MAX - 1) return -ENOMEM_VFS;

    size_t w = used;
    for (long i = 0; i < kn; i++) t->env[w++] = key[i];
    t->env[w++] = '=';
    for (long i = 0; i < vn; i++) t->env[w++] = val[i];
    t->env[w++] = '\0';
    /* Trailing end-of-list marker (already zero, but be explicit). */
    t->env[w] = '\0';
    return 0;
}

/*
 * sys_unsetenv(const char *key) — remove if present.
 * Returns 0 on success, -ENOENT if not present.
 */
static long sys_unsetenv(long key_uptr)
{
    char key[128];
    long n = copy_string_from_user(key, (uint64_t)key_uptr, sizeof(key));
    if (n < 0) return n;

    struct thread *t = thread_current();
    long off = env_find(t->env, key);
    if (off < 0) return -ENOENT_VFS;
    env_delete_at(t->env, (size_t)off);
    return 0;
}

/*
 * sys_getenv_all(char *buf, size_t cap) — copy the entire env
 * blob (used bytes + trailing extra \0) into user buf.  Used by
 * /bin/env to dump the whole environment.
 */
static long sys_getenv_all(long buf_uptr, long cap)
{
    if (cap <= 0) return -EINVAL_VFS;
    struct thread *t = thread_current();
    size_t used = env_used(t->env);
    size_t need = used + 1;     /* +1 for the final end-of-list \0 */
    if (need > (size_t)cap) return -EINVAL_VFS;
    if (uaccess_check((uint64_t)buf_uptr, need) != 0) return -EFAULT;
    if (copy_to_user((uint64_t)buf_uptr, t->env, need) < 0) return -EFAULT;
    return (long)need;
}

/*
 * sys_sleep_ms(uint64_t ms) — block for at least `ms`
 * milliseconds, then return 0.  Sleep granularity is one
 * scheduler tick (100 ms today).  A zero or negative count
 * returns immediately as if a yield.
 *
 * SVC handlers run with IRQs masked by the architecture's
 * exception-entry behaviour.  Without IRQs the timer can't
 * tick and the sleep loop spins forever.  We unmask DAIF.I
 * for the duration of the wait, then re-mask before returning
 * so the rest of the SVC return path stays in its expected
 * IRQ-masked state.
 */
static long sys_sleep_ms(long ms_signed)
{
    if (ms_signed <= 0) return 0;
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    thread_sleep_ms((uint64_t)ms_signed);
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return 0;
}

/*
 * sys_pipe(int *fds_out_user) — allocate an anonymous pipe and
 * install both ends in the calling thread's fd table.  On
 * success writes the read end into fds[0] and the write end
 * into fds[1] (POSIX-style int [2]) and returns 0.  On
 * failure returns a negative errno and the user buffer is
 * left untouched.
 */
static long sys_pipe(long fds_user_ptr)
{
    if (fds_user_ptr == 0) return -EFAULT;
    if (uaccess_check((uint64_t)fds_user_ptr, sizeof(int) * 2) < 0)
        return -EFAULT;

    struct thread *t = thread_current();
    int rfd = -1, wfd = -1;
    for (int i = 3; i < FD_TABLE_SIZE; i++) {
        if (!t->fdt->fds[i].in_use) {
            if (rfd < 0)      rfd = i;
            else if (wfd < 0) { wfd = i; break; }
        }
    }
    if (rfd < 0 || wfd < 0) return -EMFILE;

    struct pipe *p = pipe_alloc();
    if (!p) return -ENOMEM_VFS;

    t->fdt->fds[rfd].in_use = 1;
    t->fdt->fds[rfd].kind   = FD_PIPE_R;
    t->fdt->fds[rfd].pipe   = p;
    t->fdt->fds[rfd].offset = 0;
    t->fdt->fds[rfd].ramfs_index = -1;
    t->fdt->fds[rfd].osfs_start  = 0;
    t->fdt->fds[rfd].osfs_size   = 0;

    t->fdt->fds[wfd].in_use = 1;
    t->fdt->fds[wfd].kind   = FD_PIPE_W;
    t->fdt->fds[wfd].pipe   = p;
    t->fdt->fds[wfd].offset = 0;
    t->fdt->fds[wfd].ramfs_index = -1;
    t->fdt->fds[wfd].osfs_start  = 0;
    t->fdt->fds[wfd].osfs_size   = 0;

    int out[2] = { rfd, wfd };
    /* TODO: copy_to_user with bounds-check.  uaccess_check
     * above already confirmed the region is in user range. */
    int *dst = (int *)(uintptr_t)fds_user_ptr;
    dst[0] = out[0];
    dst[1] = out[1];
    return 0;
}

/*
 * sys_openpty(int *master_out, int *slave_out) -> 0 / -errno
 *
 * Chapter 79b.  Allocates a pty (two pipes + a foreground-pid
 * field) and installs two new fds in the calling thread:
 *   - *master_out: FD_PTY_MASTER, intended for the controlling
 *     app (gui_term).  Read drains the slave's output ring;
 *     write enqueues to the slave's input ring after running
 *     each byte through a tiny line discipline (today: 0x03 →
 *     SIGINT to pty->fg_pid, dropped from the byte stream).
 *   - *slave_out:  FD_PTY_SLAVE, meant to be dup2'd onto fds
 *     0/1/2 of the forked child shell.
 *
 * The pty's underlying pipe refcounts are bootstrapped at 1
 * each by pipe_alloc and survive across fd installation; closing
 * either fd drops them through pty_close_master /
 * pty_close_slave (in vfs_close), which kfree the pty when both
 * have closed.
 */
static long sys_openpty(long master_uptr, long slave_uptr)
{
    if (master_uptr == 0 || slave_uptr == 0) return -EFAULT;
    if (uaccess_check((uint64_t)master_uptr, sizeof(int)) < 0) return -EFAULT;
    if (uaccess_check((uint64_t)slave_uptr,  sizeof(int)) < 0) return -EFAULT;

    struct thread *t = thread_current();
    int mfd = -1, sfd = -1;
    for (int i = 3; i < FD_TABLE_SIZE; i++) {
        if (!t->fdt->fds[i].in_use) {
            if (mfd < 0)      mfd = i;
            else if (sfd < 0) { sfd = i; break; }
        }
    }
    if (mfd < 0 || sfd < 0) return -EMFILE;

    struct pty *p = pty_alloc();
    if (!p) return -ENOMEM_VFS;

    t->fdt->fds[mfd].in_use      = 1;
    t->fdt->fds[mfd].kind        = FD_PTY_MASTER;
    t->fdt->fds[mfd].pty         = p;
    t->fdt->fds[mfd].pipe        = NULL;
    t->fdt->fds[mfd].offset      = 0;
    t->fdt->fds[mfd].ramfs_index = -1;
    t->fdt->fds[mfd].osfs_start  = 0;
    t->fdt->fds[mfd].osfs_size   = 0;
    t->fdt->fds[mfd].socket_cid  = -1;

    t->fdt->fds[sfd].in_use      = 1;
    t->fdt->fds[sfd].kind        = FD_PTY_SLAVE;
    t->fdt->fds[sfd].pty         = p;
    t->fdt->fds[sfd].pipe        = NULL;
    t->fdt->fds[sfd].offset      = 0;
    t->fdt->fds[sfd].ramfs_index = -1;
    t->fdt->fds[sfd].osfs_start  = 0;
    t->fdt->fds[sfd].osfs_size   = 0;
    t->fdt->fds[sfd].socket_cid  = -1;

    int *dm = (int *)(uintptr_t)master_uptr;
    int *ds = (int *)(uintptr_t)slave_uptr;
    *dm = mfd;
    *ds = sfd;
    return 0;
}

/*
 * sys_fsync(int fd) -> 0 / -errno
 *
 * Chapter 82 \u2014 force every dirty cache block backing `fd` to
 * disk synchronously.  Returns only after the underlying
 * virtio-blk writes ack.
 *
 * For OSFS-2 file fds: delegates to osfs2_fsync(ino), which
 * currently flushes the whole write-back cache (see comment
 * there).  An -EIO from a writeback bubbles up here.
 *
 * For every other fd kind (console, ramfs, OSFS-1 read-only,
 * tmpfs, pipes, ptys, sockets) fsync is a no-op that returns 0.
 * The data either lives in volatile-by-design storage (pipes,
 * tmpfs, console) or in storage we never write to (ramfs,
 * OSFS-1).  Returning 0 lets userspace call fsync() defensively
 * without having to special-case the fd kind.
 */
static long sys_fsync(long fdi)
{
    if (fdi < 0 || fdi >= FD_TABLE_SIZE) return -EBADF;
    struct thread *t = thread_current();
    struct fd_entry *e = &t->fdt->fds[fdi];
    if (!e->in_use) return -EBADF;
    if (e->kind != FD_OSFS2_FILE) return 0;
    if (osfs2_fsync(e->osfs2_ino) != 0) return -EIO;
    return 0;
}

/*
 * sys_dup2(oldfd, newfd) — make `newfd` refer to the same
 * underlying object as `oldfd`.  If `newfd` was open, it is
 * closed first (with proper pipe refcount management).
 * For pipe fds the underlying pipe's refcount is bumped.
 * Returns newfd on success, negative errno on failure.
 */
static long sys_dup2(long oldfd, long newfd)
{
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -EBADF;
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -EBADF;
    struct thread *t = thread_current();
    struct fd_entry *src = &t->fdt->fds[oldfd];
    if (!src->in_use) return -EBADF;
    if (oldfd == newfd) return newfd;

    struct fd_entry *dst = &t->fdt->fds[newfd];
    /* Close newfd first if it was open (drops its pipe refcount). */
    if (dst->in_use) {
        if (dst->kind == FD_PIPE_R && dst->pipe) pipe_unref(dst->pipe, PIPE_REF_R);
        else if (dst->kind == FD_PIPE_W && dst->pipe) pipe_unref(dst->pipe, PIPE_REF_W);
        else if (dst->kind == FD_PTY_MASTER && dst->pty) pty_close_master(dst->pty);
        else if (dst->kind == FD_PTY_SLAVE  && dst->pty) pty_close_slave(dst->pty);
    }
    *dst = *src;
    dst->in_use = 1;
    /* Bump the pipe's refcount on the destination side. */
    if (dst->kind == FD_PIPE_R && dst->pipe) dst->pipe->r_refs++;
    else if (dst->kind == FD_PIPE_W && dst->pipe) dst->pipe->w_refs++;
    else if (dst->kind == FD_PTY_MASTER && dst->pty) {
        dst->pty->refs++;
        dst->pty->s2m->r_refs++;
        dst->pty->m2s->w_refs++;
    }
    else if (dst->kind == FD_PTY_SLAVE && dst->pty) {
        dst->pty->refs++;
        dst->pty->m2s->r_refs++;
        dst->pty->s2m->w_refs++;
    }
    return newfd;
}

/*
 * sys_unlink(const char *path) -> int
 *
 * Removes a name from a writable filesystem.  Today two prefixes
 * are accepted:
 *   /tmp/<name>   — in-memory tmpfs (chapter 32+).
 *   /data/<name>  — on-disk OSFS-2 (chapter 81+).
 *
 * Returns 0 on success, or a negative errno (-ENOENT, -EFAULT,
 * -EINVAL).
 *
 * Existing fds referencing the unlinked file will start returning
 * -EBADF on subsequent read/write because the underlying FS marks
 * the slot as not-in-use.  No "deferred reclaim until last fd
 * closes" yet — when we want POSIX semantics we'll add an
 * unlinked-but-open refcount.
 */
static long sys_unlink(long path_uptr)
{
    char path[128];
    long n = copy_string_from_user(path, (uint64_t)path_uptr, sizeof(path));
    if (n < 0) return n;

    static const char tmp_prefix[]  = "/tmp/";
    static const char data_prefix[] = "/data/";

    /* /tmp/ branch: in-memory writable. */
    {
        int i;
        for (i = 0; i < (int)sizeof(tmp_prefix) - 1; i++) {
            if (path[i] != tmp_prefix[i]) goto try_data;
        }
        if (!path[i]) return -EINVAL_VFS;     /* "/tmp/" with no name */
        return tmpfs_unlink(path + i);
    }
try_data:
    /* /data/ branch: on-disk OSFS-2.  Returns -EIO on a write
     * failure mid-unlink; callers can retry. */
    {
        int i;
        for (i = 0; i < (int)sizeof(data_prefix) - 1; i++) {
            if (path[i] != data_prefix[i]) return -EINVAL_VFS;
        }
        if (!path[i]) return -EINVAL_VFS;     /* "/data/" with no name */
        if (!osfs2_present()) return -ENOENT_VFS;
        if (osfs2_unlink(path + i) != 0) return -ENOENT_VFS;
        return 0;
    }
}

/*
 * sys_mkdir(const char *path) -> int
 *
 * Chapter 85.  Creates a directory at `path`.  Currently only the
 * /data/ mount (writable OSFS-2) supports directories \u2014 the
 * /tmp/ tmpfs is intentionally flat (we'd need a separate path
 * walker for it; out of scope).  The path may have any number of
 * components; all but the last must already exist.
 */
static long sys_mkdir(long path_uptr)
{
    char path[128];
    long n = copy_string_from_user(path, (uint64_t)path_uptr, sizeof(path));
    if (n < 0) return n;

    static const char data_prefix[] = "/data/";
    int i;
    for (i = 0; i < (int)sizeof(data_prefix) - 1; i++) {
        if (path[i] != data_prefix[i]) return -EINVAL_VFS;
    }
    if (!path[i]) return -EINVAL_VFS;
    if (!osfs2_present()) return -ENOENT_VFS;
    if (osfs2_mkdir(path + i) == 0) return -ENOENT_VFS;
    return 0;
}

/*
 * sys_listdir_at(const char *path, int idx, char *name,
 *                size_t cap, uint32_t *size_out,
 *                uint32_t *type_out) -> int
 *
 * Chapter 85.  Walks one directory by absolute path \u2014 unlike
 * sys_listdir which folds every mount into a single linear
 * namespace.  Path "/data/" or "/data" returns the OSFS-2 root;
 * "/data/notes" returns the contents of the notes subdirectory.
 *
 * Names returned are *leaves*, not full paths (callers know the
 * prefix \u2014 they're the ones that asked).  type_out distinguishes
 * regular files from subdirectories so the Save As dialog can
 * render `<DIR>` / `>` markers without a follow-up stat call.
 */
static long sys_listdir_at(long path_uptr, long idx, long name_ptr,
                           long cap, long size_out_ptr, long type_out_ptr)
{
    if (cap <= 0) return -EINVAL_VFS;
    if (cap > 256) cap = 256;
    if (uaccess_check((uint64_t)name_ptr, (size_t)cap) != 0) return -EFAULT;
    if (size_out_ptr &&
        uaccess_check((uint64_t)size_out_ptr, sizeof(uint32_t)) != 0)
        return -EFAULT;
    if (type_out_ptr &&
        uaccess_check((uint64_t)type_out_ptr, sizeof(uint32_t)) != 0)
        return -EFAULT;

    char path[128];
    long pn = copy_string_from_user(path, (uint64_t)path_uptr, sizeof(path));
    if (pn < 0) return pn;

    /* Chapter 99 — /proc enumeration.  Dispatch BEFORE the
     * /data prefix check so "/proc" / "/proc/12" route to the
     * pseudo-FS.  procfs_listdir handles both the root and
     * per-pid directories. */
    {
        static const char proc_prefix[] = "/proc";
        int j;
        for (j = 0; j < (int)sizeof(proc_prefix) - 1; j++) {
            if (path[j] != proc_prefix[j]) goto not_proc;
        }
        if (path[j] && path[j] != '/') goto not_proc;
        const char *sub = path + j;
        while (*sub == '/') sub++;
        char name[256];
        uint32_t type = 0;
        int got = procfs_listdir(*sub ? sub : NULL, (int)idx,
                                  name, sizeof(name), &type);
        if (got < 0) return -ENOENT_VFS;
        size_t out_n = 0;
        while (out_n + 1 < (size_t)cap && name[out_n]) out_n++;
        if (copy_to_user((uint64_t)name_ptr, name, out_n + 1) < 0)
            return -EFAULT;
        if (size_out_ptr) {
            uint32_t zero = 0;
            if (copy_to_user((uint64_t)size_out_ptr, &zero, sizeof(zero)) < 0)
                return -EFAULT;
        }
        if (type_out_ptr &&
            copy_to_user((uint64_t)type_out_ptr, &type, sizeof(type)) < 0)
            return -EFAULT;
        return (long)out_n;
    }
not_proc:

    /* Only /data/ supports per-path enumeration today. */
    static const char data_prefix[] = "/data";
    int i;
    for (i = 0; i < (int)sizeof(data_prefix) - 1; i++) {
        if (path[i] != data_prefix[i]) return -EINVAL_VFS;
    }
    if (path[i] && path[i] != '/') return -EINVAL_VFS;
    if (!osfs2_present()) return -ENOENT_VFS;

    /* osfs2_lookup returns 0 for ROOT \u2014 special-case "" /
     * "/data/" / "/data" \u2192 ROOT inode directly. */
    const char *sub = path + i;
    while (*sub == '/') sub++;
    uint32_t parent_ino;
    if (!*sub) {
        parent_ino = 1;          /* OSFS2_INODE_ROOT */
    } else {
        parent_ino = osfs2_lookup(sub);
        if (parent_ino == 0) return -ENOENT_VFS;
    }

    /* Loop dirent-by-dirent skipping holes until we hit `idx`-th
     * non-empty entry, then return that one.  We rebuild the
     * mapping each call (cheap \u2014 directories are tiny). */
    char     name[256];
    uint32_t walk = 0;
    uint32_t size = 0;
    uint32_t type = 0;
    int      cur = 0;
    while (1) {
        int rc = osfs2_listdir_at(parent_ino, &walk, name, sizeof(name),
                                  &size, &type);
        if (rc <= 0) return -ENOENT_VFS;
        if (cur == (int)idx) break;
        cur++;
    }
    /* Truncate to caller's cap. */
    size_t out_n = 0;
    while (out_n + 1 < (size_t)cap && name[out_n]) out_n++;
    if (copy_to_user((uint64_t)name_ptr, name, out_n + 1) < 0)
        return -EFAULT;
    if (size_out_ptr &&
        copy_to_user((uint64_t)size_out_ptr, &size, sizeof(size)) < 0)
        return -EFAULT;
    if (type_out_ptr &&
        copy_to_user((uint64_t)type_out_ptr, &type, sizeof(type)) < 0)
        return -EFAULT;
    return (long)out_n;
}

/*
 * sys_tty_raw(int enable) -> int
 *
 * Toggles the calling thread's console-input mode.  In raw mode
 * (enable != 0), `read(0, &c, 1)` returns one byte at a time
 * with NO echo and NO line buffering — useful for shell line
 * editors and TUIs that want to handle their own backspace,
 * cursor, and history processing.  In cooked mode (enable == 0,
 * the default), the kernel echoes typed bytes, handles backspace
 * locally, and only returns when Enter is pressed.
 *
 * Returns the previous mode (0 or 1) so callers can save/restore.
 *
 * Children spawned after this call DO NOT inherit raw mode (see
 * thread_create — tty_raw is forced to 0 for new threads).  This
 * matches the convention every Unix shell uses: the shell wears
 * raw mode, the children get a normal cooked tty.
 */
static long sys_tty_raw(long enable)
{
    struct thread *t = thread_current();
    int prev = t->tty_raw;
    t->tty_raw = enable ? 1 : 0;
    return (long)prev;
}

/*
 * sys_kill(int pid, int sig) -> int
 *
 * Sets bit (1 << sig) in the target thread's pending mask.
 * The signal is observed and acted on at the next syscall return
 * (or inside a cooked-mode console read).  Default action for
 * every signal is "terminate the thread with code 128 + sig" —
 * there are no userspace handlers yet.
 *
 * Returns 0 on success, -EINVAL_VFS if sig is out of range, or
 * -ENOENT_VFS if pid does not name a live thread.
 */
static long sys_kill(long pid, long sig)
{
    if (sig <= 0 || sig >= 32) return -EINVAL_VFS;
    if (pid <= 0) return -EINVAL_VFS;
    /* Caller-side validation that the target exists, since
     * thread_signal_pid silently no-ops on missing pids. */
    extern struct thread *thread_lookup(int id);
    if (!thread_lookup((int)pid)) return -ENOENT_VFS;
    thread_signal_pid((int)pid, (int)sig);
    return 0;
}

/*
 * sys_set_fg_pid(int pid) -> int
 *
 * Names the foreground thread that should receive a SIGINT when
 * Ctrl-C arrives at the cooked-mode console.  Pass 0 to clear
 * (no foreground process — Ctrl-C is consumed silently).  Always
 * succeeds; pid is not validated (a stale pid means the signal
 * just goes nowhere).  Returns the previous foreground pid.
 *
 * Auto-routing (chapter 79b): if the calling thread's fd 0 is a
 * pty slave, we update the pty's fg_pid field instead of the
 * global console fg_pid.  This lets /bin/sh's existing
 * set_fg_pid(child) calls work transparently when sh is run
 * inside gui_term over a pty (gui_term holds the master, the
 * shell holds the slave on fd 0/1/2, and the shell's notion
 * of "the foreground child" travels with the pty rather than
 * with the global console).  We return the previous value of
 * whichever field we touched so the contract ("returns prior
 * fg pid") still makes sense.
 */
static long sys_set_fg_pid(long pid)
{
    struct thread *t = thread_current();
    if (t && t->fdt->fds[0].in_use && t->fdt->fds[0].kind == FD_PTY_SLAVE
          && t->fdt->fds[0].pty) {
        int prev = t->fdt->fds[0].pty->fg_pid;
        t->fdt->fds[0].pty->fg_pid = (int)pid;
        return (long)prev;
    }
    int prev = thread_get_fg_pid();
    thread_set_fg_pid((int)pid);
    return (long)prev;
}

/* ── Milestone 65 — fork + exec ───────────────────────────────
 *
 * The "real" Unix process model.  Until now spawn(path, args)
 * was the only way to create a user thread; it baked the
 * "create AS, load ELF, start running" into a single atomic
 * step.  fork+exec splits that:
 *
 *   - fork() clones the caller's AS + fd table; child returns
 *     0, parent returns the child's pid.  This lets the child
 *     do per-process setup (redirect a fd, set environment, ...)
 *     before exec'ing a different program.
 *   - exec(path, argv) tears down the caller's AS, loads a fresh
 *     ELF in its place, and erets to the new entry point.  POSIX
 *     fds that are still open survive (no FD_CLOEXEC yet).
 *
 * Both syscalls need access to the caller's saved trap frame
 * (to return-twice into the right user PC for fork; to overwrite
 * elr/spsr/x0..x30 for exec), so the dispatcher passes `frame`
 * directly instead of just (a0..a3).
 */

#define MAX_EXEC_ARGV    16
#define MAX_EXEC_ARG_LEN 96       /* per-arg cap, kernel staging buffer */

static long sys_fork(struct exception_frame *frame)
{
    struct thread *parent = thread_current();
    if (!parent || !parent->as) return -EINVAL_VFS;

    /* Clone the parent's address space.  Chapter 75 switched
     * this from address_space_clone (eager full copy) to the
     * COW variant: the child shares every page with the parent
     * read-only, and writes are resolved lazily in the page-
     * fault handler.  Fork is now O(page-table size) instead of
     * O(working-set size). */
    struct address_space *child_as = address_space_clone_cow(parent->as);
    if (!child_as) return -ENOMEM_VFS;

    /* Snapshot SP_EL0 NOW.  save_context in vectors.S does not
     * capture SP_EL0 (the SVC entry only swapped SPSel from 0 to
     * 1, leaving SP_EL0 untouched), so we read it directly.  Any
     * yield / kmalloc / kfree below this point that happens to
     * cause a kernel-side context switch could push and pop
     * SP_EL0 in cswitch_to, but the value at the SVC instant —
     * which is what the child needs to resume with — is the
     * value live RIGHT NOW. */
    uint64_t parent_sp_el0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(parent_sp_el0));

    struct thread *child = thread_fork_user(parent, child_as,
                                            frame, parent_sp_el0);
    if (!child) {
        address_space_destroy(child_as);
        return -ENOMEM_VFS;
    }

    /* Parent return value = child's pid.  Dispatcher writes it
     * into frame->x[0] for us via the standard ret tail. */
    return (long)child->id;
}

/*
 * Read a NULL-terminated user argv array into kernel-side
 * storage.  On success returns the argc and fills the
 * `kargv_out` array (NULL-terminated) with pointers into
 * `storage`.  Each individual string is capped at
 * MAX_EXEC_ARG_LEN bytes including the trailing NUL.  Returns
 * -E2BIG if the array is longer than MAX_EXEC_ARGV, -EFAULT on
 * a bad pointer, -ENAMETOOLONG on an over-long string.
 */
static long copy_argv_from_user(uint64_t argv_uptr,
                                char    *storage,    /* MAX_EXEC_ARGV * MAX_EXEC_ARG_LEN */
                                const char *kargv_out[MAX_EXEC_ARGV + 1])
{
    int argc = 0;
    if (!argv_uptr) {
        kargv_out[0] = NULL;
        return 0;
    }
    for (; argc < MAX_EXEC_ARGV; argc++) {
        uint64_t up = 0;
        /* Read the argv[i] pointer slot. */
        if (copy_from_user(&up, argv_uptr + (uint64_t)argc * sizeof(uint64_t),
                           sizeof(up)) < 0)
            return -EFAULT;
        if (up == 0) break;
        char *dst = storage + (size_t)argc * MAX_EXEC_ARG_LEN;
        long  sl  = copy_string_from_user(dst, up, MAX_EXEC_ARG_LEN);
        if (sl < 0) return sl;
        kargv_out[argc] = dst;
    }
    if (argc == MAX_EXEC_ARGV) {
        /* Ran off the end — check for terminator one slot beyond. */
        uint64_t up = 0;
        if (copy_from_user(&up, argv_uptr + (uint64_t)argc * sizeof(uint64_t),
                           sizeof(up)) < 0)
            return -EFAULT;
        if (up != 0) return -EINVAL_VFS;   /* over-long argv */
    }
    kargv_out[argc] = NULL;
    return argc;
}

static long sys_exec(struct exception_frame *frame,
                     long path_uptr, long argv_uptr)
{
    char path[128];
    long pn = copy_string_from_user(path, (uint64_t)path_uptr, sizeof(path));
    if (pn < 0) return pn;

    /* Stage argv into kernel storage BEFORE we touch the AS. */
    static char argv_storage[MAX_EXEC_ARGV * MAX_EXEC_ARG_LEN];
    /* (static — saves the kernel stack from a 1.5 KiB local; this
     * syscall is single-threaded by the single-CPU constraint.) */
    const char *argv[MAX_EXEC_ARGV + 1];
    long ac = copy_argv_from_user((uint64_t)argv_uptr, argv_storage, argv);
    if (ac < 0) return ac;

    /* Load the ELF into a kheap buffer (ramfs / OSFS / /bin path
     * dispatch all handled by vfs_load). */
    uint8_t *data; size_t size;
    int rc = vfs_load(path, &data, &size);
    if (rc < 0) return rc;

    /* Build a fresh AS and load the new program into it.  If
     * anything fails here the caller's old AS is left intact —
     * exec is "all or nothing." */
    struct address_space *new_as = address_space_create();
    if (!new_as) { kfree(data); return -ENOMEM_VFS; }

    struct user_image img;
    int loaded = elf_load_user(data, size, new_as, argv, &img);
    kfree(data);
    if (loaded != 0) {
        address_space_destroy(new_as);
        return -EINVAL_VFS;
    }

    /* === point of no return ===
     * From here we're committed: swap AS, free old, patch frame,
     * eret to new entry point. */
    struct thread *t = thread_current();
    struct address_space *old_as = t->as;
    t->as = new_as;
    address_space_activate(new_as);
    if (old_as) address_space_destroy(old_as);

    /* Refresh display name + args buffer to match the new
     * program (so /bin/ps-style listings and SYS_GETARGS see
     * the new identity).  args = argv[1..argc-1] joined by
     * single spaces, mirroring what sys_spawn stashed for
     * old-style programs. */
    thread_rename(t, path);
    /* exec resets all caught signals to SIG_DFL (POSIX): the
     * old handlers point at addresses in the OLD address space
     * which we just freed.  Pending signals also clear \u2014 a
     * SIGINT raised against the old program shouldn't be
     * delivered to the new one. */
    for (int s = 0; s < 32; s++) t->sig_handlers[s] = 0;
    t->sig_restorer = 0;
    t->sig_pending  = 0;
    {
        size_t i = 0;
        int    first = 1;
        for (long k = 1; k < ac && i + 1 < THREAD_ARGS_MAX; k++) {
            if (!first && i + 1 < THREAD_ARGS_MAX) t->args[i++] = ' ';
            const char *s = argv[k];
            for (size_t j = 0; s[j] && i + 1 < THREAD_ARGS_MAX; j++)
                t->args[i++] = s[j];
            first = 0;
        }
        t->args[i] = '\0';
    }

    /* Patch the saved trap frame so restore_context will eret
     * into the new program's first instruction with a clean
     * register file.  The dispatcher's tail will then write
     * frame->x[0] = ret (= 0) — same as us zeroing it here, so
     * the order doesn't matter. */
    for (int i = 0; i < 31; i++) frame->x[i] = 0;
    frame->elr  = img.entry_va;
    frame->spsr = 0x340ULL;     /* EL0t, F=A=D=1, I=0 — IRQs on */

    /* save_context did NOT capture SP_EL0, so restore_context
     * won't restore it either.  Set it directly — the value will
     * still be correct on eret because nothing between here and
     * the eret in svc_entry touches SP_EL0. */
    __asm__ volatile("msr sp_el0, %0" :: "r"(img.stack_top_va) : "memory");

    return 0;
}

/* ── Chapter 77 — Catchable signals ─────────────────────────
 *
 * The kernel-side delivery flow is:
 *
 *   1. svc_dispatch tail (or the syscall return path) notices
 *      thread_current()->sig_pending != 0.
 *   2. For each set bit (low signum first), look up the
 *      handler in t->sig_handlers[s]:
 *        - 0 (SIG_DFL): thread_exit(128 + s).  Done.
 *        - 1 (SIG_IGN): clear bit, continue scanning.
 *        - else      : divert into user handler.
 *   3. Diverting writes a struct sigframe to the top of the
 *      user stack (SP_EL0 - sizeof(sigframe), 16-byte aligned),
 *      then patches the trap frame so eret lands at the handler
 *      with x0 = signum, x30 = sig_restorer, sp_el0 = sigframe.
 *   4. The libc-supplied restorer ret's into the handler.  When
 *      the handler ret's, control flows to the restorer, which
 *      issues SYS_SIGRETURN with x0 = sigframe pointer.
 *   5. sys_sigreturn copies the saved register state back into
 *      the trap frame, restores SP_EL0, and returns.  The eret
 *      tail of svc_entry resumes the program at the original PC.
 *
 * The user-visible struct lives in userspace/libc/signal.h; the
 * kernel-side mirror is below.  Layout MUST stay in sync.
 */
struct sigframe_k {
    uint64_t x[31];     /* x0..x30 at moment of signal              */
    uint64_t sp_el0;    /* user stack pointer at moment of signal   */
    uint64_t elr;       /* user PC at moment of signal              */
    uint64_t spsr;      /* user PSTATE at moment of signal          */
    uint32_t signum;    /* which signal is being delivered          */
    uint32_t pad;       /* alignment                                */
    uint64_t pad2;      /* round struct up to 288 (= 16B multiple)  */
};
_Static_assert(sizeof(struct sigframe_k) % 16 == 0,
               "sigframe must be 16B-aligned for AAPCS SP");
_Static_assert(sizeof(struct sigframe_k) == 288,
               "sigframe size must match userspace/libc/signal.h");

/*
 * sys_sigaction(int sig, uint64_t handler, uint64_t restorer)
 *   -> uint64_t old_handler  (or (uint64_t)-EINVAL_VFS)
 *
 * Set the disposition for `sig` and (if non-zero) install
 * `restorer` as the per-thread sigreturn trampoline.  Passing
 * restorer=0 keeps whatever was there.  SIGKILL (9) cannot be
 * caught: any non-DFL handler request for it is silently coerced
 * back to SIG_DFL, mirroring POSIX.
 */
static long sys_sigaction(long sig, long handler, long restorer)
{
    if (sig <= 0 || sig >= 32) return (long)-EINVAL_VFS;
    struct thread *t = thread_current();
    uint64_t old = t->sig_handlers[sig];
    /* SIGKILL is uncatchable.  Coerce any non-DFL handler back
     * to DFL silently — POSIX returns -EINVAL for this; we're
     * a touch more forgiving since our userspace doesn't yet
     * have a way to inspect errno cleanly. */
    if (sig == SIGKILL) {
        t->sig_handlers[sig] = 0;
    } else {
        t->sig_handlers[sig] = (uint64_t)handler;
    }
    if (restorer != 0) t->sig_restorer = (uint64_t)restorer;
    return (long)old;
}

/*
 * Build a sigframe on the user stack and patch `frame` so the
 * SVC eret lands in the handler.  Returns 0 on success or -1
 * if anything looks wrong (bad SP_EL0, no restorer registered,
 * copy_to_user failure).  Caller terminates the thread on -1
 * since the alternative is silently dropping the signal.
 */
static int deliver_signal(struct thread *t,
                          struct exception_frame *frame,
                          int signum,
                          uint64_t handler)
{
    if (t->sig_restorer == 0) return -1;        /* libc never registered one */

    /* Grab the user stack pointer captured by the SVC entry.
     * save_context doesn't put it in the frame, so we fish it
     * out of SP_EL0 directly — it will still hold the user
     * value here because we haven't ereted yet. */
    uint64_t user_sp;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(user_sp));

    /* Reserve the sigframe at the top of the user stack,
     * 16B-aligned (AAPCS).  sizeof(sigframe_k) is already a
     * multiple of 16 by the static_assert above. */
    uint64_t sf_uaddr = (user_sp - sizeof(struct sigframe_k)) & ~(uint64_t)0xF;

    struct sigframe_k sf;
    for (int i = 0; i < 31; i++) sf.x[i] = frame->x[i];
    sf.sp_el0 = user_sp;
    sf.elr    = frame->elr;
    sf.spsr   = frame->spsr;
    sf.signum = (uint32_t)signum;
    sf.pad    = 0;

    if (copy_to_user(sf_uaddr, &sf, sizeof(sf)) < 0)
        return -1;

    /* Patch the trap frame.  After this returns, svc_dispatch's
     * caller will eret using `frame`, which now describes the
     * handler invocation rather than the original syscall return. */
    for (int i = 0; i < 31; i++) frame->x[i] = 0;
    frame->x[0]  = (uint64_t)signum;        /* int signo argument         */
    frame->x[1]  = sf_uaddr;                /* convenience: also visible  */
    frame->x[30] = t->sig_restorer;         /* LR → restorer trampoline   */
    frame->elr   = handler;                 /* PC → user handler          */
    /* SPSR stays at whatever the original syscall return wanted
     * (EL0t with IRQs unmasked).  Don't touch it — exec/spawn
     * have already set the right value for this thread. */

    /* Move SP_EL0 down to the sigframe.  restore_context won't
     * touch SP_EL0, so writing it here sticks across the eret. */
    __asm__ volatile("msr sp_el0, %0" :: "r"(sf_uaddr) : "memory");
    return 0;
}

/*
 * sys_sigreturn(struct sigframe *uptr) -> long (no return on success)
 *
 * Inverse of deliver_signal.  Copies the user-supplied frame
 * back over our trap frame so the eret in svc_entry resumes the
 * interrupted code at its original PC.  The "return value" we
 * write to frame->x[0] is the signum's saved x0 — i.e. what the
 * interrupted code's x0 held before the signal — so the caller
 * sees the original return-from-svc value, not a sigreturn
 * status.  Returns -EFAULT on a bad pointer; otherwise the
 * dispatcher's tail clobbers the return value with the restored
 * x[0] anyway.
 */
static long sys_sigreturn(struct exception_frame *frame, long uptr)
{
    struct sigframe_k sf;
    if (copy_from_user(&sf, (uint64_t)uptr, sizeof(sf)) < 0)
        return -EFAULT;

    /* Restore everything the handler might have clobbered. */
    for (int i = 0; i < 31; i++) frame->x[i] = sf.x[i];
    frame->elr  = sf.elr;
    frame->spsr = sf.spsr;
    __asm__ volatile("msr sp_el0, %0" :: "r"(sf.sp_el0) : "memory");

    /* Tell the dispatcher tail not to overwrite frame->x[0].  We
     * return the restored value, which equals what's now in x[0],
     * so the post-dispatch `frame->x[0] = (uint64_t)ret` is a
     * harmless re-assignment. */
    return (long)sf.x[0];
}

/* ── Milestone 56 — sockets ─────────────────────────────────
 *
 * Three syscalls cover the active-open client side:
 *   SYS_SOCKET_CONNECT(ip4_be32, port) -> fd        (creates fd)
 *   SYS_SOCKET_STATE(fd)              -> int state  (enum tcp_state)
 *   SYS_SOCKET_SHUTDOWN(fd)            -> int       (FIN; later close()
 *                                                    drops the fd)
 *
 * No new read/write syscalls are needed: SYS_READ/WRITE/CLOSE
 * already dispatch on FD_SOCKET.
 *
 * `ip4_be32` is the IPv4 address packed into a uint32 in network
 * byte order: e.g. 10.0.2.2 = 0x0A000202.  This avoids a 4-byte
 * pointer dance for the common case; resolved hostnames will use
 * a getaddrinfo-shaped helper later.
 */
static long sys_socket_connect(long ip4_be32, long port)
{
    if (port <= 0 || port > 65535) return -EINVAL_VFS;
    uint8_t ip[4] = {
        (uint8_t)((uint32_t)ip4_be32 >> 24),
        (uint8_t)((uint32_t)ip4_be32 >> 16),
        (uint8_t)((uint32_t)ip4_be32 >>  8),
        (uint8_t)((uint32_t)ip4_be32      ),
    };
    int cid = tcp_connect(ip, (uint16_t)port);
    if (cid < 0) return -EMFILE;

    /* Wait for ESTABLISHED, RST, or a coarse timeout.  We must
     * drive net_poll() ourselves — yield() doesn't pump the
     * NIC, so without this the state machine would never advance
     * and we'd always time out. */
    for (uint64_t i = 0; i < 200000000ULL; i++) {
        (void)net_poll();
        int s = tcp_state(cid);
        if (s == TCP_ESTABLISHED) {
            int fd = vfs_alloc_socket_fd(cid);
            if (fd < 0) { tcp_close(cid); return fd; }
            return fd;
        }
        if (s == TCP_CLOSED) {
            tcp_close(cid);
            return -EIO;
        }
        if ((i & 0x3FFu) == 0) yield();
    }
    tcp_close(cid);
    return -EIO;
}

static long sys_socket_state(long fd)
{
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    struct fd_entry *e = &thread_current()->fdt->fds[fd];
    if (!e->in_use || e->kind != FD_SOCKET) return -EBADF;
    if (e->socket_cid < 0) return -EBADF;
    return (long)tcp_state(e->socket_cid);
}

static long sys_socket_shutdown(long fd)
{
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -EBADF;
    struct fd_entry *e = &thread_current()->fdt->fds[fd];
    if (!e->in_use || e->kind != FD_SOCKET) return -EBADF;
    if (e->socket_cid < 0) return -EBADF;
    /* Send FIN; the cid stays alive (so reads can drain peer
     * data) until close() releases the fd entirely. */
    return tcp_close(e->socket_cid);
}

/* ── Milestone 57 — DNS ─────────────────────────────────────
 *
 *   SYS_RESOLVE(const char *name, uint32_t *out_ip4_be) -> 0/-errno
 *
 * `name` is a NUL-terminated ASCII hostname in user memory
 * (we copy_from_user it into a kernel buffer first).  On success
 * writes the resolved IPv4 address packed network-byte-order
 * into `*out_ip4_be` (same encoding sys_socket_connect expects).
 */
static long sys_resolve(long name_uptr, long out_uptr)
{
    char name[256];
    /* Copy in up to 255 bytes + a guaranteed NUL.  We don't have
     * a copy_string_from_user; do a per-byte copy with a NUL
     * sentinel check. */
    size_t i = 0;
    for (; i < sizeof(name) - 1; i++) {
        char c;
        if (copy_from_user(&c, (uint64_t)name_uptr + i, 1) < 0)
            return -EFAULT;
        name[i] = c;
        if (c == '\0') break;
    }
    name[sizeof(name) - 1] = '\0';
    if (i == 0) return -EINVAL_VFS;

    uint8_t ip[4];
    if (dns_resolve(name, ip) < 0) return -EIO;

    uint32_t be = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                  ((uint32_t)ip[2] <<  8) |  (uint32_t)ip[3];
    if (copy_to_user((uint64_t)out_uptr, &be, sizeof(be)) < 0)
        return -EFAULT;
    return 0;
}

/* ── Milestone 40 — GUI ──────────────────────────────────────
 *
 * The argument structs for the multi-parameter GUI calls live in
 * user memory; we copy them out with copy_from_user before passing
 * to the WM.  Layout MUST match userspace/libc/syscall.h. */
struct gui_present_args_k {
    int32_t  id;
    uint32_t x, y, w, h;
    uint64_t src;            /* user pointer to BGRA pixels */
};
struct gui_fill_rect_args_k {
    int32_t  id;
    uint32_t x, y, w, h;
    uint32_t bgra;
};
struct gui_draw_text_args_k {
    int32_t  id;
    uint32_t x, y;
    uint64_t s;              /* user pointer to NUL-terminated string */
    uint32_t fg_bgra;
    uint32_t bg_bgra;
    int32_t  transparent;
};

/* Milestone 47 — extended create.  Six fields, passed via a single
 * user-memory struct so we don't run out of x0..x3 dispatcher slots. */
struct gui_create_window_ex_args_k {
    uint32_t w, h;
    uint64_t title;          /* user pointer to NUL-terminated title */
    uint32_t flags;          /* GUI_WIN_FLAG_* */
    int32_t  x, y;           /* GUI_WIN_POS_AUTO == cascade */
};

static long sys_gui_create_window(long w, long h, long title_uptr)
{
    return wm_create_window((uint64_t)thread_current()->id,
                            (uint32_t)w, (uint32_t)h,
                            (const char *)(uintptr_t)title_uptr);
}

static long sys_gui_create_window_ex(long args_uptr)
{
    struct gui_create_window_ex_args_k a;
    if (copy_from_user(&a, (uint64_t)args_uptr, sizeof(a)) < 0)
        return -EFAULT;
    return wm_create_window_ex((uint64_t)thread_current()->id,
                               a.w, a.h,
                               (const char *)(uintptr_t)a.title,
                               a.flags, a.x, a.y);
}

static long sys_gui_list_windows(long out_uptr, long max)
{
    return wm_list_windows((uint64_t)thread_current()->id,
                           (struct gui_window_info *)(uintptr_t)out_uptr,
                           (int32_t)max);
}

static long sys_gui_raise_window(long id)
{
    return wm_raise_window((uint64_t)thread_current()->id, (int32_t)id);
}

/* Milestone 51 — toggle a window's minimized state.  on != 0
 * hides; on == 0 restores (and raises + focuses, like a click on
 * the taskbar entry). */
static long sys_gui_set_minimized(long id, long on)
{
    return wm_set_minimized((uint64_t)thread_current()->id,
                            (int32_t)id, (int)on);
}

/* Returns the active scanout dimensions in pixels.  Either out
 * pointer may be NULL.  This lets userspace processes (desktop,
 * taskbar, notify) size themselves to the actual framebuffer
 * instead of hardcoding a resolution. */
static long sys_gui_get_screen_size(long out_w_uptr, long out_h_uptr)
{
    if (!fb_is_ready()) return -EINVAL_VFS;
    const struct fb_info *fb = fb_get_info();
    uint32_t w = fb->width;
    uint32_t h = fb->height;
    if (out_w_uptr) {
        if (copy_to_user((uint64_t)out_w_uptr, &w, sizeof(w)) < 0)
            return -EFAULT;
    }
    if (out_h_uptr) {
        if (copy_to_user((uint64_t)out_h_uptr, &h, sizeof(h)) < 0)
            return -EFAULT;
    }
    return 0;
}

static long sys_gui_destroy_window(long id)
{
    return wm_destroy_window((uint64_t)thread_current()->id, (int32_t)id);
}

static long sys_gui_present(long args_uptr)
{
    struct gui_present_args_k a;
    if (copy_from_user(&a, (uint64_t)args_uptr, sizeof(a)) < 0)
        return -EFAULT;
    return wm_present((uint64_t)thread_current()->id, a.id,
                      a.x, a.y, a.w, a.h,
                      (const uint8_t *)(uintptr_t)a.src);
}

static long sys_gui_fill_rect(long args_uptr)
{
    struct gui_fill_rect_args_k a;
    if (copy_from_user(&a, (uint64_t)args_uptr, sizeof(a)) < 0)
        return -EFAULT;
    return wm_fill_rect((uint64_t)thread_current()->id, a.id,
                        a.x, a.y, a.w, a.h, a.bgra);
}

static long sys_gui_draw_text(long args_uptr)
{
    struct gui_draw_text_args_k a;
    if (copy_from_user(&a, (uint64_t)args_uptr, sizeof(a)) < 0)
        return -EFAULT;
    return wm_draw_text((uint64_t)thread_current()->id, a.id,
                        a.x, a.y,
                        (const char *)(uintptr_t)a.s,
                        a.fg_bgra, a.bg_bgra, a.transparent);
}

/* Chapter 102 -- measure text width in the kernel's default font.
 * Lets userspace position carets, centre labels, and truncate to
 * fit without assuming a fixed 8-px glyph pitch (no longer true
 * with the TTF font). */
static long sys_gui_measure_text(long s_uptr)
{
    return wm_measure_text((const char *)(uintptr_t)s_uptr);
}

static long sys_gui_flush(long id)
{
    return wm_flush((uint64_t)thread_current()->id, (int32_t)id);
}

static long sys_gui_poll_event(long out_uptr)
{
    pump_input_into_wm();
    return wm_poll_event((uint64_t)thread_current()->id,
                         (struct gui_event *)(uintptr_t)out_uptr);
}

/* Chapter 101 — friendly stack-overflow diagnostic.
 *
 * Called from svc_dispatch when a data abort from EL0 lands on
 * a page tagged DESC_SW_GUARD (i.e. the one-page guard sitting
 * immediately below the user stack base).  The forensic dance
 * that chapter 27's postscript described — match ESR EC=0x24,
 * eyeball FAR against the stack base, suspect runaway recursion
 * — is now a single message printed at fault time.
 *
 * `far`  = the offending VA (somewhere in the guard page).
 * `elr`  = the user PC of the faulting instruction.
 *
 * The function only prints; the caller is responsible for
 * killing the thread (via thread_exit) so the kernel can return
 * cleanly to the scheduler. */
static void report_user_stack_overflow(struct thread *t,
                                       uint64_t far, uint64_t elr)
{
    const uint64_t stack_top    = USER_STACK_TOP;
    const uint64_t stack_bot    = stack_top -
                                  (uint64_t)USER_STACK_PAGES * 0x1000ULL;
    const uint64_t guard_va     = USER_STACK_GUARD_VA;
    /* Bytes the access overshot the stack floor.  Capped at one
     * page because the guard is one page wide; anything beyond
     * that would not be a guard-page fault in the first place. */
    const uint64_t overshoot    = (far <= stack_bot) ? (stack_bot - far) : 0;

    serial_puts("\n[svc] user stack overflow in thread \"");
    serial_puts(t ? t->name : "(null)");
    serial_puts("\" (pid ");
    serial_puthex(t ? (uint64_t)t->id : 0);
    serial_puts(")\n");

    serial_puts("        FAR_EL1  = "); serial_puthex(far);
    serial_puts("  (stack floor "); serial_puthex(stack_bot);
    serial_puts(", overran by "); serial_puthex(overshoot);
    serial_puts(" bytes)\n");

    serial_puts("        ELR_EL1  = "); serial_puthex(elr); serial_puts("\n");

    serial_puts("        stack    = ["); serial_puthex(stack_bot);
    serial_puts(", "); serial_puthex(stack_top);
    serial_puts(")  ");
    serial_puthex((uint64_t)USER_STACK_PAGES);
    serial_puts(" pages\n");

    serial_puts("        guard    = ["); serial_puthex(guard_va);
    serial_puts(", "); serial_puthex(guard_va + 0x1000ULL);
    serial_puts(")  1 page (DESC_SW_GUARD)\n");

    serial_puts("        likely cause: unbounded recursion, "
                "or one stack frame larger than 64 KiB.\n");
    serial_puts("        thread killed.\n");
}

void svc_dispatch(struct exception_frame *frame)
{
    /* Confirm this really is an SVC and not a stray fault routed
     * through the same vector slot. */
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> ESR_EC_SHIFT) & 0x3F);

    if (ec != ESR_EC_SVC64) {
        uint64_t far;
        __asm__ volatile("mrs %0, far_el1" : "=r"(far));

        /* Chapter 101 — guard-page check FIRST.  An access to a
         * page tagged DESC_SW_GUARD presents as a translation
         * fault from EL0 (DFSC 0x04..0x07), which would
         * otherwise fall through to the mmap/COW path.  Both of
         * those would return -1 for the guard VA (no vma, no
         * COW marker), but checking the SW bit up-front lets us
         * emit a domain-specific diagnostic instead of the
         * generic "non-SVC sync exception" dump.  This is the
         * payoff for installing the guard at AS-create time:
         * the kernel can name the failure mode for the user. */
        if (ec == 0x24) {
            struct thread *t = thread_current();
            if (t && t->as) {
                uint64_t pte = address_space_lookup_pte(t->as, far);
                if (pte & DESC_SW_GUARD) {
                    report_user_stack_overflow(t, far, frame->elr);
                    thread_exit(-1);
                    /* not reached */
                }
            }
        }

        /* Chapter 75 \u2014 if this is a data abort from EL0 caused
         * by a write to a permission-faulting page, give the COW
         * handler a chance to resolve it before we kill the
         * thread.  EC 0x24 = data abort from a lower EL.  ISS bit
         * 6 (WnR) = "write, not read".  ISS DFSC bits[5:0] for a
         * permission fault at translation level 3 = 0b001111
         * (= 0x0F); for level 2 = 0x0E; for level 1 = 0x0D; for
         * level 0 = 0x0C.  We accept any of those four because
         * the actual fault granule is whatever the walk produced.
         *
         * Chapter 90 \u2014 also handle translation faults (DFSC
         * 0x04..0x07) by routing through the mmap fault handler.
         * That's the lazy fault-in path: a vma covering the
         * faulting VA causes us to allocate (or page-cache-load)
         * a page and install it.  The mmap path is checked FIRST
         * because translation faults and permission faults are
         * mutually exclusive: a translation fault means "no
         * descriptor at all," which COW could never resolve. */
        if (ec == 0x24) {
            uint32_t iss  = (uint32_t)(esr & 0x01FFFFFFu);
            uint32_t dfsc = iss & 0x3F;
            int      wnr  = (iss >> 6) & 1;
            int      perm = (dfsc >= 0x0C && dfsc <= 0x0F);
            int      xlat = (dfsc >= 0x04 && dfsc <= 0x07);
            struct thread *t = thread_current();
            if (xlat && t && t->as) {
                if (address_space_handle_mmap_fault(t->as, far, wnr) == 0) {
                    /* Lazy mmap page faulted in.  Re-execute. */
                    return;
                }
            }
            if (wnr && perm && t && t->as) {
                if (address_space_handle_cow_fault(t->as, far) == 0) {
                    /* Resolved.  Eret straight back to the
                     * faulting instruction \u2014 it'll re-execute
                     * and now hit a writable page. */
                    return;
                }
            }
        }

        serial_puts("[svc] FATAL: non-SVC sync exception from EL0\n");
        serial_puts("        ESR_EL1 = "); serial_puthex(esr); serial_puts("\n");
        serial_puts("        EC      = "); serial_puthex((uint64_t)ec); serial_puts("\n");
        serial_puts("        FAR_EL1 = "); serial_puthex(far); serial_puts("\n");
        serial_puts("        ELR_EL1 = "); serial_puthex(frame->elr); serial_puts("\n");
        serial_puts("        SPSR    = "); serial_puthex(frame->spsr); serial_puts("\n");
        serial_puts("        thread  = "); serial_puts(thread_current()->name); serial_puts("\n");
        /* Kill the offending thread instead of returning to it
         * (which would just refault). */
        thread_exit(-1);
        return;
    }

    long num  = (long)frame->x[8];
    long a0   = (long)frame->x[0];
    long a1   = (long)frame->x[1];
    long a2   = (long)frame->x[2];
    long a3   = (long)frame->x[3];
    long a4   = (long)frame->x[4];
    long a5   = (long)frame->x[5];

    /* Chapter 100 — syscall tracer.  Reserve a ring slot before
     * the dispatch so the entry's `args` reflect the values the
     * handler is about to see.  The pointer is NULL when the
     * calling thread isn't traced (the common case — strace_enter
     * is a one-branch no-op).  The slot is back-filled with `ret`
     * and `completed=1` after the switch returns. */
    struct strace_entry *_tr = NULL;
    {
        struct thread *_tt = thread_current();
        if (_tt && _tt->strace) {
            _tr = strace_enter(_tt, (uint32_t)num,
                               (uint64_t)a0, (uint64_t)a1,
                               (uint64_t)a2, (uint64_t)a3,
                               (uint64_t)a4, (uint64_t)a5);
        }
    }

    long ret;
    switch (num) {
    case SYS_WRITE:  ret = sys_write(a0, a1, a2);    break;
    case SYS_EXIT:   ret = sys_exit(a0);             break;
    case SYS_GETPID: ret = sys_getpid();             break;
    case SYS_YIELD:  ret = sys_yield();              break;
    case SYS_OPEN:   ret = sys_open(a0, a1);         break;
    case SYS_READ:   ret = sys_read(a0, a1, a2);     break;
    case SYS_CLOSE:  ret = sys_close(a0);            break;
    case SYS_SPAWN:  ret = sys_spawn(a0, a1);        break;
    case SYS_WAIT:   ret = sys_wait(a0);             break;
    case SYS_GETARGS:ret = sys_getargs(a0, a1);      break;
    case SYS_SBRK:   ret = sys_sbrk(a0);             break;
    case SYS_LISTDIR:ret = sys_listdir(a0, a1, a2, a3); break;
    case SYS_UPTIME_MS: ret = sys_uptime_ms();          break;
    case SYS_CHDIR:  ret = sys_chdir(a0);            break;
    case SYS_GETCWD: ret = sys_getcwd(a0, a1);       break;
    case SYS_GETENV: ret = sys_getenv(a0, a1, a2);   break;
    case SYS_SETENV: ret = sys_setenv(a0, a1);       break;
    case SYS_UNSETENV: ret = sys_unsetenv(a0);       break;
    case SYS_GETENV_ALL: ret = sys_getenv_all(a0, a1); break;
    case SYS_SPAWN_REDIR: ret = sys_spawn_redir(a0, a1, a2); break;
    case SYS_SLEEP_MS: ret = sys_sleep_ms(a0);    break;
    case SYS_PIPE:   ret = sys_pipe(a0);             break;
    case SYS_DUP2:   ret = sys_dup2(a0, a1);         break;
    case SYS_SPAWN_PIPE: ret = sys_spawn_pipe(a0, a1, a2, a3); break;
    case SYS_UNLINK: ret = sys_unlink(a0);           break;
    case SYS_TTY_RAW: ret = sys_tty_raw(a0);         break;
    case SYS_KILL:   ret = sys_kill(a0, a1);         break;
    case SYS_SET_FG_PID: ret = sys_set_fg_pid(a0);   break;
    /* Milestone 65 — fork + exec.  Both need direct access to
     * the saved trap frame, so they take `frame` instead of the
     * unpacked a0..a3 args. */
    case SYS_FORK:   ret = sys_fork(frame);          break;
    case SYS_EXEC:   ret = sys_exec(frame, a0, a1);  break;

    /* Chapter 77 — Catchable signals.  sigreturn also needs
     * direct frame access (it overwrites it from the user's
     * saved sigframe). */
    case SYS_SIGACTION: ret = sys_sigaction(a0, a1, a2);   break;
    case SYS_SIGRETURN: ret = sys_sigreturn(frame, a0);    break;

    /* Chapter 78 — SIGCHLD + waitpid (generalised reaper). */
    case SYS_WAITPID:   ret = sys_waitpid(a0, a1, a2);     break;

    /* Chapter 79b — pty allocation. */
    case SYS_OPENPTY:   ret = sys_openpty(a0, a1);         break;

    /* Chapter 82 — durability for OSFS-2 (no-op for other fds). */
    case SYS_FSYNC:     ret = sys_fsync(a0);                break;

    /* Chapter 85 — directory namespace. */
    case SYS_MKDIR:        ret = sys_mkdir(a0);                       break;
    case SYS_LISTDIR_AT:   ret = sys_listdir_at(a0, a1, a2, a3, a4, a5); break;

    case SYS_GUI_CREATE_WINDOW:
        ret = sys_gui_create_window(a0, a1, a2);
        break;
    case SYS_GUI_DESTROY_WINDOW:
        ret = sys_gui_destroy_window(a0);
        break;
    case SYS_GUI_PRESENT:
        ret = sys_gui_present(a0);
        break;
    case SYS_GUI_FILL_RECT:
        ret = sys_gui_fill_rect(a0);
        break;
    case SYS_GUI_DRAW_TEXT:
        ret = sys_gui_draw_text(a0);
        break;
    case SYS_GUI_FLUSH:
        ret = sys_gui_flush(a0);
        break;
    case SYS_GUI_POLL_EVENT:
        ret = sys_gui_poll_event(a0);
        break;
    case SYS_GUI_CREATE_WINDOW_EX:
        ret = sys_gui_create_window_ex(a0);
        break;
    case SYS_GUI_LIST_WINDOWS:
        ret = sys_gui_list_windows(a0, a1);
        break;
    case SYS_GUI_RAISE_WINDOW:
        ret = sys_gui_raise_window(a0);
        break;
    case SYS_GUI_GET_SCREEN_SIZE:
        ret = sys_gui_get_screen_size(a0, a1);
        break;
    case SYS_GUI_SET_MINIMIZED:
        ret = sys_gui_set_minimized(a0, a1);
        break;
    case SYS_GUI_MEASURE_TEXT:
        ret = sys_gui_measure_text(a0);
        break;

    case SYS_SOCKET_CONNECT:
        ret = sys_socket_connect(a0, a1);
        break;
    case SYS_SOCKET_STATE:
        ret = sys_socket_state(a0);
        break;
    case SYS_SOCKET_SHUTDOWN:
        ret = sys_socket_shutdown(a0);
        break;
    case SYS_RESOLVE:
        ret = sys_resolve(a0, a1);
        break;

    case SYS_MMAP:
        ret = sys_mmap(a0, a1, a2, a3, a4, a5);
        break;
    case SYS_MUNMAP:
        ret = sys_munmap(a0, a1);
        break;

    /* Chapter 91 — userspace threads + futex. */
    case SYS_CLONE:
        ret = sys_clone(a0, a1, a2, a3);
        break;
    case SYS_FUTEX_WAIT:
        ret = sys_futex_wait(a0, a1);
        break;
    case SYS_FUTEX_WAKE:
        ret = sys_futex_wake(a0, a1);
        break;

    /* Chapter 92 — clone with explicit CPU placement + getcpu. */
    case SYS_CLONE2:
        ret = sys_clone2(a0, a1, a2, a3, a4);
        break;
    case SYS_GETCPU:
        ret = sys_getcpu();
        break;

    /* Chapter 93 — clone3: extended-args clone with optional
     * CLONE_FILES (shared fd table). */
    case SYS_CLONE3:
        ret = sys_clone3(a0);
        break;

    /* Chapter 95 — wall-clock time via PL031 RTC. */
    case SYS_GETTIMEOFDAY:
        ret = sys_gettimeofday(a0);
        break;

    /* Chapter 96 — virtio-snd boot chime / SYS_BEEP. */
    case SYS_BEEP:
        ret = sys_beep(a0, a1);
        break;

    /* Chapter 100 — enable per-thread syscall tracing on self.
     * Idempotent: a second call is a no-op.  Cannot be revoked
     * (yet) — the ring lives until the thread exits.  No args. */
    case SYS_TRACE_ME:
        ret = strace_enable(thread_current()) == 0 ? 0 : -ENOMEM_VFS;
        break;

    default:
        serial_puts("[svc] unknown syscall ");
        serial_puthex((uint64_t)num);
        serial_puts("\n");
        ret = -ENOSYS;
        break;
    }

    frame->x[0] = (uint64_t)ret;

    /* Chapter 100 — stamp the tracer slot we reserved on entry.
     * The slot pointer is stable across the dispatch because the
     * traced thread is the only writer to its own ring and is
     * mid-SVC (cannot re-enter).  Concurrent readers from
     * /proc/<pid>/trace may have drained past us, in which case
     * this stamp lands in a slot that's logically free — harmless. */
    if (_tr) {
        _tr->ret = (int64_t)ret;
        _tr->completed = 1;
    }

    /* Signal-delivery tail.  Run AFTER the dispatcher has written
     * the syscall return value, so the saved x[0] in the sigframe
     * (if we divert into a handler) reflects what the user code
     * would have observed had the signal not arrived.
     *
     * We deliver at most one signal per syscall return.  Lowest
     * pending signum wins.  For each candidate:
     *   - SIG_DFL (handler == 0): terminate.  Done.
     *   - SIG_IGN (handler == 1): clear bit, keep scanning.
     *   - else: try to divert into the user handler.  If the
     *     divert fails (no restorer registered yet, copy_to_user
     *     bombed, etc.) we fall back to the SIG_DFL action so
     *     the signal isn't silently dropped.
     *
     * Sigreturn is special: it has *just* restored the trap frame
     * from a sigframe and we MUST NOT immediately re-deliver the
     * same signal that the handler just acknowledged.  Easiest
     * way to enforce that is to skip the tail entirely on a
     * sigreturn syscall. */
    if (num != SYS_SIGRETURN) {
        struct thread *t = thread_current();
        if (t && t->sig_pending) {
            for (int s = 1; s < 32; s++) {
                if (!(t->sig_pending & ((uint32_t)1 << s))) continue;
                t->sig_pending &= ~((uint32_t)1 << s);
                uint64_t h = t->sig_handlers[s];
                if (h == 1) continue;                 /* SIG_IGN */
                if (h == 0) {                         /* SIG_DFL */
                    /* Chapter 78: SIGCHLD's POSIX default is
                     * "ignore".  Without this special-case, every
                     * existing program that doesn't catch SIGCHLD
                     * (i.e. all of them, pre-chapter-78) would be
                     * killed the moment one of its forked
                     * children exits. */
                    if (s == SIGCHLD) continue;
                    thread_exit(128 + s);
                    /* unreachable */
                }
                /* Catchable: divert into user handler. */
                if (deliver_signal(t, frame, s, h) < 0) {
                    thread_exit(128 + s);
                    /* unreachable */
                }
                break;     /* one delivery per dispatch */
            }
        }
    }
}
