/* userspace/linker_user.ld — link script for user programs.
 *
 * Every user process owns its own page tables.
 * The user VA range is L1 slot 64 (0x1000000000 - 0x1040000000),
 * placed well above any plausible DRAM so user VAs and pmem PAs
 * never conflict.  Binaries link at 0x1000100000 so each process
 * sees the same VAs but maps to its own physical pages.  The
 * 16 KiB user stack lives at the top of the slot
 * (0x1040000000 - 16 KiB) but is allocated and mapped by the
 * kernel side, not by the linker.
 */

ENTRY(_user_start)

USER_LOAD_ADDR = 0x1000100000;

PHDRS
{
    load PT_LOAD FLAGS(7);   /* PF_R | PF_W | PF_X — kept simple */
    bss  PT_LOAD FLAGS(6);   /* PF_R | PF_W — NOBITS so .bss doesn't bloat the disk image */
}

SECTIONS
{
    . = USER_LOAD_ADDR;

    .text : ALIGN(4K) {
        KEEP(*(.text._user_start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :load

    /* chapter 156: C-style global constructors / destructors.
     * Anything tagged __attribute__((constructor)) lands in
     * .init_array; crt0's `_user_start` walks the array
     * between argv setup and `bl main`.  __attribute__((destructor))
     * lands in .fini_array; __cxa_finalize() walks it on exit. */
    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :load

    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :load

    .data : ALIGN(8) {
        *(.data .data.*)
    } :load

    /* chapter 156: the linker generates .got / .got.plt / .rela.got
     * internally when a weak undefined-or-defined symbol appears
     * via a CALL26 reloc (our __cxa_finalize default in crt0).
     * Place them inside the loadable image so --orphan-handling=error
     * is satisfied; in practice they are usually empty. */
    .got       : ALIGN(8) { *(.got) }                :load
    .got.plt   : ALIGN(8) { *(.got.plt) }            :load

    /* .bss / COMMON go in their own NOBITS segment so static
     * arrays (e.g. paint's 600x400 canvas) don't bloat the on-
     * disk binary.  ALIGN(4K) is critical: the kernel ELF loader
     * (kernel/core/elf.c) maps each PT_LOAD page-by-page, so two
     * PT_LOADs that share a page would collide.  Forcing .bss
     * onto a fresh page guarantees the loader sees disjoint
     * virtual address ranges. */
    .bss  : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    } :bss

    /* The user stack used to live here.  It is now
     * allocated and mapped by the kernel-side ELF loader at a fixed
     * VA (top of L1 slot 2 = 0xC0000000), so it does not bloat the
     * on-disk binary. */

    /* DWARF debug sections (kept in the ELF for GDB but not in
     * the loadable image). */
    .debug_info     0 : { *(.debug_info     .debug_info.*)     }
    .debug_abbrev   0 : { *(.debug_abbrev   .debug_abbrev.*)   }
    .debug_aranges  0 : { *(.debug_aranges  .debug_aranges.*)  }
    .debug_ranges   0 : { *(.debug_ranges   .debug_ranges.*)   }
    .debug_rnglists 0 : { *(.debug_rnglists .debug_rnglists.*) }
    .debug_loc      0 : { *(.debug_loc      .debug_loc.*)      }
    .debug_loclists 0 : { *(.debug_loclists .debug_loclists.*) }
    .debug_line     0 : { *(.debug_line     .debug_line.*)     }
    .debug_line_str 0 : { *(.debug_line_str .debug_line_str.*) }
    .debug_str      0 : { *(.debug_str      .debug_str.*)      }
    .debug_frame    0 : { *(.debug_frame    .debug_frame.*)    }
    .debug_macro    0 : { *(.debug_macro    .debug_macro.*)    }

    /* Discard everything else. */
    /DISCARD/ : {
        *(.iplt)
        *(.igot.plt)
        *(.rela.iplt)
        *(.rela.text*)
        *(.rela.data*)
        *(.rela.dyn)
        *(.rela.got)
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.ARM.attributes)
    }
}
