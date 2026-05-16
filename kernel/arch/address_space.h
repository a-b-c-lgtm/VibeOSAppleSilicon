/*
 * kernel/arch/address_space.h — per-process page tables.
 *
 * Each user process owns one of these.  Kernel threads use NULL
 * (the boot L1 stays active for them).
 *
 * VA layout in a per-process AS:
 *   slot 0  (0x0..0x40000000)         inherited from boot L1
 *                                     (devices + low DRAM, EL1 only)
 *   slot 1  (0x40000000..0x80000000)  inherited from boot L1
 *                                     (kernel image, EL1 only)
 *   slot 2  (0x80000000..0xC0000000)  inherited from boot L1
 *                                     (DRAM identity, kernel +
 *                                     legacy user-RW)
 *   slot 8  (0x200000000..0x240000000) inherited from boot L1
 *                                     (kernel heap, EL1 only)
 *   slot 64 (0x1000000000..0x1040000000) per-process user range:
 *                                     text  at 0x1000100000
 *                                     stack top at 0x1040000000
 *                                     points to per-process L2 page
 *   other   inherited from boot L1 too (RAM, EL1+EL0 because boot L1
 *           uses BLOCK_NORMAL_USER for slots 2..N — but the per-process
 *           AS only inherits the slots the kernel actually needs).
 *
 * No ASIDs yet: every address_space_activate flushes the TLB.  ASIDs
 * land in a future milestone (small, tagged invalidation).
 */
#ifndef ADDRESS_SPACE_H
#define ADDRESS_SPACE_H

#include <stdint.h>

/* User VA layout, fixed across all processes.  Each process sees
 * the same VAs but maps to its own physical pages.
 *
 * The user range lives well above DRAM (slot 64 at 64 GiB).  This
 * matters because slot N in the per-process L1 covers the user
 * range and is therefore NOT identity-mapped DRAM any more.  If
 * we picked a low slot that overlaps DRAM, pmem could hand out
 * pages in that PA range and the kernel would no longer be able
 * to access them by their PA-as-VA while a user AS was active.
 *
 * Slot 64 at 64 GiB is far above any plausible DRAM (QEMU virt
 * tops out around 0x240000000 = 9 GiB for our default), so user
 * VAs and pmem PAs never collide.
 */
#define USER_VA_BASE        0x1000000000UL    /* 64 GiB                     */
#define USER_VA_END         0x1040000000UL    /* 65 GiB                     */
#define USER_TEXT_BASE      0x1000100000UL    /* matches linker_user.ld     */
#define USER_HEAP_BASE      0x1010000000UL    /* sbrk starts here           */
#define USER_HEAP_MAX       0x1030000000UL    /* hard cap (512 MiB heap)    */
/* Chapter 90 — mmap() VAs come out of a bump pointer between
 * USER_MMAP_BASE and USER_MMAP_MAX.  Sits between the heap top
 * and the stack region; deliberately disjoint from sbrk so the
 * two never trip over each other.  Bump-only for chapter 90:
 * munmap leaves the VA range fallow, never reused.  A real
 * VMA-rb-tree allocator lands when the chapter 90 floor proves
 * limiting. */
#define USER_MMAP_BASE      0x1030000000UL    /* mmap region start         */
#define USER_MMAP_MAX       0x103F000000UL    /* mmap region end (240 MiB) */
#define USER_STACK_TOP      0x1040000000UL    /* one past top of user range */
#define USER_STACK_PAGES    16                /* 64 KiB user stack          */
/* The 16 KiB original (4 pages) was just enough for shell-style
 * tools but blew up on the M64 browser when laying out deeply
 * nested HN comment threads (each comment is wrapped in 4-5
 * <table>/<tbody>/<tr>/<td> levels, and a long thread nests 50+
 * deep — layout_build_subtree recurses to that depth, then
 * css_match_chain adds more frames per node).  See repo memory
 * note `chapter-44-css-table-layout.md` and the M64 browser
 * notes for the diagnostic trail. */

struct address_space {
    uint64_t  l1_pa;        /* PA of the L1 page (4 KiB)             */
    uint64_t *l1_va;        /* same page, identity-mapped VA         */
    uint64_t  l2_pa;        /* PA of the user-slot-2 L2 page         */
    uint64_t *l2_va;
    /* L3 pages are allocated lazily via address_space_map.  We don't
     * track them individually here; teardown walks L2 to find them. */
    uint64_t  user_pages_alloced;   /* stat: count of 4 KiB user pages */
    /* Program break.  Initialized to USER_HEAP_BASE when the AS is
     * created; sys_sbrk grows or shrinks it, mapping/unmapping
     * 4 KiB pages between the old and new break.  Always page-aligned. */
    uint64_t  heap_brk;

    /* Chapter 90 — mmap support.
     *
     * vmas:        head of a singly-linked list of struct vma,
     *              one per outstanding mmap(); sorted by va.
     * mmap_brk:    bump pointer for the next mmap allocation.
     *              Starts at USER_MMAP_BASE; grows toward
     *              USER_MMAP_MAX.  Never recycled in chapter 90.
     */
    struct vma *vmas;
    uint64_t    mmap_brk;

    /* Chapter 91 — reference count.
     *
     * Set to 1 by address_space_create.  Bumped by
     * address_space_share when a new thread starts using this
     * AS (e.g. SYS_CLONE).  address_space_destroy decrements;
     * the actual page-table teardown only fires when the last
     * reference drops.
     *
     * Pre-chapter-91 semantics (one AS per user thread, owned
     * outright) is the rc==1 special case: every existing
     * caller of address_space_destroy still does what it used
     * to do, because the destroy path becomes "dec rc, return
     * if still > 0" before the existing teardown.
     *
     * volatile + atomic_* ops on it because clone/exit can
     * race across CPUs (chapter 89 SMP). */
    volatile uint32_t refcount;
};

/* Grow or shrink the heap to `new_brk` (page-aligned, must lie in
 * [USER_HEAP_BASE, USER_HEAP_MAX]).  Maps fresh anonymous pages
 * when growing, frees pages and returns them to pmem when shrinking.
 * Caller is responsible for passing only valid values; returns 0
 * on success or -1 on OOM (partial map left intact for cleanup at
 * AS destroy). */
int address_space_set_brk(struct address_space *as, uint64_t new_brk);

/* Lifecycle.
 *
 * address_space_create returns a fresh AS with refcount=1
 * (single owner).
 *
 * address_space_destroy is refcount-aware: it decrements first,
 * and only frees the page tables and struct when the count hits
 * zero.  Every existing caller (sys_spawn/exec/fork/...) still
 * does the right thing because they took the only reference at
 * create time.
 *
 * address_space_share is the chapter-91 addition: it bumps the
 * refcount atomically so a new thread (created by SYS_CLONE)
 * can run inside the same AS.  Pair with exactly one
 * address_space_destroy on the way out. */
struct address_space *address_space_create(void);
void address_space_destroy(struct address_space *as);
void address_space_share(struct address_space *as);

/* Eager full-copy clone of an existing AS.  Walks src's user
 * range, allocates a fresh pmem page for every populated user
 * page, memcpys 4 KiB of payload, and maps the new page into the
 * child AS at the same VA with the same permissions.  Heap brk
 * is copied verbatim.  Returns a new AS on success, or NULL on
 * OOM (any partial mappings are cleaned up before returning).
 *
 * This is the chapter-73 fork primitive: deliberately the slow
 * unambiguous version.  Chapter-75 will retrofit copy-on-write,
 * at which point clone() becomes "share + mark RO" and the per-
 * page memcpy moves into the page-fault path. */
struct address_space *address_space_clone(const struct address_space *src);

/* Chapter 75 \u2014 copy-on-write clone.  Same VA layout and same
 * semantics as address_space_clone, but instead of allocating
 * fresh pages the parent and child share every existing page
 * read-only.  Writable pages are tagged with a software-defined
 * "this is COW" bit; on the first write to a tagged page the
 * data-abort handler in svc_dispatch calls
 * address_space_handle_cow_fault to allocate a private copy
 * for the writer.  Read-only pages are shared without a marker.
 *
 * Side effect: src's writable pages get downgraded to RO too,
 * so an existing TLB entry holding stale RW must be flushed.
 * The function does that flush before returning.
 *
 * `src` is non-const because we mutate src's L3 entries in place. */
struct address_space *address_space_clone_cow(struct address_space *src);

/* Resolve a copy-on-write data abort.  Returns 0 if the fault
 * was a real COW fault that we resolved (caller should eret
 * back to the faulting instruction); -1 if the fault wasn't
 * COW-related (caller falls through to "kill the thread"). */
int address_space_handle_cow_fault(struct address_space *as,
                                   uint64_t fault_va);

/* Proactively make `va` writable from the kernel's perspective:
 *   - already RW              \u2192 return 0 (no-op)
 *   - RO + DESC_SW_COW        \u2192 resolve COW, return 0
 *   - RO without DESC_SW_COW  \u2192 return -1 (real write to RO)
 *   - not mapped              \u2192 return -1 (EFAULT)
 *
 * Used by copy_to_user-style helpers and by signal frame
 * delivery to break COW shares before the kernel itself writes
 * via the user VA.  Without this, copy_to_user into a forked
 * (still-shared) user stack faults with EL1 data abort because
 * AArch64 RO permissions apply to EL1 too. */
int address_space_make_writable(struct address_space *as, uint64_t va);

/* Map `pages` consecutive 4 KiB pages, user-VA `va` -> physical
 * `pa`.  Both must be 4 KiB aligned.  Pages are mapped Normal-WB,
 * AP=RW EL0+EL1, AF=1, PXN=1 (kernel can NEVER execute here),
 * UXN=0 if `executable` else 1.  Returns 0 on success. */
int address_space_map(struct address_space *as,
                      uint64_t va, uint64_t pa,
                      uint64_t pages, int writable, int executable);

/* Activate this AS by writing TTBR0_EL1 and flushing the TLB.  If
 * `as` is NULL, restores the boot L1 (kernel-thread context). */
void address_space_activate(const struct address_space *as);

/* For diagnostics: return the boot L1 PA.  Used by the activate
 * path when as == NULL. */
uint64_t address_space_boot_l1_pa(void);

/* ------------------------------------------------------------------
 * Chapter 90 — mmap regions and lazy fault-in.
 *
 * struct vma describes one outstanding mmap.  All pages within
 * the vma are lazily faulted in: the mmap syscall just records
 * the bookkeeping; nothing physical is allocated until the user
 * touches the range.  The data-abort handler in svc_dispatch
 * looks up the faulting VA via address_space_handle_mmap_fault,
 * which consults the vma list to decide what to do.
 * ------------------------------------------------------------------ */

/* vma_kind selects the lazy-fault behaviour. */
enum vma_kind {
    VMA_ANON       = 0,   /* MAP_PRIVATE | MAP_ANONYMOUS: zero-fill on fault */
    VMA_FILE_RAMFS = 1,   /* MAP_PRIVATE PROT_READ on a ramfs file index    */
};

struct vma {
    struct vma *next;
    uint64_t    va;             /* page-aligned start                  */
    uint64_t    len;            /* page-aligned length, > 0            */
    uint32_t    prot;           /* PROT_READ | PROT_WRITE              */
    uint32_t    kind;           /* enum vma_kind                       */
    /* File-backed bookkeeping.  Unused for VMA_ANON. */
    uint32_t    ramfs_index;    /* index into kernel/core/vfs.c's g_ramfs */
    uint32_t    _pad;
    uint64_t    file_offset;    /* page-aligned starting offset in file */
};

/* mmap_anon: reserve a fresh mmap region of `pages` * PAGE_SIZE
 * bytes, anonymous (zero-fill on fault), writable iff (prot &
 * PROT_WRITE).  Returns the chosen VA on success or 0 on
 * failure.  Does NOT pre-fault — pages are allocated lazily by
 * the data-abort handler. */
uint64_t address_space_mmap_anon(struct address_space *as,
                                 uint64_t pages, uint32_t prot);

/* mmap_ramfs: reserve a fresh mmap region of `pages` * PAGE_SIZE
 * bytes mapped onto the ramfs file at index `ramfs_index`,
 * starting at `file_offset` (must be page-aligned).  PROT_READ
 * only — chapter 90 does not support PROT_WRITE on file
 * mappings (would need COW on the page-cache page).  Returns
 * the chosen VA on success or 0 on failure. */
uint64_t address_space_mmap_ramfs(struct address_space *as,
                                  uint64_t pages,
                                  uint32_t ramfs_index,
                                  uint64_t file_offset);

/* munmap: remove the mmap entry covering `va` and unmap every
 * lazily-faulted-in page.  `va` must match the start of an
 * existing vma; partial unmaps are not supported in chapter 90.
 * Returns 0 on success or -1 if `va` doesn't name a known vma. */
int      address_space_munmap(struct address_space *as, uint64_t va);

/* Resolve a translation fault from EL0.  Returns 0 if the fault
 * lay inside a known vma and we lazily mapped a page for it
 * (caller should eret back to the faulting instruction);
 * returns -1 if the fault was outside every vma (caller falls
 * through to "kill the thread").  `is_write` mirrors the ESR
 * WnR bit so we can reject writes against PROT_READ vmas. */
int      address_space_handle_mmap_fault(struct address_space *as,
                                         uint64_t fault_va,
                                         int is_write);

#endif /* ADDRESS_SPACE_H */
