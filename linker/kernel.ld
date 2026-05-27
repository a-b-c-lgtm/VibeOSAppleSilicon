/* kernel.ld — aarch64 linker script for QEMU `-machine virt`.
 *
 * QEMU's `-kernel` ELF loader on aarch64 places the image at the
 * physical address declared by the program headers, then jumps to
 * the ELF entry point with x0 = DTB physical address.  The virt
 * machine reserves [0x40000000, 0x40080000) for the DTB and a few
 * other firmware-level structures, so we link the kernel starting
 * at 0x40080000 — the conventional load address used by Linux,
 * U-Boot, and OVMF on virt.
 *
 * Two PT_LOAD segments:
 *   text  — RX, holds .text.boot + .text + .rodata.
 *   data  — RW, holds .data + .bss + the boot stack.
 * Splitting them silences the linker's "LOAD segment with RWX
 * permissions" warning and is what we want once the MMU is on
 * so .text can map as Normal-Cacheable + RO + XN=0
 * while .data maps as RW + XN=1.
 *
 * --orphan-handling=error in the Makefile catches any future input
 * section we forgot to name.  All standard DWARF debug sections
 * are explicitly captured below (NOLOAD, no PHDR) so a `-g` build
 * does not break the link. */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x40080000;

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    /* --------------------------------------------------------------
     * Executable + read-only segment (PT_LOAD, RX)
     * -------------------------------------------------------------- */
    .text : ALIGN(4K) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        /* User binaries embedded via `objcopy -I binary`.  Each
         * wrapped object renames its .data section to
         * .rodata.embedded_user before the kernel link, so all
         * embedded user images land here back-to-back.  The
         * `_binary_<name>_bin_start/_end/_size` symbols come for
         * free as ELF globals. */
        . = ALIGN(8);
        *(.rodata.embedded_user)
    } :text

    /* --------------------------------------------------------------
     * Writable segment (PT_LOAD, RW)
     * -------------------------------------------------------------- */
    .data : ALIGN(4K) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    .bss : ALIGN(8) {
        bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(8);
        bss_end = .;
    } :data

    /* Boot stack.  16-byte alignment is required at any public
     * boundary on aarch64; the 64 KiB size is a multiple of 16
     * so stack_top is automatically aligned. */
    .stack : ALIGN(16) {
        stack_bottom = .;
        . = . + 0x10000;        /* 64 KiB */
        stack_top = .;
    } :data

    /* Chapter 87 — secondary CPU boot stacks.  One per non-boot
     * CPU up to SMP_MAX_CPUS=4 (cpu.h).  16 KiB each is enough
     * for the early boot path: the secondary brings up its MMU,
     * sets TPIDR_EL1, prints "[smp] CPU N ready", and then sits
     * in WFE forever (no IRQs taken on the secondary yet, so the
     * stack only ever holds the activation records of
     * secondary_start -> secondary_main).  Bumped later when the
     * SMP scheduler actually runs threads on these CPUs. */
    .secondary_stacks : ALIGN(16) {
        secondary_stacks_bottom = .;
        . = . + 0x4000;         /* CPU 1: 16 KiB */
        secondary_stack_top_1 = .;
        . = . + 0x4000;         /* CPU 2 */
        secondary_stack_top_2 = .;
        . = . + 0x4000;         /* CPU 3 */
        secondary_stack_top_3 = .;
        secondary_stacks_top = .;
    } :data

    /* End-of-image marker.  The kernel uses this so the
     * page allocator can carve out [KERNEL_LOAD_ADDR, kernel_end)
     * before adding the rest of physical RAM to the freelist.
     * Earlier builds reserved a fixed 16 MiB .heap section
     * here; the heap now lives in DTB-discovered RAM instead. */
    . = ALIGN(4K);
    kernel_end = .;

    /* --------------------------------------------------------------
     * DWARF debug sections — present when compiled with -g.  These
     * are not loaded into the image (no PHDR assignment), they only
     * appear in the ELF for GDB's benefit.  Listed explicitly so
     * --orphan-handling=error does not flag them.
     * -------------------------------------------------------------- */
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

    /* --------------------------------------------------------------
     * Sections we never want in the kernel image.
     *
     *   .iplt / .igot.plt / .rela.iplt — IFUNC trampolines emitted
     *     by some libgcc/binutils combinations.  The kernel does
     *     not use IFUNC and has nothing to resolve them at runtime;
     *     keeping them would only add unreachable bytes.
     *   .rela.* — static link is fully resolved; runtime relocations
     *     are not supported.
     *   .ARM.attributes / .note* / .comment / .eh_frame — vendor
     *     metadata of no use to a freestanding kernel.
     * -------------------------------------------------------------- */
    /DISCARD/ : {
        *(.iplt)
        *(.igot.plt)
        *(.rela.iplt)
        *(.rela.text*)
        *(.rela.data*)
        *(.rela.dyn)
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.ARM.attributes)
    }
}
