/*
 * kernel/core/win_fb.h — chapter 117.
 *
 * Server-allocated, client-mappable per-window framebuffer.
 * Lets wsd (window-server daemon) allocate a BGRA pixel buffer
 * and share it with the window's owning client process, so the
 * client can paint directly without going through wsd for
 * every primitive.
 *
 * Why this is a separate module from kernel/core/wm.c (the
 * legacy in-kernel WM) and kernel/core/wsd_fb.c (the chapter
 * 108d scanout claim): each of those is a single-instance,
 * specific-purpose primitive (window-tied pixel buffer, or
 * the unique scanout buffer respectively).  This module
 * provides a TABLE of N independent shareable backing
 * objects — closer to a tiny shared-memory subsystem than to
 * either of those.  Once chapter 117's wmclient starts
 * making these per window, the table becomes the only path
 * by which userspace gets at a window's pixels.
 *
 * Access model
 *
 *   sys_win_fb_alloc → caller becomes "owner".  Kernel
 *     allocates n pages from pmem, maps them RW into caller's
 *     AS, returns {id, va, stride, size}.  Owner-pid is
 *     recorded so SYS_WIN_FB_FREE can be ACL-gated.
 *
 *   sys_win_fb_map → caller passes an id; kernel maps the
 *     same physical pages into caller's AS at a fresh VA.
     * Chapter 117 is intentionally permissive: any caller who
 *     knows the id can map.  The ACL is the chapter-107 IPC
 *     channel — wsd only hands an id to the client that owns
 *     the window.  A future capability layer can tighten this
 *     without changing the wire protocol.
 *
 *   sys_win_fb_free → only the owner may call.  Tears down
 *     every mapper's installation FIRST, then frees the
 *     backing pages.  -EPERM if caller isn't owner; -ENOENT
 *     if id is unknown.
 *
 *   win_fb_release_pid → exit-time hook.  Called from
 *     thread_exit BEFORE address_space_destroy:
 *       * If pid was an owner: same as sys_win_fb_free
 *         (uninstall surviving mappings, free pages).
 *       * If pid was a mapper: just clear that mapping
 *         tracking-slot.  Don't bother uninstalling — the
 *         AS is already invisible to the MMU (we're past
 *         the final yield()) and its L3 tables are about to
 *         be torn down.  The kernel-arch AS-destroy code
 *         skips DESC_SW_WM_WINDOW descriptors so the pages
 *         themselves aren't pmem_free'd through it.
 *
 * Why the exit hook is mandatory: if a mapper dies and its
 * tracking slot lingers, a subsequent owner-FREE will try to
 * uninstall_wm_window against a destroyed AS struct and
 * dereference freed memory.  The hook converts client death
 * into a no-op-from-FREE's-perspective.
 */

#ifndef KERNEL_CORE_WIN_FB_H
#define KERNEL_CORE_WIN_FB_H

#include <stdint.h>

long sys_win_fb_alloc(long args_uptr);
long sys_win_fb_map(long args_uptr);
long sys_win_fb_free(long id_arg);
/* chapter 118 — resize an existing win_fb to (new_w, new_h).
 * Owner-only.  Reallocates the backing, copies top-left of the
 * old contents over, uninstalls all existing mappings (owner +
 * mappers) since their VAs back stale pages, frees old pages.
 * Owner and mappers must re-call sys_win_fb_map to get a fresh
 * VA; their old VA returns translation-fault until they do. */
long sys_win_fb_resize(long id_arg, long new_w_arg, long new_h_arg);

void win_fb_release_pid(uint64_t pid);

#endif /* KERNEL_CORE_WIN_FB_H */
