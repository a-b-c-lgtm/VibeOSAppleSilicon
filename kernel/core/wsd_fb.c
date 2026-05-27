/*
 * kernel/core/wsd_fb.c — chapter 117.
 *
 * Frames-backwards version of the chapter 114 wm_map_window
 * primitive, applied to the framebuffer instead of to a window's
 * pixel buffer.  Exposes the single syscall SYS_FB_MAP_SCANOUT
 * which takes the contiguous physical pages of the active
 * virtio-gpu scanout buffer (allocated in fb_init via
 * pmem_alloc_contig) and installs them into the calling thread's
 * AS as a read-write user mapping.
 *
 * The whole point is to give a single userspace daemon — the
 * future /bin/wsd (window-server daemon) — the same write access
 * to the framebuffer that kernel/device/fb.c has today, so a
 * future chapter can move compose_all out of the kernel.  In
 * phase A wsd doesn't actually do any composition yet: it just
 * proves it can hold the mapping.
 *
 * Access model: first-caller-wins.  The first SYS_FB_MAP_SCANOUT
 * remembers the caller's pid.  Subsequent calls from other pids
 * return -EBUSY.  Idempotent for the holding pid.  When the
 * holding process exits, wsd_fb_release_owner() is called from
 * the exit path so a respawned wsd can re-claim the mapping.
 *
 * Why first-caller-wins and not a capability/cap-list?  The OS
 * has no cap mechanism yet and inventing one for a single client
 * is overkill.  In the steady state there's exactly one wsd
 * process; if a second tries to claim it, that's a bug, and
 * -EBUSY is the right answer.  When wsd respawns, the
 * init-supervisor calls SYS_WAIT, which triggers the exit-path
 * release, which clears the slot before the supervisor spawns
 * the replacement.  No race because both transitions go through
 * the same single-CPU dispatch.
 *
 * Why reuse address_space_install_wm_window?  It already does
 * exactly the right thing: install N PA frames at a fresh user
 * VA range, RW from EL0, non-executable, tagged
 * DESC_SW_WM_WINDOW so AS teardown skips them (we own them) and
 * fork() doesn't inherit them.  The "WM_WINDOW" name is a
 * historical artifact of chapter 114; the mechanism is general.
 */

#include "syscall.h"
#include "thread.h"
#include "heap.h"
#include "serial.h"
#include "uaccess.h"
#include "../arch/address_space.h"
#include "../device/fb.h"

#include <stddef.h>
#include <stdint.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096u
#endif

#ifndef EINVAL
#define EINVAL 22
#define ENOMEM 12
#define EFAULT 14
#define EBUSY  16
#define EAGAIN 11
#define EPERM   1
#endif

/* Mirror of struct fb_map_args in userspace/libc/syscall.h.
 * Layout MUST match. */
struct fb_map_args_k {
    uint64_t va;       /* OUT: user VA where the FB is mapped */
    uint32_t w;        /* OUT: scanout width  in pixels       */
    uint32_t h;        /* OUT: scanout height in pixels       */
    uint32_t stride;   /* OUT: bytes per scanline (= w*4)     */
    uint32_t size;     /* OUT: total bytes mapped (rounded
                        *      up to a page; matches what
                        *      fb_init computed)              */
};

/* The single owner.  -1 = unclaimed. */
static int64_t  g_owner_pid = -1;

/* Cached mapping for the owner.  va is the user VA into the
 * owner's AS; valid only while g_owner_pid == owning pid. */
static uint64_t g_owner_va  = 0;

/* Cached AS pointer.  Non-owning — we don't refcount it.  When
 * the owner exits, wsd_fb_release_owner is called BEFORE
 * address_space_destroy, so we either uninstall here (best
 * effort) or just clear the slot (the AS is going away anyway). */
static struct address_space *g_owner_as = NULL;

/* Page count of the current mapping.  Cached so unmap doesn't
 * have to recompute. */
static uint64_t g_owner_n_pages = 0;

static long do_install(uint64_t pid)
{
    if (!fb_is_ready()) return -EAGAIN;

    const struct fb_info *fi = fb_get_info();
    if (!fi || fi->size_bytes == 0) return -EAGAIN;

    struct thread *t = thread_current();
    if (!t || !t->as) return -EFAULT;

    /* Build a trivial array of N contiguous PAs.  We use the
     * same install primitive as chapter 114 (which expects an
     * arbitrary list of non-contiguous PAs) — the contiguity
     * doesn't help us here, but it does let us synthesise the
     * array deterministically rather than copying it from any
     * pre-existing source. */
    uint64_t bytes = fi->size_bytes;
    uint64_t n = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *pages = (uint64_t *)kmalloc((size_t)n * sizeof(uint64_t));
    if (!pages) return -ENOMEM;
    for (uint64_t i = 0; i < n; i++)
        pages[i] = fi->phys + i * PAGE_SIZE;

    uint64_t va = 0;
    if (address_space_install_wm_window(t->as, pages, n, &va) != 0) {
        kfree(pages);
        return -ENOMEM;
    }
    /* address_space_install_wm_window copies the PAs into its
     * own bookkeeping; we don't need the array anymore. */
    kfree(pages);

    g_owner_pid     = (int64_t)pid;
    g_owner_va      = va;
    g_owner_as      = t->as;
    g_owner_n_pages = n;

    serial_puts("[wsd_fb] map_scanout pid=");
    serial_puthex(pid);
    serial_puts(" va=");
    serial_puthex(va);
    serial_puts(" bytes=");
    serial_puthex(bytes);
    serial_puts(" pages=");
    serial_puthex(n);
    serial_puts("\n");
    return 0;
}

/* SYS_FB_MAP_SCANOUT entry point.  args_uptr points at a user
 * struct fb_map_args; we fill in every OUT field. */
long sys_fb_map_scanout(long args_uptr)
{
    struct thread *t = thread_current();
    if (!t) return -EFAULT;
    uint64_t pid = (uint64_t)t->id;

    /* Idempotency / single-owner enforcement. */
    if (g_owner_pid >= 0 && (uint64_t)g_owner_pid != pid)
        return -EBUSY;

    if (g_owner_pid < 0) {
        long r = do_install(pid);
        if (r < 0) return r;
    }

    /* Copy descriptors back to user.  Re-read fb_info every
     * call so a future hot-replug (chapter > 108d) is visible. */
    const struct fb_info *fi = fb_get_info();
    if (!fi) return -EAGAIN;

    struct fb_map_args_k out;
    out.va     = g_owner_va;
    out.w      = fi->width;
    out.h      = fi->height;
    out.stride = fi->pitch;
    out.size   = fi->size_bytes;

    if (copy_to_user((uint64_t)args_uptr, &out, sizeof(out)) < 0)
        return -EFAULT;
    return 0;
}

/* Called from the thread exit path when any thread terminates.
 * If `pid` was the owner of the FB mapping, tear it down and
 * release the slot so a replacement (e.g. respawned wsd) can
 * claim it.  No-op if pid was not the owner. */
void wsd_fb_release_owner(uint64_t pid)
{
    if (g_owner_pid < 0 || (uint64_t)g_owner_pid != pid) return;

    serial_puts("[wsd_fb] release pid=");
    serial_puthex(pid);
    serial_puts("\n");

    /* Best-effort uninstall.  The owner's AS may already be
     * partway through teardown; if address_space_uninstall_*
     * disagrees, we still clear the slot — the AS is going
     * away regardless. */
    if (g_owner_as && g_owner_n_pages > 0) {
        (void)address_space_uninstall_wm_window(g_owner_as,
                                                g_owner_va,
                                                g_owner_n_pages);
    }
    g_owner_pid     = -1;
    g_owner_va      = 0;
    g_owner_as      = NULL;
    g_owner_n_pages = 0;
}

/* ------------------------------------------------------------------
 * Chapter 117 — SYS_FB_PRESENT.
 *
 * After chapter 117 the kernel WM no longer composes anything, so wsd
 * owns the whole "store pixels in scanout RAM, then flush to
 * GPU" chain.  fb_present() (in kernel/device/fb.c) is the
 * primitive that puts the GPU on the hook to re-read the
 * scanout pages; without a userspace entry point wsd's writes
 * would land in RAM forever and never reach the surface.
 *
 * The handler is intentionally trivial: clip-and-flush.  We
 * don't gate by g_owner_pid here — fb_present is idempotent,
 * the underlying virtio-gpu submit is one request, and
 * locking out non-owners would just complicate the test path
 * for no real safety win (anyone with code execution can
 * already write to the same scanout pages via the existing
 * SYS_FB_MAP_SCANOUT primitive).
 */
long sys_fb_present(long x_arg, long y_arg, long w_arg, long h_arg)
{
    /* All four args are passed through 64-bit registers but
     * fb_present takes uint32_t.  Cast at the boundary; out-
     * of-range values get clipped inside fb_present so we
     * don't validate here. */
    fb_present((uint32_t)x_arg,
               (uint32_t)y_arg,
               (uint32_t)w_arg,
               (uint32_t)h_arg);
    return 0;
}
