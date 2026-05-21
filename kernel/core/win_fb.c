/*
 * kernel/core/win_fb.c — chapter 108d.
 *
 * See win_fb.h for the design.  This file is the implementation
 * of the table + the three syscalls + the exit-time hook.
 */

#include "win_fb.h"
#include "syscall.h"
#include "thread.h"
#include "heap.h"
#include "pmem.h"
#include "serial.h"
#include "uaccess.h"
#include "../arch/address_space.h"

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
#define EPERM   1
#define ENOENT  2
#define ENOSPC 28
#endif

/* Sizing limits.  64 backing objects fits the WM_MAX_WINDOWS
 * cap (one per window plus headroom for offscreen / future
 * resize buffers).  4096x4096 caps a single allocation at 64
 * MiB which is well below the OOM threshold but large enough
 * to back any plausible window on a 1080p screen.  Per-FB
 * mapping array sized to 4 — owner + a couple of follow-on
 * mappers (debug tools, screencap once that lands).  The
 * single intended use today is owner=wsd + one client mapping,
 * so 4 is comfortable headroom. */
#define WIN_FB_TABLE_SIZE     64u
#define WIN_FB_MAX_MAPPINGS    4u
#define WIN_FB_MAX_W        4096u
#define WIN_FB_MAX_H        4096u

/* chapter 108e follow-up #3 — lazy-unmap of stale FB pages across
 * a resize.  Background:
 *
 *   The old sys_win_fb_resize was synchronous from any one caller's
 *   PoV but UN-synchronous w.r.t. mappers: it allocated new pages,
 *   COPIED top-left, then UNINSTALLED every mapper's VA against the
 *   old pages, and only then sent GUI_EVENT_RESIZE.  Mappers (clients
 *   like the browser) cache their FB VA across renders, so any access
 *   that happened to land between the kernel's uninstall and the
 *   mapper's GUI_EVENT_RESIZE handler took a translation-fault.
 *
 *   On hn.html (~35 KB) a single render takes ~100 ms and is full of
 *   per-pixel writes to the cached VA — wsd's resize tick lands
 *   inside that window with near certainty.  Result: browser dies
 *   with ESR_EL1 EC=0x24 inside render_gui_frame, leaving wsd
 *   compositing a zombie window.  The user-visible bug was "the
 *   grown area stays gray-blue" because nobody was repainting it.
 *
 * Fix: lazy unmap.  At sys_win_fb_resize time we keep the OLD pages
 * alive AND keep the mappers' old VAs installed against them.  Each
 * mapper picks up the new VA + new pages on its next sys_win_fb_map
 * call (which the mapper makes anyway in response to GUI_EVENT_RESIZE
 * via wm_window_remap_fb).  Old pages are freed when the last mapper
 * has acknowledged the resize.
 *
 * Memory cost: bounded by WIN_FB_MAX_STALE_GEN generations per FB.
 * Each generation holds one FB's worth of pages plus a tracking
 * struct.  4 generations × ~1 MB typical FB × 64 FBs = 256 MB worst
 * case, but in practice mappers process events fast and only 1-2
 * generations are alive at any moment.  If a mapper falls more than
 * WIN_FB_MAX_STALE_GEN resizes behind, the OLDEST stale generation
 * is force-uninstalled (the laggard mapper crashes — same failure
 * mode as the pre-fix behaviour, just much rarer). */
#define WIN_FB_MAX_STALE_GEN  4u

/* Mirrors of the userspace structs in userspace/libc/syscall.h.
 * Layout MUST match. */
struct win_fb_alloc_args_k {
    uint32_t w;            /* IN  */
    uint32_t h;            /* IN  */
    uint32_t id;           /* OUT */
    uint32_t _pad0;        /* keep va 8-aligned */
    uint64_t va;           /* OUT */
    uint32_t stride;       /* OUT */
    uint32_t size;         /* OUT */
};

struct win_fb_map_args_k {
    uint32_t id;           /* IN  */
    uint32_t w;            /* OUT */
    uint32_t h;            /* OUT */
    uint32_t _pad0;        /* keep va 8-aligned */
    uint64_t va;           /* OUT */
    uint32_t stride;       /* OUT */
    uint32_t size;         /* OUT */
};

struct win_fb_mapping {
    int                   in_use;
    struct address_space *as;
    uint64_t              va;
    uint64_t              pid;    /* informational only */
    /* Lazy-unmap (see WIN_FB_MAX_STALE_GEN comment).  -1 means
     * this mapping's `va` points at the CURRENT fb->pages.  >=0
     * means it points at fb->stale[stale_idx].pages and the next
     * sys_win_fb_map from this AS will refresh to current. */
    int                   stale_idx;
};

struct win_fb_stale_gen {
    int       in_use;
    uint64_t *pages;       /* kmalloc'd PA array, n_pages entries */
    uint32_t  n_pages;
    uint32_t  ref_count;   /* # mappings still pointing here */
};

struct win_fb {
    int                   in_use;
    uint32_t              id;
    uint64_t              owner_pid;
    struct address_space *owner_as;
    uint64_t              owner_va;
    uint32_t              w;
    uint32_t              h;
    uint32_t              stride;
    uint32_t              size;
    uint32_t              n_pages;
    uint64_t             *pages;   /* kmalloc'd; n_pages entries */
    struct win_fb_mapping mappings[WIN_FB_MAX_MAPPINGS];
    /* Lazy-unmap state — see WIN_FB_MAX_STALE_GEN comment. */
    struct win_fb_stale_gen stale[WIN_FB_MAX_STALE_GEN];
};

static struct win_fb g_table[WIN_FB_TABLE_SIZE];
static uint32_t      g_next_id = 1u;

/* Slot helpers ---------------------------------------------------- */

static struct win_fb *find_by_id(uint32_t id)
{
    if (id == 0) return NULL;
    for (uint32_t i = 0; i < WIN_FB_TABLE_SIZE; i++) {
        if (g_table[i].in_use && g_table[i].id == id) return &g_table[i];
    }
    return NULL;
}

static int find_free_slot(void)
{
    for (uint32_t i = 0; i < WIN_FB_TABLE_SIZE; i++) {
        if (!g_table[i].in_use) return (int)i;
    }
    return -1;
}

static int find_free_mapping(struct win_fb *fb)
{
    for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
        if (!fb->mappings[i].in_use) return (int)i;
    }
    return -1;
}

/* Free pages helper.  Used both for partial-rollback on
 * alloc failure and final tear-down on free / owner-exit. */
static void free_backing_pages(struct win_fb *fb)
{
    if (!fb->pages) return;
    for (uint32_t i = 0; i < fb->n_pages; i++) {
        if (fb->pages[i]) pmem_free_page(fb->pages[i]);
    }
    kfree(fb->pages);
    fb->pages   = NULL;
    fb->n_pages = 0;
}

/* ---- stale-generation helpers (chapter 108e follow-up #3) ---- */

/* Free the PA array AND tracking struct for one stale generation.
 * Caller must already have uninstalled every VA that referenced
 * it; that's the invariant guaranteed by ref_count reaching 0. */
static void stale_gen_free(struct win_fb_stale_gen *g)
{
    if (!g->in_use) return;
    if (g->pages) {
        for (uint32_t i = 0; i < g->n_pages; i++) {
            if (g->pages[i]) pmem_free_page(g->pages[i]);
        }
        kfree(g->pages);
    }
    g->in_use    = 0;
    g->pages     = NULL;
    g->n_pages   = 0;
    g->ref_count = 0;
}

/* Find an unused stale slot, or recycle the one with the smallest
 * ref_count (oldest / most-drained) if all are busy.  Recycling a
 * still-referenced generation force-uninstalls its mappings, which
 * crashes any client still rendering against it \u2014 the same failure
 * mode as the pre-fix behaviour, just bounded to mappers more than
 * WIN_FB_MAX_STALE_GEN resizes behind. */
static int stale_gen_alloc(struct win_fb *fb)
{
    int free_idx = -1;
    int recycle_idx = -1;
    uint32_t recycle_ref = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < WIN_FB_MAX_STALE_GEN; i++) {
        if (!fb->stale[i].in_use) { free_idx = (int)i; break; }
        if (fb->stale[i].ref_count < recycle_ref) {
            recycle_ref = fb->stale[i].ref_count;
            recycle_idx = (int)i;
        }
    }
    if (free_idx >= 0) return free_idx;
    /* Force-uninstall the chosen victim from every mapping that
     * still references it.  Those mappings revert to "current"
     * (their stale_idx is reset; their VA is unmapped).  Future
     * accesses by the client at the now-dead VA fault, exactly
     * like the pre-fix behaviour did for every resize. */
    for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
        struct win_fb_mapping *m = &fb->mappings[i];
        if (!m->in_use) continue;
        if (m->stale_idx != recycle_idx) continue;
        if (m->as) {
            (void)address_space_uninstall_wm_window(
                m->as, m->va, fb->stale[recycle_idx].n_pages);
        }
        /* Mapping slot stays in_use=1 but va=0/stale_idx=-1 so
         * a future sys_win_fb_map from the same AS finds it as
         * a "stale-empty" record and installs fresh. */
        m->va        = 0;
        m->stale_idx = -1;
        fb->stale[recycle_idx].ref_count--;
    }
    stale_gen_free(&fb->stale[recycle_idx]);
    serial_puts("[win_fb] stale: recycled gen ");
    serial_puthex((uint64_t)recycle_idx);
    serial_puts(" id=");
    serial_puthex(fb->id);
    serial_puts(" (mapper too far behind)\n");
    return recycle_idx;
}

/* Drop one reference on a stale generation; free the PA array
 * when the last referent acks. */
static void stale_gen_unref(struct win_fb *fb, int idx)
{
    if (idx < 0 || idx >= (int)WIN_FB_MAX_STALE_GEN) return;
    struct win_fb_stale_gen *g = &fb->stale[idx];
    if (!g->in_use) return;
    if (g->ref_count > 0) g->ref_count--;
    if (g->ref_count == 0) stale_gen_free(g);
}

/* Page count the mapping `m` is actually mapped at.  Stale
 * mappings use the stale generation's n_pages; current mappings
 * use fb->n_pages. */
static uint32_t mapping_n_pages(struct win_fb *fb,
                                struct win_fb_mapping *m)
{
    if (m->stale_idx >= 0 &&
        m->stale_idx < (int)WIN_FB_MAX_STALE_GEN &&
        fb->stale[m->stale_idx].in_use) {
        return fb->stale[m->stale_idx].n_pages;
    }
    return fb->n_pages;
}

/* Walk every in-use mapping for `fb` and uninstall it from
 * the mapper's AS.  Used by free/owner-exit, NOT by mapper-
 * exit (the AS is about to be destroyed; uninstalling
 * against it would dereference soon-to-be-freed memory).
 *
 * Skips mappings flagged for `skip_as` (the calling thread's
 * own AS, on the owner-exit path — that AS is itself about
 * to die and we don't want to touch it).  Passing NULL means
 * "uninstall all".  Errors from address_space_uninstall_*
 * are logged but not propagated; the caller still wants the
 * pages freed. */
static void uninstall_all_mappings(struct win_fb *fb,
                                   struct address_space *skip_as)
{
    for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
        struct win_fb_mapping *m = &fb->mappings[i];
        if (!m->in_use) continue;
        if (m->as && m->as != skip_as && m->va != 0) {
            /* Use the correct page count for whichever generation
             * this mapping is currently installed at (current vs
             * one of the stale gens).  Mismatching n_pages here
             * leaves dangling descriptors. */
            uint32_t np = mapping_n_pages(fb, m);
            int r = address_space_uninstall_wm_window(m->as, m->va, np);
            if (r != 0) {
                serial_puts("[win_fb] uninstall failed id=");
                serial_puthex(fb->id);
                serial_puts(" pid=");
                serial_puthex(m->pid);
                serial_puts("\n");
            }
        }
        /* If the mapping was pointing at a stale gen, drop its ref
         * (may free the gen).  Must happen BEFORE we free fb->pages
         * in destroy_fb so the stale gen's pmem_free still works. */
        if (m->stale_idx >= 0) stale_gen_unref(fb, m->stale_idx);
        m->in_use    = 0;
        m->as        = NULL;
        m->va        = 0;
        m->pid       = 0;
        m->stale_idx = -1;
    }
}

static void destroy_fb(struct win_fb *fb, struct address_space *skip_as)
{
    /* Uninstall every mapper's user-VA range first (still
     * needs the pages valid for the descriptor decode in
     * uninstall_wm_window), then free the backing pages,
     * then clear the slot. */
    uninstall_all_mappings(fb, skip_as);

    /* Uninstall owner too, unless owner's AS is the one we
     * were told to skip (owner-exit path). */
    if (fb->owner_as && fb->owner_as != skip_as && fb->n_pages > 0) {
        (void)address_space_uninstall_wm_window(fb->owner_as,
                                                fb->owner_va,
                                                fb->n_pages);
    }

    free_backing_pages(fb);

    /* chapter 108e follow-up #3 \u2014 free any remaining stale gens.
     * Every mapping that still pointed at them was just dropped by
     * uninstall_all_mappings (which also decremented ref_counts and
     * freed any gen that hit 0), so all remaining in-use stale gens
     * here must have ref_count == 0 from a mapper having exited
     * without remapping.  Force-free regardless. */
    for (uint32_t i = 0; i < WIN_FB_MAX_STALE_GEN; i++) {
        if (fb->stale[i].in_use) stale_gen_free(&fb->stale[i]);
    }

    serial_puts("[win_fb] destroy id=");
    serial_puthex(fb->id);
    serial_puts("\n");

    fb->in_use    = 0;
    fb->id        = 0;
    fb->owner_pid = 0;
    fb->owner_as  = NULL;
    fb->owner_va  = 0;
    fb->w = fb->h = 0;
    fb->stride = fb->size = 0;
}

/* Syscall: alloc ------------------------------------------------- */

long sys_win_fb_alloc(long args_uptr)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -EFAULT;

    struct win_fb_alloc_args_k a;
    if (copy_from_user(&a, (uint64_t)args_uptr, sizeof(a)) < 0)
        return -EFAULT;
    if (a.w == 0 || a.h == 0)            return -EINVAL;
    if (a.w > WIN_FB_MAX_W || a.h > WIN_FB_MAX_H) return -EINVAL;

    uint64_t bytes  = (uint64_t)a.w * (uint64_t)a.h * 4u;
    uint64_t pages  = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    int slot = find_free_slot();
    if (slot < 0) return -ENOSPC;
    struct win_fb *fb = &g_table[slot];

    /* Phase 1: allocate the backing pages.  Independent
     * frames; physical contiguity is unnecessary because the
     * AS layer maps them at one contiguous VA range. */
    uint64_t *pa = (uint64_t *)kmalloc((size_t)pages * sizeof(uint64_t));
    if (!pa) return -ENOMEM;
    for (uint64_t i = 0; i < pages; i++) pa[i] = 0;
    for (uint64_t i = 0; i < pages; i++) {
        pa[i] = pmem_alloc_page();
        if (!pa[i]) {
            for (uint64_t k = 0; k < i; k++) pmem_free_page(pa[k]);
            kfree(pa);
            return -ENOMEM;
        }
        /* Zero the page so the caller sees a clean buffer.
         * pmem_alloc_page does not guarantee zero. */
        uint8_t *p = (uint8_t *)(uintptr_t)pa[i];
        for (uint32_t k = 0; k < PAGE_SIZE; k++) p[k] = 0;
    }

    /* Phase 2: install into the caller's AS. */
    uint64_t va = 0;
    if (address_space_install_wm_window(t->as, pa, pages, &va) != 0) {
        for (uint64_t i = 0; i < pages; i++) pmem_free_page(pa[i]);
        kfree(pa);
        return -ENOMEM;
    }

    /* Phase 3: record on slot. */
    fb->in_use    = 1;
    fb->id        = g_next_id++;
    fb->owner_pid = (uint64_t)t->id;
    fb->owner_as  = t->as;
    fb->owner_va  = va;
    fb->w         = a.w;
    fb->h         = a.h;
    fb->stride    = a.w * 4u;
    fb->size      = (uint32_t)(pages * PAGE_SIZE);
    fb->n_pages   = (uint32_t)pages;
    fb->pages     = pa;
    for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
        fb->mappings[i].in_use    = 0;
        fb->mappings[i].as        = NULL;
        fb->mappings[i].va        = 0;
        fb->mappings[i].pid       = 0;
        fb->mappings[i].stale_idx = -1;
    }
    /* chapter 108e follow-up #3 \u2014 init lazy-unmap state. */
    for (uint32_t i = 0; i < WIN_FB_MAX_STALE_GEN; i++) {
        fb->stale[i].in_use    = 0;
        fb->stale[i].pages     = NULL;
        fb->stale[i].n_pages   = 0;
        fb->stale[i].ref_count = 0;
    }

    /* Phase 4: copy descriptors out. */
    a.id     = fb->id;
    a.va     = va;
    a.stride = fb->stride;
    a.size   = fb->size;
    if (copy_to_user((uint64_t)args_uptr, &a, sizeof(a)) < 0) {
        /* Caller's struct pointer was bogus; tear down the
         * mapping AND the FB so the slot doesn't leak. */
        destroy_fb(fb, NULL);
        return -EFAULT;
    }

    serial_puts("[win_fb] alloc id=");
    serial_puthex(fb->id);
    serial_puts(" pid=");
    serial_puthex(fb->owner_pid);
    serial_puts(" wxh=");
    serial_puthex(fb->w);
    serial_puts("x");
    serial_puthex(fb->h);
    serial_puts(" va=");
    serial_puthex(va);
    serial_puts(" pages=");
    serial_puthex(pages);
    serial_puts("\n");
    return 0;
}

/* Syscall: map --------------------------------------------------- */

long sys_win_fb_map(long args_uptr)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -EFAULT;

    struct win_fb_map_args_k a;
    if (copy_from_user(&a, (uint64_t)args_uptr, sizeof(a)) < 0)
        return -EFAULT;
    if (a.id == 0) return -EINVAL;

    struct win_fb *fb = find_by_id(a.id);
    if (!fb) return -ENOENT;

    /* Idempotency: if this AS already has a mapping, return
     * the cached VA.  Saves a re-install round-trip when an
     * app calls MAP_FB twice.
     *
     * chapter 108e follow-up #3: if the existing mapping is
     * STALE (was lazy-kept across a sys_win_fb_resize so the
     * mapper didn't translation-fault mid-render), refresh
     * it now \u2014 uninstall the stale VA, install a fresh one
     * against the current fb->pages, return the new VA.  This
     * is the "ack" point in the lazy-unmap protocol.
     *
     * Also handle the recycled-but-emptied stale slot: when
     * an oversubscribed FB recycled this AS's gen, the slot
     * was left in_use=1, va=0, stale_idx=-1.  va==0 forces
     * us through the fresh-install path below. */
    for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
        struct win_fb_mapping *m = &fb->mappings[i];
        if (!m->in_use || m->as != t->as) continue;
        if (m->stale_idx < 0 && m->va != 0) {
            a.w      = fb->w;
            a.h      = fb->h;
            a.va     = m->va;
            a.stride = fb->stride;
            a.size   = fb->size;
            if (copy_to_user((uint64_t)args_uptr, &a, sizeof(a)) < 0)
                return -EFAULT;
            return 0;
        }
        /* Stale (or va==0 from a forced recycle): refresh. */
        if (m->va != 0) {
            uint32_t np = mapping_n_pages(fb, m);
            (void)address_space_uninstall_wm_window(t->as, m->va, np);
        }
        if (m->stale_idx >= 0) {
            stale_gen_unref(fb, m->stale_idx);
            m->stale_idx = -1;
        }
        uint64_t new_va = 0;
        if (address_space_install_wm_window(t->as, fb->pages,
                                            fb->n_pages,
                                            &new_va) != 0) {
            /* Failed to install: drop the mapping slot entirely
             * so the next map call gets a fresh shot.  Client
             * will see the failure and presumably retry. */
            m->in_use = 0;
            m->as     = NULL;
            m->va     = 0;
            m->pid    = 0;
            return -ENOMEM;
        }
        m->va  = new_va;
        m->pid = (uint64_t)t->id;
        a.w      = fb->w;
        a.h      = fb->h;
        a.va     = new_va;
        a.stride = fb->stride;
        a.size   = fb->size;
        if (copy_to_user((uint64_t)args_uptr, &a, sizeof(a)) < 0) {
            (void)address_space_uninstall_wm_window(t->as, new_va,
                                                    fb->n_pages);
            m->in_use = 0;
            m->as     = NULL;
            m->va     = 0;
            m->pid    = 0;
            return -EFAULT;
        }
        serial_puts("[win_fb] map   id=");
        serial_puthex(fb->id);
        serial_puts(" pid=");
        serial_puthex((uint64_t)t->id);
        serial_puts(" va=");
        serial_puthex(new_va);
        serial_puts(" (refresh)\n");
        return 0;
    }
    /* Same AS as the owner: caller already has owner_va; just
     * return that.  Eliminates a redundant duplicate
     * installation for the (uncommon) owner-self-maps case. */
    if (fb->owner_as == t->as) {
        a.w      = fb->w;
        a.h      = fb->h;
        a.va     = fb->owner_va;
        a.stride = fb->stride;
        a.size   = fb->size;
        if (copy_to_user((uint64_t)args_uptr, &a, sizeof(a)) < 0)
            return -EFAULT;
        return 0;
    }

    int mslot = find_free_mapping(fb);
    if (mslot < 0) return -ENOSPC;

    uint64_t va = 0;
    if (address_space_install_wm_window(t->as, fb->pages, fb->n_pages,
                                        &va) != 0) {
        return -ENOMEM;
    }

    fb->mappings[mslot].in_use    = 1;
    fb->mappings[mslot].as        = t->as;
    fb->mappings[mslot].va        = va;
    fb->mappings[mslot].pid       = (uint64_t)t->id;
    fb->mappings[mslot].stale_idx = -1;

    a.w      = fb->w;
    a.h      = fb->h;
    a.va     = va;
    a.stride = fb->stride;
    a.size   = fb->size;
    if (copy_to_user((uint64_t)args_uptr, &a, sizeof(a)) < 0) {
        /* Unwind the install on bad pointer so we don't leak
         * the user-VA range. */
        (void)address_space_uninstall_wm_window(t->as, va, fb->n_pages);
        fb->mappings[mslot].in_use    = 0;
        fb->mappings[mslot].as        = NULL;
        fb->mappings[mslot].va        = 0;
        fb->mappings[mslot].pid       = 0;
        fb->mappings[mslot].stale_idx = -1;
        return -EFAULT;
    }

    serial_puts("[win_fb] map   id=");
    serial_puthex(fb->id);
    serial_puts(" pid=");
    serial_puthex((uint64_t)t->id);
    serial_puts(" va=");
    serial_puthex(va);
    serial_puts("\n");
    return 0;
}

/* Syscall: free -------------------------------------------------- */

long sys_win_fb_free(long id_arg)
{
    struct thread *t = thread_current();
    if (!t) return -EFAULT;

    uint32_t id = (uint32_t)id_arg;
    if (id == 0) return -EINVAL;

    struct win_fb *fb = find_by_id(id);
    if (!fb) return -ENOENT;
    if (fb->owner_pid != (uint64_t)t->id) return -EPERM;

    /* Owner-initiated free: uninstall everyone (including
     * owner itself), free pages, clear slot. */
    destroy_fb(fb, NULL);
    return 0;
}

/* Syscall: resize ----------------------------------------------- */

/* chapter 108e (revised by follow-up #3) \u2014 owner-only resize with
 * lazy-unmap of mappers.
 *
 * Visible from the OWNER's PoV:
 *   - On success owner_va is replaced with a fresh VA mapped to
 *     the new pages; the old owner_va is uninstalled.  Owner must
 *     call sys_win_fb_map (or read fb->owner_va out-of-band) to
 *     learn the new VA.
 *
 * Visible from a MAPPER's PoV (clients sharing the FB):
 *   - Their old va continues to map to the OLD pages until they
 *     next call sys_win_fb_map.  No translation-fault window.
 *   - On their next sys_win_fb_map call they get a fresh VA
 *     against the new pages; the old VA is uninstalled at that
 *     point.  This is the "ack" in the lazy-unmap protocol.
 *
 * Memory: stale generations are bounded by WIN_FB_MAX_STALE_GEN
 * per FB.  If a mapper lags more than that many resizes behind,
 * the oldest stale generation is force-uninstalled and the
 * laggard mapper translation-faults on its next FB access (same
 * failure mode as the pre-fix behaviour, just bounded).
 *
 * Failure modes (-EINVAL bad dims; -ENOENT bad id; -EPERM not
 * owner; -ENOMEM new pages or AS install failed) leave the FB
 * UNCHANGED: old pages, old mappings, old dims all intact.
 * This makes resize_apply in wsd safe to call without a
 * recovery path. */
long sys_win_fb_resize(long id_arg, long new_w_arg, long new_h_arg)
{
    struct thread *t = thread_current();
    if (!t || !t->as) return -EFAULT;

    uint32_t id    = (uint32_t)id_arg;
    uint32_t new_w = (uint32_t)new_w_arg;
    uint32_t new_h = (uint32_t)new_h_arg;
    if (id == 0)                          return -EINVAL;
    if (new_w == 0 || new_h == 0)         return -EINVAL;
    if (new_w > WIN_FB_MAX_W ||
        new_h > WIN_FB_MAX_H)             return -EINVAL;

    struct win_fb *fb = find_by_id(id);
    if (!fb)                              return -ENOENT;
    /* Ownership: any thread in the owning ADDRESS SPACE may
     * resize.  Using t->id alone is wrong because wsd has
     * multiple threads (accept loop, input poller, ...)
     * with distinct tids -- the FB was alloc'd on one and
     * resize is invoked from another.  AS equality is the
     * right notion of "same process".  (Same trap doesn't
     * fire for sys_win_fb_free today only because destroy
     * happens to run on the same thread as alloc; that
     * check is left alone to avoid changing unrelated
     * behaviour in this chapter.) */
    if (!fb->owner_as || fb->owner_as != t->as) return -EPERM;

    /* No-op (still nukes mappings? -- no: same dims = same
     * pages = same VAs, leave it alone).  Caller is wsd which
     * already early-outs on identical dims, but defence is
     * cheap. */
    if (new_w == fb->w && new_h == fb->h) return 0;

    uint64_t new_bytes = (uint64_t)new_w * (uint64_t)new_h * 4u;
    uint64_t new_pages = (new_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Phase 1: allocate the new backing.  Done first so a
     * shortage of free pages leaves the FB untouched. */
    uint64_t *new_pa = (uint64_t *)kmalloc((size_t)new_pages
                                            * sizeof(uint64_t));
    if (!new_pa) return -ENOMEM;
    for (uint64_t i = 0; i < new_pages; i++) new_pa[i] = 0;
    for (uint64_t i = 0; i < new_pages; i++) {
        new_pa[i] = pmem_alloc_page();
        if (!new_pa[i]) {
            for (uint64_t k = 0; k < i; k++) pmem_free_page(new_pa[k]);
            kfree(new_pa);
            return -ENOMEM;
        }
        uint8_t *p = (uint8_t *)(uintptr_t)new_pa[i];
        for (uint32_t k = 0; k < PAGE_SIZE; k++) p[k] = 0;
    }

    /* Phase 2: copy the surviving rect (top-left of old goes
     * to top-left of new).  Both are flat BGRA arrays; iterate
     * row-by-row so we don't depend on contiguous physical
     * memory.  Going via PA->VA direct-map relies on the
     * higher-half identity mapping that pmem hands out. */
    uint32_t copy_w = (fb->w < new_w) ? fb->w : new_w;
    uint32_t copy_h = (fb->h < new_h) ? fb->h : new_h;
    uint32_t new_stride = new_w * 4u;
    if (copy_w > 0 && copy_h > 0) {
        for (uint32_t row = 0; row < copy_h; row++) {
            /* Source row: byte offset row * old_stride.
             * Walk page-by-page within the row.  Old buffer
             * may straddle a page boundary; we handle that
             * by recomputing the page index on each byte. */
            for (uint32_t col = 0; col < copy_w; col++) {
                uint64_t src_off = (uint64_t)row * fb->stride
                                 + (uint64_t)col * 4u;
                uint64_t dst_off = (uint64_t)row * new_stride
                                 + (uint64_t)col * 4u;
                uint64_t src_page = src_off / PAGE_SIZE;
                uint64_t src_in   = src_off % PAGE_SIZE;
                uint64_t dst_page = dst_off / PAGE_SIZE;
                uint64_t dst_in   = dst_off % PAGE_SIZE;
                if (src_page >= fb->n_pages) break;
                if (dst_page >= new_pages)   break;
                uint32_t *src_px = (uint32_t *)(uintptr_t)
                    (fb->pages[src_page] + src_in);
                uint32_t *dst_px = (uint32_t *)(uintptr_t)
                    (new_pa[dst_page] + dst_in);
                *dst_px = *src_px;
            }
        }
    }

    /* Phase 3 (chapter 108e follow-up #3): lazy-unmap mappers.
     *
     * Instead of synchronously uninstalling each mapper's VA against
     * the old pages \u2014 which would translation-fault the mapper if it
     * happened to be mid-render \u2014 we MOVE the old pages into a new
     * stale generation and tag every still-current mapping with that
     * gen's index.  Each mapping's VA continues to address the old
     * pages until the mapping's AS next calls sys_win_fb_map (which
     * the client makes from its GUI_EVENT_RESIZE handler via
     * wm_window_remap_fb \u2014 see userspace/libgui/wmclient.c).  The
     * stale generation is freed when the last mapper acks.
     *
     * The OWNER is uninstalled and reinstalled immediately (Phase 4
     * below): owner is wsd, which is the caller of this syscall and
     * therefore not rendering against the FB right now.
     *
     * Edge cases:
     *   - Mappings ALREADY stale at some older gen: leave alone.
     *     Their ref on the older gen is preserved; their va still
     *     points at THAT gen's pages.
     *   - Mappings with va == 0 (left in_use=1 by a prior
     *     stale_gen_alloc force-recycle): not made stale here;
     *     they'll install fresh on next map call.
     *   - No mappings need stale: the old pages have no live
     *     external mapping, free them directly. */
    uint64_t *old_pages   = fb->pages;
    uint32_t  old_n_pages = fb->n_pages;
    uint64_t  old_owner_va = fb->owner_va;

    uint32_t need_stale = 0;
    for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
        struct win_fb_mapping *m = &fb->mappings[i];
        if (m->in_use && m->stale_idx < 0 && m->va != 0) need_stale++;
    }

    int new_stale_idx = -1;
    if (need_stale > 0) {
        new_stale_idx = stale_gen_alloc(fb);
        /* stale_gen_alloc may force-recycle a slot, resetting some
         * mappings' stale_idx to -1 and va to 0.  Re-count under the
         * "va != 0" rule so we don't try to keep dead VAs alive. */
        need_stale = 0;
        for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
            struct win_fb_mapping *m = &fb->mappings[i];
            if (m->in_use && m->stale_idx < 0 && m->va != 0) need_stale++;
        }
        if (need_stale == 0) new_stale_idx = -1;
    }

    /* Uninstall the owner from old pages.  Reinstall happens after
     * fb->pages is swapped in Phase 4. */
    if (fb->owner_as && old_n_pages > 0 && old_owner_va != 0) {
        (void)address_space_uninstall_wm_window(fb->owner_as,
                                                old_owner_va,
                                                old_n_pages);
    }

    /* Phase 4: swap in new pages + park or free the old ones. */
    if (new_stale_idx >= 0) {
        fb->stale[new_stale_idx].in_use    = 1;
        fb->stale[new_stale_idx].pages     = old_pages;
        fb->stale[new_stale_idx].n_pages   = old_n_pages;
        fb->stale[new_stale_idx].ref_count = need_stale;
        for (uint32_t i = 0; i < WIN_FB_MAX_MAPPINGS; i++) {
            struct win_fb_mapping *m = &fb->mappings[i];
            if (m->in_use && m->stale_idx < 0 && m->va != 0) {
                m->stale_idx = new_stale_idx;
            }
        }
    } else if (old_pages) {
        /* No live mappers to keep happy \u2014 the old pages are
         * unreferenced now that the owner has been uninstalled.
         * Free directly (don't use free_backing_pages because we've
         * already detached old_pages from fb). */
        for (uint32_t i = 0; i < old_n_pages; i++) {
            if (old_pages[i]) pmem_free_page(old_pages[i]);
        }
        kfree(old_pages);
    }
    fb->w       = new_w;
    fb->h       = new_h;
    fb->stride  = new_stride;
    fb->size    = (uint32_t)(new_pages * PAGE_SIZE);
    fb->n_pages = (uint32_t)new_pages;
    fb->pages   = new_pa;

    /* Phase 5: re-install for the owner so wsd can resume
     * compose-blitting without a second syscall.  If THIS
     * fails (extremely unlikely -- we just had enough VA
     * headroom moments ago), the FB ends up with no owner
     * mapping and owner_va = 0; wsd's compose loop will
     * skip it (fb_va == 0 check) until owner explicitly
     * remaps. */
    uint64_t new_owner_va = 0;
    if (address_space_install_wm_window(fb->owner_as, fb->pages,
                                        fb->n_pages,
                                        &new_owner_va) != 0) {
        serial_puts("[win_fb] resize: owner reinstall FAILED id=");
        serial_puthex(fb->id);
        serial_puts("\n");
        fb->owner_va = 0;
        /* Don't free anything else; FB stays valid for
         * future map_fb calls.  Return 0 anyway (resize
         * succeeded in geom terms; owner just has to call
         * map_fb to recover its VA). */
        return 0;
    }
    fb->owner_va = new_owner_va;

    serial_puts("[win_fb] resize id=");
    serial_puthex(fb->id);
    serial_puts(" -> wxh=");
    serial_puthex(fb->w);
    serial_puts("x");
    serial_puthex(fb->h);
    serial_puts(" owner_va=");
    serial_puthex(new_owner_va);
    serial_puts("\n");
    return 0;
}

/* Exit-time hook ------------------------------------------------- */

void win_fb_release_pid(uint64_t pid)
{
    struct thread *t = thread_current();
    struct address_space *my_as = t ? t->as : NULL;

    for (uint32_t i = 0; i < WIN_FB_TABLE_SIZE; i++) {
        struct win_fb *fb = &g_table[i];
        if (!fb->in_use) continue;

        if (fb->owner_pid == pid) {
            /* Owner is exiting.  Tear down the whole FB but
             * skip uninstall against `my_as` (our own AS is
             * about to be destroyed; touching it now would
             * race with the destroy walk). */
            serial_puts("[win_fb] owner exit id=");
            serial_puthex(fb->id);
            serial_puts(" pid=");
            serial_puthex(pid);
            serial_puts("\n");
            destroy_fb(fb, my_as);
            continue;
        }

        /* Otherwise look for a mapping owned by this AS and
         * just clear the tracking slot — the AS-destroy walk
         * will skip DESC_SW_WM_WINDOW descriptors and leave
         * the page contents untouched (we still own them via
         * fb->pages, the owner will free them on its own
         * exit or via SYS_WIN_FB_FREE).
         *
         * chapter 108e follow-up #3: if the mapping was stale
         * (lazy-kept across a resize), drop its ref on the
         * stale generation \u2014 the gen's pages get freed when
         * the last referent goes.  Without this an exiting
         * mid-resize mapper would leak the stale gen forever. */
        for (uint32_t j = 0; j < WIN_FB_MAX_MAPPINGS; j++) {
            struct win_fb_mapping *m = &fb->mappings[j];
            if (m->in_use && m->as == my_as) {
                serial_puts("[win_fb] mapper exit id=");
                serial_puthex(fb->id);
                serial_puts(" pid=");
                serial_puthex(pid);
                serial_puts("\n");
                if (m->stale_idx >= 0) stale_gen_unref(fb, m->stale_idx);
                m->in_use    = 0;
                m->as        = NULL;
                m->va        = 0;
                m->pid       = 0;
                m->stale_idx = -1;
            }
        }
    }
}
