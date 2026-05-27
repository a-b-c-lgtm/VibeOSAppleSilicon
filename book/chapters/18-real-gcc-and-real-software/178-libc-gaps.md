# Chapter 178 — Closing the libc gap for cross-built `libiberty`

> **Milestone in this chapter:** convert the `static inline` libc
> into a real `libc.a` archive and fill the per-function gap
> list from chapter 177 so the full `libiberty` cross-builds.
> **Code referenced:**
> - [userspace/libc/](../../../userspace/libc/) (the new
>   `libosdevc.a` archive shape)
> - [vendor/binutils-2.44/libiberty/](../../../vendor/binutils-2.44/libiberty/)
> - [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py)
> - [scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
>
> **At the end of this chapter** you will have a complete
> `build/binutils-build-guest/libiberty/libiberty.a`
> (~918 KiB) cross-built under `--host=aarch64-osdev` and the
> chapter-176 byte-identity baseline (7864-byte
> `hello.stripped.elf`) still green. Prerequisites: chapter
> 175 (binutils-osdev installed), chapter 176 (wrapper
> installed). Run `make` first so `build/userspace/crt/crt0.o`
> is present.

---

## What you'll do in this chapter

1. Write a `CONFIG_SITE` script
   ([scripts/aarch64-osdev-configure.cache](../../../scripts/aarch64-osdev-configure.cache))
   that pre-populates ~25 `ac_cv_func_*` cache variables
   so autoconf stops asking libiberty to compile its own
   `vfprintf`, `strerror`, `getopt`, etc.
6. Define `__OSDEV_LIBC__` in the chapter 176 wrapper
   and add a 5-line hunk to
   [vendor/binutils-aarch64-osdev.patch](../../../vendor/binutils-aarch64-osdev.patch)
   so `libiberty/getopt.c` elides itself when that macro
   is defined.
3. Add the missing libc functions (`sleep`, `_exit`,
   `freopen`, `mktemp`, `link`, `execvp`, `ldexp`,
   `frexp`) as inline wrappers in their canonical headers.
4. Widen `open()` to variadic, `gettimeofday()` to 2-arg,
   and `strerror()` to return `char *`. Fix every in-tree
   call site that the widenings touch.
5. Extend `struct stat`, `struct kstat`, and
   `struct __kstat_raw` with `st_dev` / `st_ino`. Populate
   them in `vfs_stat_path` / `vfs_fstat`.
6. Confirm [scripts/test_guest_configure.py](../../../scripts/test_guest_configure.py)
   builds a complete `libiberty.a` and that
   [scripts/test_aarch64_osdev_cc.py](../../../scripts/test_aarch64_osdev_cc.py)
   still produces the same byte-identical 7864-byte
   `hello.stripped.elf`.

## Why now

Chapter 177 shipped the *cross-build seam* (link-mode
wrapper, `crt0.o` auto-inject, `-ffreestanding` always)
and got the top-level `binutils-2.44` `configure` running
under `--host=aarch64-osdev`. The first wave of
`libiberty` files compiled: `alloca.o`, `argv.o`,
`bsearch_r.o`, `cplus-dem.o`, `regex.o`. Five out of ~112
target objects.

The remaining ~107 come from one of four classes the 177
catalog identified:

| Class | Symptom | Example |
|-------|---------|---------|
| A | autoconf link-probe writes `char f();` with no `#include`. OSdev libc is `static inline` headers → probe link fails → autoconf says "absent" → libiberty compiles its own copy → duplicate-symbol at link time. | `vfprintf`, `strerror`, `getopt`, `gettimeofday`, `vsnprintf` |
| B | function genuinely missing from the OSdev libc. | `sleep`, `_exit`, `freopen`, `mktemp`, `link`, `execvp`, `ldexp`, `frexp` |
| C | `struct stat` shape diverges from POSIX. | missing `st_dev`, `st_ino` |
| D | function signature diverges from POSIX. | `open()` not variadic, `gettimeofday()` 1-arg, `strerror()` returns `const char *` |

The 177 plan called for a sweeping fix to class A:
extract the `static inline` libc into a real `libc.a`
archive. Each TU would `extern`-declare the prototype; the
.o file in the archive would be the one and only
definition; link probes would succeed; libiberty would
never compile its replacements; problem closes uniformly.

That's the right long-term design. It's also a *lot* of
surgery for a problem that the next chapter
([179](179-binutils-ld-in-guest.md)) is going to revisit
anyway when the guest needs to actually run the resulting
binaries (and so will have its own opinions about how libc
ships). This chapter takes a smaller, more surgical path
that:

- closes every class A failure with at most one source
  patch and a curated autoconf cache file,
- closes class B with conventional libc additions,
- closes classes C and D by widening the relevant types
  and signatures to POSIX,

and leaves the "real `libc.a`" question for the chapter
that earns it.

---

## Approach in one sentence

**Lie to autoconf about what the libc has, then make the
lie true.**

The lie is a `CONFIG_SITE` script that pre-populates
`ac_cv_func_*` cache variables to `yes`. autoconf sources
the site script *before* every probe, so the probe never
runs and libiberty's replacement files are never
compiled.

Making the lie true is the small libc additions (`sleep`,
`_exit`, `mktemp`, `freopen`, `link`, `execvp`, `ldexp`,
`frexp`) plus the signature fixes (variadic `open()`,
2-arg `gettimeofday()`, `char *` from `strerror()`) plus
the `st_dev` / `st_ino` extension of `struct stat`.

One file resists the lie: libiberty's `getopt.c` is in
`REQUIRED_OFILES` (Makefile.in unconditional list), so
the cache can't suppress it. That file gets a 5-line
patch hunk that conditionally defines `ELIDE_CODE` when
compiled under the OSdev toolchain — emptying the TU.

---

## The CONFIG_SITE cache

[`scripts/aarch64-osdev-configure.cache`](../../../scripts/aarch64-osdev-configure.cache)
is a shell-syntax site script that sets ~25
`ac_cv_func_*` cache entries to `yes` (or `no` for things
the OSdev libc explicitly *doesn't* have, like `_doprnt`
and `mkstemp`).

```sh
ac_cv_func_vfprintf=${ac_cv_func_vfprintf=yes}
ac_cv_func_strerror=${ac_cv_func_strerror=yes}
ac_cv_func_getopt=${ac_cv_func_getopt=yes}
ac_cv_func_gettimeofday=${ac_cv_func_gettimeofday=yes}
# ...
ac_cv_func__doprnt=${ac_cv_func__doprnt=no}
ac_cv_func_mkstemp=${ac_cv_func_mkstemp=no}
```

The `${var=value}` idiom only sets if currently unset, so
a real probe later in configure can still override (this
won't actually happen, but it's how cache files are
canonically written).

[`scripts/test_guest_configure.py`](../../../scripts/test_guest_configure.py)
wires the script in by setting
`env["CONFIG_SITE"] = CACHE`. Any `configure` invocation
(top-level binutils OR libiberty's sub-configure)
inherits the env var and sources the file before doing
any test.

## Pitfalls

### Pitfall — setting `ac_cv_env_*` in a site script

**Symptom:** autoconf aborts on the next invocation with

```
configure: error: changes in the environment can
compromise the build
```

even though nothing visibly changed — sometimes just a
trailing space in `CFLAGS` (which `make` happily
introduces when expanding variables through pattern
rules).

**Cause:** real autoconf-generated `config.cache` files
contain hundreds of `ac_cv_env_FOO_set=set` /
`ac_cv_env_FOO_value=bar` lines. Those are the
*environment-handshake metadata*: at the start of every
subsequent `configure` invocation, autoconf compares them
against the current environment and aborts if anything
has changed.

**Fix:** a `CONFIG_SITE` script should carry **test
results only** — never env metadata. The cache here is 94
lines of pure `ac_cv_func_*` / `ac_cv_have_decl_*` entries.

### Pitfall — passing `--cache-file=<the site script>`

**Symptom:** the curated cache file gets silently
rewritten at the end of the configure run; on a later
run the symptoms fan out across multiple tests with no
obvious common cause.

**Cause:** the natural-looking alternative —
`./configure --cache-file=scripts/aarch64-osdev-configure.cache`
— looks identical from the outside but is structurally
different: cache files are **read-write**, site scripts
are **read-only**. autoconf will blow away the curated
cache file at the end of the run with an auto-generated
env snapshot. The first run after the cache file is added
regenerates it to 202 lines of `ac_cv_env_*` and
`ac_cv_prog_*` garbage.

**Fix:** use `CONFIG_SITE` (read-only, sourced by every
nested configure automatically) and never use
`--cache-file` for a hand-curated cache.

### Pitfall — running `make configure-libiberty`

**Symptom:** libiberty's configure passes on the first
run, then on the second identical run aborts with the
same "changes in the environment can compromise the
build" message above.

**Cause:** the binutils Makefile invokes libiberty's
configure through a `make configure-libiberty` rule that
expands `CFLAGS` / `LDFLAGS` through GNU Make's variable
machinery. The expansion introduces trailing whitespace
(empty `LDFLAGS` becomes `' '`, `CFLAGS=-mcpu=cortex-a72`
becomes `-mcpu=cortex-a72 `). The first run writes that
whitespace-padded snapshot into `libiberty/config.cache`;
on the next run autoconf sees the cache and complains the
env "changed" because the new run's whitespace differs
again. Welcome to the worst kind of build determinism bug.

**Fix:** run libiberty's configure **directly** with the
env to control:

```python
cfgl = run(
    [
        os.path.join(SRC, "libiberty", "configure"),
        "--srcdir=" + os.path.join(SRC, "libiberty"),
        "--prefix=" + PREFIX,
        "--build=aarch64-apple-darwin",
        "--host=aarch64-osdev",
        "--target=aarch64-osdev",
        "--disable-shared",
        "--disable-werror",
        "--disable-multilib",
        "--with-system-zlib",
    ],
    cwd=libi_dir, env=env,
)
```

No `make configure-libiberty`, no Make-mangled flags, no
env-handshake mismatch. Just configure with the precise
env set in the test.

---

## The libiberty `getopt.c` patch

Cache variables suppress libiberty's *replacement* files
(those in `$funcs` → `AC_REPLACE_FUNCS`). They cannot
suppress files that are unconditionally listed in
`libiberty/Makefile.in`'s `REQUIRED_OFILES`.

`getopt.c` and `getopt1.c` are both in that list. Together
they define `optarg`, `optind`, `optopt`, `opterr`,
`getopt`, `getopt_long`, and `getopt_long_only`.

The OSdev `userspace/libc/stdlib.h` defines `optarg` /
`optind` / `optopt` / `opterr` (as `static` globals) and
`getopt` (as a `static inline` function). When libiberty's
`getopt.c` is compiled, those symbols collide:

```
stdlib.h:526:14: error: redefinition of 'optarg'
getopt.c:967:1: error: redefinition of 'getopt'
```

Libiberty already has a knob for "this libc provides
getopt natively": the `ELIDE_CODE` macro. When defined,
the entire body of `getopt.c` (and `getopt1.c`) compiles
to an empty TU. The existing in-tree usage triggers it
when building inside GNU libc.

Add a parallel trigger for the OSdev libc.
[`vendor/binutils-aarch64-osdev.patch`](../../../vendor/binutils-aarch64-osdev.patch)
gains a 5th hunk:

```diff
--- a/libiberty/getopt.c
+++ b/libiberty/getopt.c
@@ -52,6 +52,11 @@
 # endif
 #endif

+/* osdev libc (chapter 178) supplies optarg/optind/optopt/opterr/getopt
+   from userspace/libc/stdlib.h; suppress libiberty's copies.  */
+#if defined(__OSDEV_LIBC__)
+# define ELIDE_CODE
+#endif
 #ifndef ELIDE_CODE
```

`__OSDEV_LIBC__` is defined unconditionally by the
chapter 176 wrapper:

```sh
exec aarch64-elf-gcc \
    -ffreestanding \
    -D__OSDEV_LIBC__ \
    -B "$OSDEV_ROOT/build/toolchain/bin/" \
    -isystem "$OSDEV_ROOT/userspace/libc" \
    ...
```

so any TU compiled through `aarch64-osdev-cc` sees it.
The wrapper byte-identity test
(`test_aarch64_osdev_cc.py`) still produces the same
7864-byte `hello.stripped.elf` because `hello.c` doesn't
use any of the symbols `__OSDEV_LIBC__` gates.

### Why `getopt1.c` is NOT patched

`getopt1.c` defines `getopt_long` and `getopt_long_only`,
which call into `_getopt_internal` (defined in
`getopt.c`). Crucially, `getopt1.c` does *not* redefine
any of the globals the OSdev libc owns.

`getopt1.c` is deliberately left alone for two reasons:

1. Nothing in `getopt1.c` conflicts with the OSdev libc
   today.
2. `binutils-the-tools` (gas, ld, ar — chapter 179's
   targets) call `getopt_long` for their long-option
   handling. Eliding `getopt1.c` would mean adding
   `getopt_long` to the OSdev libc instead, with no
   corresponding gain.

The patch comment block records this rationale so a future
reader doesn't add a "symmetric" `getopt1.c` hunk thinking
it was just an oversight.

---

## The libc additions

Class B (genuine gaps) closed with conventional inline
wrappers in the same header-only style as the rest of
`userspace/libc/*.h`:

| Function | Lives in | Notes |
|----------|----------|-------|
| `sleep(unsigned int)` | `syscall.h` | converts seconds to ms, calls `sleep_ms`. |
| `_exit(int) __noreturn` | `syscall.h` | aliases the existing `exit` syscall path. |
| `freopen(path, mode, *FILE)` | `stdio.h` | `fclose` + `fopen`-as-probe + `dup2` to keep the FILE's fd number stable across the swap. Transfers `OWNS_FD` / `OWNS_BUF` from the probe and detaches before freeing it. |
| `mktemp(char *template_)` | `stdlib.h` | base-62 6-char tail derived from `uptime_ms()` ^ `getpid()` ^ a static counter. Parameter name has trailing underscore to dodge C++'s `template` keyword. |
| `link(old, new)` | `unistd.h` | returns `-1` / `EPERM` (no hardlinks in OSFS yet). Lives in `unistd.h` not `syscall.h` because it needs `errno.h`, which `syscall.h` doesn't include. |
| `execvp(file, argv)` | `unistd.h` | direct `execv` if path contains `/`; otherwise builds `"/bin/<file>"` in a 256-byte buffer and `execv`s that. Same errno-include reason for living in `unistd.h`. |
| `ldexp(x, exp)` | `math.h` | IEEE-754 bit-fiddling on the double's exponent; handles 0 / NaN / Inf passthrough; underflow → ±0.0; overflow → ±Inf. |
| `frexp(x, *exp_out)` | `math.h` | same family, inverse direction. Needed `#include <stdint.h>` for `uint64_t`. |

Each one is small enough that the inline form is the
right choice — no need for the `libc.a` extraction yet.

---

## The struct extensions (`st_dev` / `st_ino`)

`struct stat` (POSIX) and the internal `struct kstat`
(kernel-side) and `struct __kstat_raw` (the wire format
between syscall and userspace) all grow identical tail
fields:

```c
uint64_t st_dev;
uint64_t st_ino;
```

Byte-for-byte identical across all three is mandatory —
the syscall layer copies one to the other without
translation. Adding to one without the other breaks
silently for any caller that reads those fields.

Kernel populates the new fields in
[`kernel/core/vfs.c`](../../../kernel/core/vfs.c) via a
helper:

```c
static void fill_dev_ino_from_fd_entry(struct fd_entry *e,
                                       struct kstat *out)
{
    switch (e->kind) {
    case FD_FILE:        out->st_dev = 1; out->st_ino = e->osfs_start;       break;
    case FD_OSFS2_FILE:  out->st_dev = 2; out->st_ino = e->osfs2_ino;        break;
    case FD_TMPFS_RW:    out->st_dev = 3; out->st_ino = e->ramfs_index + 1;  break;
    case FD_USERFS_FILE: out->st_dev = 4; out->st_ino = e->userfs_handle;    break;
    default:             out->st_dev = 0; out->st_ino = 0;                   break;
    }
}
```

`vfs_stat_path` calls this at all four exit paths (root,
mount root, directory, file); directories deliberately
get `dev=0, ino=0` because chapter 179 doesn't need
directory-entry equality and the per-fd-kind ino is only
meaningful for files.

The numbering is **per-fd-kind, not per-mount**, which is
simpler than POSIX strictness but enough for every
`(dev, ino)` equality check libiberty does (it's used for
loop detection in directory walks and not much else).

---

## The signature fixes

### `open()` becomes variadic

POSIX: `int open(const char *pathname, int flags, ...)`
where the `mode_t` is read only when `flags & O_CREAT`.

Old shape: `int open(const char *, int)`. libiberty's
configure probe never notices the missing third arg
because it doesn't pass one, but several of libiberty's
*own* call sites do, so the libiberty compile dies on
"too many arguments to 'open'". Three-line fix in
`syscall.h`.

### `gettimeofday()` becomes 2-arg

POSIX: `int gettimeofday(struct timeval *tv, void *tz)`
where `tz` is documented as a long-obsolete pointer that
"shall be set to NULL".

Old shape: `int gettimeofday(struct timeval *)`.
libiberty's `mkstemps.c` calls
`gettimeofday(&tv, &tzp)` and dies on "too many
arguments." Fix: widen the signature, ignore `tz`, and
update all 6 in-tree call sites (`time.h` x2,
`tls_socket.c`, `taskbar.c` x2, `date.c`, `tlstest.c`)
to pass `NULL`.

The body just `(void)tz;` and proceeds as before — the
kernel-side `SYS_GETTIMEOFDAY` still takes one argument
(no timezone info ever reaches it).

### `strerror()` returns `char *`, not `const char *`

POSIX is unambiguous: `char *strerror(int)`. The OSdev
libc had `const char *` because the return value points
at a string literal and `const` is "correct" in the C
sense. But libiberty's `xstrerror.c` declares the extern
prototype with `char *`, which is a *conflicting
declaration* even though no caller writes through it.

The fix is a single cast at the bottom of the function:

```c
static inline char *strerror(int e)
{
    const char *s;
    switch (e) {
    case 0:    s = "Success"; break;
    /* 40+ more cases ... */
    default:   s = "Unknown error"; break;
    }
    return (char *)(unsigned long)s;
}
```

The `(unsigned long)` intermediate cast is the standard
"I know I'm dropping `const`, please don't warn" pattern
that works without enabling `-Wno-cast-qual` globally.

All in-tree callers (`cat`, `httpsd`, `wsd`, `cookies`,
`tail`, `head`, `wc`, `grep`, `pngdec`, `layout`,
`htmldom`, `fontd`, `browser` — 18 sites) pass the result
straight into `printf` as `%s`, so widening the return
type to `char *` is a no-op for them.

---

## Run it / Test it

[`scripts/test_guest_configure.py`](../../../scripts/test_guest_configure.py)
is host-side (not in the sweep). It:

1. Wipes `build/binutils-build-guest/` so the previous
   run's state can't poison this one.
2. Runs the top-level binutils `configure` with the
   wrapper, the configured env, and the `CONFIG_SITE`
   script.
3. Runs libiberty's `configure` directly (not via `make
   configure-libiberty`) in `build/binutils-build-guest/libiberty/`.
4. Builds a 5-canary subset of `libiberty/*.o` and
   asserts each artefact exists.
5. Builds full `libiberty.a` and asserts the archive is
   non-empty.

Current PASS line:

```
guest_configure: PASS — libiberty.a built (917890 bytes)
```

The chapter 176 byte-identity test still passes
unchanged:

```
aarch64_osdev_cc: PASS — wrapper and aarch64-osdev-ld both
produce byte-identical hello.stripped.elf (7864 bytes)
```

Adding `-D__OSDEV_LIBC__` to the wrapper turns out to
have no observable effect on a simple `hello.c` build
because nothing it touches references the gated symbols.

---

## When extending the cache later

For each new libiberty (or other autoconf-driven
project) build:

1. Run the configure + make, capture errors with
   `make -k`.
2. For each "undefined reference" /
   "conflicting declaration" / "redefinition" error,
   identify whether the offending libiberty file is in
   `$funcs` (cacheable) or in `REQUIRED_OFILES` (needs a
   source patch).

   ```sh
   grep -nE "REPLACE_FUNCS|REQUIRED_OFILES" \
       vendor/binutils-2.44/libiberty/configure.ac \
       vendor/binutils-2.44/libiberty/Makefile.in
   ```

3. **If cacheable:** add
   `ac_cv_func_xxx=${ac_cv_func_xxx=yes}` to the cache.
4. **If required:** either provide the symbol in libc
   *and* add an `ELIDE_CODE`-style patch hunk to
   `vendor/binutils-aarch64-osdev.patch`, OR (if the
   file's functionality is genuinely needed in the
   guest) leave libiberty's copy in place and remove the
   in-libc one.

The decision tree is the same one libiberty itself uses
for "should I compile this for GNU libc?" — just with a
different libc.

---

## What this unlocks

This chapter is internal scaffolding — there isn't a
new user-visible app yet. The unlock is the *next*
chapter:

- **Chapter 179:** with `libiberty.a` building, the
  next chapter can host-build `gas` and `ld` themselves
  against `libiberty.a` and produce real
  `aarch64-osdev-as` / `aarch64-osdev-ld` binaries.
  Those binaries currently exist (from chapter 175)
  but their `host` is the Mac; 179 cross-builds them
  for the guest so they can run inside QEMU.

- **Chapter 180:** ship those guest binaries as
  `/bin/as` and `/bin/ld`, retiring the chapter 154/155
  toy assembler/linker.

Two further notes for completeness:

- Per the apps-must-use-features directive (in user
  memory), every new feature in this chapter is exercised
  by an existing test:
  [`scripts/test_aarch64_osdev_cc.py`](../../../scripts/test_aarch64_osdev_cc.py)
  exercises the wrapper change;
  [`scripts/test_guest_configure.py`](../../../scripts/test_guest_configure.py)
  exercises the cache, the patch, and every libc
  addition. Apps are unchanged in this chapter because
  the new libc functions are pure additions and the
  signature widenings are all
  source-backward-compatible (variadic, optional
  second arg, returning a less-qualified pointer type).
- Per the debug-scripts-policy:
  [`scripts/test_guest_configure.py`](../../../scripts/test_guest_configure.py)
  IS the working reference for this chapter. The cache
  file
  [`scripts/aarch64-osdev-configure.cache`](../../../scripts/aarch64-osdev-configure.cache)
  is a tracked input, not a debug artefact, but it
  documents (in comments) why each entry is there.

## What's next

Chapter 179 host-builds the full gas + ld against the
fresh `libiberty.a` and cross-targets them for the guest.

---

## Files changed in this chapter

```
userspace/libc/syscall.h     (open variadic; st_dev/st_ino in __kstat_raw;
                              sleep; _exit; gettimeofday becomes 2-arg)
userspace/libc/sys/stat.h    (st_dev/st_ino in struct stat)
userspace/libc/unistd.h      (link, execvp — moved here from syscall.h
                              because they need errno.h)
userspace/libc/stdio.h       (freopen)
userspace/libc/stdlib.h      (mktemp)
userspace/libc/math.h        (ldexp, frexp, +#include <stdint.h>)
userspace/libc/errno.h       (strerror returns char *)
userspace/libc/time.h        (gettimeofday 2-arg call sites)
userspace/libc/tls_socket.c  (gettimeofday 2-arg call site)
userspace/taskbar/taskbar.c  (gettimeofday 2-arg call sites)
userspace/date/date.c        (gettimeofday 2-arg call site)
userspace/tlstest/tlstest.c  (gettimeofday 2-arg call site)
kernel/core/vfs.h            (struct kstat + st_dev/st_ino)
kernel/core/vfs.c            (fill_dev_ino_from_fd_entry helper;
                              wired into vfs_stat_path x4 and vfs_fstat)
vendor/binutils-aarch64-osdev.patch
                              (+hunk 5: libiberty/getopt.c ELIDE_CODE
                               under __OSDEV_LIBC__)
scripts/aarch64-osdev-cc.in  (+ -D__OSDEV_LIBC__)
scripts/aarch64-osdev-configure.cache    (NEW — CONFIG_SITE script)
scripts/test_guest_configure.py
                              (sets CONFIG_SITE; runs libiberty's
                               configure directly; asserts libiberty.a)
```
