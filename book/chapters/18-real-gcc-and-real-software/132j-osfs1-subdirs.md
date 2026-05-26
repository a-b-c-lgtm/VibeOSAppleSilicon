# Chapter 132j — `sys/` headers without a hierarchical filesystem

> **Status:** shipped (Phase 4.6 of guest-gcc bring-up).
> The in-guest `/bin/gcc` now resolves `#include <sys/stat.h>`,
> `#include <sys/types.h>`, `#include <sys/time.h>`,
> `#include <sys/wait.h>`, `#include <sys/times.h>`,
> `#include <sys/param.h>`, and `#include <unistd.h>`.
> A program can call `stat()`, `access()`, `wait()`, and
> friends from inside the guest with the same `#include` lines
> it would use on Linux.
> `scripts/test_gcc_sys_stat.py` runs **PASS 6 / FAIL 0**.
> Three earlier-chapter regressions remain green:
> `test_gcc_hello.py` 10/0, `test_gcc_bf.py` 6/0,
> `test_gcc_stdio.py` 7/0.
> **Prereq:** chapter 132i (the 24 libc + 16 freestanding
> headers shipped on `/bin`).
> **Opens:** chapter 133a (`/bin/tar`, which needs
> `<sys/stat.h>` for permission bits), chapter 133c (rebuild
> the DoomGeneric portable shim in-guest).

---

## What you'll do in this chapter

1. Add seven dirent entries whose 20-byte name field
   literally contains a `/` byte (`sys/stat.h`,
   `sys/types.h`, `sys/time.h`, `sys/times.h`,
   `sys/wait.h`, `sys/param.h`, `unistd.h`) so OSFS-1
   serves them through its existing byte-exact lookup.
2. Write `scripts/stage_libc_headers.py` — a 50-line
   rewriter that turns `#include "../foo.h"` into
   `#include <foo.h>` for the guest's copies only,
   leaving the host-build sources untouched.
3. Wire `LIBC_TOP_HEADERS` / `LIBC_SYS_HEADERS` /
   `STAGED_LIBC_DIR` into the Makefile and stage the
   rewritten sys/ headers onto `/bin/` via the
   `name=path` pair mechanism.
4. Ship `assets/osfs/sys_stat_test.c` to exercise
   `stat()` + `access()` + `S_ISREG` end-to-end and add
   `scripts/test_gcc_sys_stat.py` (6-step ladder).
5. Verify the four chapter-132g–i tests stay green so
   the extra dirents + staging step don't disturb the
   existing toolchain image.

---

## Why now

Chapter 132i shipped 24 user-facing libc headers + 16 GCC
freestanding headers to `/bin`, all named with a single
flat name like `stdio.h`, `string.h`, `stdint.h`. That was
enough for the smoke test (`#include <stdio.h>`, call
`printf`, exit 7), but it explicitly skipped two groups:

1. **`<sys/foo.h>` headers** — six of them under
   `userspace/libc/sys/`: `stat.h`, `types.h`, `time.h`,
   `times.h`, `wait.h`, `param.h`. These are where POSIX
   puts `struct stat`, `pid_t`, `time_t`, `struct timeval`,
   the `WEXITSTATUS` family, and `MAXPATHLEN`. Almost any
   non-trivial Unix program includes at least one of them.
2. **`<unistd.h>`** — the umbrella POSIX header. Declares
   `read`/`write`/`close`/`access`/`getpid`/`fork`/`execve`/
   `pipe`/`dup`. Cosmetically a top-level header, but in
   this tree it `#include`s `sys/stat.h` and `sys/types.h`,
   so it lives or dies with the `sys/` group.

The reason 132i skipped them: OSFS-1's root directory is
*flat*. Every dirent is a fixed 32-byte record with a
20-byte name field, all 256 records living in sectors 1–16
of the disk image. There is no concept of a subdirectory.
The kernel's path resolver, faced with `/bin/sys/stat.h`,
strips the `/bin` mount prefix and passes `"sys/stat.h"` to
`osfs_op_open`, which then runs a byte-exact compare against
each dirent's name. There is no recursive descent because
there's nowhere to descend *to*.

Two ways out:

1. Extend OSFS-1 with subdirectory support. This means a new
   dirent flag, a recursive lookup loop, an updated
   `mkosfs.py`, and probably a format-version bump so old
   images don't silently misparse. A weekend's worth of work
   if you do it carefully — and the new format would
   immediately become legacy the moment OSFS-2 lands with
   proper inode tables.
2. Notice that `mkosfs.py` already accepts `/` as a literal
   byte inside a name field, that "sys/stat.h" is 10 bytes
   (well under the 20-byte limit), and that the kernel's
   byte-exact `name_eq` doesn't care whether the bytes are
   alphanumeric or punctuation. Just ship a flat dirent
   literally named `sys/stat.h` and the lookup that was
   already correct for `stdio.h` is also correct for
   `sys/stat.h`.

Option (2) is what 132j ships. It's about a hundred lines of
build glue, no kernel change.

---

## The trick

It really is this simple. OSFS-1's dirent name field is
`char[20]`, NUL-terminated. The current entries look like:

```
b'stdio.h\0\0\0\0\0\0\0\0\0\0\0\0\0'
b'string.h\0\0\0\0\0\0\0\0\0\0\0\0'
b'gcc.elf\0\0\0\0\0\0\0\0\0\0\0\0\0'
b'as.stripped.elf\0\0\0\0\0'
```

Seven more get added:

```
b'sys/stat.h\0\0\0\0\0\0\0\0\0\0'
b'sys/types.h\0\0\0\0\0\0\0\0\0'
b'sys/time.h\0\0\0\0\0\0\0\0\0\0'
b'sys/times.h\0\0\0\0\0\0\0\0\0'
b'sys/wait.h\0\0\0\0\0\0\0\0\0\0'
b'sys/param.h\0\0\0\0\0\0\0\0\0'
b'unistd.h\0\0\0\0\0\0\0\0\0\0\0\0'
```

Why this works end-to-end:

* **cpp**, when asked to expand `#include <sys/stat.h>`,
  joins the path against each `-isystem` directory. The
  gccw shim from chapter 132g injects `-isystem /bin`. So
  cpp calls `open("/bin/sys/stat.h", O_RDONLY)`.
* **The VFS** looks up `/bin/sys/stat.h`. The longest mount
  prefix that matches is `/bin`, mounted on OSFS-1. The
  remainder is `/sys/stat.h`, which `osfs_op_open` strips
  the leading slash from and hands to `osfs_lookup` as
  `"sys/stat.h"`.
* **`osfs_lookup`** walks its 256-entry cached dirent list
  and runs `name_eq("sys/stat.h", dirent[i].name, 20)`.
  When `i` is the index of the literal-named entry, the
  comparison succeeds, and the open returns a handle.

There is no part of this path that examines the `/` byte as
a path separator. The kernel sees an opaque 20-byte string,
matches it byte-for-byte, opens the file. Identical to how
it has always handled `stdio.h`.

---

## Pitfalls

### Pitfall — `"../foo.h"` relative includes can't be normalised in the kernel

**Symptom:** Browsing `userspace/libc/sys/stat.h`:

```c
#include <stdint.h>
#include <stddef.h>
#include "../syscall.h"
#include "../errno.h"
#include "types.h"
```

With the seven literal-named dirents in place but no further
changes, cpp inside the guest fails on `"../syscall.h"`:
the constructed open path is `/bin/sys/../syscall.h`, the
kernel does not normalise `..`, `osfs_lookup` runs
`name_eq("sys/../syscall.h", …)`, and nothing matches.

**Cause:** In a real hierarchical filesystem cpp would form
`/bin/sys/../syscall.h` and the host's resolver would
normalise the `..` and hand `/bin/syscall.h` to `open`. The
kernel here does no such normalisation — `vfs_resolve`
passes the string through verbatim, and the byte-exact
`name_eq` returns no match.

Four ways to address this:

1. **Teach the VFS to normalise `..`** — works, but touches
   every filesystem driver (osfs, ramfs, userfs, devfs)
   because each can receive a raw relative path. Big blast
   radius for a problem that's specific to a build artefact.
2. **Edit the libc source** to drop `"../foo.h"` in favour
   of `<foo.h>`. Works in-guest, but on the host build the
   `<foo.h>` form would search `/usr/include` first, which
   is wrong — the host build is freestanding and wants the
   in-tree headers. Every host translation unit that pulls
   in a sys/ header (most of them) would then need
   `-I userspace/libc`. Bigger blast radius than option 1.
3. **Symlink farm** — copy `syscall.h` and friends into
   `userspace/libc/sys/`. Doubles the source-of-truth
   problem; future edits to `syscall.h` silently desync.
4. **Stage the headers through a small rewriter at build
   time** — read the libc sources, rewrite `#include
   "../foo.h"` to `#include <foo.h>`, write the staged copy
   into `build/staged-libc-headers/sys/`, ship those onto
   the disk image. Host build is untouched (the staging
   only runs as part of the disk recipe). Guest build sees
   only the rewritten copies, where the angle-bracket form
   resolves to `/bin/syscall.h` via `-isystem /bin`
   (chapter 132i already proved `<stdio.h>` works).

**Fix:** Option (4). 50 lines of Python plus six lines of
Makefile. The rewriter is `scripts/stage_libc_headers.py`
and the Makefile wiring lives in the `$(DISK)` recipe —
both shown below.

---

## What you'll write

### 1. The rewriter — `scripts/stage_libc_headers.py`

```python
import re
import sys
from pathlib import Path

_REL_PARENT = re.compile(r'#(\s*)include\s+"\.\./([^"/]+)"')

def stage(src: Path, dst: Path) -> None:
    text = src.read_text(encoding="utf-8")
    rewritten = _REL_PARENT.sub(r'#\1include <\2>', text)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(rewritten, encoding="utf-8")

def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: stage_libc_headers.py <src> <dst>",
              file=sys.stderr)
        return 2
    stage(Path(argv[1]), Path(argv[2]))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

That's the whole tool. One regex, one substitution, one
write. The regex deliberately handles only single-segment
parent references (`"../foo.h"`, not `"../../foo.h"`)
because that's what the libc actually uses; if a future
header needs deeper parent navigation, the regex will fail
loudly rather than silently mis-rewrite.

### 2. The Makefile wiring

In the libc-headers block (around line 2187):

```make
LIBC_TOP_HEADERS := assert.h atexit.h ctype.h dirent.h env.h \
    errno.h fcntl.h inttypes.h locale.h malloc.h math.h printf.h \
    scanf.h setjmp.h signal.h stdio.h stdlib.h string.h strings.h \
    syscall.h thread.h time.h unistd.h wchar.h zlib.h
LIBC_SYS_HEADERS := stat.h types.h time.h times.h wait.h param.h
STAGED_LIBC_DIR := $(BUILD)/staged-libc-headers

$(STAGED_LIBC_DIR)/sys/%.h: userspace/libc/sys/%.h \
                            scripts/stage_libc_headers.py
	@mkdir -p $(dir $@)
	python3 scripts/stage_libc_headers.py $< $@
```

Note `unistd.h` was added to `LIBC_TOP_HEADERS`; the other
24 entries are the chapter-132i set.

In the `$(DISK)` recipe, the staged-sys files become
dependencies and inputs to `mkosfs`:

```make
$(DISK): ... \
         $(addprefix userspace/libc/,$(LIBC_TOP_HEADERS)) \
         $(addprefix $(STAGED_LIBC_DIR)/sys/,$(LIBC_SYS_HEADERS)) \
         ...
	python3 scripts/mkosfs.py $@ \
	    ... \
	    $(foreach h,$(LIBC_TOP_HEADERS),$(h)=userspace/libc/$(h)) \
	    $(foreach h,$(LIBC_SYS_HEADERS),sys/$(h)=$(STAGED_LIBC_DIR)/sys/$(h)) \
	    sys_stat_test.c=assets/osfs/sys_stat_test.c \
	    ...
```

The `sys/$(h)=...` foreach is the literal-name trick: the
*dirent name* (left of the `=`) contains a slash; the
*source path* (right of the `=`) points at the staged
rewritten copy.

### 3. The smoke source — `assets/osfs/sys_stat_test.c`

```c
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
    struct stat st;
    if (stat("/bin/stdio_test.c", &st) != 0) {
        puts("stat /bin/stdio_test.c FAILED"); return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        puts("stdio_test.c is not a regular file?"); return 2;
    }
    if (access("/bin/stdio_test.c", F_OK) != 0) {
        puts("access /bin/stdio_test.c FAILED"); return 3;
    }
    printf("stdio_test.c size=%lld mode=0x%x\n",
           (long long)st.st_size, (unsigned)st.st_mode);
    puts("sys_stat_test OK");
    return 0;
}
```

Three POSIX calls, each exercising a different aspect of
the new headers: `stat()` uses the `struct stat` layout
from `<sys/stat.h>`, `S_ISREG` exercises the mode-bit macro
from the same header, `access()` and `F_OK` come from
`<unistd.h>`. If any header is missing or malformed, this
won't compile in-guest.

The file the test stats is one already shipped in 132i —
`/bin/stdio_test.c` — so the test has a known target with a
stable size to compare against.

### 4. The test harness — `scripts/test_gcc_sys_stat.py`

Three-step ladder modelled on `test_gcc_stdio.py`:

1. **Sanity:** `cat /bin/sys_stat_test.c` returns the source.
   `cat /bin/sys/stat.h` returns the header. The header
   contents contain no `"../"` substring (proving the
   staging rewriter ran).
2. **Preprocess:** `gcc -E /bin/sys_stat_test.c
   > /tmp/sys_stat_test.i` succeeds without "fatal error"
   or "No such file". This is where a missing header would
   blow up first.
3. **Build + run:** `gcc /bin/sys_stat_test.c -o
   /tmp/sys_stat_test` produces an ELF; running it prints
   `sys_stat_test OK` and `size=142`.

The final number — `size=142` — is the size of
`/bin/stdio_test.c` itself, which `mkosfs` reports
truthfully in the dirent's `size_bytes` field. Watching
this number stay stable across boots is a nice side-check
that 132i's text-file shipping is reliable.

---

## Run it / Test it

```
$ python3 scripts/test_gcc_sys_stat.py
[chapter 132j] in-guest #include <sys/stat.h>
PASS: sanity: /bin/sys_stat_test.c shipped on OSFS
PASS: sanity: /bin/sys/stat.h shipped on OSFS
PASS: sanity: staging rewrote ../foo.h -> <foo.h>
PASS: step 1: cpp finds <sys/stat.h>, <unistd.h>, <sys/types.h>
PASS: step 2: /bin/gcc compiles sys_stat_test.c to ELF
PASS: step 3: stat() + access() + S_ISREG all succeeded

PASS: 6
FAIL: 0
```

Disk grew from 155 files (post-132i) to 163 (post-132j):
+6 sys/* headers + 1 `unistd.h` + 1 `sys_stat_test.c`.
Total image size is unchanged at 256 MiB — the headers are
tiny relative to the 100+ MiB occupied by `gcc.elf`,
`as.stripped.elf`, and `ld.stripped.elf`.

---

## What's deferred

Worth saying out loud. The "real" answer to a flat directory
is a hierarchical one: parent-pointer entries, recursive
lookup, `mkdir` syscall, `readdir` that recurses, etc. All
the pieces exist in the userfs implementation (chapter 113+)
— userfs is a real tree. OSFS-1 wasn't extended for three
reasons:

1. **OSFS-1 is a boot-time read-only image.** Its job is to
   hold the contents of `/bin` (compiler, linker, libc
   headers, smoke programs) and the contents of `/data`
   before any writes have happened. It has never needed
   to grow at runtime, never needed `mkdir`, never needed
   `unlink`. Adding tree semantics to a read-only flat image
   is mostly cost, little benefit.
2. **The literal-name trick is bytes-for-bytes equivalent
   to a real subdir for cpp's purposes.** cpp doesn't
   introspect the filesystem to ask "is `/bin/sys` a real
   directory?"; it just `open(2)`s a constructed pathname
   and reads what comes back. As long as the *path* matches
   *something* on disk, cpp is happy.
3. **The real successor is `OSFS-2`** — a proper inode
   filesystem with extent maps, journalled writes, and
   directory inodes. A sketch lives in the chapter-133
   plan. When `OSFS-2` lands, the staging rewriter goes
   away in a one-line Makefile change.

The trick has one downside: programs that *enumerate*
`/bin/sys/` (e.g. a `ls /bin/sys`) will see nothing,
because there's no directory inode to enumerate. If a
future chapter needs that, add a stub `sys` dirent of
length zero and have `readdir` synthesise entries from
prefix matches. Not needed yet.

---

## What this unlocks

* `<sys/stat.h>` + `<unistd.h>` are the most-included POSIX
  headers after `<stdio.h>`. Any portable Unix program will
  now `cpp` cleanly inside the guest.
* `/bin/tar` (chapter 133a) needs `<sys/stat.h>` for the
  permission bits on extracted files. Now unblocked.
* DoomGeneric's portable shim (`vendor/doomgeneric/src/`)
  includes a handful of `sys/*.h` headers; chapter 133c
  will rebuild Doom in-guest, and this chapter is what
  lets `cpp` get through the shim's prologue.

Per the standing "apps must use the OS features the book
builds" rule:

* **`assets/osfs/sys_stat_test.c`** *(new app)* — a tiny
  three-syscall demonstrator shipped to `/bin`. Users can
  `cat /bin/sys_stat_test.c` to see how `stat()` + `access()`
  look from in-guest. Also serves as the smoke test for the
  chapter.
* **`scripts/test_gcc_sys_stat.py`** *(new regression)* —
  added to the per-chapter sweep.
* **`scripts/stage_libc_headers.py`** *(new tooling)* — a
  pure-Python rewriter that runs as part of the disk recipe;
  no host-side libraries required.
* **`userspace/libc/sys/*.h`** — *unchanged*. The host build
  continues to use the sources directly. Only the guest's
  on-disk copies are rewritten.

Regressions kept green:

* `test_gcc_hello.py` — 10/0
* `test_gcc_bf.py` — 6/0
* `test_gcc_stdio.py` — 7/0
* `test_bin_as.py` — 8/0
* `test_bin_ld_ar.py` — 12/0

---

## What's next

The remaining gap before Doom can be rebuilt in-guest is
not a kernel or libc gap — it's the lack of a tool that
can take `vendor/doomgeneric.tar.gz` off the disk and
unpack it into `/data/build/`. That's chapter 133a:
`/bin/tar`. Once that ships, chapter 133b audits whether
the in-guest `make` (from chapter 126) handles the
DoomGeneric Makefile's pattern rules + automatic
variables; chapter 133c is the actual rebuild; chapter
133d is the smoke test where the rebuilt-in-guest Doom
plays the demo.

