# Chapter 132h — `/bin/gcc` builds a medium real program

> **Milestone in this chapter:** prove the in-guest gcc on a
> non-trivial real program — a brainfuck interpreter compiled
> from source on disk — and verify the output is byte-identical
> to the host-built reference.
> **Code referenced:**
> - [userspace/bf/bf.c](../../../userspace/bf/bf.c)
> - [scripts/test_gcc_bf.py](../../../scripts/test_gcc_bf.py)
>
> **At the end of this chapter** you will have `/bin/bf` and
> its source at `/bin/bf.c` on the OSFS-1 image, a host
> Makefile rule that links `bf.elf` using the same crt0 +
> linker_user.ld + `--start-group -losdevc --end-group` shape
> the in-guest gcc produces, and `test_gcc_bf.py` at **PASS 6
> / FAIL 0**. Prerequisite: chapter 132g (default-spec link
> works).

---

## What you'll do in this chapter

1. Add `userspace/bf/bf.c` — ~210 lines of freestanding
   C: argv parsing, `compute_jumps`, a tight interpreter
   loop, all symbols `extern`-declared so no libc header
   is required.
2. Ship `bf.c` itself at `/bin/bf.c` on the OSFS-1 image
   so the in-guest gcc has source to compile, alongside
   the host-built `/bin/bf` and the asset `/bin/hello.bf`.
3. Wire a host Makefile rule that links `bf.elf` with the
   same `crt0.o + linker_user.ld + --start-group
   -losdevc --end-group` shape the in-guest gcc produces,
   so the byte-parity check actually means something.
4. Add `scripts/test_gcc_bf.py` (6-step ladder: host bf
   prints, in-guest gcc builds, guest bf2 prints, output
   matches byte-for-byte) and prove it PASS 6/6.

---

## Why now

The chapter-128 plan listed three candidates for "first
medium real program after default specs are working":
`sl(1)`, `bf` (brainfuck), and `cmatrix`. The winner was bf,
for five reasons:

1. **It is genuinely real code**, not a hello-world. It has
   argv parsing, malloc/free, file I/O, a precomputed jump
   table, and a tight interpreter loop. It exercises every
   pillar of `libosdevc.a`.
2. **It is small enough to debug.** ~210 lines of C, no
   external headers, one translation unit.
3. **The output is deterministic and byte-exact.** Given a
   canonical "Hello, World!" brainfuck program (the 1993
   one from `hello.b`), the only correct output is
   `Hello World!\n` — 13 bytes, no variation. That makes
   the parity check trivial.
4. **It doesn't need libc headers shipped on disk** — and
   that constraint, it turns out, is the real story of this
   chapter (see [The header-shipping problem](#the-header-shipping-problem)).
5. **It's a useful tool.** Long after this chapter ships,
   the OS will still have a working brainfuck interpreter
   in `/bin/`. Compare that to `sl(1)`, which is a joke
   command that prints an ASCII train, or `cmatrix`, which
   needs ncurses that isn't ported.

---

## Run it / Test it

```
$ ls -la build/userspace/bf/
-rwxr-xr-x  ... 11168 bf.elf
-rwxr-xr-x  ...  8744 bf.stripped.elf

$ ls -la assets/osfs/hello.bf
-rw-r--r--  ...   ~600 bytes (one canonical hello-world bf program)

$ ls -la build/disk.img
-rw-r--r--  ... 268435456 build/disk.img
```

The disk image grew from 113 to 114 files (still well under
the 128 cap). Two new entries: `bf` (the stripped binary)
and `bf.c` (the source). One asset added: `hello.bf`.

Test ladder:

```
[chapter 132h] /bin/gcc rebuilds bf from source
PASS: step 1: host-built /bin/bf prints 'Hello World!'
PASS: step 1: host bf banner present on stderr
PASS: step 2: in-guest gcc produced /tmp/bf2 ELF
PASS: step 3: guest-built /tmp/bf2 prints 'Hello World!'
PASS: step 3: guest bf2 banner present on stderr
PASS: step 4: host bf and guest-built bf2 agree byte-for-byte

PASS: 6
FAIL: 0
```

The invocation in step 2 is the chapter's payoff line:

```
$ /bin/gcc /bin/bf.c -o /tmp/bf2
```

No `-nostdlib`. No `-nostdinc`. No `-e _start`. No `-T`. No
`-L`. No `-l`. Just `gcc`, the source file, and the output
name. The default specs from chapter 132g, plus the on-disk
`crt0.o` / `libosdevc.a` / `linker_user.ld`, do the rest.

---

## The header-shipping problem

There is a tension between two facts in the toolchain that
this chapter forced into the open:

| Build mode      | Headers           | Libraries        |
|-----------------|-------------------|------------------|
| Host-side build | `userspace/libc/*.h` (43 headers, 716 KiB, `static inline`)         | none — everything is inlined from headers |
| In-guest build  | NONE — `/bin/gcc` has no `-isystem` path | `/bin/libosdevc.a` via LIB_SPEC's `--start-group -losdevc --end-group` |

The in-guest gcc has **no system include directory at all**.
The gccw shim (`userspace/gccw/gccw.c`) prepends only
`-B/bin/` to xgcc's argv. `-B<prefix>` extends the
startfile-prefix list and exec-prefix list, but does not
add to ld's `-L` search path (that fight is what chapter
132g was about) and definitely does not add to the
preprocessor's include path.

So an in-guest source file cannot `#include "stdio.h"`,
`#include "string.h"`, or `#include "syscall.h"`. Those
headers don't exist on disk.

### Why not just ship the headers?

Tempting. Count them:

```
$ ls userspace/libc/ | wc -l
43
$ du -sh userspace/libc/
716K
```

Shipping all of them would push OSFS-1's file count from
114 to 157, exceeding the **128-file cap** hard-coded into
the OSFS-1 superblock layout (DIR_SECTORS=8, dirent stride
32 bytes → 128 dirents max). Bumping that cap is a kernel
ABI change. It deserves its own chapter — not a footnote
in a "ship a brainfuck" chapter.

And even setting the count aside: `static inline` headers
don't actually compose well with a real cc1. Many of them
emit large code from a single `#include` (printf's format
machinery, for instance, expands to several hundred
instructions per call site). The right answer for in-guest
builds is real symbols in `libosdevc.a` plus thin headers
that declare them — but that's a refactor of the entire
`userspace/libc/` tree, and again, it's not this chapter.

### Freestanding-by-design

So `bf.c` is written as if there are no libc headers at all:

```c
/* userspace/bf/bf.c -- excerpt */

typedef unsigned long       size_t;
typedef long                ssize_t;
typedef unsigned char       uint8_t;
typedef unsigned long       uintptr_t;
typedef long                off_t;

#define O_RDONLY    0
#define TAPE_CELLS  30000
#define MAX_PROG    (1 * 1024 * 1024)

/* Forward-declare the libosdevc.a symbols this TU uses. */
extern int   open(const char *path, int flags, ...);
extern int   close(int fd);
extern ssize_t read(int fd, void *buf, size_t n);
extern ssize_t write(int fd, const void *buf, size_t n);
extern void  exit(int status);
extern void *malloc(size_t n);
extern void  free(void *p);
extern void *memset(void *p, int c, size_t n);
extern size_t strlen(const char *s);
```

No `#include` anywhere in the file. This is ugly — no
sensible long-term userspace program should be written
this way. But it is a faithful reflection of what the
in-guest gcc can actually compile today, and the chapter
needs an honest demonstration, not a misleading one.

The implication is general: **any source file the in-guest
gcc must compile has to be freestanding until the
libc-on-disk story is sorted out.** That's the next major
toolchain chapter.

---

## Host build vs in-guest build symmetry

The Makefile rule for `bf` is deliberately written to model
the in-guest default-spec link as closely as possible:

```makefile
BF_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/bf/bf.o
BF_ELF  := $(BUILD)/userspace/bf/bf.elf

$(BF_ELF): $(BF_OBJS) userspace/linker_user.ld $(XGCC_SYS_LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(BF_OBJS) \
	    -L$(XGCC_SYSROOT_LIB) --start-group -losdevc --end-group
```

That's the same command line the in-guest gcc effectively
produces from its default specs. Compare to the in-guest
invocation `gcc bf.c -o bf2`, which after spec expansion
runs:

```
cc1 bf.c                                            # CC1_SPEC -> -ffreestanding
as -o bf.o /tmp/cc...s
ld -T /bin/linker_user.ld                           # LINK_SPEC
   -L /bin                                          # LINK_SPEC (chapter 132g)
   /bin/crt0.o bf.o                                 # STARTFILE_SPEC
   --start-group -losdevc --end-group               # LIB_SPEC
   -o bf2
```

Modulo `-ffreestanding` (which `USER_CFLAGS` also passes
on the host build) and the precise input file order,
these are the same link line. That symmetry is what makes
step 4 of the test ("byte-identical output") a meaningful
claim rather than a tautology — the host build and the
guest build are genuinely doing the same work against the
same archive.

A more pedantic chapter would have done `cmp` between the
host `bf.stripped.elf` and the guest `/tmp/bf2`. This one
skipped that because `cmp`-via-serial is a pain and the
program-level output comparison is what users actually
care about. The symbol-level cross-check is left for a
future chapter once `cmp` works in the guest.

---

## The interpreter itself

For completeness, here's what `bf.c` actually does. (Skip
this section if you already know brainfuck.)

Brainfuck has 8 instructions, each one byte:

| Char | Effect                                 |
|------|----------------------------------------|
| `>`  | tape pointer ++                        |
| `<`  | tape pointer --                        |
| `+`  | tape[pointer] ++                       |
| `-`  | tape[pointer] --                       |
| `.`  | write tape[pointer] as a byte to fd 1  |
| `,`  | read one byte from fd 0 into tape[ptr] |
| `[`  | if tape[ptr]==0, jump to matching `]`  |
| `]`  | if tape[ptr]!=0, jump to matching `[`  |

`bf.c`'s `compute_jumps()` does a single pre-pass with a
stack of `[` positions, filling in a parallel `jump[]`
array so `[` and `]` are O(1) jumps instead of O(n) scans.
Non-bracket characters get `jump[i] = -1` and are ignored
in the run loop.

The tape is 30,000 cells of `uint8_t`, zero-initialized
via `memset`. `,` reads one byte and leaves the cell
unchanged on EOF (the bff / dbfi convention; the
alternative — set to 0 or -1 — would have made the
`hello.bf` program slightly different).

Output goes to fd 1 via `write(1, &byte, 1)`. The startup
banner (`bf: loaded N bytes from PATH\n`) goes to fd 2
via a small `puts_raw()` helper. Errors (no file, file too
big, missing argv) go through `die()` which prints `bf:
MSG\n` to fd 2 and calls `exit(1)`.

That's the whole program. ~210 lines including the typedefs
and forward declarations.

---

## Pitfalls

### Pitfall — the diagnostic that didn't fire

**Symptom:** This chapter ran through with zero kernel-
level diagnostics. The in-guest gcc just worked. The link
finished. The binary ran. The output matched.

**Cause:** Everything that could have gone wrong was
already shaken out by chapters 132d–132g.

**Fix:** No fix needed for this chapter — but treat the
suspicious silence as a checklist. If a future
"real program" chapter starts failing, the suspects are:

- **cc1 OOM** — chapter 132 increased exec stack limits.
- **`-losdevc` not found** — chapter 132g baked `-L /bin`
  into LINK_SPEC.
- **crt0 not found** — STARTFILE_SPEC + `-B/bin/` already
  handled that.
- **`__OSDEV_LIBC__` undefined → libiberty drags in
  hosted headers** — CPP_SPEC sets it (chapter 132d).
- **xgcc rebuild silently undoes the spec changes** —
  the memory note `gcc-B-prefix-does-not-imply-L.md`
  captured the touch-the-generators trick before this
  chapter started.

A real piece of upstream software (e.g. `make`) will
likely surface a fresh batch of these, but the brainfuck
interpreter — small, freestanding, no system headers — is
clean.

---

## What this unlocks

The thing this chapter actually validates is that a user
can drop a C source file onto the disk, run `/bin/gcc
$file -o $out`, and get a working binary. That's the
baseline contract a "real" toolchain owes its users.

What it does NOT validate:

- That a program can `#include` anything. (See header-
  shipping problem above. Next major toolchain chapter.)
- That `make` works in the guest. (Chapter 126 already
  ported a make, but a fresh end-to-end test against a
  multi-file project would be good. Chapter 133.)
- That floating point works in compiled-on-guest code.
  (libgcc soft-float is in libosdevc.a; needs a
  dedicated test.)
- That C++ works. (cc1plus is not on disk.)
- That static analysis warnings produced by guest gcc
  match host gcc. (Probably do; not tested.)

But it does validate the only thing it claims to: a
non-trivial program survives the round-trip from disk
source → in-guest gcc → on-disk binary → execution →
expected output.

Per the standing "apps must use the OS features the book
builds" rule:

- **New app:** `userspace/bf/bf.c` (~210 LOC). Lives in
  `/bin/bf` on the OSFS image. Useful as both a working
  interpreter and as the canonical "thing the guest can
  rebuild from source."
- **New asset:** `assets/osfs/hello.bf` — the 1993
  canonical "Hello World!" brainfuck program. Lives at
  `/bin/hello.bf` in the guest.
- **Source on disk:** `bf.c` itself is shipped at
  `/bin/bf.c` so the in-guest gcc has something to
  compile. (This is the first non-`crt0.o` source file
  on the image.)
- **Existing app modified:** `/bin/gcc` is exercised
  against a new, larger source than chapter 132g's
  `int main(void) { return 7; }`.
- **New test:** `scripts/test_gcc_bf.py`. PASS 6/6.
- **Existing tests still green:** `test_gcc_hello.py`
  (chapter 132g) still passes 10/10 — adding bf to the
  disk didn't disturb anything.

The pattern of "ship the source alongside the binary
and have the test rebuild it in-guest" is the right
shape for every future "real program" chapter (133+).
This chapter is the first instance of that pattern.

---

## What's next

- Chapter 133a ports `make` so multi-file programs
  compile without hand-driving `gcc` one TU at a time.
- Chapter 133b–f drive a full upstream port through the
  in-guest toolchain, ending in `/bin/doom` produced by
  guest-built binaries.

