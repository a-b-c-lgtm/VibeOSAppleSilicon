/* assets/osfs/osdev.ld — in-guest linker script for /bin/ld
 *
 * Chapter 131f: shipped on the OSFS-1 disk as /bin/osdev.ld so
 * the guest binutils /bin/ld (and /bin/cc, which invokes /bin/ld
 * with `-T /bin/osdev.ld`) can place user binaries at the VA
 * the kernel ELF loader expects.
 *
 * Stay byte-identical to userspace/linker_user.ld (the host-
 * side script used by aarch64-osdev-cc) so a binary built
 * in-guest is layout-equivalent to one cross-built from the
 * host.  Keep both files in sync when the toolchain contract
 * changes.
 *
 * Contract (chapter 122):
 *   - Static AArch64 ET_EXEC, single .text/.rodata/.data PT_LOAD
 *     at VA 0x1000100000, .bss in its own NOBITS PT_LOAD.
 *   - Entry symbol `_user_start` (overridable on the ld command
 *     line via `-e <other>`; the test_bin_ld_ar.py harness does
 *     exactly that for the bare-asm exit-42 program).
 */

ENTRY(_user_start)

USER_LOAD_ADDR = 0x1000100000;

PHDRS
{
    load PT_LOAD FLAGS(7);
    bss  PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = USER_LOAD_ADDR;

    .text : ALIGN(4K) {
        KEEP(*(.text._user_start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :load

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

    .got       : ALIGN(8) { *(.got) }                :load
    .got.plt   : ALIGN(8) { *(.got.plt) }            :load

    .bss  : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    } :bss

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
