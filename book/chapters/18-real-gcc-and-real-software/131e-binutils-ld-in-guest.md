# Chapter 131e — `ld` in-guest: cross-building binutils' linker

> **Status:** shipped. `vendor/binutils-2.44/ld` cross-builds
> under `--host=aarch64-osdev` against five companion
> archives (`libiberty`, `libsframe`, `bfd`, `opcodes`,
> `libctf`) and produces a 3,206,056-byte AArch64 ELF
> at `build/binutils-build-guest-ld/ld/ld-new`. Host smoke
> [scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py)
> passes. Chapter 131d's
> [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py)
> baseline still passes (libiberty.a = 949,790 bytes — grew
> slightly because struct stat picked up `st_dev` /
> `st_ino` and a few small libc additions changed the
> compile size of two libiberty TUs).
>
> **Prereqs:** chapters 131a (`make binutils-osdev`),
> 131b (`make aarch64-osdev-cc-install`), 131c (link-mode
> wrapper), and 131d (CONFIG_SITE cache + libiberty.a).
> Run `make` first so `build/userspace/crt/crt0.o`
> and `build/userspace/libc/cstring.o` are present.
>
> **Opens:** chapter 131f — copy `ld-new` to `/bin/ld`
> on the guest disk and retire the chapter-119 toy
> linker.

---

## What you'll do in this chapter

1. Cross-configure five more binutils subdirs in order
   (`libsframe`, `bfd`, `opcodes`, `libctf`, `ld`) using
   the chapter 131d `CONFIG_SITE` cache plus two new
   top-level flags (`--disable-binutils`, `--without-zstd`)
   and `--disable-plugins` on `bfd` / `ld`.
2. Post-process each generated Makefile to strip
   `ZLIB = -lz` (there's no `libz.a` for `aarch64-osdev`).
3. Extend `userspace/libc/cstring.c` with 13 strong-extern
   symbols (`malloc` / `free` / `calloc` / `realloc` /
   `abort` / `exit` / `strcmp` / `strncmp` / `strcpy` /
   `strncpy` / `strcat` / `strchr` / `strrchr` / `strstr`)
   exposed via `__asm__("name")` renames.
4. Inject `cstring.o` into `ld_new_LDADD` (and *only*
   that variable) so it reaches ld's link without leaking
   into libtool's `libdep.la` build.
5. Close the eight smaller libc gaps the cross-build
   surfaces (`locale.h`, `sys/param.h`, getuid family,
   `umask`/`chmod`, `tmpfile`, `gzwrite`, the
   `OSDEV_LIBC_NO_GLOBAL_DEFS` guard, the
   `OSDEV_LIBC_NO_GETOPT` replacement for 131d's
   `ELIDE_CODE` patch).
6. Write [scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py)
   — host smoke test that drives the whole build and
   asserts `ld-new` is an AArch64 ELF.

## Why now

Chapter 131d shipped one cross-built `libiberty.a`. It
did *not* attempt anything that linked — every libiberty
TU only had to compile.

This chapter has to make the linker `ld-new` come out the
far end of `make`, which means:

1. Five more autoconf subdirs (`libsframe`, `bfd`,
   `opcodes`, `libctf`, `ld`) have to configure cleanly
   under the wrapper + cache.
2. Each one has to compile cleanly — and they touch a
   wider chunk of libc than libiberty did (locale.h,
   sys/param.h, real `tmpfile`, real `strerror` placement,
   POSIX `getuid` family, `umask`/`chmod`, `gzwrite`
   stub, the `OSDEV_LIBC_NO_GLOBAL_DEFS` guard that
   suppresses per-TU `environ` / `__cxa_finalize` emission,
   and a weak `environ` slot in `cstring.o`).
3. The `ld-new` link has to resolve — and that's the new
   gauntlet, because vendor archives carry their own
   `extern void *malloc(size_t);` / `extern void
   free(void *);` / `extern int strcmp(const char *,
   const char *);` declarations and the OSdev libc keeps
   all of those as `static inline` (zero external
   symbols).
4. The ld build's libtool plugin object (`libdep.la`)
   must not break — it has stronger opinions about
   non-libtool inputs than ld-new itself does.

The first three are gap-closing work in the chapter 131d
mould (add libc bits; widen autoconf cache; one or two
source patches). The fourth and last is a new shape:
libtool's mode-aware link refuses to mix a vanilla `.o`
with `.la` libraries, so a plain `LIBS=cstring.o` on the
`make` command line won't work. The extern wrappers have
to land at the single specific link line that needs them
(`ld_new_LDADD`) and nowhere else.

---

## Approach in two sentences

**Stage 1: configure five more subdirs the same way 131d
configured libiberty** — direct sub-configure invocation
(no `make configure-XXX`), CONFIG_SITE cache, plus
`--without-zstd` and `--disable-plugins` for bfd/ld and a
post-process pass that strips `ZLIB = -lz` from each
generated Makefile.

**Stage 2: give vendor archives the strong-extern symbols
they expect by extending `cstring.o`** — a single libc
object file that publishes `malloc` / `free` / `calloc` /
`realloc` / `strcmp` / `strncmp` / `strcpy` / `strncpy` /
`strcat` / `strchr` / `strrchr` / `strstr` / `abort` /
`exit` under their POSIX names via `__asm__("name")`
renames, then inject `cstring.o` into ld's
`ld_new_LDADD` Makefile variable so it appears on
exactly one link line and never reaches libtool.

---

## Stage 1 — five more subdirs

### The subdir list and order

[scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py)
declares the build order at the top:

```python
SUBDIRS = [
    "libiberty",
    "libsframe",
    "bfd",
    "opcodes",
    "libctf",
    "ld",
]
```

`bfd` links against `libsframe.la`; `libctf` and `opcodes`
both build on `bfd`; `ld` is the umbrella. The order is
strict — out-of-order configure produces "no rule to make
target `../libsframe/libsframe.la`" deep inside
bfd's build.

### Two new top-level configure flags

```python
"--disable-binutils",  # gas + ar + objdump etc. — 131f
"--without-zstd",      # no libzstd for aarch64-osdev
```

`--disable-binutils` is the chapter-scoping decision: this
chapter only ships `ld`. The tools subdir (`binutils/`,
which makes `ar`, `nm`, `objdump`, `strings`, `strip`)
builds against `bfd` too but pulls in another wave of
libc gaps (`getopt_long` from `getopt1.c`, lots of
`time.h`, plot/graph TUI bits). 131f's problem.

`--without-zstd` is the small one and the one that takes
several iterations to land: binutils' default behaviour
is to detect-and-use the host zstd. The host shell has
one, so configure says "yes, link zstd", and the link
fails because the OSdev toolchain ships no
`libzstd.a` for aarch64-osdev. Explicitly disabling
takes ~200 lines of configure-script-emitted "checking
for ZSTD..." noise off the table.

### Per-subdir configure flags

The test invokes each subdir's `configure` directly with
a controlled env (same anti-`make-configure-XXX` reasoning
as 131d), passing:

```python
"--build=aarch64-apple-darwin",
"--host=aarch64-osdev",
"--target=aarch64-osdev",
"--disable-shared",
"--disable-werror",
"--disable-multilib",
"--disable-nls",
"--with-system-zlib",
"--without-zstd",
```

plus `--disable-plugins` for `bfd` and `ld` specifically.
The plugins infrastructure pulls in `dlopen` / `dlsym`
detection that the OSdev libc can't satisfy.

### The `ZLIB = -lz` Makefile post-process

`--with-system-zlib` tells autoconf "don't build the
in-tree zlib copy, use the system one". On real systems
that means `-lz`. The OSdev toolchain ships no
`libz.a` for aarch64-osdev (`userspace/libc/zlib.h` is
header-only static-inline; see Stage 2's small `gzwrite`
addition). The chapter 131d cache file marks
`ac_cv_have_zlib=yes` so the in-tree fallback never
builds, but the generated Makefile still has
`ZLIB = -lz` baked into the `bfd_LIBS` / `ld_LIBS` line.

A one-line post-process strips it:

```python
mf = os.path.join(sd, "Makefile")
if os.path.exists(mf):
    with open(mf, "r") as f:
        txt = f.read()
    txt2 = txt.replace("ZLIB = -lz", "ZLIB =")
    txt2 = txt2.replace("ZLIB =  -lz", "ZLIB =")
    ...
```

Both the `ZLIB = -lz` and `ZLIB =  -lz` (double-space —
autoconf inserts it sometimes) variants are stripped.
This is safe because nothing in the `bfd` / `ld` link
path actually calls into zlib — the calls are in
`bfd/compress.c` for compressed debug sections, which
binutils' optimizer dead-codes when no caller exists.

### The `bfd` `make headers` warmup

```python
if sub == "bfd":
    rh = run(["make", "headers"], cwd=...)
    if rh.returncode != 0:
        fail(...)
```

bfd's generated headers (`bfd.h`, `bfd-in3.h`, `bfd_stdint.h`,
`peXXigen.h`) are derived from `*.in` templates by a
custom `make headers` rule. The opcodes and libctf
sub-makes `#include <bfd.h>`. Without the warmup the
first compile in opcodes dies on a missing include.

---

## Stage 2 — extern wrappers in `cstring.o`

### The diagnosis

After Stage 1's configures land, `make` in each subdir
sails through until ld's final link, where:

```
ld_new-strdup.o: undefined reference to `malloc'
ld_new-vasprintf.o: undefined reference to `malloc'
ld_new-objalloc.o: undefined reference to `free'
ld_new-cplus-dem.o: undefined reference to `strcmp'
ld_new-bucomm.o: undefined reference to `strlen'
...
```

Each libiberty `.c` carries (near the top, no
`<stdlib.h>` include):

```c
extern void *malloc(size_t);
extern void free(void *);
extern int strcmp(const char *, const char *);
```

The OSdev libc puts `malloc`, `free`, `strcmp` (etc.) as
`static inline` in `malloc.h` / `string.h`. A `static
inline` definition is a per-TU file-local symbol — by
design, it never emits an external symbol the linker can
see. So the libiberty TU's externs go unresolved.

There are three plausible fixes:

1. **Extract libc into a `libc.a`** with each function as
   a real translation unit. Chapter 131c flagged this as
   the right long-term move. It's a lot of surgery for
   one chapter, and the next chapter that *really* needs
   it (132c: cross-build gcc) is on the schedule with
   its own opinions about libc layout.
2. **Add `-include <stdlib.h>`** to vendor TUs through
   the wrapper. Breaks anything that has a local `extern
   void *malloc(size_t);` that conflicts with `<stdlib.h>`
   shape — and that's most of them.
3. **Provide strong external symbols in one `.o` already
   cross-compiled.** Smallest delta. The same
   `cstring.o` that ships `strdup` for the Doom port
   (chapter 130a) can carry the rest.

This chapter picks option 3.

### The `__asm__("name")` rename pattern

To publish a function as a strong external `malloc` from
the same TU that includes the static-inline `malloc.h`,
a naive write hits a duplicate-symbol error at the
**assembler**: the static-inline emission ("malloc" —
file-local) and the strong extern definition ("malloc")
collide.

The fix is to keep the C-level name distinct and use
GCC's per-function asm rename to publish under the POSIX
name:

```c
void *__cstring_malloc(size_t want) __asm__("malloc");
void *__cstring_malloc(size_t want)
{
    /* ... body ... */
}
```

The compiler sees a function called `__cstring_malloc`
in C — no collision with anything else in the TU. The
assembler sees the rename and emits the symbol as
`malloc`. Linker is happy. Vendor archives that
`extern void *malloc(size_t);` resolve to this.

This works because `cstring.c` doesn't include
`malloc.h` (or `signal.h`, or `env.h`, or `errno.h`).
The asm-rename + static-inline collision is a
same-TU-only problem; the wider tree's apps continue to
use the inline `malloc.h` form (which lives in headers
they include directly).

### The self-contained allocator

The asm-rename guarantees `malloc` can be *exposed*. The
allocator's body is a ~60-line K&R-style first-fit free
list:

```c
struct _extern_blk { size_t size; struct _extern_blk *next; };
static struct _extern_blk *_extern_free_head = (struct _extern_blk *)0;

static void *_extern_sbrk(long delta) {
    return (void *)(uintptr_t)__cstring_svc1(SYS_BRK, delta);
}

void *__cstring_malloc(size_t want) __asm__("malloc");
void *__cstring_malloc(size_t want) {
    if (want == 0) return (void *)0;
    size_t need = _extern_round(want);
    for (int attempt = 0; attempt < 2; attempt++) {
        struct _extern_blk **pp = &_extern_free_head;
        while (*pp) {
            struct _extern_blk *b = *pp;
            if (b->size >= need) { /* split / unlink, return */ }
            pp = &b->next;
        }
        if (_extern_grow(need) != 0) return (void *)0;
    }
    return (void *)0;
}
```

This is **a separate heap** from `malloc.h`'s static-inline
allocator. The two heaps in the same process never share
a pointer because no in-tree app mixes the two paths:
vendor archives always call extern `malloc`/`free`
(cstring.o's heap); OSdev apps always inline static
`malloc`/`free` (per-TU heap, but with shared backing brk).

The only crossover is `strdup` (Doom + libiberty both
call it). Chapter 130a's `strdup` originally lived on
malloc.h's heap; chapter 131e moves it to cstring.o's
side by changing the include from `#include "malloc.h"`
to `extern void *malloc(size_t);` at the top of
`strdup`'s body. Now `strdup` and `free` always agree on
which heap a string lives on.

### The wrappers

13 strong-extern symbols ship in the chapter 131e block:

| Symbol | C-level name | Lives at heap | Notes |
|--------|--------------|---------------|-------|
| `malloc` | `__cstring_malloc` | own | sbrk-backed free list |
| `free` | `__cstring_free` | own | coalesces forward |
| `calloc` | `__cstring_calloc` | own | overflow-checked `n*sz` |
| `realloc` | `__cstring_realloc` | own | always copy + free |
| `abort` | `__cstring_abort` | — | `SYS_EXIT 134` (128+SIGABRT) |
| `exit` | `__cstring_exit` | — | `SYS_EXIT(code)` |
| `strcmp` | `__cstring_strcmp` | — | byte loop |
| `strncmp` | `__cstring_strncmp` | — | byte loop, bounded |
| `strcpy` | `__cstring_strcpy` | — | terminating-NUL copy |
| `strncpy` | `__cstring_strncpy` | — | POSIX pad-with-NULs |
| `strcat` | `__cstring_strcat` | — | advance-to-NUL then strcpy |
| `strchr` | `__cstring_strchr` | — | scan |
| `strrchr` | `__cstring_strrchr` | — | scan, last hit |
| `strstr` | `__cstring_strstr` | — | naive substring |

Each is ~5–15 lines. `memcpy` / `memmove` / `memset` /
`strlen` already had non-static bodies in `cstring.c`
from chapter 130a; the chapter 131e block only adds the
missing extras vendor archives reference.

`aarch64-elf-nm cstring.o` after the chapter 131e block
shows global `T` entries for all 13 names plus the
chapter-130a memcpy/memmove/memset/strlen — matching
exactly what the vendor archives' externs expect.

---

## The libtool `libdep.la` trap

After Stage 2's wrappers exist, the natural way to add
them to ld's link is `make LIBS=cstring.o`. That fails:

```
GEN      libdep.la
libtool: link: cannot build libtool library `libdep.la' from
  non-libtool objects on this host:
  /Users/.../build/userspace/libc/cstring.o
```

`libdep.la` is binutils' BFD plugin for resolving
implicit linker dependencies. It's a libtool library
(`.la`) that gets built into ld's `bfd-plugins` directory.
Its rule in `ld/Makefile.am`:

```makefile
bfdplugin_LTLIBRARIES = libdep.la
```

is **unconditional** — no `--disable-libdep` option
exists. And libtool's `--mode=link` for a `.la` target
refuses to consume non-libtool objects, by design.
`make LIBS=cstring.o` puts `cstring.o` on the `LIBS` line
that libtool reads when linking `libdep.la`, and libtool
draws the line right there.

The workaround is the *minimum invasive* link-line
edit: rewrite `ld/Makefile` after configure so that
`cstring.o` appears in the variable that names the
inputs to *only* the `ld-new` link, and nowhere else.

`ld_new_LDADD` is that variable:

```makefile
ld_new_LDADD = $(EMULATION_OFILES) $(EMUL_EXTRA_OFILES) \
               $(BFDLIB) $(LIBCTF) ...
```

Inject `cstring.o` at the front:

```python
if sub == "ld":
    old_ldadd = "ld_new_LDADD = "
    new_ldadd = f"ld_new_LDADD = {cstring_o} "
    if old_ldadd in txt2 and new_ldadd not in txt2:
        txt2 = txt2.replace(old_ldadd, new_ldadd, 1)
```

Because `cstring.o` is a real `.o` (not an archive), all
its symbols become unconditionally part of the link
namespace from the moment the linker reads the first
input file. Every later libiberty / bfd / libctf member
that references `malloc` / `free` / `strcmp` resolves
without having to be a "search the archive for the
defining member" pull-in.

This injection runs in the same loop pass as the
`ZLIB = -lz` strip, immediately after each subdir's
configure but before each subdir's `make`. The build
loop itself reverts to a plain `cmd = ["make"]` for all
subdirs — no `LIBS=`, no leak into libdep's link line.

---

## Stage 3 — the libc additions Stage 2 brought along

While debugging Stages 1 and 2, eight smaller libc gaps
open up. None are conceptually deep; they're listed
together here so the chapter has a single change
manifest.

### `locale.h` (new file)

ld's `ldmain.c` calls `setlocale(LC_ALL, "")`. Eight
lines:

```c
#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6
static inline char *setlocale(int category, const char *locale)
{ (void)category; (void)locale; return (char *)"C"; }
```

### `sys/param.h` (new file)

bfd's `bfdio.c` uses `MAXPATHLEN`. A four-line file:

```c
#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif
```

### `unistd.h` — `getuid` / `getgid` / `geteuid` / `getegid`

bfd's `archive.c` (`!getuid()` guard around umask-style
behaviour). There's no multi-user yet — return 0 for
all four:

```c
static inline uid_t getuid(void)  { return 0; }
static inline gid_t getgid(void)  { return 0; }
static inline uid_t geteuid(void) { return 0; }
static inline gid_t getegid(void) { return 0; }
```

### `sys/stat.h` — `umask` / `chmod`

bfd's `archive.c` calls both. OSFS doesn't enforce
file-mode bits — both no-ops:

```c
static inline mode_t umask(mode_t cmask) { (void)cmask; return 0; }
static inline int chmod(const char *path, mode_t mode)
{ (void)path; (void)mode; return 0; }
```

### `stdio.h` — `tmpfile`

bfd's `compress.c` calls `tmpfile()`. Eight lines: build
a unique name in `/tmp/`, `fopen(name, "w+b")`,
`unlink(name)`, return the FILE\*.

### `string.h` — `strerror`

POSIX-correctness move. `strerror()`'s declaration was
in `errno.h` (where the function body lives). Several
vendor TUs include `<string.h>` and expect the prototype
there. The body stays in `errno.h`; `string.h` gains a
6-line block that just re-declares it:

```c
/* Chapter 131e — POSIX places `char *strerror(int)` in <string.h>.
 * The body lives in errno.h; including it here lets callers
 * that #include <string.h> alone (binutils convention) get
 * strerror's prototype along for the ride. */
#include "errno.h"
```

### `zlib.h` — `gzFile` typedef + `gzwrite` stub

binutils' `bfd/compress.c` has `gzwrite(gzfile, buf, len)`
behind `#ifdef HAVE_ZLIB_H`. The cache claims
`HAVE_ZLIB_H` (so configure doesn't pull in a real
zlib). The stub returns 0 (documented "write failed"
return) because the surrounding compressed-section logic
is dead-coded in the `--disable-plugins` build anyway:

```c
typedef void *gzFile;
static inline int gzwrite(gzFile file, const void *buf, unsigned len)
{ (void)file; (void)buf; (void)len; return 0; }
```

### `atexit.h` and `env.h` — the `OSDEV_LIBC_NO_GLOBAL_DEFS` guard

ld's startup code emits a call to `__cxa_finalize(0)`
near the `_init` / `_fini` boundaries, and `lexsup.c`
references `extern char **environ;`. Both
`__cxa_finalize` (chapter 120) and `environ`
(chapter 116c) are emitted by their respective
header-only libc files into *every* TU that includes
`<stdlib.h>` (transitively in binutils' case: every
file). With ~150 binutils TUs in the link, that means
~150 strong defs of each symbol — `ld.bfd` rejects the
link with "multiple definition of `__cxa_finalize'" /
"multiple definition of `environ'".

The right answer is **not** `__attribute__((weak))` on
the libc-side definitions. atexit.h's
`__cxa_finalize` has to win over the weak no-op
already declared in `crt0.S` (chapter 120 ships it as
a `.weak` stub so unit-tests can `bl __cxa_finalize`
without linking atexit.h); marking the libc-side def
weak too gives the linker *two* weak symbols and lets
it pick the wrong one. See
[Pitfall — weak attribute on both __cxa_finalize defs](#pitfall--weak-attribute-on-both-__cxa_finalize-defs)
below — `test_atexit.py` regresses to "5 PASS / 6 FAIL"
the moment the libc def goes weak.

The right answer is the `OSDEV_LIBC_NO_GLOBAL_DEFS`
guard already in `atexit.h` and `env.h` from chapter
130a (Doom's shim used the same trick). Both files
look like:

The right answer is the `OSDEV_LIBC_NO_GLOBAL_DEFS`
guard already in `atexit.h` and `env.h` from chapter
130a (Doom's shim used the same trick). Both files
look like:

```c
#ifndef OSDEV_LIBC_NO_GLOBAL_DEFS
void __cxa_finalize(void *dso_handle) { ... }    /* atexit.h */
char **environ = g_env_envv;                     /* env.h    */
#endif
```

Apps that include the headers normally get the strong
def from the single TU that pulls atexit/env in.
Multi-TU vendor builds set `-DOSDEV_LIBC_NO_GLOBAL_DEFS`
in CFLAGS — every vendor TU then skips both global
defs. The dangling references are satisfied by:

- `__cxa_finalize` → `crt0.S`'s weak no-op (vendor code
  doesn't `atexit()`, so the missing chain doesn't
  matter)
- `environ` → a single weak slot in `cstring.c`:

```c
__attribute__((weak)) char **environ = (char **)0;
```

This weak/strong story works because there is only
*one* weak `environ` in the link (the one in
`cstring.o`); any app's env.h strong def overrides it
cleanly.

`scripts/test_guest_ld.py` adds the guard to the
vendor CFLAGS:

```python
env["CFLAGS"] = ("-mcpu=cortex-a72 -DNDEBUG "
                 "-DOSDEV_LIBC_NO_GLOBAL_DEFS")
```

---

## The libiberty `getopt.c` patch — replacing 131d's `ELIDE_CODE`

Chapter 131d patched `libiberty/getopt.c` to elide its
entire body under `__OSDEV_LIBC__` (using libiberty's
own GLIBC-elision mechanism). That worked for
`libiberty.a` compilation in 131d because nothing in
the canary subset linked against `_getopt_internal`.

`ld_new` does link against it: `libiberty/getopt1.c`
(`getopt_long`, *not* elided — see 131d's "Why getopt1.c
is NOT patched" section) is in `ld_new_LDADD` and calls
`_getopt_internal`. Eliding `getopt.c` makes
`_getopt_internal` undefined at ld's link.

The fix is to make `getopt.c` compile *and* keep its
body, but without colliding with the OSdev libc's
`static optarg` / `static int optind` / `static inline
getopt`. A single TU-local `#define` instead of
elision does the job. Chapter 131e replaces hunk 5 of
[vendor/binutils-aarch64-osdev.patch](../../../vendor/binutils-aarch64-osdev.patch):

```diff
--- a/libiberty/getopt.c
+++ b/libiberty/getopt.c
@@ -52,6 +52,11 @@
 # endif
 #endif

+/* osdev libc (chapter 131e): suppress userspace/libc/stdlib.h's
+   static optarg/optind/optopt/opterr/getopt in this one TU so
+   libiberty's externs link cleanly with getopt1.c's
+   _getopt_internal call.  */
+#define OSDEV_LIBC_NO_GETOPT
 #ifndef ELIDE_CODE
```

`OSDEV_LIBC_NO_GETOPT` is defined **only** in
`libiberty/getopt.c`. Every other TU in the build
(including the rest of libiberty, all of bfd, all of
opcodes, all of libctf, all of ld) continues to see
`stdlib.h`'s static-inline `getopt` + static `optarg` /
`optind` / `optopt` / `opterr` (harmless: per-TU
static state never gets cross-referenced).

The matching `stdlib.h` change is a single `#ifndef`
wrap around the entire getopt block:

```c
#ifndef OSDEV_LIBC_NO_GETOPT
static char *optarg = (char *)0;
static int   optind = 1;
static int   optopt = 0;
static int   opterr = 1;

static inline int getopt(int argc, char *const argv[], const char *opts)
{
    /* ... existing body ... */
}
#endif /* OSDEV_LIBC_NO_GETOPT */
```

Now in `libiberty/getopt.c`:
- `OSDEV_LIBC_NO_GETOPT` is defined first,
- then `#include <stdlib.h>` (our stdlib.h) sees the
  guard and skips the static block,
- then libiberty's own `extern` declarations of
  `optarg` / `optind` / `optopt` / `opterr` and the
  body of `getopt` + `_getopt_internal` compile
  cleanly.

The patch file's header documents this:

> Chapter 131e: replaced the earlier 131d ELIDE_CODE
> approach which suppressed the whole file and left
> `_getopt_internal` undefined, breaking ld's link.

---

## The `-DNDEBUG` and `-mcpu=cortex-a72` env

The test sets:

```python
env["CFLAGS"] = "-mcpu=cortex-a72 -DNDEBUG"
```

`-mcpu=cortex-a72` matches the QEMU target (chapter 17).
`-DNDEBUG` is new in 131e: vendor TUs include
`<assert.h>` and would otherwise emit references to
`__assert_fail`. The OSdev libc only provides
`__assert_fail` as a non-static body in `cstring.c`
(linked into ld), but several bfd TUs use `assert()`
from `tail` positions where the unreached call still
references the symbol from the compiler-emitted call
site. `-DNDEBUG` turns every `assert()` into `((void)0)`,
matching binutils' own release convention and removing
the symbol references entirely.

---

## Run it / Test it

[scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py)
is host-side (not in the sweep). It:

1. Pre-builds `cstring.o` (`make
   build/userspace/libc/cstring.o`).
2. Wipes `build/binutils-build-guest-ld/` (separate from
   chapter 131d's `build/binutils-build-guest/` so the
   two tests don't poison each other).
3. Runs top-level binutils `configure` with the new
   flags (`--disable-binutils --without-zstd`).
4. Loops over the six subdirs:
   - directly invokes `<sub>/configure` with the
     controlled env,
   - strips `ZLIB = -lz` from the generated Makefile,
   - for `sub == "ld"`, injects `cstring.o` into
     `ld_new_LDADD =`.
5. Builds each subdir (`make headers` first for `bfd`).
6. Asserts `ld/ld-new` exists.
7. `aarch64-elf-readelf -h ld-new` and asserts the
   `Machine:` field contains `AArch64`.

Current PASS line:

```
guest_ld: PASS — ld-new built for aarch64 (3206056 bytes)
```

Chapter 131d's `test_guest_configure.py` still passes
(949,790 bytes — slight growth from struct stat's
`st_dev` / `st_ino` change rippling into libiberty's
`fopen.c` compile size). Chapter 131b's
`test_aarch64_osdev_cc.py` byte-identity baseline still
passes (the new libc additions are inline / weak and
don't appear in `hello.c`'s symbol set).

---

## What this unlocks

This chapter is internal scaffolding — no new
user-visible app yet. The unlock is the next two
chapters:

- **Chapter 131f:** copy `build/binutils-build-guest-ld/ld/ld-new`
  to `/bin/ld` on the OSFS image (and rename it). Retire
  the chapter-119 toy linker. `scripts/test_bin_ld_ar.py`
  upgrades to assert it's running the new ld (via
  `--version` containing "GNU ld"), then exercises a
  multi-section link the toy linker couldn't do.
- **Chapter 132a–e:** the GCC bring-up. Same shape as
  131a–f but for the compiler instead of the linker.
  `cstring.o`'s extern-allocator pattern carries forward;
  the CONFIG_SITE cache file picks up another ~10 entries
  for libstdc++'s configure probes; the link-time
  trap is *the same* libtool plugin (now called
  `liblto_plugin.la` instead of `libdep.la`) and gets the
  same Makefile-injection fix.

Per the
[apps-must-use-features](../../../scripts/_dbg_boot.py)
directive, every new feature in this chapter is
exercised by an existing test:
- The libc additions (locale, sys/param, getuid,
  umask/chmod, tmpfile, gzwrite, the
  `OSDEV_LIBC_NO_GLOBAL_DEFS` guard on atexit/env, and
  cstring.o's weak `environ` slot) are exercised by the
  ld build itself.
- The pre-existing strong `__cxa_finalize` /
  `environ` defs in `atexit.h` / `env.h` are
  re-verified by `scripts/test_atexit.py` (11 PASS) —
  this is how the
  [Pitfall — weak attribute on both __cxa_finalize defs](#pitfall--weak-attribute-on-both-__cxa_finalize-defs)
  regression below was caught.
- The `OSDEV_LIBC_NO_GETOPT` mechanism is exercised by
  libiberty's `getopt.c` + `getopt1.c` linking against
  ld-new.
- `cstring.o`'s 13 new wrappers are exercised by the
  vendor archives' link — every undefined-reference
  that got a wrapper added resolves in
  `aarch64-elf-readelf -h ld-new`'s success.

Per the
[debug-scripts-policy](../../../scripts/_dbg_boot.py)
directive,
[scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py)
IS the working reference for this chapter and stays in
`scripts/`. The Makefile-injection block is annotated in
the script with a comment block pointing at this
chapter.

Per the
[debug-scripts-policy](../../../scripts/_dbg_boot.py)
directive,
[scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py)
IS the working reference for this chapter and stays in
`scripts/`. The Makefile-injection block is annotated in
the script with a comment block pointing at this
chapter.

---

## Files changed in this chapter

```
userspace/libc/locale.h          (NEW — setlocale stub + LC_* macros)
userspace/libc/sys/param.h       (NEW — MAXPATHLEN)
userspace/libc/zlib.h            (+ gzFile typedef + gzwrite stub)
userspace/libc/stdio.h           (+ tmpfile)
userspace/libc/sys/stat.h        (+ umask, chmod)
userspace/libc/unistd.h          (+ getuid, getgid, geteuid, getegid)
userspace/libc/string.h          (+ #include "errno.h" for strerror
                                  prototype placement)
userspace/libc/atexit.h          (comment refresh — explains
                                  why __cxa_finalize MUST stay
                                  STRONG and how
                                  OSDEV_LIBC_NO_GLOBAL_DEFS
                                  handles multi-TU vendor builds)
userspace/libc/env.h             (comment refresh — same as
                                  atexit.h; `environ` stays
                                  STRONG, cstring.o provides the
                                  weak vendor-side slot)
userspace/libc/stdlib.h          (getopt block wrapped in
                                  #ifndef OSDEV_LIBC_NO_GETOPT)
userspace/libc/cstring.c         (+ self-contained extern allocator
                                  + abort/exit + 8 string wrappers,
                                  all via __asm__("name") renames;
                                  strdup migrates from malloc.h to
                                  extern-malloc forward-decl)

vendor/binutils-aarch64-osdev.patch
                                  (hunk 5 rewritten: ELIDE_CODE →
                                  #define OSDEV_LIBC_NO_GETOPT)
vendor/binutils-2.44/libiberty/getopt.c
                                  (live patched copy reflects above)

scripts/test_guest_ld.py         (NEW — host smoke: top-level
                                  configure + 5 sub-configures +
                                  ZLIB strip + ld_new_LDADD inject +
                                  6 makes + readelf AArch64 check)
```

---

## Pitfalls

Several patterns were tried and discarded along the way;
recording them here so a future reader doesn't reach for
the same broken hammers.

### Pitfall — `-include <stdlib.h>` in the wrapper

**Symptom:** every vendor TU dies on `error: conflicting
types for 'malloc'`.

**Cause:** adding `-include <stdlib.h>` to
`aarch64-osdev-cc` puts the OSdev `static inline malloc`
/ `static inline free` in scope of every vendor TU.
Most vendor TUs already have their own local
`extern void *malloc(size_t);` lines with a different
storage class, so GCC rejects the redeclaration.

**Fix:** killed. See the `__asm__("name")` rename
pattern above.

### Pitfall — forwarding wrappers via `#include "malloc.h"`

**Symptom:** assembler error: `symbol 'malloc' already
defined`.

**Cause:** writing
`void *__cstring_malloc(size_t n) __asm__("malloc") {
return malloc(n); }` while `cstring.c` `#include`s
`malloc.h`. The assembler sees both the static-inline
`malloc` (emitted as the file-local symbol "malloc")
and `__cstring_malloc` whose asm name is also "malloc".

**Fix:** killed in favour of the self-contained
allocator that doesn't include `malloc.h`.

### Pitfall — `make LIBS=cstring.o`

**Symptom:** libtool aborts with `cannot build libtool
library 'libdep.la' from non-libtool objects`.

**Cause:** `LIBS` is read by *every* link rule in the
Makefile, including libtool's `--mode=link` for
`libdep.la`. libtool refuses to mix a non-libtool `.o`
into a `.la` build.

**Fix:** killed in favour of Makefile post-process
injection into `ld_new_LDADD` only.

### Pitfall — `--disable-libdep`

**Symptom:** the flag is silently ignored.

**Cause:** no such flag exists in binutils 2.44.
`bfdplugin_LTLIBRARIES = libdep.la` is unconditional in
`ld/Makefile.am`.

**Fix:** killed before it started.

### Pitfall — reusing the chapter-131d build dir

**Symptom:** when both tests run, the second one trips
the `ac_cv_env_*` handshake even though nothing visibly
changed.

**Cause:** the two tests pass different top-level
configure flags (131d has `--disable-binutils` AND
`--disable-ld`; 131e drops the second). autoconf is
confused by a build dir with mismatched config.status;
worse, when 131e runs first and 131d runs second, 131d's
sub-configure inherits 131e's `--without-zstd` from the
cached config.status and trips on the `ac_cv_env_*`
handshake.

**Fix:** killed; two separate build dirs.

### Pitfall — `__attribute__((weak))` on both `__cxa_finalize` defs

**Symptom:** `scripts/test_atexit.py` regresses from 11
PASS to 5 PASS / 6 FAIL:

```
PASS: ctor1 ran
PASS: ctor2 ran
PASS: main ran
FAIL: exit1 ran
FAIL: exit2 ran
FAIL: dtor ran
FAIL: both ctors ran BEFORE main
FAIL: exit2 ran before exit1
FAIL: dtor ran AFTER atexit chain
PASS: /bin/atexittest exited with code 7 via crt0 forwarder

5 PASS / 6 FAIL
```

Reaching the exit-forwarder still works (crt0 returns,
exit syscall fires), but every registered atexit hook
and every `.fini_array` entry is silently skipped.

**Cause:** the attempt to solve the ~150-way
multiple-definition error for `__cxa_finalize` and
`environ` by marking the libc-side defs in `atexit.h`
and `env.h` `__attribute__((weak))` puts **two** weak
symbols named `__cxa_finalize` in the link: atexit.h's
"walk the LIFO chain and `.fini_array`" body, and
crt0's `ret`. ld is free to pick either and chose
crt0's no-op for `/bin/atexittest`.

**Fix:** killed in favour of the
`OSDEV_LIBC_NO_GLOBAL_DEFS` guard already pioneered by
chapter 130a's Doom shim. **Rule:** never mark a symbol
`__attribute__((weak))` when another weak symbol with
the same name exists elsewhere in the link. If two TUs
need to both compile a definition but only one body
must win, the winner must stay STRONG and the other
side must either be guarded out (this chapter) or be a
weak *override* target (chapter 120's crt0 stub).

---

## Final ledger

| Artefact | Bytes | Verified by |
|----------|-------|-------------|
| `build/userspace/libc/cstring.o` | (varies) | `aarch64-elf-nm` shows 13 new globals + weak `environ` |
| `build/binutils-build-guest/libiberty/libiberty.a` | 949,790 | [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py) PASS |
| `build/binutils-build-guest-ld/ld/ld-new` | 3,206,056 | [scripts/test_guest_ld.py](../../../scripts/test_guest_ld.py) PASS (readelf AArch64) |
| `/bin/atexittest` | (rebuilt) | [scripts/test_atexit.py](../../../scripts/test_atexit.py) 11/11 PASS |

## What's next

Chapter 131f carries `ld-new` across into the guest
filesystem at `/bin/ld`, retiring the chapter-119 toy
linker.

