/*
 * kernel/core/elf.h — minimal AArch64 ELF64 loader.
 *
 * Just enough of the ELF spec to consume the user binaries we
 * produce ourselves: ELF64, EM_AARCH64, little-endian, ET_EXEC,
 * PT_LOAD program headers only.  No relocations, no dynamic
 * linking, no notes, no symbol resolution.
 *
 * The loader populates a per-process address
 * space (kernel/arch/address_space.h).  PT_LOAD segments are
 * loaded at their link-time VAs (no PA-arithmetic translation any
 * more); each segment's pmem-allocated pages are mapped at the
 * matching user VA via address_space_map.  A user stack is
 * allocated and mapped at the top of the user range.
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

struct address_space;        /* forward decl; see arch/address_space.h */

struct user_image {
    uint64_t entry_va;        /* user-visible entry point             */
    uint64_t stack_top_va;    /* initial SP_EL0 value (points at argc) */
};

/* Load `data` (a complete ELF file) into `as`.  Returns 0 on
 * success, -1 on parse / OOM / mapping failure.
 *
 * `argv` is a NULL-terminated kernel-side array of NUL-terminated
 * argument strings.  The loader writes them into the topmost user
 * stack page along with an argc word and a NULL-terminated pointer
 * vector, then sets out->stack_top_va so SP_EL0 lands pointing at
 * argc on entry to user code.  argv may be NULL, in which case
 * argc=0 and argv vector is just the terminator. */
int elf_load_user(const uint8_t *data, size_t size,
                  struct address_space *as,
                  const char *const argv[],
                  struct user_image *out);

#endif
