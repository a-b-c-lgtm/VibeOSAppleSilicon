# Chapter 27 — `argc`, `argv`, and the user stack

For ten chapters our user programs have all looked the same:

```c
int main(void)
{
    /* ... */
}
```

When `cat` needed a path it called a custom `getargs()` syscall
that copied a single string out of `thread.args` (a kernel-side
char buffer the shell stuffed). That worked for `cat /motd` but
broke down the moment you wanted `cmd arg1 arg2 arg3`: the kernel
was just shipping the raw shell-line tail and asking the program
to re-tokenize it.

This chapter retires the cheat. Programs now see the standard
Unix entry point:

```c
int main(int argc, char **argv);
```

with `argc` and `argv` laid out on the user stack by the kernel
ELF loader before the first instruction runs.

## The Unix process-startup contract

System V on AArch64 says: when control reaches `_start` (our
`crt0._user_start`), the stack pointer points at `argc`, with the
following layout above it (in increasing addresses):

```
sp        argc            (8 bytes; held in a 64-bit slot)
sp+8      argv[0]         pointer to argv[0] string
sp+16     argv[1]         pointer to argv[1] string
...
sp+8(argc)  NULL          argv terminator
sp+8(argc+1)  NULL        envp terminator (no env yet)
...
<padding to 16-byte align>
<the argv strings themselves, NUL-terminated, packed>
<top of stack region>
```

Two AAPCS rules pin this down:

- `sp` must be 16-byte aligned at function call boundaries — so
  the loader rounds the total layout up to a multiple of 16.
- The first two integer args to `main` go in `x0` and `x1` — so
  `crt0` just loads `argc` from `[sp]` and computes
  `&argv[0] = sp + 8` before branching to `main`.

That's the whole contract. There's no kernel involvement *during*
the call to `main`; the kernel just laid out memory before
issuing `eret` into user mode.

## Where the loader writes

The user stack is four pages, mapped at `[USER_STACK_TOP-16K,
USER_STACK_TOP)`. The loader writes the argv blob into the
**topmost** page only — that's the one whose VA range is
`[USER_STACK_TOP-PAGE_SIZE, USER_STACK_TOP)`. Bounding the layout
to one page keeps the math simple and fits any sane command line.

Here's where the timing gets subtle: the per-process address
space is *not active* when `elf_load_user` runs. We're still on
the boot L1 in `TTBR0_EL1`. So we can't write to `USER_STACK_TOP`
directly — there's no mapping for that VA in the boot tables. We
write via the **physical address** of the top stack page (which
is identity-mapped in the boot L1, our usual lever). To do that
we have to remember the PA when we allocate the page:

```c
for (size_t k = 0; k < USER_STACK_PAGES; k++) {
    uint64_t pa = pmem_alloc_page();
    /* ... map at va = stack_va_bot + k * PAGE_SIZE ... */
    if (k == USER_STACK_PAGES - 1) top_page_pa = pa;
}
```

The topmost page is `k == USER_STACK_PAGES - 1`. Once we have
its PA, we treat the page as a flat byte buffer and lay out
`argc`, the argv pointer table, the envp NULL, and the strings
one after another.

The trickiest detail: `argv[i]` is a *user-VA* pointer, not a
physical address. While we're writing via the PA, we still need
each `argv[i]` slot to hold a value the user code can dereference
later. Easy: the strings live at known offsets within the top
page, and each VA is just `USER_STACK_TOP - strings_bytes +
offset_within_strings`.

## The full layout function

```c
static int build_user_init_stack(uint64_t top_page_pa,
                                 const char *const argv[],
                                 uint64_t *sp_out)
{
    int    argc = 0;
    size_t strings_bytes = 0;
    if (argv) {
        for (; argv[argc]; argc++) {
            if (argc >= MAX_USER_ARGV) return -1;
            strings_bytes += strlen(argv[argc]) + 1;
        }
    }

    size_t vector_bytes = 8                      /* argc */
                        + (argc + 1) * 8         /* argv[]+NULL */
                        + 8;                     /* envp NULL */
    size_t total = (vector_bytes + strings_bytes + 15) & ~15;
    if (total > PAGE_SIZE) return -1;

    uint8_t *page = (uint8_t *)(uintptr_t)top_page_pa;
    /* zero whole page, then write strings at top, vector at bottom */
    /* ... see kernel/core/elf.c for the full code ... */

    *sp_out = USER_STACK_TOP - total;
    return 0;
}
```

The new SP is what `out->stack_top_va` returns from the loader.
`user_thread_create` plumbs that value through to the trampoline
that sets `SP_EL0` before `eret`-ing to EL0.

## The new `crt0`

Three lines longer than the old one:

```asm
_user_start:
    ldr     x0, [sp]            /* x0 = argc        */
    add     x1, sp, #8          /* x1 = &argv[0]    */
    bl      main
```

Programs that declare `int main(void)` simply never read `x0` or
`x1`, which AAPCS allows. So `hello.c` keeps compiling unchanged
— the new ABI is fully backward compatible at the source level.

## How the shell tokenizes

The shell still calls `spawn(path, args_string)` exactly as
before:

```c
int tid = spawn(line, args);   /* args = "hello argv world" */
```

The kernel side of `sys_spawn` does the splitting. After copying
`args` into a kernel-side scratch buffer, it walks the buffer
splitting on whitespace, NUL-terminating each token in place, and
collecting pointers into a stack array:

```c
const char *argv[MAX_SPAWN_ARGV + 1];
int argc = 0;
argv[argc++] = path;        /* argv[0] is always the program path */
char *p = args_split;
while (*p && argc < MAX_SPAWN_ARGV) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p = '\0'; p++; }
}
argv[argc] = NULL;
```

Then it hands `argv` to `elf_load_user`, which copies the strings
into the user stack (because the kernel scratch buffer goes away
the moment the syscall returns). Note: this is the dumbest shell
tokenizer that could possibly work — no quoting, no escaping, no
glob expansion. We'll grow that out when it starts hurting.

`thread.args` and `SYS_GETARGS` still exist for backward compat
but no in-tree program uses them any more. They'll go away in a
later cleanup.

## Verification

A new test program, `echo`, prints its args:

```c
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) write(1, " ", 1);
        write(1, argv[i], strlen(argv[i]));
    }
    write(1, "\n", 1);
    return 0;
}
```

Three test runs:

```
$ /bin/echo hello argv world
hello argv world
$ /bin/echo just_one
just_one
$ /bin/echo
                                   ← blank line, argc==1
```

The init thread's startup frame validates the math too — the
kernel logs the SP it picked:

```
[user] entry = 0x0000001000100000, sp = 0x000000103fffffd0
```

`USER_STACK_TOP = 0x1040000000`. SP is `0x30` below that, i.e.
48 bytes. For `argv = { "init", NULL }`:

- vector = `8 + 2*8 + 8 = 32` bytes
- strings = `"init\0"` = 5 bytes
- total = `(32 + 5 + 15) & ~15 = 48` ✓

All other regression tests still pass: heaptest, cat
(now using `argv[1]`), hello, badpoke, badptr.

## What this unlocks

- Multi-argument utilities: `cp src dst`, `mkdir -p path`, etc.
  We don't have those yet, but their entry-point ABI is now
  correct and they'll plug in naturally.
- `int main(int, char **)` matches what the System V ABI says
  programs should expect, so toolchain support for things like
  argv-aware libc init code (when we get a real libc) drops in
  without surgery.
- We're one step closer to a real `exec` syscall, which will
  rebuild the AS in place and re-run this same loader path.

## What's still missing

- **No `envp` yet.** The loader writes a single NULL where
  `envp[0]` would go. When we add environment variables, the
  layout extends naturally — push envp pointers between the
  argv NULL terminator and the strings region.
- **No auxv.** Real ELF startup has an auxiliary vector with
  things like `AT_PHDR`, `AT_PAGESZ`, `AT_RANDOM`. We don't ship
  any of these because we have no dynamic linker, no security
  hardening that needs ASLR seed, and no ELF programs that look
  for them. They'd extend the same layout convention.
- **No quoting in the shell tokenizer.** `echo "hello world"`
  becomes three arguments, not one. Trivial to fix once we care.
- **`MAX_SPAWN_ARGV = 16` and one-page total cap.** Both are
  arbitrary defensive limits. Bigger when needed; until something
  hits them, smaller is simpler.

## What changed

```
kernel/core/elf.{h,c}            argv parameter, build_user_init_stack
kernel/core/syscall.c            sys_spawn tokenizes args -> argv
kernel/core/main.c               banner -> milestone 18; init argv
userspace/crt/crt0.S             loads argc/argv from sp
userspace/cat/cat.c              uses argv[1] instead of getargs()
userspace/echo/echo.c            NEW — multi-arg argv test
Makefile                         echo target + disk image entry
```

## Postscript (milestone 64): why 16 KiB stopped being enough

The "four pages, mapped at `[USER_STACK_TOP-16K, USER_STACK_TOP)`"
claim above held all the way through the GUI, the TCP stack, and
the first browser milestones — right up until we tried to render
a real Hacker News comment thread. The first click into
`/news.ycombinator.com/item?id=...` produced this on the serial
port:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x0000000092000047
        EC      = 0x24                 (data abort, EL0)
        FAR_EL1 = 0x000000103fffbfe0   (32 B *below* stack base)
        ELR_EL1 = 0x00000010001005cc   (function prologue)
```

The FAR is the giveaway. With four pages of stack the bottom of
the mapped region is `USER_STACK_TOP - 4*PAGE_SIZE =
0x103fffc000`, and `0x103fffbfe0` is exactly 32 bytes below that.
ELR landed on a `stp x29, x30, [sp, #-32]!` — a function
prologue trying to push its frame after `sp` had already walked
off the bottom of the stack. Disassembling the binary at ELR
showed `css_match_chain`, the recursive selector matcher in
`userspace/libc/css.h`.

The call graph is the rest of the story. `layout_build_subtree`
recurses to DOM depth, and HN wraps each comment in 4-5 levels
of `<table>/<tbody>/<tr>/<td>`. A 50-comment thread is ~250
frames deep, and each level calls `layout_resolve` which calls
`css_rule_matches` which calls `css_match_chain` which itself
recurses on the selector chain. At a few dozen bytes of frame
overhead per call, 16 KiB runs out partway through the document.

The surgical fix is a one-word change: bump `USER_STACK_PAGES`
from 4 to 16, in both `kernel/arch/address_space.h` and
`kernel/core/elf.c` (the loader has a local `#define` shadow that
must be kept in sync with the header). The argv builder still
writes to the topmost page only, so the bump is a pure capacity
increase — no layout change, no ABI change, no behavioural
difference for any existing program.

The deeper fix — making `layout_build_subtree` iterative — is
the right thing to do eventually, and so is adding a guard page
below the stack so the next overflow produces a message that
actually says "user stack overflow" rather than a generic
translation fault. Both are deferred until a page demands more
depth than 64 KiB allows.

### Recognising the signature

If a future user-mode crash shows ESR EC=0x24 (data abort from
EL0) and FAR within a few hundred bytes below the bottom of the
mapped stack, it is almost certainly a stack overflow. The
ELR-disassembled function tells you *where* the recursion ran
out, but the cure is usually upstream — in whichever caller
produces the unbounded depth.

