# Chapter 132g — `gcc hello.c -o hello` (no escape-hatch flags)

> **Milestone in this chapter:** make the in-guest `/bin/gcc`
> compile a plain C source with no `-nostdlib -nostdinc -e
> _start` workaround, linking via the on-disk `crt0.o` and
> `libosdevc.a`.
> **Code referenced:**
> - [vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h](../../../vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h)
>   (`LINK_SPEC` gains `-L /bin`)
> - [scripts/test_gcc_hello.py](../../../scripts/test_gcc_hello.py)
>   (now exercises bare `/bin/gcc /tmp/hello2.c -o /tmp/hello2`)
> - [scripts/_dbg_gcc_libc_probe.py](../../../scripts/_dbg_gcc_libc_probe.py)
>
> **At the end of this chapter** you will have the in-guest
> gcc compiling, linking, and running a program with `exit=7`
> using nothing more than `/bin/gcc /tmp/hello2.c -o
> /tmp/hello2`, and `test_gcc_hello.py` at **PASS 10 / FAIL
> 0**. Prerequisite: chapter 132f (xgcc runs in the guest
> end-to-end).

---

## What you'll do in this chapter

1. Add `-L /bin` to LINK_SPEC in
   `vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h`
   so ld can find `-losdevc` on the flat OSFS-1 namespace.
2. Rebuild xgcc with the build-side guard
   (`touch build/gencheck.o ... tm.h`; explicit
   `CC_FOR_BUILD=clang CXX_FOR_BUILD='clang++'`) so the
   regenerated `tm.h` doesn't drag the host generators
   onto the cross compiler.
3. Add step 8 to `scripts/test_gcc_hello.py` exercising
   bare `/bin/gcc /tmp/hello2.c -o /tmp/hello2` and
   `exit=7`.
4. Land `scripts/_dbg_gcc_libc_probe.py` as a reference
   diagnostic (kept per debug-scripts policy) for
   "is the libc on disk, is xgcc invoking it, does ld see
   it?"
5. Remember the `-B` vs `-L` distinction so the next
   LINK_SPEC edit doesn't repeat the lesson.

---

## Why now

Chapter 132f ended on step 7 of the test ladder: `/tmp/hello`,
linked with the synthetic `-nostdlib -nostdinc -e _start
-Wl,-T,/bin/osdev.ld` flag set, ran and returned 42. Useful as
a proof of life — but no real program is going to be invoked
that way.

The user-facing promise is `gcc hello.c -o hello`. That means
GCC has to:

1. Preprocess with the right `__OSDEV_LIBC__` define (LIB_SPEC
   side of `aarch64-osdev.h`, already in place since 132d).
2. Run cc1 with `-ffreestanding` (CC1_SPEC, also already 132d).
3. Run `as` to produce an `.o` (no spec involvement).
4. Drive `ld` with:
   - `-T /bin/linker_user.ld` (LINK_SPEC, 132d).
   - `crt0.o` *as the first object* (STARTFILE_SPEC, 132d).
   - `--start-group -losdevc --end-group` (LIB_SPEC, 132d).

Items 1-3 already worked. Item 4 was the chapter. The first
attempt blew up at link with:

```
ld: cannot find -losdevc: file format not recognized
```

That error message is misleading — `libosdevc.a` IS a valid
archive, ld just isn't looking in the right directory.

This chapter is the diagnosis, the one-line fix in LINK_SPEC,
the test that proves it, and the rebuild gotcha that nearly
ate the chapter.

---

## What this chapter adds, by the byte

```
$ ls -la build/disk.img build/kernel.elf
-rw-r--r--  ... 268435456 build/disk.img
-rwxr-xr-x  ...   1061584 build/kernel.elf

$ ls -la build/gcc-build-guest/gcc/gcc/xgcc
-rwxr-xr-x  ...   2749720 xgcc
```

Test ladder, with the new step 8 highlighted:

```
[chapter 132g] /bin/gcc end-to-end smoke test
PASS: step 1: /bin/gcc --version executes
PASS: step 1: reports GCC 14.x
PASS: step 2: /tmp/hello.c contents staged
PASS: step 3: cc1 preprocesses (output retains _start)
PASS: step 4: cc1 emitted assembly
PASS: step 5: cc1 + as produced ELF .o
PASS: step 6: linked /tmp/hello is an ELF
PASS: step 7: /tmp/hello returned 42
PASS: step 8: default-spec link produces ELF       <-- new
PASS: step 8: default-spec /tmp/hello2 returned 7  <-- new

PASS: 10
FAIL: 0
```

Step 8 source is the kind of program a textbook would call
"hello world for the second day":

```c
int main(void) { return 7; }
```

And the command line is what every C programmer expects to
work:

```
$ /bin/gcc /tmp/hello2.c -o /tmp/hello2
$ /tmp/hello2
$ echo $?
7
```

---

## The bug: `-B<prefix>` is not the same as `-L<prefix>`

`/bin/gcc` is a tiny shim (`userspace/gccw/gccw.c`) that
just `execv`s `/bin/xgcc` with `-B/bin/` prepended:

```c
execv("/bin/xgcc",
      (char *const[]) { "/bin/xgcc", "-B/bin/",
                        argv[1], argv[2], ..., NULL });
```

`-B<dir>` is GCC's "this is where everything lives" flag. It
makes the driver:

- Add `<dir>` to **`startfile_prefixes`** — the list it
  searches when STARTFILE_SPEC contains a `%s` lookup like
  `crt0%O%s`.
- Add `<dir>` to **`exec_prefixes`** — the list it searches
  when resolving subprograms like `cc1`, `as`, `ld`,
  `collect2`.

That covers items 4a and 4b of the link spec list above:
`crt0.o` is found via startfile-prefix; `cc1` and friends
are found via exec-prefix.

What `-B` does **not** do is add `<dir>` to the linker's
`-L` search path. So when LIB_SPEC produces `--start-group
-losdevc --end-group`, ld is invoked with a `-l` reference
but no `-L`:

```
$ /bin/gcc -v /tmp/hello2.c -o /tmp/hello2
...
COLLECT_GCC_OPTIONS='-v' '-B/bin/' ...
 /bin/ld -EL -X -T /bin/linker_user.ld -maarch64elf \
   -o /tmp/hello2 /bin/crt0.o /tmp/cc-XXXXXX.o \
   --start-group -losdevc --end-group
ld: cannot find -losdevc: file format not recognized
```

(Note the missing `-L` anywhere on that command line. ld
searches its compiled-in `SEARCH_DIR` list and a few defaults;
none of them include `/bin` on OSFS-1's flat namespace.)

ld's bewildering error message ("file format not recognized")
is actually how aarch64-elf-ld says "I couldn't find a
candidate file at all" — it tried every prefix/suffix
combination it could think of, gave up, and reported the
last attempt's diagnostic as if that explained anything.

### Verifying the diagnosis

`scripts/_dbg_gcc_libc_probe.py` is a focused boot-then-run
harness that:

1. Lists `/bin` to confirm `libosdevc.a` is there.
2. `cat`s the first 64 bytes of `libosdevc.a` so the `!<arch>`
   header is visible by eye (rules out OSFS-1 corruption).
3. Runs `/bin/gcc --version` (rules out xgcc itself being
   broken; that was chapter 132f).
4. Runs `/bin/gcc -v /tmp/hello2.c -o /tmp/hello2` — the
   `-v` makes the driver dump every subprogram command line.
5. Runs `/bin/ld -L/bin -losdevc -o /tmp/foo` directly,
   bypassing the gcc driver entirely.

Probe 2 showed a valid `!<arch>\n` archive header — the file
is fine. Probe 4 showed the ld command line above, with no
`-L` anywhere. Probe 5 succeeded (only warned about the
missing `_start` entry, which is normal for an ld test that
links no actual code) — proving the archive **can** be found
and read if ld is told where to look.

Hypothesis confirmed: the bug is in how the gcc driver
constructs the ld command line, not in the linker, archive,
or filesystem.

---

## The fix: bake `-L /bin` into LINK_SPEC

For a flat-namespace target like osdev (where everything that
ships with the OS lives in `/bin`), the cleanest answer is to
put `-L /bin` directly in LINK_SPEC.
`vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h` now has:

```c
/* `-L /bin` is added so `-l<name>` lookups (from LIB_SPEC's
   `-losdevc` and any user `-l<x>`) find archives on the flat
   OSFS-1 namespace.  GCC's `-B/bin/` (from the /bin/gcc shim)
   adds /bin to `startfile_prefixes` and `exec_prefixes` but
   NOT to ld's library search path — that needs an explicit
   `-L` on the link line.  On the host this `-L /bin` is
   harmless: aarch64-elf-ld has no AArch64 archives in macOS's
   /bin and will silently not match.  (Chapter 132g.)  */
#undef LINK_SPEC
#define LINK_SPEC "                              \
   %{static:-Bstatic}                            \
   %{mbig-endian:-EB} %{mlittle-endian:-EL} -X   \
   -L /bin                                       \
   %{!T*:-T /bin/linker_user.ld}                 \
   -maarch64elf%{mbig-endian:b}"                 \
  AARCH64_ERRATA_LINK_SPEC
```

Three alternatives considered and rejected:

1. **Pass `-L/bin` from the `/bin/gcc` shim**
   (`userspace/gccw/gccw.c`). Works, but means the shim now
   has to know about the spec — duplication of policy
   between the shim and the compiler. The moment something
   else compiles by bypassing the shim (e.g. `make`
   invoking xgcc directly), the policy splits.

2. **Set `LIBRARY_PATH=/bin` in the guest's `/etc/profile`**
   (which doesn't exist yet, but it would once chapter 133
   lands). GCC honours `LIBRARY_PATH` and prepends it to
   ld's `-L` list. Same problem as #1: the policy lives
   outside the compiler. Also, env-var-driven config is the
   first thing that goes wrong when a build runs from cron.

3. **Move `libosdevc.a` to GCC's compiled-in `--with-sysroot`
   path.** Would also work, but requires a `--with-sysroot`
   reconfigure of xgcc and adds another moving part — a
   sysroot tree on the disk image that has to stay in sync
   with what `make` installs to `/bin`. The flat namespace
   is the point of osdev's filesystem design; the spec
   should reflect that.

Baking `-L /bin` into LINK_SPEC keeps all "where things live"
knowledge in one place: `aarch64-osdev.h`. Every other tool
just talks to xgcc through standard channels.

---

## The rebuild gotcha that nearly ate this chapter

After the LINK_SPEC edit, `gcc.o` (which includes the
regenerated `tm.h` chain) needs to be rebuilt and `xgcc`
relinked. The naïve incantation is:

```bash
cd build/gcc-build-guest/gcc/gcc
rm -f gcc.o xgcc        # force rebuild of these two
make -j4 xgcc
```

That fails:

```
build/gencheck.o: fatal error: sys/mman.h: No such file or
directory
```

Here's what happens. `gcc.o` depends on `tm.h`. `tm.h` is
regenerated from the chain that includes `aarch64-osdev.h`,
so it's now newer than `build/gencheck.o`. Make therefore
decides `gencheck.o` needs rebuilding too. The Makefile rule
for `build/%.o` uses `COMPILER_FOR_BUILD = $(CXX_FOR_BUILD)`,
and configure left `CXX_FOR_BUILD = $(CXX) =
aarch64-osdev-CC` — the cross-compiler wrapper.
`aarch64-osdev-CC` includes the OSdev header-only freestanding
libc; no `<sys/mman.h>`; the host-side generator tool fails
to build.

(`gencheck.o` was originally built correctly on a different
day, in a different shell, where `CXX_FOR_BUILD` resolved to
a real host `clang++`. The Mach-O file was sitting there
unchanged, but make's mtime check forced a regenerate-and-
recompile cycle that fell off the cross-compiler cliff.)

### The right way

Two protections:

1. Touch the build-side generator objects so make sees them
   as already-fresh:

   ```bash
   touch build/gencheck.o build/gencheck \
         build/genchecksum.o build/genchecksum \
         tm.h tree-check.h
   ```

2. Override `CC_FOR_BUILD` / `CXX_FOR_BUILD` to a real host
   compiler in case anything else triggers a build-side
   rebuild the touch didn't predict:

   ```bash
   make -j4 \
        CC_FOR_BUILD=clang  CXX_FOR_BUILD='clang++' \
        BUILD_CC=clang      BUILD_CXX='clang++' \
        xgcc
   ```

Combined incantation:

```bash
cd build/gcc-build-guest/gcc/gcc
touch build/gencheck.o build/gencheck \
      build/genchecksum.o build/genchecksum \
      tm.h tree-check.h
make -j4 CC_FOR_BUILD=clang CXX_FOR_BUILD='clang++' \
         BUILD_CC=clang BUILD_CXX='clang++' xgcc
ls -la xgcc   # expect ~2.7 MB
```

This is worth remembering for any future LINK_SPEC edit:
`-B` only adjusts where the driver *finds* programs, not
where `ld` *searches* for libraries.

---

## The test step

`scripts/test_gcc_hello.py` step 8 is small but does the work
of three checks at once:

```python
# --- step 8 (chapter 132g): default-specs hello ----
# The real prize: `gcc hello.c -o hello` with NO
# `-nostdlib -nostdinc -e _start` escape hatch.  The
# specs in aarch64-osdev.h already wire crt0%O%s,
# `-T /bin/linker_user.ld`, and `-losdevc` by default,
# and the /bin/gcc shim prepends `-B/bin/` so the
# startfile-prefix list picks up /bin/crt0.o and
# /bin/libosdevc.a.  If this lights up green the
# toolchain is ready for real upstream programs.
send_cmd(s, "rm /tmp/hello2.c", timeout=10.0)
send_cmd(s, "echo 'int main(void) { return 7; }' "
            "> /tmp/hello2.c", timeout=10.0)
out = send_cmd(s, "/bin/gcc /tmp/hello2.c -o /tmp/hello2",
               timeout=180.0)
out2 = send_cmd(s, "cat /tmp/hello2", timeout=20.0)
expect(b"\x7FELF" in out2,
       "step 8: default-spec link produces ELF")
out = send_cmd(s, "/tmp/hello2", timeout=20.0)
out += send_cmd(s, "echo exit=$?", timeout=10.0)
expect(b"exit=7" in out,
       "step 8: default-spec /tmp/hello2 returned 7")
```

The `return 7` (and not the more famous `return 42`) is on
purpose: distinguishes step 8's exit code from step 7's in
the kernel reaper log, so a confused grep can't make the test
look like it passed when only step 7 ran.

180-second timeout for the link itself because cc1 takes a
real moment to wake up on the guest (chapter 132f's stale-
binary pitfall — TTF font init for "Helvetica" etc. is a
no-op fast path when no GUI is attached, but cc1 still does
its 50 MB of self-test setup before main).

---

## What works now end-to-end

Inside the OS shell:

```
$ cat > /tmp/h.c <<EOF
> int main(void) {
>     return 42;
> }
> EOF
$ /bin/gcc /tmp/h.c -o /tmp/h
$ /tmp/h ; echo $?
42
```

That command sequence — three keystrokes a C programmer
performs in their sleep — was the entire point of part 18
of the book. From chapter 118 (assembler) through chapter
132f (xgcc runs in the guest) every piece arrived one at a
time. Chapter 132g is the rung where every piece is finally
on the wall.

A few things the disk image can do as soon as it boots:

- Compile any single-source C program that uses only
  `userspace/libc`. The libc has `printf`, `malloc`,
  `read`/`write`, `fork`/`exec`/`wait`, `signal`,
  `socket`, `mmap`, `stat`/`opendir`, `setjmp`, `time`,
  `assert`, `getopt`, math.h functions, and POSIX string/
  ctype/stdlib (chapters 116-128). That's enough for the
  vast majority of small Unix utilities.

- Build a multi-file program by compiling each `.c` to a
  `.o` with `gcc -c` and linking them with `gcc -o`. (One
  invocation per TU; `make` arrives in 133a.)

- Pipe `gcc` into the shell from inside notepad's build
  button (chapter 127). The placeholder PATH lookup from
  127 now resolves to a real compiler.

---

## What's deferred

Deliberately NOT done in this chapter:

- **Add a `--with-sysroot` configure flag to xgcc.** The
  filesystem is flat; sysroot is a layout convention for
  systems with `/usr` / `/lib` / `/usr/include`. The flat
  namespace works fine here once `-L /bin` is in the spec.

- **Add `-I /bin` (the header equivalent).** Not needed —
  the `-B/bin/` from the shim already plumbs cc1's
  `-iprefix` chain through the chapter-132f
  `lib/gcc/aarch64-osdev/14.2.0/` tree, which is where
  `stdint-gcc.h`, `stdarg.h`, `stddef.h` (the
  freestanding-required headers) live. The libc headers
  are also pulled in by the `-D__OSDEV_LIBC__` define
  (CPP_SPEC) plus per-header inclusion of `osdev/libc.h`,
  not by the include path. The header tree shipped on
  disk needs careful management — the
  `userspace/libc/include/` tree is the source of truth
  and must be staged onto OSFS-1 rather than copied.

- **Wire up `-lm`.** There's no separate `libm.a` — math
  symbols (`sqrt`, `pow`, `floor`, ...) are in the same
  `libosdevc.a`. Programs that pass `-lm` get a
  "cannot find -lm" error today. `-lm` could land as a
  no-op stub archive in 133a if any port asks for it; so
  far none have.

- **Wire up `-lpthread`.** Threads in osdev are a
  kernel-side feature (chapter 11, chapter 91) reachable
  from the libc via direct syscall wrappers. No POSIX
  `pthread_*` surface yet. When a chapter-133 port lays
  pthread emulation on top of `thread_create`,
  `-lpthread` becomes another no-op stub.

---

## What you'll write

| File | Change |
| ---- | ------ |
| `vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h` | LINK_SPEC: added `-L /bin` |
| `scripts/test_gcc_hello.py` | Added step 8 (default-spec hello.c → ELF → exit=7) |
| `scripts/_dbg_gcc_libc_probe.py` | New diagnostic probe (kept per debug-scripts policy) |

The libc, the kernel exec limits, the inline-svc diagnostic
helpers, and the env-arena rework from chapter 132f all
stay put. This chapter is purely a spec change plus a test
plus a probe.

---

## What this unlocks

After chapter 132g:

- `gcc hello.c -o hello` works as a user-facing command
  inside the OS shell.
- `notepad`'s build button (chapter 127) can now compile any
  C file you save in the editor — the placeholder PATH
  lookup falls away because `/bin/gcc` resolves through the
  shim end-to-end.
- The doom build (eventual chapter 134) and the first port
  in 133b have a real default-specs compiler to drive
  through `make`. Each port becomes "compile its `.c` files
  with `/bin/gcc -c`, link with `/bin/gcc -o`," same as on
  Linux.

Per the standing "apps must use the OS features the book
builds" rule:

- **Existing apps modified**: `userspace/notepad/notepad.c`'s
  build button (chapter 127) was already invoking `/bin/gcc`
  with `-nostdlib -nostdinc -e _start` as a workaround. The
  workaround is now obsolete — the next chapter touch on
  notepad strips those flags and lets the user write
  `int main(void) { ... }` like a normal C programmer. The
  placeholder flags stay in the source until 133a so the
  notepad UX doesn't change inside a chapter that's about
  the spec layer, but the comment now reads "remove these
  in 133a now that 132g ships default specs."
- **New apps added**: none in this chapter; the first new
  app that uses the new compiler arrives in 133b. The
  `/tmp/hello2.c` step-8 source is the smoke test, not a
  shipping app.
- **Existing test scripts upgraded**: `scripts/test_gcc_hello.py`
  gained step 8 (the default-spec link).
- **New test scripts added**: `scripts/_dbg_gcc_libc_probe.py`
  — a 5-probe diagnostic harness for "is the libc on disk,
  is xgcc invoking it, and does ld see it." Kept as
  reference material per the debug-scripts policy. Future
  spec/libc bring-ups can copy this template wholesale.

---

## Things to remember

- The `-B` vs `-L` distinction: `-B` only adjusts where the
  driver finds *programs*, not where `ld` searches for
  libraries. Three fix options exist (LINK_SPEC, `-L` on
  the user command line, `LIBRARY_PATH` env var); this
  chapter uses LINK_SPEC. Rebuilding xgcc after the spec
  change requires the `gencheck.o` workaround above.

132g is a one-trick chapter, but the trick is the one that
turns the toolchain from "runs" into "useful."

---

## What's next

- Chapter 132h drives `gcc` against a real multi-source
  program built by the in-guest shell.
- Chapter 133a ports `make`, so user programs stop being
  hand-driven `gcc` invocations.

