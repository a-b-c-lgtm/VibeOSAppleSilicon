# Chapter 15 — ELF loading and the first user program

> **Where the code lives.**
> Loader: [kernel/core/elf.c](../../../kernel/core/elf.c), [kernel/core/elf.h](../../../kernel/core/elf.h)
> Embedded user binary glue: [kernel/core/embedded_user.h](../../../kernel/core/embedded_user.h)
> First user program: [userspace/hello/hello.c](../../../userspace/hello/hello.c)
> User CRT: [userspace/crt/crt0.S](../../../userspace/crt/crt0.S)
> User link script: [userspace/linker_user.ld](../../../userspace/linker_user.ld)
> Kernel link section for embedded users: [linker/kernel.ld](../../../linker/kernel.ld) (`.rodata.embedded_user`)

[Chapter 14](14-svc-and-syscalls.md) gave us a syscall channel and
the ability to launch a thread at EL0 — but that thread has to
*come from somewhere*. We can't write user code in C and link it
into the kernel image with `gcc -c` the way we wrote
`hello_kernel_thread`, because the user code needs to live at a
user-space virtual address, talk to the kernel only through the
syscall ABI, and never accidentally call a kernel function. This
chapter is about getting from `int main(void) { puts("hi"); }` to
a `pid` running at EL0.

The plan:

1. Build the user program with its own toolchain flags and link
   script.
2. Embed the resulting ELF into the kernel image at link time.
3. Write a small ELF loader that walks the program headers,
   allocates physical pages, and copies the segments into them.
4. Hand control to a brand-new user thread whose initial PC and
   SP point into the loaded segments.

Step 4 we already wrote in Chapter 14 (`user_thread_create` and
`user_trampoline`). This chapter covers steps 1–3.

## Building user code separately

The kernel and the user program have very different requirements
for the toolchain:

| Concern              | Kernel                 | User program           |
|----------------------|------------------------|------------------------|
| Target               | EL1                    | EL0                    |
| Link address         | `0x40080000`           | `0x100000`             |
| Stack provided by    | Linker `.stack` section | Caller (kernel)       |
| External symbols     | None (freestanding)    | None (freestanding)    |
| Calling convention   | AAPCS                  | AAPCS                  |
| Optimization         | `-O2` (tight loops)    | `-Os` (small image)    |
| Embedded into        | itself                 | the kernel             |

The shared bits (`-ffreestanding -nostdlib -mcpu=cortex-a72
-mgeneral-regs-only -fno-pie -fno-pic -Wall -Wextra -Werror`) are
consequences of the platform, not the kernel/user split — we'd
use them either way. The differences live in:

- **`USER_LDFLAGS`** in the [Makefile](../../../Makefile) points
  at `userspace/linker_user.ld` instead of `linker/kernel.ld`.
- **`USER_CFLAGS`** uses `-Os` instead of `-O2`. User code is
  small and rarely on a hot loop in milestone 7; saving bytes in
  the embedded image matters more than saving cycles.

The user link script puts everything at `USER_LOAD_ADDR =
0x100000`:

```ld
ENTRY(_user_start)
USER_LOAD_ADDR = 0x100000;

PHDRS {
    load PT_LOAD FLAGS(7);   /* PF_R | PF_W | PF_X */
}

SECTIONS {
    . = USER_LOAD_ADDR;
    .text : ALIGN(4K) { ... } :load
    .data : ALIGN(8)  { ... } :load
    .stack : ALIGN(16) {
        . = . + 0x4000;
        user_stack_top = .;
    } :load
}
```

Three things stand out:

- **Single PT_LOAD segment, all permissions.** Producing a single
  RWX segment is correct for milestone 7 because we don't have
  per-process page tables yet; the loader just allocates pages
  and copies the bytes. Once Chapter 17 introduces per-process
  L1 tables we'll split this into RX text + RW data and map them
  with the right permissions.

- **The link address is `0x100000` but the load address will be
  whatever pmem hands out.** This works because the user code
  uses only PC-relative addressing for globals. GCC with
  `-fno-pic -fno-pie` still emits `adrp`/`add` for global
  references, which compute `(PC & ~0xFFF) + offset`. As long as
  the *relative* distance between the instruction and its target
  is the same at runtime as at link time — and that holds when
  the entire single-PT_LOAD blob is loaded contiguously — the
  references resolve correctly.

- **A `user_stack_top` symbol is defined inside the segment.**
  We don't actually use it: the kernel allocates a separate
  stack via `pmem_alloc_page` and passes that to
  `user_thread_create`. The symbol exists so the link doesn't
  fail when crt0 (or future user code) takes its address.

## Embedding the ELF into the kernel

The kernel needs the user binary in memory before the page
allocator is even up — there's no file system yet, no virtio-blk,
no ramdisk. The simplest path: bake the ELF into the kernel image
at link time and look it up via symbols.

`objcopy -I binary` is the standard tool for this. Given a raw
file, it produces an ELF object exposing three symbols:

- `_binary_<filename>_start` — first byte of the blob
- `_binary_<filename>_end` — one byte past the last
- `_binary_<filename>_size` — an absolute symbol whose *address*
  equals the blob size

The Makefile rule:

```make
$(HELLO_EMBED): $(HELLO_STRIPPED)
	cd $(dir $<) && cp $(notdir $<) hello.elf.bin && \
	    $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	        --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	        hello.elf.bin $(notdir $@) && \
	    rm hello.elf.bin
```

Three details worth flagging:

- **`-O elf64-littleaarch64 -B aarch64`** forces the wrapper
  object to be aarch64-shaped. Without these flags `objcopy`
  emits an object in the host's default architecture (x86_64 on
  most build hosts), which the kernel link rejects.
- **The rename hides a usability nit.** `objcopy -I binary`
  puts the blob in a `.data` section; we rename it to
  `.rodata.embedded_user` so it lands in the kernel's rodata
  segment (read-only, executable-permitted, but the loader
  copies it out anyway).
- **The temporary `cp` rename** controls the symbol name. The
  symbol is derived from the input filename by replacing every
  non-identifier character with `_`. Renaming the file to
  `hello.elf.bin` before passing it to `objcopy` produces
  symbols of the form `_binary_hello_elf_bin_start`, which is
  what `kernel/core/embedded_user.h` declares.

The kernel link script then captures the section into rodata:

```ld
.rodata : ALIGN(4K) {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    . = ALIGN(8);
    *(.rodata.embedded_user)
} :text
```

Adding more user binaries means adding another wrapper rule and
another set of declarations in `embedded_user.h`. The order
inside the section is whatever order the linker sees the objects
in — fine for milestone 7 because we look each one up by symbol.

## Strip before embedding

We `objcopy --strip-all` the user ELF before wrapping it. Without
this, the embedded blob carries debug info, the symbol table, and
section name strings that the loader doesn't need. For the hello
binary the savings are modest (a few KB), but they grow quickly
once user code includes string literals and inline functions. The
*loader* still needs the program headers and the `.text`/`.data`
contents, but those are not stripped — `--strip-all` only removes
debug and symbol-table information, leaving the loadable segments
untouched.

## The loader

`elf_load_user(data, size, &out)` does six things:

1. Validate the ELF header — magic bytes `0x7F 'E' 'L' 'F'`,
   class 64, little-endian, type EXEC, machine `EM_AARCH64`
   (183).
2. Walk every program header. For each `PT_LOAD`:
   a. Round `p_memsz` up to a whole number of pages.
   b. Allocate that many *contiguous* physical pages from pmem.
      Verification: `pmem_alloc_page` returns pages
      highest-address-first, so each subsequent call should
      return exactly `previous - PAGE_SIZE`. If it doesn't,
      something earlier in the boot sequence consumed pages out
      of order — abort.
   c. Copy `p_filesz` bytes from `data + p_offset` to
      `base_pa`. The remaining `p_memsz - p_filesz` bytes (BSS)
      are already zero because `pmem_alloc_page` zero-fills
      every page it hands out.
3. Compute the *load-time* entry point: `segment_load_pa +
   (e_entry - first_vaddr)`. Because we load the single segment
   contiguously, this gives us the runtime address of whatever
   instruction `e_entry` named.
4. Allocate a separate user stack of `USER_STACK_PAGES` (4 ×
   4 KiB = 16 KiB) contiguous pages from pmem.
5. Set `out->stack_top_va = stack_first + PAGE_SIZE`. That is
   the address one byte past the top of the stack region — the
   correct initial value for SP on a downward-growing stack.
6. Return 0.

A subtle bug we hit on the way: the first version computed
`out->stack_top_va` *and* `out->entry_va` to the same numerical
value (`0x22fffb000`). That's not a bug — the two regions abut at
that address, with the segment going up from `0x22fffb000` and
the stack going down from `0x22fffb000`. But it looks
suspicious in the kernel log, and it would be a real bug if the
segment were larger than one page (because then the segment top
would extend past where the stack region ends). The current code
allocates the segment first and the stack second, so pmem hands
out lower addresses for the stack — which is what we want.

## The PC-relative trick, in detail

The user binary is linked at VA `0x100000`, but loaded at PA
`0x22fffb000` (or wherever pmem decides). For this to work, every
instruction in the user code that references an address must
either be position-independent or must reference an address that
the loader actually mapped at the link-time VA.

GCC compiled with `-fno-pic -fno-pie` does *not* emit absolute
relocations for code references; it emits `adrp` + `add` pairs
that are PC-relative under the hood:

```asm
adrp x0, label    ; loads (current PC & ~0xFFF) + page-offset-to-label
add  x0, x0, #imm ; adds in-page offset
```

The linker resolves the relocation `R_AARCH64_ADR_PREL_PG_HI21`
by computing `(target_page - instruction_page) >> 12`. That
*difference* is invariant under any whole-binary relocation,
because both addresses move together. So even though we're loaded
at `0x22fffb000` instead of `0x100000`, every `adrp` produces the
right answer.

Branches (`bl`, `b`) use `R_AARCH64_CALL26` and friends, which
are also pure PC-relative. The 26-bit immediate is signed and
multiplied by 4, giving a ±128 MiB range — enormous for our
single-segment binaries.

We have to be careful never to introduce *absolute* relocations.
The compiler can emit one if it sees a function pointer that
escapes; the linker would then emit `R_AARCH64_ABS64`, which
hard-codes the link-time VA into a writable slot in the binary.
Our hello program doesn't do this, but it's worth checking with
`readelf -r build/userspace/hello/hello.elf` before any new user
program goes live.

## The user CRT

`crt0.S` is the entry point the linker bakes into the ELF header
as `e_entry`. Its job is minimal:

```asm
.global _user_start
_user_start:
    bl      main
    mov     x8, #2          /* SYS_EXIT */
    svc     #0
1:  wfe
    b       1b
```

No BSS zeroing — the kernel's `pmem_alloc_page` zeroes every
page on hand-out, so the BSS portion of `p_memsz - p_filesz` is
guaranteed-zero. No argv/envp setup either; that's a Chapter 17
concern. If `main` returns, we invoke `SYS_EXIT` with its return
code so the kernel can reap the thread cleanly.

The `wfe` loop after the SVC is belt-and-braces. `SYS_EXIT`
should not return, but if a future bug ever makes it do so, the
user thread parks instead of falling off the end of the segment
into garbage.

## The first user program

```c
#include "../libc/syscall.h"

int main(void)
{
    puts("hello from EL0!");

    int pid = getpid();
    char buf[32];
    char *p = buf;
    *p++ = 'p'; *p++ = 'i'; *p++ = 'd'; *p++ = '=';
    *p++ = '0'; *p++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned nib = (unsigned)((pid >> shift) & 0xF);
        *p++ = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
    }
    *p++ = '\n';
    write(1, buf, (size_t)(p - buf));

    yield();
    puts("after yield, still alive");

    return 0;
}
```

This program exists to **prove all four syscalls work, in a
minimum number of source lines**. It calls:

- `puts` → `write(1, ..., ...)` → `SYS_WRITE`
- `getpid` → `SYS_GETPID`
- `write` again with a hand-formatted hex PID
- `yield` → `SYS_YIELD`
- `puts` after the yield (to prove control returns from the
  scheduler)
- implicit `exit(0)` from crt0 → `SYS_EXIT`

The hand-rolled hex formatting is intentional. `printf` is not
available at EL0 (we don't have a libc; we don't even have a
heap). Writing this routine at EL0 once shows just how thin the
syscall surface is.

## Putting it together: the kernel-side smoke test

```c
static void userspace_demo(void)
{
    serial_puts("\n[user] loading embedded hello.bin (");
    serial_puthex((uint64_t)embedded_hello_size());
    serial_puts(" bytes)\n");

    struct user_image img;
    if (elf_load_user(embedded_hello_data(),
                      embedded_hello_size(), &img) != 0) {
        serial_puts("[user] FATAL — elf_load_user failed\n");
        return;
    }
    serial_puts("[user] entry = ");
    serial_puthex(img.entry_va);
    serial_puts(", sp = ");
    serial_puthex(img.stack_top_va);
    serial_puts("\n");

    struct thread *u = user_thread_create(img.entry_va,
                                          img.stack_top_va,
                                          "hello");
    serial_puts("[user] spawned pid ");
    serial_puthex((uint64_t)u->id);
    serial_puts("\n");

    while (thread_count() > 1)
        yield();

    serial_puts("[user] hello exited cleanly\n\n");
}
```

The `while (thread_count() > 1)` loop is the boot thread
cooperatively yielding until the user thread reaches `SYS_EXIT`
and is reaped. Without it, the boot thread would race past the
user-thread launch and start the `wfe` heartbeat loop before the
user thread even got CPU time.

## What we *didn't* build

This chapter is about the smallest end-to-end thing that runs at
EL0. There are several things you might expect from a real ELF
loader that we are deferring:

- **Per-process page tables.** Every user process shares the
  same TTBR0_EL1 table the kernel uses. They can read each
  other's memory and they can read kernel pages allocated from
  pmem. Chapter 17 fixes this by giving each process its own L1
  table.
- **`fork`/`execve`.** No process control yet. The kernel
  spawns user threads directly via `user_thread_create`. There
  is no parent/child relationship, no copy-on-write, no
  argv/envp.
- **A real syscall surface.** Four calls (`write`, `exit`,
  `getpid`, `yield`) is not a usable userspace. Chapter 16
  adds files, the VFS, and `read`/`open`/`close`.
- **Dynamic linking.** Every user binary is statically linked
  with all its dependencies (currently just the inline
  syscall wrappers). No shared libraries, ever — at least not
  in the milestones we plan to ship.
- **Loading from disk.** The user binary is *embedded* in the
  kernel image. Chapter 16's VFS makes it possible to load
  user binaries off virtio-blk; until then, every new user
  program means rebuilding the kernel.

Each of those is a chapter in its own right. The fact that
milestone 7 is so small — under 600 lines of new code, end to
end — is precisely because we drew the line at "first user
program runs and exits cleanly", and not a feature further.

## Summary

- The user program is built with its own link script
  (`userspace/linker_user.ld`) at VA `0x100000`. A single
  PT_LOAD segment with RWX permissions keeps things simple.
- `objcopy -I binary -O elf64-littleaarch64` wraps the user
  ELF as an embeddable kernel object exposing
  `_binary_<name>_start/_end/_size` symbols.
- `elf_load_user` walks the program headers, allocates pmem
  pages for each PT_LOAD, copies the bytes, allocates a
  separate user stack, and returns the runtime entry PC + SP.
- The user binary uses only PC-relative addressing, so loading
  it at any 4 KiB boundary works without runtime relocations.
- `crt0.S` calls `main` and falls into `SYS_EXIT` if `main`
  returns.
- The `userspace_demo` in `kernel_main` boots → loads → spawns
  → waits → cleans up the first user thread, exercising every
  milestone-7 path in 30 lines of kernel C.

Next chapter: [Files, VFS, and a simple file system](16-files-and-vfs.md)
(coming with the next milestone).
