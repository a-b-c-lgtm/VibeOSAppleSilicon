# Chapter 156 — Bootstrap glue: crt0, atexit, libgcc stubs

> **Milestone in this chapter:** ship the runtime every in-guest
> compiler will assume — a real `crt0`, an `atexit` table, and
> the tiny libgcc stubs (`__udivti3` and friends).
> **Code referenced:**
> - [userspace/crt/crt0.S](../../../userspace/crt/crt0.S)
> - [userspace/libc/atexit.h](../../../userspace/libc/atexit.h)
> - The libgcc stub TU
>
> **At the end of this chapter** you will have a `crt0.o` that
> runs constructors, calls `main`, runs the `atexit` chain on
> the way out, and exits cleanly — proven by a 6-line demo
> whose six lines of output must come out in the correct
> order. Builds on chapter 155 (`/bin/ld`).

## Why this chapter exists

[Chapter 155](155-bin-ld-linker.md) gave us `/bin/ld`.
Anything you link with it starts at whatever symbol you
pass to `-e` (defaulting to `_user_start`). For our own
hand-written C programs that's fine — the existing
`crt0.S` just loaded `argc`/`argv` from the kernel-laid-
out user stack and tail-called `main`.

Real compilers — even our small `/bin/cc` in
[chapter 157](157-bin-cc.md), and any future GCC port
(Part XVIII) — don't target a hand-written entry
point. They emit code that assumes:

1. A C startup file (`crt0.o`) walks
   `__init_array[]` so functions tagged
   `__attribute__((constructor))` run *before* `main`.
2. After `main` returns, `__cxa_finalize(NULL)` runs the
   atexit chain in LIFO order and then walks
   `__fini_array[]` so `__attribute__((destructor))`
   functions run in reverse.
3. A handful of helper symbols from `libgcc.a` exist
   for ops the ISA can't do in one instruction
   (`__popcountdi2`, `__clzdi2`, `__ctzdi2`,
   `__stack_chk_fail`).

If any of those pieces is missing, the smallest possible
C program won't link — or worse, it links but the
destructors silently never fire and a heap leak first
shows up 80 chapters from now.

This chapter writes the four pieces and proves they're
correct end-to-end with `userspace/atexittest/`.

## What this chapter adds

| Component | File | Role |
|---|---|---|
| crt0 rewrite | [userspace/crt/crt0.S](../../../userspace/crt/crt0.S) | argc/argv load → `__init_array` walk → `main` → `__cxa_finalize` → `SYS_EXIT`. |
| atexit header-only libc | [userspace/libc/atexit.h](../../../userspace/libc/atexit.h) | `atexit()` LIFO slot table + strong `__cxa_finalize` override. |
| libgcc stubs | [userspace/libc/libgcc.h](../../../userspace/libc/libgcc.h) | `__popcountdi2`, `__clzdi2`, `__ctzdi2`, `__stack_chk_fail`. |
| Linker script patch | [userspace/linker_user.ld](../../../userspace/linker_user.ld) | `.init_array`, `.fini_array`, `.got`, `.got.plt` sections + `PROVIDE_HIDDEN` bounds. |
| Demo binary | [userspace/atexittest/atexittest.c](../../../userspace/atexittest/atexittest.c) | 6-line app exercising constructors, atexit, destructors. |
| Build wiring | [Makefile](../../../Makefile) | `ATEXITTEST_*` rules, mkosfs manifest entry. |
| Smoke test | [scripts/test_atexit.py](../../../scripts/test_atexit.py) | 11 assertions on output ordering and exit code. |

## The new crt0, end to end

The entry point is `_user_start`. The kernel's ELF
loader still places it in the first PT_LOAD segment;
nothing about how a process boots has changed. What
changed is what `_user_start` *does*.

```asm
_user_start:
    ldr     x0, [sp]               ; argc
    add     x1, sp, #8             ; &argv[0]

    mov     x19, x0                ; save argc/argv across ctors
    mov     x20, x1
    adrp    x21, __init_array_start
    add     x21, x21, :lo12:__init_array_start
    adrp    x22, __init_array_end
    add     x22, x22, :lo12:__init_array_end
0:  cmp     x21, x22
    b.eq    1f
    ldr     x0, [x21], #8          ; fn pointer, post-increment
    blr     x0
    b       0b
1:  mov     x0, x19                ; restore argc/argv
    mov     x1, x20

    bl      main
    mov     x19, x0                ; save rc across destructors

    mov     x0, #0
    bl      __cxa_finalize         ; atexit chain + .fini_array

    mov     x0, x19
    mov     x8, #2                 ; SYS_EXIT
    svc     #0
```

Two contract points worth pinning down:

- **`x19`/`x20` are AAPCS callee-saved.** Constructors
  are ordinary C functions and must preserve them. We
  rely on that to thread `argc`/`argv` past
  arbitrary user code into `main`.
- **`__init_array_start` and `__init_array_end` are
  always defined**, even in a binary that registered
  zero constructors. The linker script's `PROVIDE_HIDDEN`
  emits them as bounds of the (possibly empty) section.
  The loop is then a no-op: start == end.

After `main`, the same trick on the destructor side:
`__cxa_finalize` is called *unconditionally* with arg
`NULL`. A binary with no atexits / no destructors still
calls it; the call resolves to a weak no-op (see
"The two `__cxa_finalize` symbols" below).

## The two `__cxa_finalize` symbols

`bl __cxa_finalize` in crt0 generates a `R_AARCH64_CALL26`
relocation. If the symbol were merely undefined the link
would fail. If the symbol were strong-defined in
[crt0.S](../../../userspace/crt/crt0.S) the strong defn
would always win — there'd be no way for
[atexit.h](../../../userspace/libc/atexit.h) to install a
"do something useful" version.

The fix is the classic weak-default / strong-override
pair:

- crt0.S provides a `.weak __cxa_finalize` that's a one-
  instruction `ret`. This guarantees a binary that
  doesn't include `atexit.h` still links.
- `atexit.h` provides a *non-weak* `void
  __cxa_finalize(void *)` that walks the slot table and
  `.fini_array`. The strong override silently displaces
  the weak default at link time.

```c
/* userspace/libc/atexit.h */
#define ATEXIT_MAX 32
static void (*g_atexit_fns[ATEXIT_MAX])(void);
static int   g_atexit_n;
static int   g_atexit_in_finalize;

static int atexit(void (*fn)(void))
{
    if (!fn || g_atexit_n >= ATEXIT_MAX) return -1;
    g_atexit_fns[g_atexit_n++] = fn;
    return 0;
}

void __cxa_finalize(void *dso_handle)
{
    (void)dso_handle;
    if (g_atexit_in_finalize) return;     /* re-entry guard */
    g_atexit_in_finalize = 1;
    while (g_atexit_n > 0) {
        void (*fn)(void) = g_atexit_fns[--g_atexit_n];
        if (fn) fn();
    }
    extern void (*__fini_array_start[])(void) __attribute__((weak));
    extern void (*__fini_array_end[])(void)   __attribute__((weak));
    if (__fini_array_start && __fini_array_end) {
        void (**p)(void) = __fini_array_end;
        while (p > __fini_array_start) { p--; if (*p) (*p)(); }
    }
}
```

The re-entry guard matters: if an atexit handler itself
calls `_exit()`, the kernel re-enters `_user_start`?
No — `_exit` is `SYS_EXIT` and terminates. The guard
exists for the C++-style "destructor calls another
destructor that registered itself" pattern; we don't
generate that today, but the cost is one byte of
`.bss` and one branch.

Capacity is fixed at 32 slots. Real libc grows the table
via `realloc`; we don't, because we don't ship any binary
that needs more than two slots. Overflow returns -1 from
`atexit` (POSIX-conformant) and is silently dropped.

## The `.init_array` and `.fini_array` contract

The C runtime ABI says global constructors and
destructors live in two specially-named output sections
whose contents are arrays of function pointers. The
linker is responsible for:

1. Collecting `.init_array.NNN` / `.fini_array.NNN`
   input sections, sorting by NNN (init priority),
   appending the unsuffixed `.init_array` / `.fini_array`
   at the end.
2. Defining the symbols `__init_array_start`,
   `__init_array_end`, `__fini_array_start`,
   `__fini_array_end` to bound the resulting arrays.

We do both in `userspace/linker_user.ld`:

```ld
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
```

`KEEP` is required because the linker's `--gc-sections`
pass would otherwise drop these arrays (no symbol
references them by name — crt0 only ever uses the
bounds). `PROVIDE_HIDDEN` makes the bound symbols
participate in resolution but not show up in dynamic
symbol tables (irrelevant for us — we don't link
dynamic — but it's the canonical incantation).

`SORT_BY_INIT_PRIORITY` honours `__attribute__((init_priority(N)))`,
which the demo doesn't use but a future libc port would.

## The `.got` trap

This one cost a build. Before adding the weak
`__cxa_finalize`, no userspace binary in this repo had a
GOT. As soon as crt0 contained `bl __cxa_finalize` with
`__cxa_finalize` declared `.weak`, the linker's
internal logic decided "the call target might come from
elsewhere; I need a GOT slot for it" and silently
generated `.got`, `.got.plt`, and `.rela.got` output
sections.

Our `USER_LDFLAGS` include `--orphan-handling=error` —
the very flag whose entire purpose is to refuse to
silently invent placements. The build failed every
binary with:

```
aarch64-elf-ld: error: unplaced orphan section `.rela.got'
  from `build/userspace/crt/crt0.o'
aarch64-elf-ld: error: unplaced orphan section `.got'
  from `build/userspace/crt/crt0.o'
aarch64-elf-ld: error: unplaced orphan section `.got.plt'
  from `build/userspace/crt/crt0.o'
```

`objdump -h crt0.o` confirms the input had no `.got*`
sections — the linker created them. The fix is to give
each one a home explicitly:

```ld
.got       : ALIGN(8) { *(.got) }                :load
.got.plt   : ALIGN(8) { *(.got.plt) }            :load
```

And to send the relocation table to `/DISCARD/` (we
don't need runtime fixups — every binary is statically
linked and loaded at a fixed VA):

```ld
/DISCARD/ : {
    ...
    *(.rela.got)
}
```

In practice these sections end up empty for every
binary we ship today, because no actual GOT entry is
needed at the chosen layout. The placement is there so
the linker doesn't have to invent one.

## libgcc helpers

GCC at `-Os` on aarch64 can emit calls to a small set of
helpers when the operation doesn't compress into one
instruction. The four we know are reachable from C code
we expect to compile in chapter 157+:

| Symbol | What | Implementation |
|---|---|---|
| `__popcountdi2(int64_t)` | popcount low 64 bits | bit-walk loop |
| `__clzdi2(int64_t)` | count leading zeros | left-shift until MSB set |
| `__ctzdi2(int64_t)` | count trailing zeros | right-shift until LSB set |
| `__stack_chk_fail()` | SSP trap | `write(2, "*** stack smashing detected ***\n", 32); _exit(127)` |

All four live in
[userspace/libc/libgcc.h](../../../userspace/libc/libgcc.h)
as `static` functions. The header-only convention
matches `printf.h`, `malloc.h`, and the chapter 148
libc additions: each TU that includes the header gets
its own copy. That's fine for now — these symbols
aren't called from many places and the dedup cost is
trivial. If a future chapter packages these as a real
`/lib/libgcc.a`, the bookkeeping is bit-for-bit
compatible with our `/bin/ar` from chapter 155.

The deliberately-missing items:

- **`__udivti3` / `__divti3` / `__multi3` (128-bit
  arithmetic).** No C source we ship triggers these,
  and our small `/bin/cc` never emits 128-bit
  arithmetic. They would land the day a higher-quality
  compiler port (Part XVIII GCC, or similar) emits a
  division-heavy translation.
- **`__aarch64_ldadd*_acq_rel` (pre-LSE atomics).** Our
  `-mcpu=cortex-a72` has LSE; GCC lowers atomic ops to
  single instructions. The helpers would only be
  needed if we ever downshifted to `cortex-a53`.
- **`__stack_chk_guard`.** SSP needs a randomised
  cookie; we have no RNG hooked into libc yet. Chapter
  120 ships only the *failure* path so a binary
  compiled with `-fstack-protector-strong` still links.
  Wiring `__stack_chk_guard` to a real entropy source
  is a chapter-130-ish concern.

## The demo: `userspace/atexittest`

The full source is 50 lines. It exercises every piece of
the new runtime and prints six tagged lines whose order
is the entire test surface:

```c
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/atexit.h"

static void __attribute__((constructor)) ctor1(void)
    { printf("ctor1\n"); }
static void __attribute__((constructor)) ctor2(void)
    { printf("ctor2\n"); }

static void exit1(void) { printf("exit1\n"); }
static void exit2(void) { printf("exit2\n"); }

static void __attribute__((destructor)) dtor(void)
    { printf("dtor\n"); }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("main\n");
    atexit(exit1);
    atexit(exit2);
    return 7;
}
```

Expected output:

```
ctor1
ctor2
main
exit2
exit1
dtor
```

Followed by exit code 7. Notice the orderings the test
enforces:

- Both `ctorN` come **before** `main` — proves
  `__init_array` is walked by crt0 prologue.
- `exit2` comes **before** `exit1` — proves the atexit
  chain is LIFO.
- `dtor` comes **last** — proves `.fini_array` runs
  *after* the atexit chain, not before.

[scripts/test_atexit.py](../../../scripts/test_atexit.py)
verifies all four orderings plus the exit code via 11
assertions.

## tmpfs exec gap, again

Chapter 155 already taught us that any filesystem opting
into exec needs a `load` op in its `fs_ops` vtable
(returning `long` — not `int`, see the chapter-119
memory note). Chapter 156's atexittest doesn't poke at
tmpfs — but if you ever try to `cp atexittest /tmp/foo
&& /tmp/foo`, the chapter 155 fix is what makes that
work. Reminder: the kernel patch is in
[kernel/core/tmpfs.c](../../../kernel/core/tmpfs.c) at
`tmpfs_op_load`.

## Lessons

- **A weak symbol referenced via `CALL26` triggers
  internal GOT generation.** This is true even when
  both definition and use are in the same TU, even when
  no relocation actually needs to indirect. The linker
  doesn't know in the abstract that the weak symbol
  won't be displaced by something requiring a GOT slot
  later, so it provisions space. Any project using
  `--orphan-handling=error` *must* place `.got`,
  `.got.plt`, and `.rela.got` (or `/DISCARD/` the last)
  to avoid the build break.
- **`PROVIDE_HIDDEN` always defines both bounds.** This
  is the entire reason `_user_start` can call the
  init-array walk loop unconditionally — even binaries
  with no constructors get well-defined empty bounds.
- **AAPCS callee-saved registers are the right place to
  park `argc`/`argv` across constructor calls.** Using
  `x0`/`x1` would force the loop to save/restore them
  every iteration. Using `x19`/`x20` makes the prologue
  a fixed cost paid once.
- **The atexit re-entry guard is one byte of `.bss`.**
  Worth it. If a destructor accidentally registers
  another destructor, the unguarded version would loop
  forever and hang the test forever; the guard fails
  closed.

## Applied to

- **Existing apps:** every binary in `userspace/`
  relinks against the rewritten crt0 + linker script.
  Behaviour for binaries that registered no
  constructors / destructors / atexits is identical to
  chapter 155 — `_user_start` does the trivial path
  (empty init/fini array bounds, weak no-op
  `__cxa_finalize`).
- **New apps:** `userspace/atexittest/` — first binary
  in this codebase that depends on the new runtime
  ordering.
- **New tests:** `scripts/test_atexit.py` — 11
  assertions on output ordering and exit code.
- **Existing tests upgraded:** none required. The full
  19-test regression sweep is green
  (`test_atexit` + the 18 from chapter 155).

## What gets exercised in tests

```
[chapter 156] /bin/atexittest smoke test
PASS: ctor1 ran
PASS: ctor2 ran
PASS: main ran
PASS: exit1 ran
PASS: exit2 ran
PASS: dtor ran
PASS: both ctors ran BEFORE main (__init_array walk)
PASS: main ran before atexit handlers
PASS: exit2 ran before exit1 (atexit LIFO)
PASS: dtor ran AFTER atexit chain (.fini_array last)
PASS: /bin/atexittest exited with code 7 via crt0 forwarder

11 PASS / 0 FAIL
```

Full sweep:

```
test_atexit                    PASS
test_bin_as                    PASS
test_bin_ld_ar                 PASS
test_libc_stat                 PASS
test_libc_errno                PASS
test_libc_stdio                PASS
test_libc_env                  PASS
test_boot_to_desktop           PASS
test_userfs_echo               PASS
test_clipboard                 PASS
test_mount_ro                  PASS
test_userfs_timeout            PASS
test_httpd_forward             PASS
test_browser_proxy             PASS
test_cow                       PASS
test_fork_exec                 PASS
test_busy_on_mix               PASS
test_clone_files               PASS
test_directories               PASS

SUMMARY: 19 PASS / 0 FAIL out of 19
```

## Deferred

- 128-bit divides (`__udivti3`, `__divti3`, `__umodti3`).
  Deferred until a compiler port emits them; `/bin/cc`
  does not.
- `__stack_chk_guard` wired to a real RNG. Currently
  only the failure handler is provided; an actual
  smash isn't yet detectable because the cookie is
  uninitialised. The compiler still accepts
  `-fstack-protector-strong` because the symbol just
  has to *exist*.
- Pre-LSE atomic helpers — irrelevant for
  `-mcpu=cortex-a72` but would need shipping if we
  ever target a smaller core.
- `.preinit_array` walk. We honour `.init_array` only.
  No tool we ship emits preinit entries, but a full
  glibc-style crt0 would walk preinit *before* init.
- C++ DSO-handle finalization ordering — we ignore the
  `dso_handle` argument to `__cxa_finalize` and always
  finalize everything.

## Next

[Chapter 157 — `/bin/cc`](157-bin-cc.md) is the first
chapter where a binary on disk is built *inside the
guest* and uses libgcc.h-style stubs for code the
compiler can't lower itself.
