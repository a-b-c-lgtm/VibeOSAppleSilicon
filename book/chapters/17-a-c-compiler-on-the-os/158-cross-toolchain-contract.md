# Chapter 158 — Cross-building from the host: the contract

## Why this chapter exists

[Chapter 157](157-bin-cc.md) shipped a tiny native
compiler at `/bin/cc`. Chapter 159 will grow it from
"literals only" to "locals + arithmetic." Between them
there is one piece of plumbing that has been holding
up every userspace binary in the repo and has never had
a dedicated chapter: **the host cross-toolchain
contract.**

That contract is the answer to a question every reader
will ask while reading Part XVII:

> "OK, we want a compiler running inside the OS.
>  But while we're getting there — and even after —
>  how do we know the binaries we cross-build on the
>  host are runnable inside the guest at all?"

The answer is buried in our [Makefile](../../../Makefile)
and in [userspace/linker_user.ld](../../../userspace/linker_user.ld),
but it has never had a dedicated chapter. This is that
chapter — and the first regression test that fails
loudly if the contract ever drifts.

## The contract, in one paragraph

A guest-runnable userspace ELF on osdev is **any** static,
AArch64, position-dependent `EXEC` ELF whose first
`PT_LOAD` lands at virtual address `0x1000100000`, with
its `_user_start` entry symbol resolved to a valid byte
inside that load segment, built against our freestanding
C ABI (no libc, no startup files, no PLT/GOT). The
binary is invoked from `/bin/sh` by name. The kernel
loader maps the segments, jumps to `e_entry`, and the
binary makes raw `svc #0` syscalls from there.

That sentence is enforced today by exactly five things:

| Knob | Value | Where it lives |
| --- | --- | --- |
| Cross prefix | `aarch64-elf-` | [Makefile](../../../Makefile) |
| CFLAGS | `-ffreestanding -nostdlib -nostartfiles -mcpu=cortex-a72 -mgeneral-regs-only -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables -Os` | [Makefile](../../../Makefile) (`USER_CFLAGS`) |
| LDFLAGS | `-T userspace/linker_user.ld -nostdlib --orphan-handling=error -z noexecstack -z max-page-size=0x1000` | [Makefile](../../../Makefile) (`USER_LDFLAGS`) |
| Load base | `0x1000100000` | [userspace/linker_user.ld](../../../userspace/linker_user.ld) |
| Entry symbol | `_user_start` (from [userspace/crt/crt0.s](../../../userspace/crt/crt0.s)) | [Makefile](../../../Makefile) link rule |

Any toolchain — host today, a future in-guest GCC port,
or any other backend — that produces a binary satisfying
that table is a valid osdev cross toolchain. **That is
the contract every userspace binary in the repo already
respects, and the one a future compiler port (Part XVIII)
would have to satisfy.**

## Why we are not porting a real compiler in this chapter

Real GCC is two orders of magnitude bigger than the
biggest thing we've built so far. Even a stripped-down
`--enable-languages=c` GCC needs:

- `gmp`, `mpfr`, `mpc` (multiprecision libraries)
- `libiberty`
- A real `configure`/`make` driver
- A sysroot of headers we don't yet have
- `as` and `ld` matching its calling convention (we
  have `/bin/as` and `/bin/ld`, but they only assemble
  a tiny subset of the AArch64 ISA — see chapters
  [118](154-bin-as-assembler.md) and
  [119](155-bin-ld-linker.md))
- A timer budget significantly longer than a chapter

Shipping a broken GCC build in this chapter would teach
nothing. The honest move — the one Part XVII actually
follows — is:

1. **Chapter 158 (this one):** lock down the contract,
   prove the existing host cross-build is a clean
   reference implementation, regression-gate it.
2. **Chapters 159–160:** keep growing the small
   `/bin/cc` (locals, arithmetic, then a real on-disk
   compile) against this exact contract. The cross
   build stays the reference; `/bin/cc`'s output is
   measured by booting it and running it.
3. **Chapter 161:** name the gap between `/bin/cc` and
   a self-hosting compiler explicitly, and stop. A real
   GCC port (Part XVIII) inherits the contract this
   chapter wrote down.

This is the same shape we used for chapter 157 — pick a
scope small enough to actually ship, and write the spec
the next chapter will be measured against.

## The reference cross toolchain

We've actually been using a cross toolchain since
chapter 7. Every single binary in `/bin` got there by
this exact pipeline:

```
.c ──aarch64-elf-gcc────► .o
.s ──aarch64-elf-as────► .o
.o … ─aarch64-elf-ld──► .elf
.elf ─aarch64-elf-objcopy──► .stripped
.stripped ──mkosfs.py──► /bin/<name> on disk.img
```

It just never had a chapter calling that out. Chapter
122 turns it into a first-class artifact.

The reference pipeline lives in [Makefile](../../../Makefile):

```makefile
$(BUILD)/userspace/%.o : userspace/%.c
	$(USER_CC) $(USER_CFLAGS) -c -o $@ $<

$(BUILD)/userspace/<name>/<name>.elf : <obj-list> userspace/linker_user.ld
	$(LD) $(USER_LDFLAGS) -o $@ <obj-list>
```

The crucial detail buried in `USER_CFLAGS` is
`-mgeneral-regs-only`. The kernel does not save or
restore SIMD/FP context across the EL1↔EL0 boundary
(see [Chapter 171](../18-real-gcc-and-real-software/171-fp-simd-at-el0.md),
which finally extends the context-switch frame to cover
`q0..q31` + `fpsr` + `fpcr`), so any
binary that touches `v0`–`v31` corrupts whatever else
was using them. The contract forbids them, and the
flag enforces it at compile time.

The crucial detail buried in `USER_LDFLAGS` is
`-z max-page-size=0x1000`. `aarch64-elf-ld` defaults
to a 64 KiB max page size, which inflates segment
alignment and produces ELFs that don't lay out cleanly
under our 4 KiB MMU configuration. Forcing 4 KiB makes
`PT_LOAD` segments align exactly with the kernel's page
mapper.

## The smoke test

[scripts/test_gcc_cross_hello.py](../../../scripts/test_gcc_cross_hello.py)
is the chapter's regression artifact. It does six
checks, three host-side and three guest-side:

1. **Host: compile size sane.** Cross-builds a tiny
   `hello.c` with `aarch64-elf-gcc` + `aarch64-elf-ld`
   using `USER_CFLAGS` and `USER_LDFLAGS` from
   [Makefile](../../../Makefile), then asserts the
   stripped ELF is between 256 B and 64 KiB. Smaller
   means link failed silently; larger means startfiles
   leaked back in.
2. **Host: ELF magic.** Byte 0–3 of the stripped output
   are `\x7fELF`.
3. **Host: machine type.** `e_machine == EM_AARCH64`
   (183). Catches the trivial "you forgot the cross
   prefix" mistake.
4. **Guest: file visible at `/data/cross_hello`.** The
   test seeds `build/data.img` via
   [scripts/mkosfs2.py](../../../scripts/mkosfs2.py)
   using the `name=path` syntax, boots the guest, and
   `ls /data` must show the file. This proves the
   binary survived the on-disk format and the OSFS-2
   mount at `/data`.
5. **Guest: marker printed.** Runs the binary; output
   must contain `M122-CROSS-OK`. This proves
   `_user_start` was reached, the syscall ABI is honored,
   and the load address actually mapped where the
   linker said it would.
6. **Guest: exit code 7.** Proves `SYS_EXIT` carried the
   value through the kernel boundary correctly.

Each check that fails is loud:

```
PASS: cross-built ELF has reasonable size
PASS: cross-built file starts with ELF magic
PASS: cross-built file is EM_AARCH64
PASS: /data/cross_hello visible on disk
PASS: cross-built binary printed its marker
PASS: cross-built binary exited with code 7

6 PASS / 0 FAIL
```

If any of those six FAILs after a future Makefile edit,
the cross-toolchain contract has been broken — and
any future compiler port (a Part XVIII GCC port, or
anything else) will not work until the test goes green
again.

## The `hello.c` we cross-build

The test's source file is inlined into the harness:

```c
static long sys_write(long fd, const void *buf, long n) {
    register long x8 __asm__("x8") = 1;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = n;
    __asm__ volatile("svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2)
        : "memory");
    return x0;
}

static long sys_exit(long code) {
    register long x8 __asm__("x8") = 2;
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc #0"
        : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long slen(const char *s) {
    const char *p = s; while (*p) p++; return p - s;
}

int main(void) {
    const char *m = "M122-CROSS-OK\n";
    sys_write(1, m, slen(m));
    sys_exit(7);
    return 0;
}
```

It uses only freestanding C and our two simplest
syscalls. There is no libc, no startup, no global
constructors. The link line is:

```
aarch64-elf-gcc $USER_CFLAGS -c hello.c -o hello.o
aarch64-elf-ld  $USER_LDFLAGS -o hello.elf \
    build/userspace/crt/crt0.o hello.o
aarch64-elf-objcopy --strip-debug hello.elf hello.stripped
```

Three commands. That is the whole cross-toolchain
surface a future GCC port (Part XVIII) would have to
satisfy.

## How `/data` ended up in this story

We needed somewhere to stage the cross-built binary
that wasn't `/bin`, because `/bin` lives on `disk.img`
and we don't want to rebuild the disk image inside a
test. The OSFS-2 mount at `/data` (registered in
[kernel/core/vfs.c](../../../kernel/core/vfs.c) by
`osfs2_register_mount()`) is backed by the second
virtio-blk drive (`hd1` → `build/data.img`), which
[scripts/mkosfs2.py](../../../scripts/mkosfs2.py) can
seed with arbitrary files via:

```
python3 scripts/mkosfs2.py build/data.img cross_hello=/host/path
```

That `name=path` form was already there but
undocumented — chapter 158 is also where it gets
called out. `/data` is now the canonical "staged at
boot" mount for test artifacts that don't belong in
`/bin`.

## The flaky-first-prompt gotcha

While writing the test we hit a transient where the
first `ls /data` after boot would return an empty buffer
because `wait_for(PROMPT)` had matched a `/$` that the
WM's `[wmclient] DAMAGE failed status=-5` retry chatter
emitted partway through boot. Fix: after `wait_for(PROMPT)`,
sleep 2 s and drain the serial buffer before sending
the first real command. This is the same idiom every
desktop-aware test in this codebase uses — see
[scripts/test_boot_to_desktop.py](../../../scripts/test_boot_to_desktop.py)
for the canonical version.

## Applied to (per the apps-must-use-features rule)

- **No existing app changed.** This chapter is pure
  contract + regression infrastructure. The contract
  is implicitly used by *every* userspace binary in
  the repo — none of them stopped working when the
  test went green, which is the point.
- **New test added:** [scripts/test_gcc_cross_hello.py](../../../scripts/test_gcc_cross_hello.py).
- **New mount documented:** `/data` (OSFS-2 on hd1)
  as the canonical "staged at boot" location for test
  fixtures. Now used by this chapter's test; chapter
  124 will reuse it for `/data/src/hello.c`.
- **Debug script kept** (per the
  [debug-scripts-policy](../../../scripts/_dbg_data_mount.py)):
  [scripts/_dbg_data_mount.py](../../../scripts/_dbg_data_mount.py)
  — boots, seeds, prints `ls /data` + `ls /` +
  `cat /data/<file>`. Used to root-cause the
  flaky-first-prompt bug above.

## Lessons

1. **Write the contract before the implementation.**
   The five-row table at the top of this chapter is
   what every future compiler port — starting with the
   Part XVIII GCC work — will be measured against.
   Without it, "port a compiler" is unscopable — every
   release would trigger arguments about what counts
   as done. With it, the contract is six asserts and
   an `exit 7`.
2. **`/data` is the right place for test fixtures.**
   `/bin` requires a disk.img rebuild; `/tmp` is empty
   at boot; `/data` survives boot and is per-test
   reseedable via `mkosfs2.py name=path`. Use it.
3. **`-mgeneral-regs-only` is load-bearing.** It is
   the one flag that makes the kernel/userspace
   contract honest about FP state. Removing it would
   silently corrupt FP-using kernel threads. Document
   it; the next person who reads `USER_CFLAGS` will
   not know why it's there otherwise.
4. **Cosmetic linker warnings are fine.** `aarch64-elf-ld`
   will print `has a LOAD segment with RWX permissions`
   on our binaries because we collapse text and rodata
   into one segment for the loader's benefit. The
   kernel maps that segment R-X regardless; the warning
   is wrong. Don't suppress it — it documents a real
   asymmetry between the on-disk ELF and the in-memory
   mapping.

## What's deferred to later chapters (and Part XVIII)

| Feature | Why deferred |
| --- | --- |
| Real `gcc` binary running in-guest | ~25 MB build artifact, ~minutes to compile, needs full GCC port (Part XVIII) |
| `as` / `ld` covering the full AArch64 ISA | Same scope as the `/bin/cc` work in 121 — additive over many chapters |
| `libgcc.a` real implementation | Today's `libgcc.h` covers exactly the helpers our toolchain emits; a higher-quality compiler at higher opt levels will demand more |
| Sysroot of standard headers | Our headers are deliberately minimal (chapter 148); a real GCC port expects POSIX |
| `-O2` semantic equivalence | We currently build at `-Os`; opt-level changes belong to whichever backend we adopt |

## Next

[Chapter 159 — `/bin/cc` grows variables and arithmetic](159-cc-variables-and-arithmetic.md)
takes the contract this chapter formalized and exercises
it by growing the in-guest compiler one step further.
