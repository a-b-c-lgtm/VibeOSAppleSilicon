# Chapter 132f — `gcc hello.c` works on the OS

> **Status:** shipped (Phase 2 of guest-gcc bring-up).
> The cross-built `xgcc` binary from chapter 132c now runs
> **inside the guest**, drives the cc1 / as / ld pipeline,
> and produces an executable ELF that the OS can run.
> `scripts/test_gcc_hello.py` boots, stages a six-line
> hello-world, walks the ladder `--version → -E → -S → -c
> → link → run`, and ends with **PASS 8 / FAIL 0**.
> **Prereq:** chapters 131e (in-guest `ld`), 131f (real
> `/bin/as` + `/bin/ld`), 132c (cross-built `xgcc`),
> 132d (real specs file), 132e (gmp/mpfr/mpc in the
> guest sysroot).
> **Opens:** chapter 133a (port `make`, then larger
> programs); the doom build now has a real toolchain
> sitting on `/bin/`.

---

## What you'll do in this chapter

1. Patch `vendor/gcc-14.2.0/libiberty/lrealpath.c` so the
   `#ifdef`-fall-through path returns `strdup(filename)`
   instead of `filename` itself (the bug that corrupts
   `argv[0]`).
2. Bump the kernel exec caps in `kernel/core/syscall.c`
   (`MAX_EXEC_ARGV` 16→64, `MAX_EXEC_ARG_LEN` 96→256) and
   `kernel/core/elf.c` (`MAX_USER_ARGV` 16→64) so cc1's
   20-element argv vector passes through `execvp`.
3. Move `_env_init`'s 16 KiB staging buffer in
   `userspace/libc/env.h` from the stack into a BSS arena
   so userspace doesn't burn 25% of its 64 KiB stack on
   env init.
4. Install `xgcc`, `cc1`, real `/bin/as` and `/bin/ld`,
   plus a tiny `gcc` shim that prepends `-B/bin/` onto
   the OSFS-1 image.
5. Add the seven-step ladder
   `scripts/test_gcc_hello.py` (`--version → -E → -S
   → -c → link → run`) and the bisection harness
   `scripts/_dbg_gcc_hex.py` (kept per debug-scripts
   policy).

---

## Why now

By the end of chapter 132e everything required to **link** a
guest gcc existed under `build/gcc-build-guest/`. What
chapter 132e did **not** verify is that the resulting `xgcc`
actually runs when the kernel `exec`s it.

It didn't. The first attempt produced:

```
$ /bin/gcc -v -S -o /tmp/hello.s /tmp/hello.c
COLLECT_GCC=
COLLECT_GCC_OPTIONS='-v' '-S' '-o' '���?' '-mlittle-endian' ...
xgcc: error: cannot execute '': No such file or directory
```

`COLLECT_GCC` is empty. The value for `-o` shows up as Unicode
replacement glyphs. The program name xgcc is trying to exec is
the empty string. None of these are random — every one of them
is `argv[0]`, with its contents trashed somewhere between
kernel-side argv copying and the GCC driver's first use of
the path.

This chapter is the bug-hunt that fixes it, the cleanup that
makes the fix shipable, and the test ladder that proves the
bug stays fixed.

---

## What shipped, by the byte

```
$ ls -la build/disk.img build/kernel.elf
-rw-r--r--  ... 268435456 build/disk.img
-rwxr-xr-x  ...   1061584 build/kernel.elf
```

Files installed under `/bin/` on the OSFS-1 image:

```
xgcc         2,749,952 B    real GCC driver
cc1         55,985,584 B    the actual C front-end
as          11,XXX,000 B    binutils-2.44 assembler (ch 131f)
ld           7,XXX,000 B    binutils-2.44 linker    (ch 131f)
gcc          ~6,000 B       libc shim that prepends -B/bin/
```

`scripts/test_gcc_hello.py` runs end-to-end:

```
[chapter 132f] /bin/gcc end-to-end smoke test
PASS: step 1: /bin/gcc --version executes
PASS: step 1: reports GCC 14.x
PASS: step 2: /tmp/hello.c contents staged
PASS: step 3: cc1 preprocesses (output retains _start)
PASS: step 4: cc1 emitted assembly
PASS: step 5: cc1 + as produced ELF .o
PASS: step 6: linked /tmp/hello is an ELF
PASS: step 7: /tmp/hello returned 42

PASS: 8
FAIL: 0
```

That step-7 line is the payoff: the guest GCC produced an
ELF, the guest loaded it, the program ran, and `exit(42)`
came back through the kernel reaper.

---

## Pitfalls

This chapter shipped on top of five separate failures
stacked on each other, each one masking the next. Each
pitfall below records the symptom that surfaced, the
cause uncovered during triage, and the fix that landed.

### Pitfall — `lrealpath` returns its input pointer unchanged

**Symptom:** the very first run of `/bin/gcc` printed
`COLLECT_GCC=` empty, `-o` showed up as Unicode replacement
glyphs in `COLLECT_GCC_OPTIONS`, and xgcc died with
`cannot execute ''`. Every garbled value traced back to
`argv[0]`.

**Cause:** GCC's driver wants to canonicalize argv[0] so it
can derive `/lib/gcc/...` relative paths. It calls
`make_relative_prefix(progname, ...)`, which calls
`make_relative_prefix_1`, which calls `lrealpath(progname)`.

`vendor/gcc-14.2.0/libiberty/lrealpath.c` is a tower of
`#ifdef`s:

```c
char *
lrealpath (const char *filename)
{
#if defined(REALPATH_LIMIT)
  /* ...realpath() implementation... */
#endif
#if defined(HAVE_CANONICALIZE_FILE_NAME)
  /* ...canonicalize_file_name() implementation... */
#endif
#if defined(HAVE_REALPATH) && defined(HAVE_UNISTD_H)
  /* ...realpath() implementation, take two... */
#endif
#if defined(_WIN32)
  /* ...GetFullPathName() implementation... */
#endif
}
```

The cross libiberty's `config.h` leaves all four `#ifdef`s
undefined. Control falls through to the closing brace with
**no return statement**. The compiler emits a function whose
body is `nop; ret` — `x0` is whatever the caller put there,
which is `filename`. So `lrealpath(progname)` returns
`progname` unchanged, as if it were a fresh heap allocation.

Then `make_relative_prefix_1` does:

```c
full_progname = lrealpath (progname);   /* == argv[0] */
...
prog_dirs = split_directories (full_progname, &prog_num);
free (full_progname);   /* free(argv[0]) */
```

The OSdev libc's `free()` happily writes its free-list
bookkeeping into the "freed" block — which is actually
argv[0]'s string storage on the user stack. The free-list
pointer it writes is `argv[0]_addr - 8`. That decodes back
to the `cb ff ff 3f 10 00` byte sequence that mangles
argv[0]'s contents. Hence the Unicode-replacement-glyphs
in the error output.

**Fix** (`vendor/gcc-14.2.0/libiberty/lrealpath.c`, appended
just before the closing `}`):

```c
  /* osdev chapter 132f: when no implementation was selected
   * by the #ifdef tower, return a heap copy of the input so
   * callers that free() the result don't corrupt their input
   * pointer. */
  return strdup (filename);
```

Verified by `aarch64-elf-objdump -d lrealpath.o`:

```
0000000000000000 <lrealpath>:
   stp  x29, x30, [sp, #-32]!
   mov  x29, sp
   str  x0, [sp, #24]
   ldr  x0, [sp, #24]
   bl   <strdup>
   ldp  x29, x30, [sp], #32
   ret
```

### Pitfall — `MAX_EXEC_ARGV = 16` is too small for cc1

**Symptom:** after the lrealpath fix, `xgcc` correctly
assembled cc1's 20-element argv vector:

```
cc1 -quiet -v -iprefix /bin/../lib/gcc/aarch64-osdev/14.2.0/ \
    -D__OSDEV_LIBC__ /tmp/hello.c -ffreestanding -quiet \
    -dumpdir /tmp/ -dumpbase hello.c -dumpbase-ext .c \
    -mlittle-endian -mabi=lp64 -version -o /tmp/hello.s
```

But the kernel rejected it:

```
xgcc: error trying to exec 'cc1': execvp: Invalid argument
```

**Cause:** two hard-coded `16`s in the kernel collided with
this:

```c
/* kernel/core/syscall.c */
#define MAX_EXEC_ARGV    16
#define MAX_EXEC_ARG_LEN 96

/* kernel/core/elf.c */
#define MAX_USER_ARGV    16
```

The first cap lives in `copy_argv_from_user`, which returns
`-EINVAL_VFS` ("over-long argv") when it walks past slot 16
without seeing a NULL. The second cap lives in
`build_user_init_stack`, which prints `[elf] too many argv
entries` and refuses to lay out the user stack.

**Fix:** bump **both**. 64 leaves headroom for the link
step (cc1 + as + ld together can produce 50+ arg
invocations) and for future tools whose argv shape is not
yet known:

```c
#define MAX_EXEC_ARGV    64
#define MAX_EXEC_ARG_LEN 256      /* paths can be long */
#define MAX_USER_ARGV    64       /* must match */
```

Static storage for argv staging grows from 1.5 KiB to 16 KiB
of BSS (acceptable; 8 GiB of RAM is available). The on-stack
`arg_vas[]` in `build_user_init_stack` grows from 128 B to
512 B (acceptable; the kernel stack is 16 KiB).

The hard ceiling that still applies is the single 4 KiB
user stack page that holds the initial argv blob. With 20
args of average 20 chars each, total is around 600 B —
plenty of room. The layout will need re-thinking once argv
exceeds ~3 KiB, but that's a long way off.

See `/memories/repo/kernel-exec-limits-too-small-for-cc1.md`.

### Pitfall — cc1 sees `-o ""` for one invocation

**Symptom:** after bumping the kernel limits, cc1 actually
started and printed its full banner — but then immediately
died with:

```
cc1: fatal error: cannot open '' for writing: Read-only file system
compilation terminated.
```

Suspicious: cc1 sees an empty filename for `-o`, even
though xgcc clearly printed `-o /tmp/hello.s` in its trace.

**Cause:** a stale cc1 binary. A temporary probe at the
top of cc1's `main()` (`vendor/gcc-14.2.0/gcc/main.cc`)
that dumps every argv element via inline `svc #0` write
syscalls showed:

```
[cc1-argv] argc=20
[cc1-argv] argv[0]="cc1"
[cc1-argv] argv[1]="-quiet"
...
[cc1-argv] argv[18]="-o"
[cc1-argv] argv[19]="/tmp/hello.s"
```

argv is **completely correct**. cc1 then printed its full
banner, ran the search for `#include` directories, and
exited cleanly with code 0. The error was gone. The
previous run was using a stale cc1 binary from before the
kernel exec-limit fix took effect (the "build/kernel.elf
is up to date" path had silently let an old kernel
through). Once cc1 was rebuilt alongside the kernel, the
failure mode disappeared.

**Fix:** when something looks wrong but the trace says it
shouldn't be, `rm` the suspect `.o` files manually. The
`gcc-build-guest` Makefile does **not** generate `.Po`
dependency files in the `--without-headers
--disable-bootstrap` configuration. Touching a header does
not force `gcc.o` to rebuild.

That isn't really a bug in OSdev code — it's a configuration
quirk of in-tree GCC builds when most options have been
stripped off `configure`. But it cost a rebuild cycle of
sneaking-suspicion before the test result became trustworthy.

### Pitfall — `env_init()` had a 16 KiB stack frame

Triaged but turned out not to be on the critical path. Worth
documenting because it would have bitten userspace eventually.

**Symptom:** no overt failure yet, but
`userspace/libc/env.h::_env_init()` originally allocated a
`char tmp[16384]` local to stage the env blob before copying
to a static arena:

```c
static void _env_init (void) {
  char tmp[16384];                         /* !! 16 KiB on a 64 KiB stack */
  long n = __sys_getenv_all (tmp, sizeof tmp);
  ...copy tmp into g_env_arena...
}
```

**Cause:** the user stack is 16 pages × 4 KiB = 64 KiB
total (`USER_STACK_PAGES=16`). Burning 25% of that in one
frame is a loaded gun: any deeper call would have over-run
into the guard page and faulted at a kernel address that
has nothing to do with the actual code.

**Fix:** read the blob directly into the static arena:

```c
static char g_env_arena[16384];

static void _env_init (void) {
  long n = __sys_getenv_all (g_env_arena, sizeof g_env_arena);
  ...parse g_env_arena into key/value table...
}
```

This defensive change shipped alongside its own memory
rule:

- `/memories/repo/userspace-libc-no-large-stack-buffers.md`

### Pitfall — toolchain-include is a copy, not a symlink

**Symptom:** while chasing the `env_init` fix, repeated
`cat build/toolchain/aarch64-osdev/include/env.h` kept
showing the **old** content even after three rebuilds.
Edits appeared not to land.

**Cause:** `build/toolchain/aarch64-osdev/include/` is
populated by a Makefile rule that **copies** every
`userspace/libc/*.h` into the sysroot once. But the actual
compiler wrapper at `build/toolchain/bin/aarch64-osdev-cc`
uses `-isystem $ROOT/userspace/libc` directly. The sysroot
copy is never consulted by the compile.

**Fix:** the env.h fix was being seen by the compile;
only the eyeball was reading the wrong file. Recorded as:

- `/memories/repo/toolchain-include-is-stale-copy-not-symlink.md`

---

## The diagnostic technique that found the lrealpath bug

The lrealpath bug was the hard one. The corruption was
happening hundreds of GCC-driver-statements after argv was
first read, deep inside libiberty, in a routine called from
a routine called from a routine that the GCC driver had no
reason to call out specially.

Bisect it by sprinkling **inline-svc probes** through
`gcc.cc::process_command()`. The helper triplet looks like:

```c
static inline void __osdev_diag_write(int fd, const char *p,
                                      unsigned long n)
{
  register long x8 asm("x8") = 1;     /* SYS_WRITE */
  register long x0 asm("x0") = fd;
  register long x1 asm("x1") = (long)p;
  register long x2 asm("x2") = (long)n;
  asm volatile("svc #0" : "+r"(x0)
               : "r"(x8), "r"(x1), "r"(x2) : "memory");
}

static inline void __osdev_diag_dump_bytes(const char *label,
                                           const void *p,
                                           unsigned long n)
{
  __osdev_diag_puts (label);
  const unsigned char *b = p;
  for (unsigned long i = 0; i < n; i++) {
    char hex[3];
    hex[0] = "0123456789abcdef"[(b[i] >> 4) & 0xf];
    hex[1] = "0123456789abcdef"[b[i] & 0xf];
    hex[2] = ' ';
    __osdev_diag_write (2, hex, 3);
    if (b[i] == 0) break;
  }
  __osdev_diag_write (2, "\n", 1);
}
```

A file-scope `static const char *__osdev_diag_argv0` then
gets set once in `driver::main`, and every interesting call
site in `process_command` and `set_up_specs` dumps the bytes
at that pointer:

```c
__osdev_diag_dump_bytes ("[diag] pc:4 before get_relative_prefix: ",
                          __osdev_diag_argv0, 16);
new_exec = get_relative_prefix (...);
__osdev_diag_dump_bytes ("[diag] pc:5 after get_relative_prefix: ",
                          __osdev_diag_argv0, 16);
```

Between pc:4 and pc:5 the bytes flipped from `2f 62 69 6e
2f 78 67 63 63 00` (`"/bin/xgcc\0"`) to `cb ff ff 3f 10 00`.
That localised the corruption to one call: the first
`get_relative_prefix`, which delegates to `make_relative_
prefix`, which delegates to `lrealpath`.

A `aarch64-elf-objdump -d lrealpath.o` revealed the
`nop; ret` body, and the fix wrote itself.

Why inline-svc and not `fprintf`? Three reasons:

1. The corruption might be in stdio init — `fprintf` cannot
   be trusted to print the truth about itself.
2. argv[0] is what stdio would print, and that's the thing
   being trampled.
3. The inline assembly is 4 instructions; no libc state,
   no buffers, no allocator. Nothing else to go wrong.

This pattern is now memo-ed at:

- `/memories/repo/inline-svc-diagnostic-pattern.md`

All the probes were removed once the bug was fixed. The
final `xgcc` ships clean.

---

## What you'll write

| File                                          | Change                                  |
| --------------------------------------------- | --------------------------------------- |
| `vendor/gcc-14.2.0/libiberty/lrealpath.c`     | `return strdup (filename);` fallback    |
| `kernel/core/syscall.c`                       | `MAX_EXEC_ARGV` 16→64, `MAX_EXEC_ARG_LEN` 96→256 |
| `kernel/core/elf.c`                           | `MAX_USER_ARGV` 16→64                   |
| `userspace/libc/env.h`                        | `_env_init` reads into BSS arena, not stack |
| `scripts/test_gcc_hello.py`                   | 7-step ladder, the regression test      |
| `scripts/_dbg_gcc_hex.py`                     | Diagnostic probe runner (kept per debug-scripts policy) |

The driver patch from chapter 132d
(`vendor/gcc-14.2.0/gcc/gcc.cc::process_command` — preserve
the `standard_*` fallback when `get_relative_prefix` returns
NULL) stayed in place. That patch defended against a NULL
`gcc_libexec_prefix` after `lrealpath` returned NULL — once
lrealpath stops returning NULL the guard is a no-op, but
it's still correct and stays in tree.

---

## Run it / Test it

`scripts/test_gcc_hello.py` is structured so each rung
isolates one stage of the cc1 / as / ld pipeline. If a
future regression lands, the failure mode tells you which
stage broke.

```python
HELLO_C = r"""void _start(void) {
    register long x0 asm("x0") = 42;
    register long x8 asm("x8") = 2;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x8));
    __builtin_unreachable();
}
"""

# step 1: /bin/gcc --version           — xgcc loads
# step 2: stage hello.c                — sanity of the test fixture
# step 3: /bin/gcc -E -nostdinc        — cc1 preprocesses
# step 4: /bin/gcc -S -nostdinc        — cc1 emits .s
# step 5: /bin/gcc -c -nostdinc        — cc1 + as produce .o
# step 6: /bin/gcc -nostdlib -nostdinc -e _start \
#                  -Wl,-T,/bin/osdev.ld -o /tmp/hello hello.c
#                                       — cc1 + as + ld produce ELF
# step 7: /tmp/hello                   — kernel loads it, it exits 42
```

The `-nostdlib -nostdinc -e _start` on step 6 keeps the link
synthetic: no crt0, no libc, no libgcc. That's deliberate
for this chapter — the goal here is "the toolchain itself
runs in the guest." Wiring up default specs so a bare `gcc
hello.c` finds the right crt0 / libc is chapter 132g's
problem.

---

## What this unlocks

After chapter 132f:

- `/bin/gcc` is a real C compiler running inside the OS.
- A user can edit a C file in the in-guest text editor, run
  `gcc -nostdlib hello.c -o hello`, and the result is an
  ELF the OS will load.
- The doom build (chapter 133b — port `make` and then build
  doomgeneric in-guest, eventual chapter) now has a real
  compiler sitting on the path.

Per the standing "apps must use the OS features the book
builds" rule:

- **Existing tools that use the new compiler**: `notepad`
  can now save a `.c` file and have it compiled by `gcc`
  via the gui_term shell. Chapter 127's "build button"
  prototype was a placeholder; chapter 132f wires it to a
  real compiler. (See `userspace/notepad/notepad.c::on_build`
  — the placeholder PATH lookup will go away in chapter 132g
  once specs are wired right.)
- **New tools**: none yet — the chapter is about the
  compiler itself running; new C-using utilities arrive
  in chapter 133 once `make` lands.
- **Existing test scripts upgraded**: `scripts/_dbg_gcc_hex.py`
  was the bisection harness used to find the lrealpath
  bug; kept per debug-scripts policy.
- **New test scripts added**:
  - `scripts/test_gcc_hello.py` — the 7-step ladder; runs
    in the regression sweep. If it breaks, the compiler
    stopped working.

---

## Things to remember

- `/memories/repo/chapter-132f-libiberty-lrealpath-stub-frees-argv.md`
  (the lrealpath bug; written during triage)
- `/memories/repo/kernel-exec-limits-too-small-for-cc1.md`
- `/memories/repo/inline-svc-diagnostic-pattern.md`
- `/memories/repo/userspace-libc-no-large-stack-buffers.md`
- `/memories/repo/toolchain-include-is-stale-copy-not-symlink.md`

The first one was written during the bug hunt; the other
four codify lessons that came out of the same hunt and will
save time the next time a vendor tree gets opened under
the OSdev freestanding libc.

---

## What's next

- Chapter 132g wires up default specs so a bare
  `gcc hello.c` finds the right crt0 / libc instead of
  needing `-nostdlib -nostdinc -e _start -Wl,-T,...` on
  the command line.
- Chapter 133a ports `make` into the guest so larger
  programs can be built without hand-driving the
  compiler.

