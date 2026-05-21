/*
 * kernel/arch/address_space.c — per-process page table management.
 *
 * Allocates/frees AArch64 4 KiB-granule, 39-bit VA L1/L2/L3 page
 * tables.  The boot L1 (page_tables.c::l1_pgtable) stays as the
 * kernel-thread address space; per-process L1s are built fresh
 * with kernel slots inherited from boot.
 *
 * Slot 2 (0x80000000-0xC0000000) is the user range.  Slot 2 in
 * the per-process L1 is a TABLE descriptor pointing at a fresh
 * per-process L2.  Each L2 entry that gets used becomes a TABLE
 * descriptor pointing at a fresh L3.  Each L3 entry is a 4 KiB
 * page descriptor mapping one user page.
 *
 * No ASIDs.  Every activate does dsb ish + tlbi vmalle1 + dsb ish
 * + isb.  Slow but correct.
 */
#include "address_space.h"
#include "../core/serial.h"
#include "../core/pmem.h"
#include "../core/pmem_refcount.h"
#include "../core/heap.h"
#include "../core/page_cache.h"
#include "../core/vfs.h"
#include "../core/mmap_uapi.h"
#include "atomic.h"

#include <stdint.h>
#include <stddef.h>

/* The boot L1 is defined in page_tables.c.  Used as the source of
 * inherited kernel slots, and as the active table when no AS is
 * installed (kernel threads). */
extern uint64_t l1_pgtable[512];

#define PTES_PER_TABLE  512
#define L1_SHIFT        30      /* slot covers 1 GiB */
#define L2_SHIFT        21      /* slot covers 2 MiB */
#define L3_SHIFT        12      /* slot covers 4 KiB */
#define L1_INDEX(va)    (((va) >> L1_SHIFT) & 0x1FF)
#define L2_INDEX(va)    (((va) >> L2_SHIFT) & 0x1FF)
#define L3_INDEX(va)    (((va) >> L3_SHIFT) & 0x1FF)

/* Descriptor fields. */
#define DESC_VALID      (1ULL << 0)
#define DESC_TABLE      (1ULL << 1)     /* at L1/L2: table type   */
#define DESC_PAGE       (1ULL << 1)     /* at L3: page type       */
#define DESC_AF         (1ULL << 10)
#define DESC_SH_INNER   (3ULL << 8)
#define DESC_AP_RW_EL0  (1ULL << 6)     /* AP[2:1]=01: RW EL1+EL0 */
#define DESC_AP_RO_EL0  (3ULL << 6)     /* AP[2:1]=11: RO EL1+EL0 */
#define DESC_PXN        (1ULL << 53)    /* kernel never executes  */
#define DESC_UXN        (1ULL << 54)    /* user never executes    */
/* Software-defined bit reserved for OS use.  Bits [58:55] are
 * "reserved for software" in stage-1 descriptors per ARM ARM
 * D5.4.5; we claim bit 55 to mark "this RO mapping is COW: on
 * write fault, allocate a fresh page and copy."  The bit is
 * preserved verbatim by the MMU walk \u2014 only the OS reads it. */
#define DESC_SW_COW     (1ULL << 55)
/* Chapter 90 \u2014 also software-defined.  Bit 56 marks "this page
 * is owned by the page cache, not by this AS."  AS teardown
 * sees the bit and calls page_cache_release(pa) instead of
 * pmem_dec_and_free(pa), so the cache keeps the page until the
 * cache itself decides to evict it.  Fork (address_space_clone_cow)
 * also checks this bit and SKIPS page-cache mappings instead of
 * sharing them \u2014 chapter 90 floor: mmaps do not survive fork.
 * The child can re-mmap if it wants the same content. */
#define DESC_SW_PAGECACHE (1ULL << 56)
/* Chapter 108a \u2014 bit 58 marks "this page is a WM window pixel
 * buffer mapped into userspace; the WM owns it, the AS does not."
 * Teardown SKIPS pmem-free entirely (the WM holds the pages until
 * the window is destroyed); fork SKIPS the mapping (a child
 * starts with no inherited window).  Symbolic name is also
 * exposed via address_space.h so the WM can sanity-check the
 * descriptors it installs. */
#ifndef DESC_SW_WM_WINDOW
#define DESC_SW_WM_WINDOW (1ULL << 58)
#endif
/* Chapter 101 — software-defined bit 57 marks "this is an
 * intentional guard page (invalid, no backing)."  The MMU
 * ignores software bits when DESC_VALID is clear, so the entry
 * faults normally; the data-abort handler then reads the bit
 * back via address_space_lookup_pte to distinguish a guard
 * fault (= friendly "user stack overflow" diagnostic + thread
 * exit) from a generic translation fault (= unmapped VA bug).
 * Defined in address_space.h so the syscall layer can see it. */
#define ATTR_NORMAL     (0ULL << 2)     /* MAIR slot 0            */

/* The user range is slot 64 of the L1 (covers 64 GiB..65 GiB).
 * Far above any plausible DRAM, so pmem PAs and user VAs never
 * conflict.  See address_space.h header comment for the full
 * rationale. */
#define USER_L1_SLOT    64

uint64_t address_space_boot_l1_pa(void)
{
    return (uint64_t)(uintptr_t)l1_pgtable;   /* identity-mapped */
}

struct address_space *address_space_create(void)
{
    struct address_space *as = (struct address_space *)kmalloc(sizeof(*as));
    if (!as) return NULL;

    /* Allocate a fresh L1 page (zeroed by pmem_alloc_page). */
    uint64_t l1_pa = pmem_alloc_page();
    if (!l1_pa) { kfree(as); return NULL; }
    /* Allocate the per-process L2 for slot 2 (zeroed). */
    uint64_t l2_pa = pmem_alloc_page();
    if (!l2_pa) { kfree(as); return NULL; }

    as->l1_pa = l1_pa;
    as->l1_va = (uint64_t *)(uintptr_t)l1_pa;
    as->l2_pa = l2_pa;
    as->l2_va = (uint64_t *)(uintptr_t)l2_pa;
    as->user_pages_alloced = 0;
    /* Heap starts empty: brk == base means "no pages mapped yet". */
    as->heap_brk = USER_HEAP_BASE;
    /* Chapter 90 — mmap region starts empty too. */
    as->vmas     = NULL;
    as->mmap_brk = USER_MMAP_BASE;
    /* chapter 108e follow-up #4 — WM-window VA freelist starts
     * empty.  First install hits the bump-pointer path; only
     * after the first uninstall does the freelist get any
     * candidate ranges. */
    as->wm_freelist = NULL;
    /* Chapter 91 — single owner at creation.  SYS_CLONE bumps
     * this when it spawns another thread into the same AS. */
    as->refcount = 1;

    /* Inherit kernel slots from boot L1.  Copy everything EXCEPT
     * slot 2 (which is the user range we own).  Slot 0 (devices),
     * slot 1 (kernel image), and slots 3..N (kernel heap, pmem)
     * all come along for free. */
    for (int i = 0; i < PTES_PER_TABLE; i++) {
        if (i == USER_L1_SLOT) continue;
        as->l1_va[i] = l1_pgtable[i];
    }

    /* Install per-process L2 in slot 2.  TABLE descriptor at L1
     * just needs the L2 PA + (DESC_VALID | DESC_TABLE). */
    as->l1_va[USER_L1_SLOT] = (l2_pa & ~0xFFFULL) | DESC_VALID | DESC_TABLE;

    return as;
}

/* Walk the per-process L2 and L3 tables freeing every page they
 * own (the L3 tables themselves AND the user data pages they
 * point at).  Then free the L2 and the L1 themselves.
 *
 * User-data pages go through pmem_dec_and_free so COW-shared
 * pages are not actually freed until every sharer's AS is
 * destroyed.  L3 / L2 / L1 page-table pages are *never* shared
 * across address spaces, so they get pmem_free_page directly.
 *
 * Chapter 90 \u2014 page-cache-owned pages (DESC_SW_PAGECACHE) take
 * a different path: page_cache_release(pa) instead of
 * pmem_dec_and_free.  The cache keeps the page so the next
 * mmap of the same content is a hit; the cache itself decides
 * when to evict. */
static void teardown_user_range(struct address_space *as)
{
    for (int i = 0; i < PTES_PER_TABLE; i++) {
        uint64_t l2_ent = as->l2_va[i];
        if ((l2_ent & DESC_VALID) == 0) continue;
        if ((l2_ent & DESC_TABLE) == 0) continue;   /* a 2 MiB block, not used yet */

        uint64_t l3_pa = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
        uint64_t *l3   = (uint64_t *)(uintptr_t)l3_pa;
        for (int j = 0; j < PTES_PER_TABLE; j++) {
            uint64_t l3_ent = l3[j];
            if ((l3_ent & DESC_VALID) == 0) continue;
            uint64_t pg_pa = l3_ent & ~0xFFFULL & ((1ULL << 48) - 1);
            if (l3_ent & DESC_SW_WM_WINDOW) {
                /* Chapter 108a — the WM owns this page.  Drop the
                 * descriptor but do NOT pmem-free the backing
                 * frame; the window keeps it until destroy time
                 * (or sys_gui_unmap_window).  AS teardown is the
                 * normal path here when a process holding a
                 * mapped window exits: wm_destroy_owner will run
                 * shortly after and reclaim the pages. */
                (void)pg_pa;
            } else if (l3_ent & DESC_SW_PAGECACHE) {
                page_cache_release(pg_pa);
            } else {
                pmem_dec_and_free(pg_pa);
            }
        }
        pmem_free_page(l3_pa);
    }
}

/* Chapter 90 — free the vma list. Page-table cleanup is handled
 * separately by teardown_user_range; this just releases the
 * vma struct themselves. */
static void teardown_vmas(struct address_space *as)
{
    struct vma *v = as->vmas;
    while (v) {
        struct vma *n = v->next;
        kfree(v);
        v = n;
    }
    as->vmas = NULL;
}

/* chapter 108e follow-up #4 — free the WM-window VA freelist.
 * Page-table cleanup is already handled by teardown_user_range
 * (the freelist entries describe ranges that have NO L3 entries
 * — uninstall already zeroed them).  This just releases the
 * tracking nodes themselves. */
static void teardown_wm_freelist(struct address_space *as)
{
    struct wm_va_range *r = as->wm_freelist;
    while (r) {
        struct wm_va_range *n = r->next;
        kfree(r);
        r = n;
    }
    as->wm_freelist = NULL;
}

void address_space_destroy(struct address_space *as)
{
    if (!as) return;
    /* Chapter 91 — refcount-aware teardown.  Drop OUR reference
     * first; if other threads (clone siblings) are still using
     * the AS, return without touching the page tables.  Only
     * the last reference does the actual teardown. */
    if (atomic_sub_return32(&as->refcount, 1) > 0) return;
    teardown_user_range(as);
    teardown_vmas(as);
    teardown_wm_freelist(as);
    pmem_free_page(as->l2_pa);
    pmem_free_page(as->l1_pa);
    kfree(as);
}

/* Chapter 91 — bump the refcount so a new thread can share this
 * AS.  Caller must arrange exactly one matching
 * address_space_destroy() at the new thread's exit time. */
void address_space_share(struct address_space *as)
{
    if (!as) return;
    atomic_add_return32(&as->refcount, 1);
}

/* Get-or-create the L3 table for the L2 slot containing `va`.
 * Returns the L3 table's identity-mapped VA pointer, or NULL on
 * OOM. */
static uint64_t *l3_for(struct address_space *as, uint64_t va)
{
    uint64_t l2i = L2_INDEX(va);
    uint64_t l2_ent = as->l2_va[l2i];
    if (l2_ent & DESC_VALID) {
        uint64_t l3_pa = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
        return (uint64_t *)(uintptr_t)l3_pa;
    }
    /* Allocate a fresh L3 page. */
    uint64_t l3_pa = pmem_alloc_page();
    if (!l3_pa) return NULL;
    as->l2_va[l2i] = (l3_pa & ~0xFFFULL) | DESC_VALID | DESC_TABLE;
    return (uint64_t *)(uintptr_t)l3_pa;
}

int address_space_map(struct address_space *as,
                      uint64_t va, uint64_t pa,
                      uint64_t pages, int writable, int executable)
{
    if (!as || !pages) return -1;
    if (va  & 0xFFFULL) return -1;
    if (pa  & 0xFFFULL) return -1;
    if (va  < USER_VA_BASE || va + pages * PAGE_SIZE > USER_VA_END) return -1;

    uint64_t base = ATTR_NORMAL | DESC_AF | DESC_SH_INNER |
                    (writable ? DESC_AP_RW_EL0 : DESC_AP_RO_EL0) |
                    DESC_PXN |                          /* never EL1-exec */
                    (executable ? 0 : DESC_UXN) |
                    DESC_VALID | DESC_PAGE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t this_va = va + i * PAGE_SIZE;
        uint64_t this_pa = pa + i * PAGE_SIZE;
        uint64_t *l3 = l3_for(as, this_va);
        if (!l3) return -1;
        l3[L3_INDEX(this_va)] = (this_pa & ~0xFFFULL) | base;
        as->user_pages_alloced++;
    }

    /* Page tables modified — make the writes visible before the
     * next walk.  We don't TLBI here because the new mappings are
     * for VAs that were previously unmapped (no stale TLB entry
     * exists).  TLBI happens on activate(). */
    __asm__ volatile("dsb ishst" ::: "memory");
    return 0;
}

/* Chapter 101 — install a one-page guard at `va`.
 *
 * The L3 entry is written *invalid* (DESC_VALID=0) but tagged
 * with DESC_SW_GUARD.  Any load/store to the page produces a
 * translation fault routed through svc_dispatch; the fault
 * handler reads the descriptor back, sees the SW bit, and turns
 * the fault into a friendly stack-overflow report instead of a
 * generic register dump.
 *
 * No physical page is consumed.  We do allocate the covering
 * L3 page if it doesn't already exist, but the common case
 * (guard sits one page below the user stack, which has just
 * been mapped) hits an existing L3 and costs nothing.
 *
 * Note on the descriptor format: when DESC_VALID is clear the
 * MMU treats the entry as "invalid descriptor" and reserves
 * bits [58:55] for software (ARM ARM D5.4.5).  We are free to
 * store whatever marker we like there. */
int address_space_install_guard(struct address_space *as, uint64_t va)
{
    if (!as) return -1;
    if ((va & 0xFFFULL) != 0) return -1;  /* must be page-aligned */
    uint64_t *l3 = l3_for(as, va);
    if (!l3) return -1;
    /* Tag with SW_GUARD only; DESC_VALID stays clear so the
     * MMU still faults.  Any previous descriptor at this slot
     * (there shouldn't be one, but be explicit) is overwritten. */
    l3[L3_INDEX(va)] = DESC_SW_GUARD;
    __asm__ volatile("dsb ishst" ::: "memory");
    return 0;
}

/* Chapter 101 — read the raw L3 descriptor that backs `va`, or
 * 0 if no L2/L3 covers it.  Used by the fault handler to read
 * software bits (DESC_SW_GUARD) on entries that are invalid
 * from the MMU's perspective. */
uint64_t address_space_lookup_pte(const struct address_space *as,
                                  uint64_t va)
{
    if (!as) return 0;
    uint64_t l2i = L2_INDEX(va);
    uint64_t l2_ent = as->l2_va[l2i];
    if ((l2_ent & DESC_VALID) == 0) return 0;
    if ((l2_ent & DESC_TABLE) == 0) return 0;
    uint64_t l3_pa = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
    return ((const uint64_t *)(uintptr_t)l3_pa)[L3_INDEX(va)];
}

void address_space_activate(const struct address_space *as)
{
    uint64_t ttbr = as ? as->l1_pa : address_space_boot_l1_pa();
    __asm__ volatile(
        "msr ttbr0_el1, %0  \n"
        "isb                \n"
        "tlbi vmalle1       \n"
        "dsb ish            \n"
        "isb                \n"
        :: "r"(ttbr) : "memory");
}

/* Walk to the L3 entry for `va`.  Returns NULL if no L3 exists
 * yet (the page has never been mapped) or the entry is invalid. */
static uint64_t *l3_entry_lookup(struct address_space *as, uint64_t va)
{
    uint64_t l2i = L2_INDEX(va);
    uint64_t l2_ent = as->l2_va[l2i];
    if ((l2_ent & DESC_VALID) == 0) return NULL;
    if ((l2_ent & DESC_TABLE) == 0) return NULL;
    uint64_t l3_pa = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
    return &((uint64_t *)(uintptr_t)l3_pa)[L3_INDEX(va)];
}

int address_space_set_brk(struct address_space *as, uint64_t new_brk)
{
    if (!as) return -1;
    /* Page-align the request upward (caller may pass anything). */
    new_brk = (new_brk + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    if (new_brk < USER_HEAP_BASE) new_brk = USER_HEAP_BASE;
    if (new_brk > USER_HEAP_MAX)  return -1;

    if (new_brk > as->heap_brk) {
        /* Grow: map fresh pages between heap_brk and new_brk. */
        for (uint64_t va = as->heap_brk; va < new_brk; va += PAGE_SIZE) {
            uint64_t pa = pmem_alloc_page();
            if (!pa) return -1;
            /* Anonymous page: writable, non-executable. */
            if (address_space_map(as, va, pa, 1, /*write*/1, /*exec*/0) != 0) {
                pmem_free_page(pa);
                return -1;
            }
        }
    } else if (new_brk < as->heap_brk) {
        /* Shrink: unmap pages and return them to pmem (refcount-
         * aware: COW-shared pages are only physically freed once
         * the last sharer drops them). */
        for (uint64_t va = new_brk; va < as->heap_brk; va += PAGE_SIZE) {
            uint64_t *ent = l3_entry_lookup(as, va);
            if (!ent) continue;          /* never mapped, skip */
            uint64_t e = *ent;
            if ((e & DESC_VALID) == 0) continue;
            uint64_t pa = e & ~0xFFFULL & ((1ULL << 48) - 1);
            *ent = 0;                    /* invalidate descriptor */
            pmem_dec_and_free(pa);
            if (as->user_pages_alloced) as->user_pages_alloced--;
        }
        /* Page tables changed; flush stale TLB entries.  We're
         * (almost certainly) running in `as` right now, so a
         * full local invalidate is correct. */
        __asm__ volatile(
            "dsb ishst       \n"
            "tlbi vmalle1    \n"
            "dsb ish         \n"
            "isb             \n"
            ::: "memory");
    }

    as->heap_brk = new_brk;
    return 0;
}

/* Eager full-copy clone for chapter 73 (fork).  Walk src's user
 * range one L2 entry at a time; for each valid L3 entry, allocate
 * a fresh pmem page, memcpy 4 KiB of contents from the source PA
 * (identity-mapped via the boot L1) to the destination PA, then
 * map the new page into the child AS at the same VA preserving
 * AP / UXN bits.  On any OOM along the way we tear the partial
 * child down via address_space_destroy and return NULL.
 *
 * NOTE: PXN is always set (kernel can never execute user pages).
 * AP bit (RW vs RO) and UXN bit (executable from EL0?) are read
 * from the source descriptor.  All other Normal-WB attribute
 * bits come back via address_space_map's standard formula.
 *
 * The memcpy walks PAs as VAs, which is safe because slots
 * 2..N of the boot L1 identity-map DRAM and the child AS
 * inherits those slots — pmem PAs handed back to us are always
 * inside that range. */
struct address_space *address_space_clone(const struct address_space *src)
{
    if (!src) return NULL;

    struct address_space *dst = address_space_create();
    if (!dst) return NULL;

    for (int i = 0; i < PTES_PER_TABLE; i++) {
        uint64_t l2_ent = src->l2_va[i];
        if ((l2_ent & DESC_VALID) == 0) continue;
        if ((l2_ent & DESC_TABLE) == 0) continue;

        uint64_t src_l3_pa = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
        uint64_t *src_l3   = (uint64_t *)(uintptr_t)src_l3_pa;

        for (int j = 0; j < PTES_PER_TABLE; j++) {
            uint64_t src_ent = src_l3[j];
            if ((src_ent & DESC_VALID) == 0) {
                /* Chapter 101 — preserve guard pages.  These
                 * carry DESC_SW_GUARD but no DESC_VALID; if we
                 * don't re-install them in the child, the
                 * child's stack overflow will fall through to
                 * the generic "non-SVC sync exception" message
                 * instead of the friendly one. */
                if (src_ent & DESC_SW_GUARD) {
                    uint64_t va = USER_VA_BASE
                                + ((uint64_t)i << L2_SHIFT)
                                + ((uint64_t)j << L3_SHIFT);
                    if (address_space_install_guard(dst, va) != 0) {
                        address_space_destroy(dst);
                        return NULL;
                    }
                }
                continue;
            }

            /* Chapter 90: skip page-cache pages (mmaps don't
             * survive eager clone either). */
            if (src_ent & DESC_SW_PAGECACHE) continue;
            /* Chapter 108a: WM window mappings are also skipped
             * by eager clone, for the same reason
             * address_space_clone_cow skips them \u2014 the WM owns
             * the pages, the child should re-request its own
             * window if it wants one. */
            if (src_ent & DESC_SW_WM_WINDOW) continue;

            uint64_t va     = USER_VA_BASE
                            + ((uint64_t)i << L2_SHIFT)
                            + ((uint64_t)j << L3_SHIFT);
            uint64_t src_pa = src_ent & ~0xFFFULL & ((1ULL << 48) - 1);

            /* Decode source permissions.  AP[2:1] field at bit 6
             * is either DESC_AP_RW_EL0 (01) or DESC_AP_RO_EL0 (11);
             * the writable bit is "AP[2]==0".  UXN at bit 54 means
             * "no exec from EL0"; executable iff UXN clear. */
            int writable   = ((src_ent & (3ULL << 6)) == DESC_AP_RW_EL0);
            int executable = ((src_ent & DESC_UXN) == 0);

            uint64_t dst_pa = pmem_alloc_page();
            if (!dst_pa) { address_space_destroy(dst); return NULL; }

            /* memcpy via identity mapping. */
            const uint8_t *s = (const uint8_t *)(uintptr_t)src_pa;
            uint8_t       *d = (uint8_t       *)(uintptr_t)dst_pa;
            for (uint64_t b = 0; b < PAGE_SIZE; b++) d[b] = s[b];

            if (address_space_map(dst, va, dst_pa, 1,
                                  writable, executable) != 0) {
                pmem_free_page(dst_pa);
                address_space_destroy(dst);
                return NULL;
            }
        }
    }

    /* Heap brk survives verbatim.  Stack pages were just cloned
     * above (they live above USER_HEAP_MAX in the user range). */
    dst->heap_brk = src->heap_brk;
    return dst;
}

/* Chapter 75 \u2014 copy-on-write clone.
 *
 * Walks src exactly the same way as address_space_clone, but
 * instead of allocating + memcpying every page we just SHARE
 * the physical page between parent and child.  Writable pages
 * get downgraded to RO in BOTH address spaces and tagged with
 * DESC_SW_COW; on the next write fault, address_space_handle_cow_fault
 * copies the page and restores RW for the faulter.
 *
 * Read-only pages (program text, RO data) are shared too, but
 * never need to be unshared, so they don't get the COW marker.
 *
 * The refcount table is bumped once per shared page \u2014 not once
 * per AS \u2014 because the *page* now has two holders (parent + child).
 *
 * After the walk we issue a TLB invalidate.  This matters
 * because we just downgraded src's writable pages to RO; if src
 * later writes one of those VAs and the TLB still has the stale
 * RW entry the write would silently succeed and skip the COW
 * resolution.  Single-CPU + no-ASID, so a local tlbi vmalle1
 * is the right hammer. */
struct address_space *address_space_clone_cow(struct address_space *src)
{
    if (!src) return NULL;

    struct address_space *dst = address_space_create();
    if (!dst) return NULL;

    int touched_src = 0;

    for (int i = 0; i < PTES_PER_TABLE; i++) {
        uint64_t l2_ent = src->l2_va[i];
        if ((l2_ent & DESC_VALID) == 0) continue;
        if ((l2_ent & DESC_TABLE) == 0) continue;

        uint64_t src_l3_pa = l2_ent & ~0xFFFULL & ((1ULL << 48) - 1);
        uint64_t *src_l3   = (uint64_t *)(uintptr_t)src_l3_pa;

        for (int j = 0; j < PTES_PER_TABLE; j++) {
            uint64_t src_ent = src_l3[j];
            if ((src_ent & DESC_VALID) == 0) {
                /* Chapter 101 — preserve guard pages across
                 * COW fork too.  Same reasoning as
                 * address_space_clone: a forked child whose
                 * stack overflows should also get the friendly
                 * diagnostic. */
                if (src_ent & DESC_SW_GUARD) {
                    uint64_t va = USER_VA_BASE
                                + ((uint64_t)i << L2_SHIFT)
                                + ((uint64_t)j << L3_SHIFT);
                    if (address_space_install_guard(dst, va) != 0) {
                        address_space_destroy(dst);
                        if (touched_src) {
                            __asm__ volatile(
                                "dsb ishst        \n"
                                "tlbi vmalle1     \n"
                                "dsb ish          \n"
                                "isb              \n"
                                ::: "memory");
                        }
                        return NULL;
                    }
                }
                continue;
            }

            /* Chapter 90 floor: mmaps do not survive fork.  Page-
             * cache-owned pages are skipped entirely \u2014 the child
             * will fault on these VAs and (since we're not
             * copying the parent's vma list either) get killed.
             * The parent's mapping is untouched.  Document this
             * limit in chapter 90; future chapters will copy
             * vmas + bump page_cache refcounts. */
            if (src_ent & DESC_SW_PAGECACHE) continue;
            /* Chapter 108a: WM window mappings are also skipped.
             * Same reasoning: they reference physical pages owned
             * by the kernel-side WM, not by this AS.  A child that
             * wanted its own window would gui_create_window +
             * gui_window_fb to get a fresh mapping; inheriting
             * the parent's would be wrong (two processes drawing
             * into the same physical page with no synchronisation)
             * AND would break the WM's per-window ownership
             * accounting (which is keyed off owner_pid). */
            if (src_ent & DESC_SW_WM_WINDOW) continue;

            uint64_t va     = USER_VA_BASE
                            + ((uint64_t)i << L2_SHIFT)
                            + ((uint64_t)j << L3_SHIFT);
            uint64_t pa     = src_ent & ~0xFFFULL & ((1ULL << 48) - 1);

            /* Decode the source perm bits exactly so we can
             * restore them at fault time.
             *
             * "Writable" here means LOGICALLY writable from the
             * userspace point of view: a page that was RW in
             * the source AS, OR a page already in the COW state
             * (RO + DESC_SW_COW) from a previous fork.  Treating
             * already-COW pages as writable is what makes
             * grandchildren of a long fork chain still get the
             * lazy-copy treatment instead of losing it on the
             * second fork. */
            int rw_now     = ((src_ent & (3ULL << 6)) == DESC_AP_RW_EL0);
            int already_cow= ((src_ent & DESC_SW_COW) != 0);
            int writable   = rw_now || already_cow;
            int executable = ((src_ent & DESC_UXN) == 0);

            /* Build the SHARED descriptor.  RW pages are
             * downgraded to RO and marked DESC_SW_COW; RO pages
             * stay RO and don't need the marker.  PXN, UXN, AF,
             * SH, attr all carry over. */
            uint64_t desc = ATTR_NORMAL | DESC_AF | DESC_SH_INNER |
                            DESC_AP_RO_EL0 |
                            DESC_PXN |
                            (executable ? 0 : DESC_UXN) |
                            DESC_VALID | DESC_PAGE |
                            (pa & ~0xFFFULL);
            if (writable) desc |= DESC_SW_COW;

            /* Install in dst at the same VA. */
            uint64_t *dst_l3 = l3_for(dst, va);
            if (!dst_l3) {
                /* OOM allocating an L3 in the child.  We've
                 * already mutated src for the pages we got
                 * through, so we have to leave src's COW marks
                 * in place \u2014 they're harmless on their own
                 * (the fault handler unshares them on the next
                 * write).  Just tear down dst and return. */
                address_space_destroy(dst);
                if (touched_src) {
                    __asm__ volatile(
                        "dsb ishst       \n"
                        "tlbi vmalle1    \n"
                        "dsb ish         \n"
                        "isb             \n"
                        ::: "memory");
                }
                return NULL;
            }
            dst_l3[L3_INDEX(va)] = desc;
            dst->user_pages_alloced++;

            /* Mutate src's descriptor in place. */
            src_l3[j] = desc;
            touched_src = 1;

            /* Bump refcount: the page now has TWO holders. */
            pmem_refcount_share(pa);
        }
    }

    dst->heap_brk = src->heap_brk;

    /* TLBI: src lost RW on every shared page; if any of them
     * was cached in TLB as RW the next write would slip past
     * the COW handler. */
    __asm__ volatile(
        "dsb ishst       \n"
        "tlbi vmalle1    \n"
        "dsb ish         \n"
        "isb             \n"
        ::: "memory");

    return dst;
}

/* COW fault handler.  Called from svc_entry's data-abort branch
 * when an EL0 write hits a page mapped RO with DESC_SW_COW set.
 *
 * Returns 0 if the fault was a real COW fault that we resolved
 * (caller should eret straight back to the faulting instruction).
 * Returns -1 if the fault was NOT a COW fault \u2014 caller should
 * fall through to the normal "kill the thread" path.
 *
 * Algorithm:
 *   1. Walk current AS's L2/L3 to find the descriptor for fault_va.
 *   2. If not present / not RO / not DESC_SW_COW \u2014 return -1.
 *   3. If the page's refcount is already 1 (we are the sole
 *      remaining holder), just drop the COW bit and flip AP back
 *      to RW.  No copy needed \u2014 the other sharer already
 *      unshared this page some time ago.
 *   4. Otherwise allocate a fresh page, memcpy old\u2192new (4 KiB
 *      via identity map), update the descriptor to point at the
 *      new PA with RW perms and no COW marker, and dec the old
 *      page's refcount.
 *   5. Invalidate the TLB entry for fault_va so the next access
 *      sees the fresh perms. */
int address_space_handle_cow_fault(struct address_space *as,
                                   uint64_t fault_va)
{
    if (!as) return -1;
    fault_va &= ~(uint64_t)(PAGE_SIZE - 1);
    if (fault_va < USER_VA_BASE || fault_va >= USER_VA_END) return -1;

    uint64_t *ent = l3_entry_lookup(as, fault_va);
    if (!ent) return -1;
    uint64_t e = *ent;
    if ((e & DESC_VALID) == 0) return -1;
    if ((e & DESC_SW_COW) == 0) return -1;
    /* Sanity: a COW page is always RO; if it's RW the fault was
     * something else (real write protection violation never
     * happens on RW). */
    if ((e & (3ULL << 6)) != DESC_AP_RO_EL0) return -1;

    uint64_t old_pa     = e & ~0xFFFULL & ((1ULL << 48) - 1);
    int      executable = ((e & DESC_UXN) == 0);

    /* Strip the per-page bits we'll re-derive; keep the rest. */
    uint64_t base_attrs = ATTR_NORMAL | DESC_AF | DESC_SH_INNER |
                          DESC_PXN |
                          (executable ? 0 : DESC_UXN) |
                          DESC_VALID | DESC_PAGE;

    uint16_t rc = pmem_refcount_get(old_pa);

    if (rc <= 1) {
        /* Sole holder \u2014 just upgrade to RW in place.  Refcount
         * was 0 ("never shared") OR 1 ("we're the last sharer
         * and the other one already unshared").  Either way no
         * copy is needed; if rc==1 we also drop our last share
         * by dec'ing it back to 0 so the page reverts to
         * "fully owned, untracked". */
        if (rc == 1) pmem_refcount_dec(old_pa);
        *ent = base_attrs | DESC_AP_RW_EL0 | (old_pa & ~0xFFFULL);
    } else {
        /* True share \u2014 allocate, copy, repoint. */
        uint64_t new_pa = pmem_alloc_page();
        if (!new_pa) return -1;     /* OOM \u2014 caller treats as fault */

        const uint8_t *s = (const uint8_t *)(uintptr_t)old_pa;
        uint8_t       *d = (uint8_t       *)(uintptr_t)new_pa;
        for (uint64_t b = 0; b < PAGE_SIZE; b++) d[b] = s[b];

        *ent = base_attrs | DESC_AP_RW_EL0 | (new_pa & ~0xFFFULL);

        /* We dropped our share of the old page. */
        pmem_refcount_dec(old_pa);
    }

    /* Make the new descriptor visible and flush the stale RO
     * TLB entry for this VA.  tlbi vaae1, va>>12 is the VA-only
     * (any-ASID) variant \u2014 fine for our no-ASID world. */
    __asm__ volatile(
        "dsb ishst              \n"
        "tlbi vaae1, %0         \n"
        "dsb ish                \n"
        "isb                    \n"
        :: "r"(fault_va >> 12) : "memory");

    return 0;
}

/* Proactively make the page containing `va` writable from the
 * kernel's perspective.  Used by copy_to_user-style helpers so
 * the kernel can write into a freshly-fork'd (still-COW-shared)
 * user buffer without itself triggering an EL1 data abort.
 *
 * AArch64 RO permissions (AP=11) apply to EL1 too, so a kernel
 * memcpy through a user VA that happens to point at a COW page
 * faults at EL1.  The fault has nowhere good to land (panic_entry
 * just dumps state and halts), so we resolve eagerly here.
 *
 * Returns 0 if the page is now writable, -1 otherwise (genuine
 * RO page like program text, or unmapped). */
int address_space_make_writable(struct address_space *as, uint64_t va)
{
    if (!as) return -1;
    va &= ~(uint64_t)(PAGE_SIZE - 1);
    if (va < USER_VA_BASE || va >= USER_VA_END) return -1;

    uint64_t *ent = l3_entry_lookup(as, va);
    if (!ent) return -1;
    uint64_t e = *ent;
    if ((e & DESC_VALID) == 0) return -1;

    /* Already writable? Nothing to do. */
    if ((e & (3ULL << 6)) == DESC_AP_RW_EL0) return 0;

    /* RO without COW marker = genuine read-only mapping (program
     * text).  Caller's write would fault even on a private copy. */
    if ((e & DESC_SW_COW) == 0) return -1;

    /* RO + COW \u2014 reuse the fault handler.  It does the right
     * thing: refcount-aware copy or in-place upgrade. */
    return address_space_handle_cow_fault(as, va);
}

/* ------------------------------------------------------------------
 * Chapter 90 \u2014 mmap support.
 *
 * vmas is a singly-linked list sorted by va.  Tiny linear scan is
 * fine: the chapter-90 ceiling is "a handful of mmaps per
 * process," and even if it grows we'd hit the page_cache slot
 * limit (32) long before this scan gets noticeable.
 * ------------------------------------------------------------------ */

/* Insert `v` into `as->vmas` keeping the list sorted by va. */
static void vma_insert_sorted(struct address_space *as, struct vma *v)
{
    struct vma **slot = &as->vmas;
    while (*slot && (*slot)->va < v->va) slot = &(*slot)->next;
    v->next = *slot;
    *slot = v;
}

/* Find the vma containing `va`, or NULL. */
static struct vma *vma_find(struct address_space *as, uint64_t va)
{
    for (struct vma *v = as->vmas; v; v = v->next) {
        if (va >= v->va && va < v->va + v->len) return v;
        if (v->va > va) break;       /* sorted: gone too far */
    }
    return NULL;
}

/* Reserve `pages` of fresh user VA from the mmap bump pointer.
 * Returns 0 if we'd overflow USER_MMAP_MAX, otherwise the start
 * VA. */
static uint64_t mmap_brk_alloc(struct address_space *as, uint64_t pages)
{
    uint64_t bytes = pages * PAGE_SIZE;
    if (as->mmap_brk + bytes < as->mmap_brk) return 0; /* overflow */
    if (as->mmap_brk + bytes > USER_MMAP_MAX) return 0;
    uint64_t va = as->mmap_brk;
    as->mmap_brk += bytes;
    return va;
}

/* Build the per-page descriptor we install at mmap fault time.
 * `is_pagecache` controls whether DESC_SW_PAGECACHE is set (so
 * teardown routes to page_cache_release). */
static uint64_t build_user_desc(uint64_t pa, int writable, int is_pagecache)
{
    uint64_t d = ATTR_NORMAL | DESC_AF | DESC_SH_INNER |
                 (writable ? DESC_AP_RW_EL0 : DESC_AP_RO_EL0) |
                 DESC_PXN | DESC_UXN |
                 DESC_VALID | DESC_PAGE |
                 (pa & ~0xFFFULL);
    if (is_pagecache) d |= DESC_SW_PAGECACHE;
    return d;
}

uint64_t address_space_mmap_anon(struct address_space *as,
                                 uint64_t pages, uint32_t prot)
{
    if (!as || !pages) return 0;
    if ((prot & (PROT_READ | PROT_WRITE)) == 0) return 0;

    uint64_t va = mmap_brk_alloc(as, pages);
    if (!va) return 0;

    struct vma *v = (struct vma *)kmalloc(sizeof(*v));
    if (!v) {
        /* Roll back the bump.  Safe because we're single-threaded
         * within this AS. */
        as->mmap_brk -= pages * PAGE_SIZE;
        return 0;
    }
    v->next        = NULL;
    v->va          = va;
    v->len         = pages * PAGE_SIZE;
    v->prot        = prot;
    v->kind        = VMA_ANON;
    v->ramfs_index = 0;
    v->_pad        = 0;
    v->file_offset = 0;
    vma_insert_sorted(as, v);
    return va;
}

uint64_t address_space_mmap_ramfs(struct address_space *as,
                                  uint64_t pages,
                                  uint32_t ramfs_index,
                                  uint64_t file_offset)
{
    if (!as || !pages) return 0;
    if (file_offset & (PAGE_SIZE - 1)) return 0;
    /* PROT_WRITE on a file mapping would need COW on the cached
     * page; chapter 90 punts that. */

    /* Validate the ramfs index up front.  Cheaper here than at
     * fault time \u2014 caller learns about ENOENT before the
     * mmap_brk pointer moves. */
    const uint8_t *blob; uint64_t blob_size;
    if (vfs_ramfs_blob((int)ramfs_index, &blob, &blob_size) != 0) return 0;
    /* Allow oversize mmaps (mmap is allowed to extend past EOF;
     * the post-EOF tail just zero-fills lazily).  We don't
     * enforce here \u2014 fault handler clamps. */
    (void)blob; (void)blob_size;

    uint64_t va = mmap_brk_alloc(as, pages);
    if (!va) return 0;

    struct vma *v = (struct vma *)kmalloc(sizeof(*v));
    if (!v) {
        as->mmap_brk -= pages * PAGE_SIZE;
        return 0;
    }
    v->next        = NULL;
    v->va          = va;
    v->len         = pages * PAGE_SIZE;
    v->prot        = PROT_READ;          /* enforced */
    v->kind        = VMA_FILE_RAMFS;
    v->ramfs_index = ramfs_index;
    v->_pad        = 0;
    v->file_offset = file_offset;
    vma_insert_sorted(as, v);
    return va;
}

int address_space_munmap(struct address_space *as, uint64_t va)
{
    if (!as) return -1;

    /* Detach the vma. */
    struct vma **slot = &as->vmas;
    while (*slot && (*slot)->va != va) slot = &(*slot)->next;
    if (!*slot) return -1;
    struct vma *v = *slot;
    *slot = v->next;

    /* Unmap each page we lazily faulted in.  Pages we haven't
     * touched yet leave a 0 descriptor behind \u2014 no work needed. */
    for (uint64_t p = 0; p < v->len; p += PAGE_SIZE) {
        uint64_t pva = v->va + p;
        uint64_t *ent = l3_entry_lookup(as, pva);
        if (!ent) continue;
        uint64_t e = *ent;
        if ((e & DESC_VALID) == 0) continue;
        uint64_t pa = e & ~0xFFFULL & ((1ULL << 48) - 1);
        *ent = 0;
        if (e & DESC_SW_PAGECACHE) {
            page_cache_release(pa);
        } else {
            pmem_dec_and_free(pa);
        }
        if (as->user_pages_alloced) as->user_pages_alloced--;
    }

    /* Flush stale TLB entries (we just yanked mappings out from
     * under the running AS). */
    __asm__ volatile(
        "dsb ishst       \n"
        "tlbi vmalle1    \n"
        "dsb ish         \n"
        "isb             \n"
        ::: "memory");

    kfree(v);
    return 0;
}

/* Loader for VMA_FILE_RAMFS pages.  Copies up to PAGE_SIZE bytes
 * from the ramfs blob at `offset_bytes` into `dst_pa`; pmem_alloc
 * has already zero-filled the page so a short copy leaves the
 * tail zeroed (POSIX mmap semantics for mappings beyond EOF). */
struct ramfs_loader_ctx { uint32_t ramfs_index; };

static int ramfs_loader(uint64_t dst_pa, uint64_t offset_bytes, void *cv)
{
    struct ramfs_loader_ctx *c = (struct ramfs_loader_ctx *)cv;
    const uint8_t *blob; uint64_t blob_size;
    if (vfs_ramfs_blob((int)c->ramfs_index, &blob, &blob_size) != 0) return -1;

    uint64_t to_copy = 0;
    if (offset_bytes < blob_size) {
        uint64_t avail = blob_size - offset_bytes;
        to_copy = avail < PAGE_SIZE ? avail : PAGE_SIZE;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)dst_pa;
    const uint8_t *src = blob + offset_bytes;
    for (uint64_t k = 0; k < to_copy; k++) dst[k] = src[k];
    /* Tail (if any) was zeroed by pmem_alloc_page. */
    return 0;
}

int address_space_handle_mmap_fault(struct address_space *as,
                                    uint64_t fault_va,
                                    int is_write)
{
    if (!as) return -1;
    uint64_t va = fault_va & ~(uint64_t)(PAGE_SIZE - 1);
    if (va < USER_VA_BASE || va >= USER_VA_END) return -1;

    struct vma *v = vma_find(as, va);
    if (!v) return -1;
    if (is_write && (v->prot & PROT_WRITE) == 0) return -1;

    /* Already mapped? Then the fault was something else (a
     * permission fault not covered by COW, etc.) \u2014 caller will
     * kill the thread. */
    uint64_t *ent = l3_entry_lookup(as, va);
    if (ent && (*ent & DESC_VALID)) return -1;

    int writable = (v->prot & PROT_WRITE) != 0;

    if (v->kind == VMA_ANON) {
        /* Fresh zero-filled page from pmem.  The page is owned
         * by this AS \u2014 not by the page cache \u2014 so future
         * teardown frees it via pmem_dec_and_free, and fork
         * COW-shares it like any other anonymous user page. */
        uint64_t pa = pmem_alloc_page();
        if (!pa) return -1;
        uint64_t *l3_tbl = l3_for(as, va);
        if (!l3_tbl) { pmem_free_page(pa); return -1; }
        l3_tbl[L3_INDEX(va)] = build_user_desc(pa, writable,
                                               /*is_pagecache=*/0);
        as->user_pages_alloced++;
    } else {
        /* File-backed: consult the page cache.  Cache key is
         * (1<<24 | ramfs_index) so future cache_kinds (osfs,
         * osfs2) can claim disjoint id ranges without clashing. */
        uint64_t in_vma_off = va - v->va;
        uint64_t file_off   = v->file_offset + in_vma_off;
        struct ramfs_loader_ctx ctx = { .ramfs_index = v->ramfs_index };
        uint32_t cache_id = (1u << 24) | (v->ramfs_index & 0x00FFFFFFu);
        uint64_t pa = page_cache_get_or_load(cache_id, file_off,
                                             ramfs_loader, &ctx);
        if (!pa) return -1;
        /* l3_for allocates the L3 table page if needed. */
        uint64_t *l3_tbl = l3_for(as, va);
        if (!l3_tbl) { page_cache_release(pa); return -1; }
        l3_tbl[L3_INDEX(va)] = build_user_desc(pa, /*writable=*/0,
                                               /*is_pagecache=*/1);
        as->user_pages_alloced++;
    }

    /* The descriptor change must be visible before we eret back
     * to the user.  No TLBI needed: the slot was previously
     * unmapped (translation fault), so no stale entry exists. */
    __asm__ volatile("dsb ishst" ::: "memory");
    return 0;
}

/* ------------------------------------------------------------------
 * Chapter 108a \u2014 WM-owned window mappings.
 *
 * The WM allocates one or more 4 KiB physical pages per window
 * and asks the AS layer to expose them at a fresh user VA range
 * with EL0-RW + DESC_SW_WM_WINDOW.  These mappings:
 *   - Are eager-installed (no fault path, since the WM already
 *     has the pages in hand).
 *   - Are skipped by AS teardown (the WM keeps the pages until
 *     it explicitly unmaps the window).
 *   - Are skipped by fork() in both clone modes.
 *   - Carry no per-page refcount and don't interact with the
 *     page cache.
 *
 * Address VAs come from the same `mmap_brk` bump pointer used
 * by sys_mmap so the two never collide.  No support for a fixed
 * VA — caller takes whatever the bump pointer gives.
 *
 * chapter 108e follow-up #4 — uninstall now feeds the freed
 * (va, n_pages) range into as->wm_freelist (sorted by va,
 * coalesced).  Install consults the freelist FIRST for a
 * best-fit candidate before falling back to mmap_brk_alloc.
 * Without this, a long sequence of resizes leaks VA space
 * (bump pointer has no reclaim) and eventually hits
 * USER_MMAP_MAX, breaking owner reinstall during resize.
 * ------------------------------------------------------------------ */

/* Take `n_pages` of VA off the WM-window freelist.  Returns the
 * starting VA, or 0 if no entry can satisfy the request.
 *
 * Strategy: best-fit (smallest range that fits) to keep large
 * gaps available for future big windows.  When the chosen
 * range exceeds the request, we trim from the LOW side and
 * keep the high-side remainder on the list — this keeps the
 * list sorted by va with minimal pointer churn. */
static uint64_t wm_freelist_take(struct address_space *as,
                                 uint64_t n_pages)
{
    struct wm_va_range *best = NULL;
    struct wm_va_range **best_link = NULL;
    struct wm_va_range **link = &as->wm_freelist;
    while (*link) {
        struct wm_va_range *r = *link;
        if (r->n_pages >= n_pages) {
            if (!best || r->n_pages < best->n_pages) {
                best = r;
                best_link = link;
            }
        }
        link = &r->next;
    }
    if (!best) return 0;
    uint64_t va = best->va;
    if (best->n_pages == n_pages) {
        /* Exact fit — unlink and free the node. */
        *best_link = best->next;
        kfree(best);
    } else {
        /* Trim from the low side; keep the high-side remainder. */
        best->va      += n_pages * PAGE_SIZE;
        best->n_pages -= n_pages;
    }
    return va;
}

/* Insert (va, n_pages) into the WM-window freelist, coalescing
 * with adjacent entries (low-side and/or high-side neighbours).
 * Returns 0 on success or -1 if the tracking node couldn't be
 * allocated AND no existing entry was adjacent (so no merge
 * could absorb the range).  On -1 the VA range is effectively
 * lost (we don't have anywhere to track it), but the L3 entries
 * are still zeroed by the caller — leakage of VA, not memory.
 *
 * Coalescing rules:
 *   prev->va + prev->n_pages*4K == va           → merge with prev
 *   va + n_pages*4K == next->va                 → merge with next
 *   both                                        → merge all three
 *   neither                                     → insert new node
 */
static int wm_freelist_release(struct address_space *as,
                               uint64_t va, uint64_t n_pages)
{
    if (!n_pages) return 0;
    uint64_t end_va = va + n_pages * PAGE_SIZE;

    struct wm_va_range *prev = NULL;
    struct wm_va_range *cur  = as->wm_freelist;
    while (cur && cur->va < va) {
        prev = cur;
        cur  = cur->next;
    }
    /* `cur` is the first entry with va >= our va (or NULL).
     * `prev` is its predecessor (or NULL if we're inserting
     * at the head). */
    int merge_prev = (prev && prev->va + prev->n_pages * PAGE_SIZE == va);
    int merge_next = (cur  && end_va == cur->va);

    if (merge_prev && merge_next) {
        /* Bridge: prev absorbs us AND cur. */
        prev->n_pages += n_pages + cur->n_pages;
        prev->next     = cur->next;
        kfree(cur);
        return 0;
    }
    if (merge_prev) {
        prev->n_pages += n_pages;
        return 0;
    }
    if (merge_next) {
        /* Slide cur down to start at our va. */
        cur->va       = va;
        cur->n_pages += n_pages;
        return 0;
    }
    /* No adjacency — allocate a fresh node and link it in. */
    struct wm_va_range *r = (struct wm_va_range *)kmalloc(sizeof(*r));
    if (!r) return -1;
    r->va      = va;
    r->n_pages = n_pages;
    r->next    = cur;
    if (prev) prev->next     = r;
    else      as->wm_freelist = r;
    return 0;
}

int address_space_install_wm_window(struct address_space *as,
                                    const uint64_t *page_pas,
                                    uint64_t n_pages,
                                    uint64_t *va_out)
{
    if (!as || !page_pas || !n_pages || !va_out) return -1;

    /* Try the freelist first.  Falls through to mmap_brk_alloc
     * if no entry is large enough (or the freelist is empty,
     * which is the steady state for the first install in any
     * AS).  `from_freelist` tracks the source so the rollback
     * path below can return the range to the right pool. */
    int from_freelist = 0;
    uint64_t va_base = wm_freelist_take(as, n_pages);
    if (va_base != 0) {
        from_freelist = 1;
    } else {
        va_base = mmap_brk_alloc(as, n_pages);
        if (!va_base) return -1;
    }

    /* Walk the pages and install each L3 entry.  L3 page-table
     * pages get allocated on demand inside l3_for.  If any
     * allocation fails we have to back out everything installed
     * so far AND give back the VA reservation. */
    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t va = va_base + i * PAGE_SIZE;
        uint64_t pa = page_pas[i];
        if (pa & 0xFFFULL) goto rollback;       /* not page-aligned */
        uint64_t *l3 = l3_for(as, va);
        if (!l3) goto rollback;
        /* Build the descriptor by hand so we can OR in
         * DESC_SW_WM_WINDOW without going through
         * build_user_desc (which only knows about pagecache). */
        uint64_t desc = ATTR_NORMAL | DESC_AF | DESC_SH_INNER |
                        DESC_AP_RW_EL0 |
                        DESC_PXN | DESC_UXN |
                        DESC_VALID | DESC_PAGE |
                        (pa & ~0xFFFULL) |
                        DESC_SW_WM_WINDOW;
        l3[L3_INDEX(va)] = desc;
        as->user_pages_alloced++;
        continue;
    rollback:
        /* Unwind already-installed entries.  Same shape as
         * uninstall_wm_window's inner loop but we don't TLBI
         * here — these are fresh mappings that haven't been
         * cached yet.  Then return the VA reservation to whichever
         * pool we took it from. */
        for (uint64_t k = 0; k < i; k++) {
            uint64_t kva = va_base + k * PAGE_SIZE;
            uint64_t *ent = l3_entry_lookup(as, kva);
            if (ent) { *ent = 0; }
            if (as->user_pages_alloced) as->user_pages_alloced--;
        }
        if (from_freelist) {
            /* Best-effort release back to the freelist.  If the
             * tracking-node kmalloc fails here we lose the VA
             * range entirely — uncommon (kmalloc just succeeded
             * for the take's potential split), and the only
             * cost is some bump-pointer-equivalent slack. */
            (void)wm_freelist_release(as, va_base, n_pages);
        } else {
            as->mmap_brk -= n_pages * PAGE_SIZE;
        }
        return -1;
    }

    /* Make the new descriptors visible to the MMU before user
     * code can touch the range.  No TLBI: the VAs were unmapped
     * a moment ago (translation fault) so there's no stale
     * entry to invalidate. */
    __asm__ volatile("dsb ishst" ::: "memory");

    *va_out = va_base;
    return 0;
}

int address_space_uninstall_wm_window(struct address_space *as,
                                      uint64_t va, uint64_t n_pages)
{
    if (!as || !n_pages) return -1;
    if (va & 0xFFFULL) return -1;

    /* Phase 1: verify the entire run is WM-owned before touching
     * anything.  If any descriptor is missing or doesn't carry
     * DESC_SW_WM_WINDOW the caller is using us wrong (probably
     * mixed up window ids); refuse rather than corrupt their
     * AS. */
    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t pva = va + i * PAGE_SIZE;
        uint64_t *ent = l3_entry_lookup(as, pva);
        if (!ent) return -1;
        uint64_t e = *ent;
        if ((e & DESC_VALID) == 0) return -1;
        if ((e & DESC_SW_WM_WINDOW) == 0) return -1;
    }

    /* Phase 2: zero each descriptor.  No pmem_free \u2014 the WM
     * owns the pages.  No page_cache_release \u2014 these were
     * never cache entries.  Just drop the mapping. */
    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t pva = va + i * PAGE_SIZE;
        uint64_t *ent = l3_entry_lookup(as, pva);
        if (!ent) continue;                 /* impossible per phase 1 */
        *ent = 0;
        if (as->user_pages_alloced) as->user_pages_alloced--;
    }

    /* Phase 3: flush stale TLB.  Unlike install, the user MAY
     * have touched these VAs (most likely scenario: app called
     * gui_window_fb, painted, then unmap on exit).  A local
     * vmalle1 is sufficient on this single-CPU build. */
    __asm__ volatile(
        "dsb ishst       \n"
        "tlbi vmalle1    \n"
        "dsb ish         \n"
        "isb             \n"
        ::: "memory");

    /* Phase 4 (chapter 108e follow-up #4) — return the VA range to
     * the freelist so a future install can reuse it.  Without
     * this, the bump pointer just grows monotonically and a long
     * sequence of resize-uninstall/install cycles eventually
     * exhausts USER_MMAP_MAX, breaking owner reinstall during
     * sys_win_fb_resize.  Best-effort: a kmalloc failure inside
     * release just drops the range entirely — same VA loss as
     * the pre-fix behaviour, but only on actual OOM. */
    (void)wm_freelist_release(as, va, n_pages);
    return 0;
}
