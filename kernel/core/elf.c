/*
 * kernel/core/elf.c — minimal ELF64 loader.
 *
 * The user binary is linked at a fixed VA (USER_LOAD_ADDR =
 * 0x100000) and contains a single PT_LOAD segment.  We could
 * cheat and just memcpy the raw image, but parsing the program
 * headers properly costs no extra code and means whoever rewrites
 * the loader for the real per-process page-table layer has
 * one fewer thing to refactor.
 *
 * Identity mapping caveat: the loader places each segment at
 * `paddr = pmem_alloc_page() ...` and does NOT remap that physical
 * page to the segment's link-time VA.  In this initial version we get away
 * with this because the user link-time VA (0x100000) sits inside
 * the same 1 GiB device-mapped block as MMIO, which is harmful for
 * code execution — so we do something subtler: we pick a load
 * region inside known-good RAM (at or above 0x40000000) and pass
 * that physical address back as the "user-visible" entry point.
 * The user binary was linked PIC-friendly (no absolute addresses
 * baked in beyond local jumps that the linker resolved relative)
 * so it runs correctly at whatever address we put it at.
 *
 * Specifically: the early hello.elf has its single PT_LOAD
 * segment marked p_vaddr = 0x100000 but ALL its code uses PC-
 * relative addressing (adrp/add) so the actual load address is
 * irrelevant.  Once chapter 13 introduces per-process page tables,
 * we will start honouring p_vaddr exactly.
 */

#include "elf.h"
#include "pmem.h"
#include "serial.h"
#include "../arch/address_space.h"
#include <stdint.h>
#include <stddef.h>

#define EI_NIDENT  16
#define ELFMAG0    0x7f
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC    2
#define EM_AARCH64 183
#define PT_LOAD    1

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

#define USER_STACK_PAGES 16  /* 64 KiB user stack — must match
                              * arch/address_space.h.  Bumped from
                              * 4 (16 KiB) to handle deeply
                              * nested DOMs (HN comment threads). */
#define MAX_USER_ARGV    64  /* hard cap on argc; matches sys_exec */

/*
 * Build the initial user-mode stack frame in the TOP user-stack
 * page.  Layout (low -> high addresses, sp at the lowest):
 *
 *   sp -> argc                   (8 bytes)
 *         argv[0]                 (8 bytes; ptr into strings region)
 *         argv[1]                 (8 bytes)
 *         ...
 *         argv[argc] = NULL       (8 bytes; argv terminator)
 *         envp[0]    = NULL       (8 bytes; envp terminator -- empty env)
 *         <padding to 16-byte align>
 *         <argv strings, NUL-terminated, packed>
 *         USER_STACK_TOP
 *
 * Everything must fit in PAGE_SIZE bytes (we only ever write into
 * the topmost stack page).  Returns the new SP_EL0 in *sp_out, or
 * -1 if the layout overflows the page or argc > MAX_USER_ARGV.
 *
 * top_page_pa MUST be the physical address of the page whose user
 * VA is [USER_STACK_TOP - PAGE_SIZE, USER_STACK_TOP).  We write
 * via the PA because the per-process address space is not yet
 * active at load time (boot L1 is still installed in TTBR0).
 */
static int build_user_init_stack(uint64_t top_page_pa,
                                 const char *const argv[],
                                 uint64_t *sp_out)
{
    /* Count argc and total string bytes (including the trailing
     * NUL on each argument). */
    int    argc = 0;
    size_t strings_bytes = 0;
    if (argv) {
        for (; argv[argc] != NULL; argc++) {
            if (argc >= MAX_USER_ARGV) {
                serial_puts("[elf] too many argv entries\n");
                return -1;
            }
            const char *s = argv[argc];
            size_t l = 0;
            while (s[l]) l++;
            strings_bytes += l + 1;
        }
    }

    /* vector_bytes = argc word + (argc + 1) argv pointers + 1 envp NULL */
    size_t vector_bytes   = sizeof(uint64_t)
                          + (size_t)(argc + 1) * sizeof(uint64_t)
                          + sizeof(uint64_t);
    size_t total_unaligned = vector_bytes + strings_bytes;
    /* Round up to 16 bytes so SP stays AAPCS-aligned at entry. */
    size_t total = (total_unaligned + 15U) & ~(size_t)15U;
    if (total > PAGE_SIZE) {
        serial_puts("[elf] argv blob does not fit in one stack page\n");
        return -1;
    }

    /* Map of the top page in physical address space.  We use the
     * 1:1 RAM identity mapping that boot installs so this PA
     * doubles as a kernel-side VA. */
    uint8_t *page = (uint8_t *)(uintptr_t)top_page_pa;
    /* Zero the entire page first so any uninitialized read after
     * sp gets a deterministic 0. */
    for (size_t i = 0; i < PAGE_SIZE; i++) page[i] = 0;

    /* Strings live at the very top of the page; build a parallel
     * table of their user-VA addresses so we can fill argv[i]. */
    size_t   strings_off  = PAGE_SIZE - strings_bytes;
    uint64_t strings_va_base = USER_STACK_TOP - strings_bytes;
    uint64_t arg_vas[MAX_USER_ARGV];
    {
        size_t cursor = 0;
        for (int i = 0; i < argc; i++) {
            const char *s = argv[i];
            arg_vas[i] = strings_va_base + cursor;
            for (size_t l = 0; s[l]; l++) {
                page[strings_off + cursor] = (uint8_t)s[l];
                cursor++;
            }
            page[strings_off + cursor] = 0; /* trailing NUL */
            cursor++;
        }
    }

    /* sp_off is the page-relative offset where SP will land. */
    size_t sp_off = PAGE_SIZE - total;
    /* argc word */
    *(uint64_t *)(page + sp_off) = (uint64_t)argc;
    /* argv pointers + NULL terminator */
    for (int i = 0; i < argc; i++)
        *(uint64_t *)(page + sp_off + sizeof(uint64_t)
                                   + (size_t)i * sizeof(uint64_t)) = arg_vas[i];
    *(uint64_t *)(page + sp_off + sizeof(uint64_t)
                              + (size_t)argc * sizeof(uint64_t)) = 0;
    /* envp[0] = NULL (no environment yet) */
    *(uint64_t *)(page + sp_off + sizeof(uint64_t)
                              + ((size_t)argc + 1) * sizeof(uint64_t)) = 0;

    *sp_out = USER_STACK_TOP - total;
    return 0;
}

int elf_load_user(const uint8_t *data, size_t size,
                  struct address_space *as,
                  const char *const argv[],
                  struct user_image *out)
{
    if (!as) return -1;
    if (size < sizeof(struct elf64_ehdr)) return -1;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)data;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("[elf] bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        serial_puts("[elf] not 64-bit LE\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_AARCH64) {
        serial_puts("[elf] not aarch64 ET_EXEC\n");
        return -1;
    }

    int seen_load = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (data + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        /* Round vaddr down and memsz up to a whole number of pages.
         * The user linker script guarantees PT_LOAD is page-aligned
         * (-z max-page-size=0x1000) but we belt-and-braces it here. */
        uint64_t va_lo  = ph->p_vaddr & ~(PAGE_SIZE - 1ULL);
        uint64_t va_hi  = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
        uint64_t pages  = (va_hi - va_lo) / PAGE_SIZE;
        if (pages == 0) continue;

        /* Allocate one pmem page at a time and map each into the
         * AS at the matching VA.  Pages do NOT need to be physically
         * contiguous now that proper page tables exist. */
        uint64_t copied = 0;
        for (uint64_t k = 0; k < pages; k++) {
            uint64_t pg_pa = pmem_alloc_page();
            if (!pg_pa) {
                serial_puts("[elf] OOM allocating segment page\n");
                return -1;
            }
            uint64_t pg_va = va_lo + k * PAGE_SIZE;

            /* Copy whatever bytes from this page lie within
             * [ph->p_offset, ph->p_offset + ph->p_filesz). */
            uint8_t *dst = (uint8_t *)(uintptr_t)pg_pa;
            for (uint64_t b = 0; b < PAGE_SIZE; b++) {
                uint64_t va_byte = pg_va + b;
                if (va_byte >= ph->p_vaddr &&
                    va_byte <  ph->p_vaddr + ph->p_filesz) {
                    uint64_t off = va_byte - ph->p_vaddr;
                    dst[b] = data[ph->p_offset + off];
                    copied++;
                }
                /* Else: leave zero (BSS portion). */
            }

            /* Permissions: we keep things simple and permissive for
             * now — every PT_LOAD page is RW + executable.  Real
             * permissions (PT_LOAD's p_flags PF_R/W/X) plumb in once
             * we split text and data into separate segments. */
            int writable   = 1;
            int executable = 1;
            if (address_space_map(as, pg_va, pg_pa, 1,
                                  writable, executable) != 0) {
                serial_puts("[elf] map failed for segment page\n");
                return -1;
            }
        }
        (void)copied;

        seen_load = 1;
    }

    if (!seen_load) {
        serial_puts("[elf] no PT_LOAD segment\n");
        return -1;
    }

    out->entry_va = eh->e_entry;       /* link-time VA, real now */

    /* Allocate USER_STACK_PAGES one at a time and map them at the
     * top of the user range.  No contiguity requirement.  Remember
     * the top page's PA so we can write the argv blob into it. */
    uint64_t stack_va_top = USER_STACK_TOP;
    uint64_t stack_va_bot = stack_va_top - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    uint64_t top_page_pa  = 0;
    for (size_t k = 0; k < USER_STACK_PAGES; k++) {
        uint64_t pa = pmem_alloc_page();
        if (!pa) { serial_puts("[elf] OOM allocating user stack\n"); return -1; }
        uint64_t va = stack_va_bot + k * PAGE_SIZE;
        if (address_space_map(as, va, pa, 1, /*write*/1, /*exec*/0) != 0) {
            serial_puts("[elf] map failed for user stack\n");
            return -1;
        }
        if (k == USER_STACK_PAGES - 1) top_page_pa = pa;
    }

    /* Chapter 103 — install a one-page guard immediately below
     * the stack base.  No physical backing; the L3 entry is
     * invalid + tagged with DESC_SW_GUARD.  When a runaway
     * recursion (or a single fat frame) pokes through the
     * stack floor, the data-abort handler reads the SW bit and
     * turns the fault into a "[svc] user stack overflow"
     * diagnostic instead of a generic register dump.
     *
     * The L3 page covering the stack was just allocated by the
     * loop above, so installing the guard one page below the
     * stack base hits an existing L3 \u2014 zero extra physical
     * memory is consumed. */
    if (address_space_install_guard(as, USER_STACK_GUARD_VA) != 0) {
        serial_puts("[elf] failed to install user-stack guard\n");
        return -1;
    }

    /* Build the initial argc/argv/envp frame in the top stack page.
     * On return out->stack_top_va is the SP_EL0 the user thread
     * will start with (it points at argc). */
    uint64_t sp_va;
    if (build_user_init_stack(top_page_pa, argv, &sp_va) != 0)
        return -1;
    out->stack_top_va = sp_va;
    return 0;
}
