# Hobby OS — aarch64 build
#
# Targets the QEMU `-machine virt` board. On Apple Silicon this runs
# natively under HVF; on x86 hosts it falls back to TCG (slower but
# functional, useful for CI).

# ----------------------------------------------------------------------
# Toolchain
# ----------------------------------------------------------------------
CROSS    ?= aarch64-elf-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy
OBJDUMP  := $(CROSS)objdump
QEMU     := qemu-system-aarch64

# ----------------------------------------------------------------------
# Flags
#
# -ffreestanding         no hosted libc; we provide our own startup.
# -nostdlib              do not link libgcc/libc by default.
# -mgeneral-regs-only    forbid FP/SIMD; CPACR_EL1.FPEN is still 0 so
#                        any FP/SIMD instruction would trap.
# -fno-stack-protector   no canary support yet.
# -fno-pie / -fno-pic    kernel is loaded at a fixed address.
# --orphan-handling=error fail the build if the linker script does
#                        not name an input section.
# ----------------------------------------------------------------------
CFLAGS   := -ffreestanding -nostdlib -nostartfiles \
            -mcpu=cortex-a72 -mgeneral-regs-only \
            -fno-stack-protector -fno-pie -fno-pic \
            -fno-asynchronous-unwind-tables \
            -Wall -Wextra -Werror -O2 -g \
            -MMD -MP \
            -I kernel/core -I kernel/device -I kernel/arch

ASFLAGS  := -mcpu=cortex-a72 -g

LDFLAGS  := -T linker/kernel.ld -nostdlib --orphan-handling=error \
            -z noexecstack -z max-page-size=0x1000

# ----------------------------------------------------------------------
# Layout
# ----------------------------------------------------------------------
BUILD    := build
KERNEL   := $(BUILD)/kernel.elf
KIMG     := $(BUILD)/kernel.img

C_SRCS   := kernel/core/main.c \
            kernel/core/serial.c \
            kernel/core/exception.c \
            kernel/core/irq.c \
            kernel/core/timer.c \
            kernel/core/walltime.c \
            kernel/core/heap.c \
            kernel/core/thread.c \
            kernel/core/fdt.c \
            kernel/core/pmem.c \
            kernel/core/pmem_refcount.c \
            kernel/core/page_cache.c \
            kernel/core/syscall.c \
            kernel/core/elf.c \
            kernel/core/vfs.c \
            kernel/core/osfs.c \
            kernel/core/uaccess.c \
            kernel/core/pipe.c \
            kernel/core/pty.c \
            kernel/core/tmpfs.c \
            kernel/core/osfs2.c \
            kernel/core/osfs2_cache.c \
            kernel/core/osfs2_journal.c \
            kernel/core/strace.c \
            kernel/core/srv.c \
            kernel/core/userfs.c \
            kernel/core/console_in.c \
            kernel/core/wm.c \
            kernel/core/wm_font.c \
            kernel/core/wsd_fb.c \
            kernel/core/win_fb.c \
            kernel/core/net.c \
            kernel/core/icmp.c \
            kernel/core/udp.c \
            kernel/core/dhcp.c \
            kernel/core/tcp.c \
            kernel/core/dns.c \
            kernel/core/random.c \
            kernel/device/gic.c \
            kernel/device/virtio_blk.c \
            kernel/device/virtio_gpu.c \
            kernel/device/virtio_input.c \
            kernel/device/virtio_tablet.c \
            kernel/device/virtio_net.c \
            kernel/device/virtio_snd.c \
            kernel/device/virtio_rng.c \
            kernel/device/blk_cache.c \
            kernel/device/fb.c \
            kernel/device/font.c \
            kernel/device/text.c \
            kernel/arch/page_tables.c \
            kernel/arch/address_space.c \
            kernel/arch/cpu.c \
            kernel/arch/psci.c \
            kernel/arch/ipi.c

S_SRCS   := kernel/arch/boot.s \
            kernel/arch/mmu.S \
            kernel/arch/vectors.S \
            kernel/arch/context_switch.S

C_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
S_OBJS   := $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(S_SRCS))) \
            $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(S_SRCS)))

# ----------------------------------------------------------------------
# Userspace programs
#
# Each user program is built independently with the user-side link
# script and then objcopy'd into a raw binary.  The raw binary is
# wrapped into an ELF object with `objcopy -I binary`, exposing
# `_binary_<name>_bin_start/_end/_size` symbols that the kernel
# references via include kernel/core/embedded_user.h.  The wrapper
# object is then linked into the kernel ELF alongside the rest.
# ----------------------------------------------------------------------
USER_CFLAGS := -ffreestanding -nostdlib -nostartfiles \
               -mcpu=cortex-a72 \
               -fno-stack-protector -fno-pie -fno-pic \
               -fno-asynchronous-unwind-tables \
               -Wall -Wextra -Werror -Os -g \
               -MMD -MP

USER_LDFLAGS := -T userspace/linker_user.ld -nostdlib --orphan-handling=error \
                -z noexecstack -z max-page-size=0x1000

# `make` with no target should build everything (kernel + disk image),
# not just the first ELF rule the file happens to mention.  Without
# this line make picks $(HELLO_ELF) as .DEFAULT_GOAL (first non-pattern
# target in the file) and silently skips kernel.elf -- chapter 106b
# debugging spent an hour rebuilding the WRONG thing because edits to
# kernel/core/tcp.c looked compiled in but weren't.  See the
# `all:` rule near the bottom for what gets built.
.DEFAULT_GOAL := all

HELLO_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/hello/hello.o
HELLO_ELF  := $(BUILD)/userspace/hello/hello.elf
HELLO_STRIPPED := $(BUILD)/userspace/hello/hello.stripped.elf
HELLO_EMBED:= $(BUILD)/userspace/hello/hello.elf.o

CAT_OBJS := $(BUILD)/userspace/crt/crt0.o \
            $(BUILD)/userspace/cat/cat.o
CAT_ELF  := $(BUILD)/userspace/cat/cat.elf
CAT_STRIPPED := $(BUILD)/userspace/cat/cat.stripped.elf
CAT_EMBED:= $(BUILD)/userspace/cat/cat.elf.o

INIT_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/init/init.o
INIT_ELF  := $(BUILD)/userspace/init/init.elf
INIT_STRIPPED := $(BUILD)/userspace/init/init.stripped.elf
INIT_EMBED:= $(BUILD)/userspace/init/init.elf.o

SH_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/sh/sh.o
SH_ELF  := $(BUILD)/userspace/sh/sh.elf
SH_STRIPPED := $(BUILD)/userspace/sh/sh.stripped.elf
SH_EMBED:= $(BUILD)/userspace/sh/sh.elf.o

# milestone-16 isolation smoke test: pokes a kernel address from
# EL0 and is expected to fault.  See userspace/badpoke/badpoke.c.
BADPOKE_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/badpoke/badpoke.o
BADPOKE_ELF  := $(BUILD)/userspace/badpoke/badpoke.elf
BADPOKE_STRIPPED := $(BUILD)/userspace/badpoke/badpoke.stripped.elf

# milestone-16 syscall-pointer test: hands the kernel a kernel
# address as a buffer pointer and expects -EFAULT every time.
BADPTR_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/badptr/badptr.o
BADPTR_ELF  := $(BUILD)/userspace/badptr/badptr.elf
BADPTR_STRIPPED := $(BUILD)/userspace/badptr/badptr.stripped.elf

# chapter-101 guard-page test: recurses until it pokes the guard
# below the user stack, expecting the kernel's friendly
# "[svc] user stack overflow" diagnostic instead of a generic dump.
STACKBOMB_OBJS := $(BUILD)/userspace/crt/crt0.o \
                  $(BUILD)/userspace/stackbomb/stackbomb.o
STACKBOMB_ELF  := $(BUILD)/userspace/stackbomb/stackbomb.elf
STACKBOMB_STRIPPED := $(BUILD)/userspace/stackbomb/stackbomb.stripped.elf

# milestone-17 user-heap test: exercises malloc/free + sbrk.
HEAPTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/heaptest/heaptest.o
HEAPTEST_ELF  := $(BUILD)/userspace/heaptest/heaptest.elf
HEAPTEST_STRIPPED := $(BUILD)/userspace/heaptest/heaptest.stripped.elf

# chapter-90 mmap test: exercises anon + ramfs file mmap.
MMAPTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/mmaptest/mmaptest.o
MMAPTEST_ELF  := $(BUILD)/userspace/mmaptest/mmaptest.elf
MMAPTEST_STRIPPED := $(BUILD)/userspace/mmaptest/mmaptest.stripped.elf

# chapter-91 thread test: spawns N workers, increments shared
# counter under a mutex, joins all, verifies the count.
THREADTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(BUILD)/userspace/threadtest/threadtest.o
THREADTEST_ELF  := $(BUILD)/userspace/threadtest/threadtest.elf
THREADTEST_STRIPPED := $(BUILD)/userspace/threadtest/threadtest.stripped.elf

# chapter-92 SMP thread test: same shape as threadtest but pins
# half the workers to CPU 0 and half to CPU 1 via clone2().
# Verifies cross-CPU futex correctness AND CPU pinning.
THREADTEST2_OBJS := $(BUILD)/userspace/crt/crt0.o \
                    $(BUILD)/userspace/threadtest2/threadtest2.o
THREADTEST2_ELF  := $(BUILD)/userspace/threadtest2/threadtest2.elf
THREADTEST2_STRIPPED := $(BUILD)/userspace/threadtest2/threadtest2.stripped.elf

# chapter-93 CLONE_FILES test: spawns a worker that shares the
# parent's fd table, has it write to a parent-opened tmpfs fd,
# and verifies the bytes round-trip.  Then re-spawns without
# CLONE_FILES and verifies the worker's write fails with EBADF
# (private fd table). 
THREADTEST3_OBJS := $(BUILD)/userspace/crt/crt0.o \
                    $(BUILD)/userspace/threadtest3/threadtest3.o
THREADTEST3_ELF  := $(BUILD)/userspace/threadtest3/threadtest3.elf
THREADTEST3_STRIPPED := $(BUILD)/userspace/threadtest3/threadtest3.stripped.elf

# milestone-18 argv test: prints argv[1..argc-1] joined by spaces.
ECHO_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/echo/echo.o
ECHO_ELF  := $(BUILD)/userspace/echo/echo.elf
ECHO_STRIPPED := $(BUILD)/userspace/echo/echo.stripped.elf

# milestone-19 printf test: exercises the libc printf.h header.
PRINTFTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(BUILD)/userspace/printftest/printftest.o
PRINTFTEST_ELF  := $(BUILD)/userspace/printftest/printftest.elf
PRINTFTEST_STRIPPED := $(BUILD)/userspace/printftest/printftest.stripped.elf

# chapter-128a setjmp/longjmp test: links the aarch64 asm in
# userspace/libc/setjmp.S plus a C driver that exercises every
# case the spec promises (0-first, val-passes-through, 0->1).
SETJMPTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(BUILD)/userspace/libc/setjmp.o \
                   $(BUILD)/userspace/setjmptest/setjmptest.o
SETJMPTEST_ELF  := $(BUILD)/userspace/setjmptest/setjmptest.elf
SETJMPTEST_STRIPPED := $(BUILD)/userspace/setjmptest/setjmptest.stripped.elf

# chapter-128b raise() / abort() / expanded SIG* test (sigtest2)
# and a companion aborttest binary that drives abort() and the
# 128+SIGABRT exit-status convention.
SIGTEST2_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/sigtest2/sigtest2.o
SIGTEST2_ELF  := $(BUILD)/userspace/sigtest2/sigtest2.elf
SIGTEST2_STRIPPED := $(BUILD)/userspace/sigtest2/sigtest2.stripped.elf

ABORTTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                  $(BUILD)/userspace/aborttest/aborttest.o
ABORTTEST_ELF  := $(BUILD)/userspace/aborttest/aborttest.elf
ABORTTEST_STRIPPED := $(BUILD)/userspace/aborttest/aborttest.stripped.elf

# chapter-128c ctype.h / string.h / assert.h regression.
# strtest needs cstring.o for the extern memcpy/memset/memmove
# /memcmp/strlen symbols that string.h declares.  assertfail
# also needs cstring.o for __assert_fail.
# NB: CSTRING_OBJ is defined further down; we reference its
# literal path here because Make's `:=` resolves immediately
# and would otherwise expand to empty.
STRTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/strtest/strtest.o \
                $(BUILD)/userspace/libc/cstring.o
STRTEST_ELF  := $(BUILD)/userspace/strtest/strtest.elf
STRTEST_STRIPPED := $(BUILD)/userspace/strtest/strtest.stripped.elf

ASSERTFAIL_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(BUILD)/userspace/assertfail/assertfail.o \
                   $(BUILD)/userspace/libc/cstring.o
ASSERTFAIL_ELF  := $(BUILD)/userspace/assertfail/assertfail.elf
ASSERTFAIL_STRIPPED := $(BUILD)/userspace/assertfail/assertfail.stripped.elf

# chapter-128d POSIX <time.h> regression.
TIMETEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/timetest/timetest.o \
                 $(BUILD)/userspace/libc/cstring.o
TIMETEST_ELF  := $(BUILD)/userspace/timetest/timetest.elf
TIMETEST_STRIPPED := $(BUILD)/userspace/timetest/timetest.stripped.elf

# chapter-128e <stdlib.h> regression: qsort / bsearch / strtol /
# strtoul / strtoll / strtoull / atol / atoll / abs+labs+llabs /
# div+ldiv+lldiv / getopt.  Same shape as TIMETEST: crt0 + the
# .o + the literal cstring path (CSTRING_OBJ is defined later in
# this file but := captures the literal path immediately; see
# makefile-pattern-rule-ordering repo memory).
STDLIBTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(BUILD)/userspace/stdlibtest/stdlibtest.o \
                   $(BUILD)/userspace/libc/cstring.o
STDLIBTEST_ELF  := $(BUILD)/userspace/stdlibtest/stdlibtest.elf
STDLIBTEST_STRIPPED := $(BUILD)/userspace/stdlibtest/stdlibtest.stripped.elf

# chapter-128f real <printf.h> + <scanf.h> regression: %o,
# precision, +/space/# flags, %n, sscanf %d/%i/%u/%o/%x/%s/%c/
# scansets/%n.
PRINTFTEST2_OBJS := $(BUILD)/userspace/crt/crt0.o \
                    $(BUILD)/userspace/printftest2/printftest2.o \
                    $(BUILD)/userspace/libc/cstring.o
PRINTFTEST2_ELF  := $(BUILD)/userspace/printftest2/printftest2.elf
PRINTFTEST2_STRIPPED := $(BUILD)/userspace/printftest2/printftest2.stripped.elf

# chapter-129 FP/SIMD-at-EL0 regression: plain double arithmetic
# at EL0, FP register preservation across cooperative yields, and
# d8..d15 preservation through setjmp/longjmp.
FPTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/fptest/fptest.o \
               $(BUILD)/userspace/libc/setjmp.o \
               $(BUILD)/userspace/libc/cstring.o
FPTEST_ELF  := $(BUILD)/userspace/fptest/fptest.elf
FPTEST_STRIPPED := $(BUILD)/userspace/fptest/fptest.stripped.elf

# chapter-130a — DoomGeneric port.
#
# DoomGeneric (github.com/ozkl/doomgeneric) is Chocolate Doom
# 1.7.0 with all I/O routed through six function pointers the
# platform port implements: DG_Init, DG_DrawFrame, DG_SleepMs,
# DG_GetTicksMs, DG_GetKey, DG_SetWindowTitle.  Our port is
# `userspace/doom/doomgeneric_osdev.c` (~250 lines).
#
# We exclude every upstream platform shim (SDL/X11/win/soso/etc)
# and the SDL/Allegro music backends (which drag in mus2mid).
# That leaves 83 portable .c files compiled by the
# `$(BUILD)/vendor/doomgeneric/%.o` pattern rule below.
#
# Vendor sources are NOT held to our -Werror standard (the
# upstream code has plenty of "unused but set" and similar
# warnings; fixing them would create a maintenance burden every
# time we re-sync from upstream).  Our own shim IS held to
# -Werror via the default USER_CFLAGS pattern rule.
DOOM_VENDOR_EXCLUDES := \
    vendor/doomgeneric/src/doomgeneric_allegro.c \
    vendor/doomgeneric/src/doomgeneric_emscripten.c \
    vendor/doomgeneric/src/doomgeneric_linuxvt.c \
    vendor/doomgeneric/src/doomgeneric_sdl.c \
    vendor/doomgeneric/src/doomgeneric_soso.c \
    vendor/doomgeneric/src/doomgeneric_sosox.c \
    vendor/doomgeneric/src/doomgeneric_win.c \
    vendor/doomgeneric/src/doomgeneric_xlib.c \
    vendor/doomgeneric/src/i_sdlsound.c \
    vendor/doomgeneric/src/i_sdlmusic.c \
    vendor/doomgeneric/src/i_allegrosound.c \
    vendor/doomgeneric/src/i_allegromusic.c \
    vendor/doomgeneric/src/mus2mid.c

DOOM_VENDOR_SRCS := $(filter-out $(DOOM_VENDOR_EXCLUDES), \
    $(wildcard vendor/doomgeneric/src/*.c))
DOOM_VENDOR_OBJS := $(patsubst vendor/doomgeneric/src/%.c, \
    $(BUILD)/vendor/doomgeneric/%.o, $(DOOM_VENDOR_SRCS))

DOOM_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/doom/doomgeneric_osdev.o \
             $(BUILD)/userspace/libc/setjmp.o \
             $(BUILD)/userspace/libc/cstring.o \
             $(BUILD)/userspace/libgui/wmclient.o \
             $(DOOM_VENDOR_OBJS)
DOOM_ELF  := $(BUILD)/userspace/doom/doom.elf
DOOM_STRIPPED := $(BUILD)/userspace/doom/doom.stripped.elf

# chapter-116a errno test: drives the new __errno_value plumbing
# from a few representative syscalls (open / close / read / getpid).
ERRNOTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                  $(BUILD)/userspace/errnotest/errnotest.o
ERRNOTEST_ELF  := $(BUILD)/userspace/errnotest/errnotest.elf
ERRNOTEST_STRIPPED := $(BUILD)/userspace/errnotest/errnotest.stripped.elf

# chapter-116b stdio test: drives the new FILE * layer end-to-end
# (fopen/fread/fwrite/fseek/ftell/fgetc/fputc/fprintf + stderr).
STDIOTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                  $(BUILD)/userspace/stdiotest/stdiotest.o
STDIOTEST_ELF  := $(BUILD)/userspace/stdiotest/stdiotest.elf
STDIOTEST_STRIPPED := $(BUILD)/userspace/stdiotest/stdiotest.stripped.elf

# chapter-116c env arena test: drives env.h's POSIX surface
# (getenv -> char*, setenv with overwrite flag, unsetenv, putenv,
# environ[] iteration, kernel write-through).
ENVTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/envtest/envtest.o
ENVTEST_ELF  := $(BUILD)/userspace/envtest/envtest.elf
ENVTEST_STRIPPED := $(BUILD)/userspace/envtest/envtest.stripped.elf

# chapter-117 stat test: drives SYS_STAT / SYS_FSTAT, the new
# struct stat layout, S_ISREG / S_ISDIR macros, opendir / readdir,
# and access() against a curated set of known files / directories.
STATTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/stattest/stattest.o \
                 $(BUILD)/userspace/libc/cstring.o
STATTEST_ELF  := $(BUILD)/userspace/stattest/stattest.elf
STATTEST_STRIPPED := $(BUILD)/userspace/stattest/stattest.stripped.elf

# chapter-118 assembler: tiny AArch64 /bin/as that consumes
# `.s` input and produces ELF64-LSB relocatable objects.
# Curated mnemonic subset (mov, add, sub, ldr, str, b, bl,
# svc, ret, ...).  See userspace/as/as.c.
AS_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/as/as.o
AS_ELF  := $(BUILD)/userspace/as/as.elf
AS_STRIPPED := $(BUILD)/userspace/as/as.stripped.elf

# chapter-119 linker + archiver: /bin/ld combines /bin/as's
# relocatable outputs into an ET_EXEC the kernel ELF loader
# can mmap and run, and /bin/ar wraps a list of .o files into
# a SysV ar archive.  See userspace/ld/ld.c and
# userspace/ar/ar.c.
LD_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/ld/ld.o
LD_ELF  := $(BUILD)/userspace/ld/ld.elf
LD_STRIPPED := $(BUILD)/userspace/ld/ld.stripped.elf
AR_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/ar/ar.o
AR_ELF  := $(BUILD)/userspace/ar/ar.elf
AR_STRIPPED := $(BUILD)/userspace/ar/ar.stripped.elf

# chapter-131f real binutils as/ld: the toy chapter-118 /bin/as
# and chapter-119 /bin/ld served as pedagogical stepping stones
# but cannot link the full GNU binutils, gcc, doom, etc. that
# chapters 131g+ will build in-guest.  scripts/test_guest_ld.py
# cross-builds gas/as-new + ld/ld-new against our freestanding
# osdev libc (cstring.o as the libc bridge); the OSFS recipe
# below ships the stripped binaries as /bin/as and /bin/ld in
# place of the toy versions.  The toy AS_ELF / LD_ELF rules
# stay buildable for chapter 118/119 readers but are no longer
# wired into the disk image.
BINUTILS_GUEST_BUILD := $(BUILD)/binutils-build-guest-ld
BINUTILS_AS_NEW      := $(BINUTILS_GUEST_BUILD)/gas/as-new
BINUTILS_LD_NEW      := $(BINUTILS_GUEST_BUILD)/ld/ld-new
BINUTILS_AS_STRIPPED := $(BUILD)/userspace/binutils/as.stripped.elf
BINUTILS_LD_STRIPPED := $(BUILD)/userspace/binutils/ld.stripped.elf

# chapter-132a: vendored gcc-14.2.0 source tree (no build yet,
# just the patched source).  The actual fetch/patch rule lives
# further down in the chapter-132a block alongside the binutils
# rules; we declare the variables up here so `all` / `run` /
# `run-graphical` can list the marker as a prereq (make expands
# prereq lists at parse time, so the variable must be defined
# before its first use).
GCC_SRC    := vendor/gcc-14.2.0
GCC_MARKER := $(GCC_SRC)/.patched-osdev

# chapter-132b: GMP / MPFR / MPC symlinked in-tree under
# $(GCC_SRC)/{gmp,mpfr,mpc}.  Same forward-reference reason as
# $(GCC_MARKER) above.
GCC_PREREQS_MARKER := $(GCC_SRC)/.prereqs-osdev

# chapter-120 bootstrap glue: a demo that walks the new
# __init_array / atexit / __cxa_finalize machinery added to
# crt0.S and userspace/libc/atexit.h.  Prints six lines in a
# specific order (see userspace/atexittest/atexittest.c).
ATEXITTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(BUILD)/userspace/atexittest/atexittest.o
ATEXITTEST_ELF  := $(BUILD)/userspace/atexittest/atexittest.elf
ATEXITTEST_STRIPPED := $(BUILD)/userspace/atexittest/atexittest.stripped.elf

# chapter-121 one-chapter native C compiler: /bin/cc.  Takes
# a curated C subset (main + printf/puts/write/return), emits
# AArch64 asm using the bl-past-string trick to materialise
# string literals without needing ADRP relocs that /bin/as
# does not support, then drives /bin/as and /bin/ld to
# produce a runnable ELF.  See userspace/cc/cc.c.
CC_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/cc/cc.o
CC_ELF  := $(BUILD)/userspace/cc/cc.elf
CC_STRIPPED := $(BUILD)/userspace/cc/cc.stripped.elf

# chapter-132f /bin/gcc shim.  Tiny C program that prepends
# `-B/bin/` to argv and execs /bin/xgcc.  See
# userspace/gccw/gccw.c for the rationale.
GCCW_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/gccw/gccw.o
GCCW_ELF  := $(BUILD)/userspace/gccw/gccw.elf
GCCW_STRIPPED := $(BUILD)/userspace/gccw/gccw.stripped.elf

# chapter-132f xgcc binary.  Built out-of-tree under
# build/gcc-build-guest/ by scripts/test_guest_gcc.py.  Treated
# as an external input here: if it's missing the OSFS build
# falls back to a placeholder that prints a "run
# scripts/test_guest_gcc.py first" message.  See chapter 132f.
XGCC_GUEST_BIN  := $(BUILD)/gcc-build-guest/gcc/gcc/xgcc
CC1_GUEST_BIN   := $(BUILD)/gcc-build-guest/gcc/gcc/cc1
XGCC_GUEST_STRIPPED := $(BUILD)/userspace/gccw/xgcc.stripped.elf
CC1_GUEST_STRIPPED  := $(BUILD)/userspace/gccw/cc1.stripped.elf

# chapter-126 tiny make: parses a 16-rule Makefile, runs
# recipe commands via spawn()+waitpid().  See userspace/make/make.c.
MAKE_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/make/make.o
MAKE_ELF  := $(BUILD)/userspace/make/make.elf
MAKE_STRIPPED := $(BUILD)/userspace/make/make.stripped.elf

# milestone-20 ls: walks the SYS_LISTDIR namespace.
# Includes cstring.o so dirent.h's opendir/closedir can resolve
# the __asm__("malloc")/__asm__("free") rename trampolines
# introduced in chapter 132f (dirent.h had to stop including
# malloc.h to keep the gcc-tree poisoning happy).
LS_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/ls/ls.o \
           $(BUILD)/userspace/libc/cstring.o
LS_ELF  := $(BUILD)/userspace/ls/ls.elf
LS_STRIPPED := $(BUILD)/userspace/ls/ls.stripped.elf

# milestone-21 uptime: prints monotonic ms since boot.
UPTIME_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/uptime/uptime.o
UPTIME_ELF  := $(BUILD)/userspace/uptime/uptime.elf
UPTIME_STRIPPED := $(BUILD)/userspace/uptime/uptime.stripped.elf

# chapter-99 ps: walks /proc and prints one row per process.
PS_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/ps/ps.o
PS_ELF  := $(BUILD)/userspace/ps/ps.elf
PS_STRIPPED := $(BUILD)/userspace/ps/ps.stripped.elf

# chapter-99 top: refreshes the ps view every second.
TOP_OBJS := $(BUILD)/userspace/crt/crt0.o \
            $(BUILD)/userspace/top/top.o
TOP_ELF  := $(BUILD)/userspace/top/top.elf
TOP_STRIPPED := $(BUILD)/userspace/top/top.stripped.elf

# chapter-100 strace: forks a child, calls SYS_TRACE_ME, execs
# the target, and pumps /proc/<child>/trace into stderr until
# the child exits.  Tracer machinery itself is in kernel/core/strace.c.
STRACE_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/strace/strace.o
STRACE_ELF  := $(BUILD)/userspace/strace/strace.elf
STRACE_STRIPPED := $(BUILD)/userspace/strace/strace.stripped.elf

# chapter-95 date: prints wall-clock time via SYS_GETTIMEOFDAY.
DATE_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/date/date.o
DATE_ELF  := $(BUILD)/userspace/date/date.elf
DATE_STRIPPED := $(BUILD)/userspace/date/date.stripped.elf

# chapter-96 beep: synthesise a square wave via SYS_BEEP.
BEEP_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/beep/beep.o
BEEP_ELF  := $(BUILD)/userspace/beep/beep.elf
BEEP_STRIPPED := $(BUILD)/userspace/beep/beep.stripped.elf

# chapter-112 getrand: print N random bytes as hex via SYS_GETRANDOM.
GETRAND_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/getrand/getrand.o
GETRAND_ELF  := $(BUILD)/userspace/getrand/getrand.elf
GETRAND_STRIPPED := $(BUILD)/userspace/getrand/getrand.stripped.elf

# chapter-113 mount: print the kernel's mount table via SYS_MOUNTS.
MOUNT_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/mount/mount.o
MOUNT_ELF  := $(BUILD)/userspace/mount/mount.elf
MOUNT_STRIPPED := $(BUILD)/userspace/mount/mount.stripped.elf

# ----------------------------------------------------------------------
# chapter-112a BearSSL static library (libbearssl.a).
#
# Vendored at vendor/bearssl/ from BearSSL 0.6 (MIT licensed).
# We compile every src/**/*.c EXCEPT sysrng.c (that file probes
# /dev/urandom and similar host-OS surfaces; our seeding comes
# from kernel/core/random.c via SYS_GETRANDOM, so sysrng.c would
# both not link and not be used).  T0-generated parsers are
# already checked in, so no Mono required.
#
# Vendor code is built with a permissive flag set: -w to drop
# -Wall/-Wextra/-Werror noise (BearSSL is clean against its own
# author's flag set, not necessarily ours), and -O2 because
# AES/SHA/GCM are the kind of code that benefits from inlining.
# Everything else mirrors USER_CFLAGS so the resulting .o files
# are ABI-compatible with the rest of userspace.
# ----------------------------------------------------------------------
BEARSSL_SRCS := $(shell find vendor/bearssl/src -name '*.c' \
                    -not -path 'vendor/bearssl/src/rand/sysrng.c')
BEARSSL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(BEARSSL_SRCS))
BEARSSL_LIB  := $(BUILD)/vendor/bearssl/libbearssl.a
BEARSSL_INC  := -I vendor/bearssl-shim -I vendor/bearssl/inc -I vendor/bearssl/src
BEARSSL_CFLAGS := -ffreestanding -nostdlib \
                  -mcpu=cortex-a72 -mgeneral-regs-only \
                  -fno-stack-protector -fno-pie -fno-pic \
                  -fno-asynchronous-unwind-tables \
                  -O2 -g -MMD -MP -w $(BEARSSL_INC)

# Bridge object: extern memcpy/memset/memmove/memcmp/strlen + time()
# stub.  Required by every binary that links libbearssl.a.
CSTRING_OBJ := $(BUILD)/userspace/libc/cstring.o

# chapter-112b shared objects (used by both tlstest and httpsd):
#
#   TLS_SOCKET_OBJ -- thin BearSSL-engine + br_sslio_ wrapper that
#                     exposes a tls_socket_t with connect/send/
#                     recv/close methods over our chapter-104
#                     TCP sockets and chapter-112 SYS_GETRANDOM.
#   TEST_CHAIN_OBJ -- re-exports BearSSL's sample CN=localhost
#                     RSA-2048 cert chain + private key as extern
#                     symbols (test_server_chain[], test_server_key)
#                     for use by the in-guest httpsd server and the
#                     tlstest knownkey client.
#
# Both need BearSSL's umbrella header on the include path; the
# test cert TU also wants vendor/bearssl/samples/ so the sample
# .h files (chain-rsa.h, key-rsa.h) resolve.  Pattern-rule
# overrides for each are below.
TLS_SOCKET_OBJ := $(BUILD)/userspace/libc/tls_socket.o
TEST_CHAIN_OBJ := $(BUILD)/vendor/testcerts/test_chain.o
# Chapter 112e: second test chain (ECDSA / P-256) re-exported
# from a sibling TU because the BearSSL sample headers all use
# `static const` for their CERT0/CERT1/CHAIN symbols.
TEST_CHAIN_EC_OBJ := $(BUILD)/vendor/testcerts/test_chain_ec.o

# chapter-112a tlstest: links libbearssl.a and runs br_sha256 against
# two NIST KAT vectors.  Proves the build works; no TLS yet.
# chapter 112b extends this with `tlstest --handshake HOST PORT`
# which dials an in-guest TLS server and verifies the handshake
# end-to-end -- so tlstest now also needs the tls_socket + cert
# objects.
TLSTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/tlstest/tlstest.o \
                $(TLS_SOCKET_OBJ) \
                $(TEST_CHAIN_OBJ) \
                $(CSTRING_OBJ)
TLSTEST_ELF  := $(BUILD)/userspace/tlstest/tlstest.elf
TLSTEST_STRIPPED := $(BUILD)/userspace/tlstest/tlstest.stripped.elf

# chapter-112b httpsd: in-guest TLS test server.  Same socket
# accept loop as httpd, but each connection runs the BearSSL
# server engine on top.  Serves a fixed marker body so the
# handshake KAT can verify end-to-end encryption.
HTTPSD_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/httpsd/httpsd.o \
               $(TEST_CHAIN_OBJ) \
               $(TEST_CHAIN_EC_OBJ) \
               $(CSTRING_OBJ)
HTTPSD_ELF  := $(BUILD)/userspace/httpsd/httpsd.elf
HTTPSD_STRIPPED := $(BUILD)/userspace/httpsd/httpsd.stripped.elf

# chapter-97 pngdec: decode a PNG and print w x h + checksum.
PNGDEC_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/pngdec/pngdec.o
PNGDEC_ELF  := $(BUILD)/userspace/pngdec/pngdec.elf
PNGDEC_STRIPPED := $(BUILD)/userspace/pngdec/pngdec.stripped.elf

# milestone-24 env: prints the per-process environment.
ENV_OBJS := $(BUILD)/userspace/crt/crt0.o \
            $(BUILD)/userspace/env/env.o
ENV_ELF  := $(BUILD)/userspace/env/env.elf
ENV_STRIPPED := $(BUILD)/userspace/env/env.stripped.elf

# milestone-27 grep / wc / head / tail: classic text-shaping tools.
GREP_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/grep/grep.o
GREP_ELF  := $(BUILD)/userspace/grep/grep.elf
GREP_STRIPPED := $(BUILD)/userspace/grep/grep.stripped.elf

WC_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/wc/wc.o
WC_ELF  := $(BUILD)/userspace/wc/wc.elf
WC_STRIPPED := $(BUILD)/userspace/wc/wc.stripped.elf

HEAD_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/head/head.o
HEAD_ELF  := $(BUILD)/userspace/head/head.elf
HEAD_STRIPPED := $(BUILD)/userspace/head/head.stripped.elf

TAIL_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/tail/tail.o
TAIL_ELF  := $(BUILD)/userspace/tail/tail.elf
TAIL_STRIPPED := $(BUILD)/userspace/tail/tail.stripped.elf

# Chapter 133a /bin/tar: ustar archive reader (list + extract).
# Read-only -- archives are produced at build time by
# scripts/mktar.py; in-guest we only ever extract.
TAR_OBJS := $(BUILD)/userspace/crt/crt0.o \
            $(BUILD)/userspace/tar/tar.o
TAR_ELF  := $(BUILD)/userspace/tar/tar.elf
TAR_STRIPPED := $(BUILD)/userspace/tar/tar.stripped.elf

# chapter-132h bf: brainfuck interpreter, the "medium real program"
# smoke for /bin/gcc.  Same crt0 + linker_user.ld + libosdevc.a as
# every other userspace binary.  The source is also shipped on the
# disk image (bf.c) so the in-guest /bin/gcc can rebuild it.
BF_OBJS := $(BUILD)/userspace/crt/crt0.o \
           $(BUILD)/userspace/bf/bf.o
BF_ELF  := $(BUILD)/userspace/bf/bf.elf
BF_STRIPPED := $(BUILD)/userspace/bf/bf.stripped.elf

# milestone-29 sleep: SYS_SLEEP_MS demo + scriptable pause.
SLEEP_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/sleep/sleep.o
SLEEP_ELF  := $(BUILD)/userspace/sleep/sleep.elf
SLEEP_STRIPPED := $(BUILD)/userspace/sleep/sleep.stripped.elf

# chapter-82 sync: SYS_FSYNC demo + scriptable durability barrier.
SYNC_OBJS := $(BUILD)/userspace/crt/crt0.o \
             $(BUILD)/userspace/sync/sync.o
SYNC_ELF  := $(BUILD)/userspace/sync/sync.elf
SYNC_STRIPPED := $(BUILD)/userspace/sync/sync.stripped.elf

# milestone-30 pipes: self-test for SYS_PIPE / SYS_DUP2 / pipe_*.
PIPETEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/pipetest/pipetest.o
PIPETEST_ELF  := $(BUILD)/userspace/pipetest/pipetest.elf
PIPETEST_STRIPPED := $(BUILD)/userspace/pipetest/pipetest.stripped.elf

# milestone-65 fork + exec: self-test for SYS_FORK / SYS_EXEC.  Forks
# three times (pure fork, fork+exec /bin/hello, heap-copy verification)
# and prints `[forktest] all checks passed` on success.
FORKTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/forktest/forktest.o
FORKTEST_ELF  := $(BUILD)/userspace/forktest/forktest.elf
FORKTEST_STRIPPED := $(BUILD)/userspace/forktest/forktest.stripped.elf

# chapter-77 catchable signals: self-test for SYS_SIGACTION /
# SYS_SIGRETURN (catch SIGTERM, SIG_IGN, fork+cross-pid signal).
SIGTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/sigtest/sigtest.o
SIGTEST_ELF  := $(BUILD)/userspace/sigtest/sigtest.elf
SIGTEST_STRIPPED := $(BUILD)/userspace/sigtest/sigtest.stripped.elf

# chapter-78 SIGCHLD + waitpid: self-test for SYS_WAITPID.
# Five checks (handler fires on exit, waitpid by pid, WNOHANG
# poll, SIGCHLD default = ignore, legacy wait() compat).
CHLDTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/chldtest/chldtest.o
CHLDTEST_ELF  := $(BUILD)/userspace/chldtest/chldtest.elf
CHLDTEST_STRIPPED := $(BUILD)/userspace/chldtest/chldtest.stripped.elf

# chapter-75 copy-on-write fork: self-test for the lazy clone
# path.  Verifies that fork()'d children share heap + stack pages
# with the parent until one side writes (then they diverge), and
# that a multi-megabyte fork is fast (proxy: completes inside the
# regression timeout).
COWTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/cowtest/cowtest.o
COWTEST_ELF  := $(BUILD)/userspace/cowtest/cowtest.elf
COWTEST_STRIPPED := $(BUILD)/userspace/cowtest/cowtest.stripped.elf

# chapter 108c mixtest: asserts the one-window-one-draw-path
# contract.  A window that installed gui_window_fb must refuse
# gui_fill_rect / gui_draw_text / gui_present with -EBUSY; an
# unmapped window must still accept them (legacy notify path).
MIXTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/mixtest/mixtest.o
MIXTEST_ELF  := $(BUILD)/userspace/mixtest/mixtest.elf
MIXTEST_STRIPPED := $(BUILD)/userspace/mixtest/mixtest.stripped.elf

# milestone-56 httpget: TCP client over the M55 stack via the new
# socket syscall surface.  Useful end-to-end test that the
# kernel-side fd path correctly demuxes onto tcp_send/tcp_recv.
HTTPGET_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/httpget/httpget.o
HTTPGET_ELF  := $(BUILD)/userspace/httpget/httpget.elf
HTTPGET_STRIPPED := $(BUILD)/userspace/httpget/httpget.stripped.elf

# chapter-110 cookies: user-facing inspector for the /data/cookies
# jar that browser + httpget share.  Pure text -- equivalent to
# `cat` over the same files, plus a `clear` subcommand and an
# expired-cookie annotation.
COOKIES_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/cookies/cookies.o
COOKIES_ELF  := $(BUILD)/userspace/cookies/cookies.elf
COOKIES_STRIPPED := $(BUILD)/userspace/cookies/cookies.stripped.elf

# chapter-104 echod: TCP echo daemon, the server-side counterpart
# of httpget.  Exercises the new SYS_SOCKET_LISTEN / SYS_SOCKET_ACCEPT
# syscall surface end-to-end (listen fd, blocking accept, per-peer
# echo loop).  Single-connection-at-a-time on purpose -- we don't
# have non-blocking accept yet.
ECHOD_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/echod/echod.o
ECHOD_ELF  := $(BUILD)/userspace/echod/echod.elf
ECHOD_STRIPPED := $(BUILD)/userspace/echod/echod.stripped.elf

# chapter-114 libfs: shared user-space helper that turns a daemon
# into a 9P-shaped filesystem server.  Linked into every user-fs
# daemon (echofs, eventually clipboardd/procd).  Header at
# userspace/libfs/userfs.h.
LIBFS_OBJ := $(BUILD)/userspace/libfs/userfs.o

# chapter-114 echofs: smoke-test daemon that mounts /echo/ and
# serves three files (hello, buf, echo).  Smallest possible user
# of libfs; exercised by scripts/test_userfs_echo.py.
ECHOFS_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(LIBFS_OBJ) \
               $(BUILD)/userspace/echofs/echofs.o
ECHOFS_ELF  := $(BUILD)/userspace/echofs/echofs.elf
ECHOFS_STRIPPED := $(BUILD)/userspace/echofs/echofs.stripped.elf

# chapter-114f hangfs: deliberately-broken daemon that mounts
# /hang/ and then sleeps forever without ever servicing its
# request pipe.  Used by scripts/test_userfs_timeout.py to
# prove the kernel's 5 s per-request deadline fires and the
# client sees -ETIMEDOUT_VFS (-110) instead of blocking
# forever.  Does NOT link libfs: the whole point is to skip
# the serve loop.
HANGFS_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/hangfs/hangfs.o
HANGFS_ELF  := $(BUILD)/userspace/hangfs/hangfs.elf
HANGFS_STRIPPED := $(BUILD)/userspace/hangfs/hangfs.stripped.elf

# chapter-105 httpd: static-file HTTP/1.0 server, builds on the
# ch104 accept syscall.  Serves arbitrary VFS paths (GET /mnt/foo
# opens kernel path /mnt/foo) with Content-Type sniffing and
# Connection: close framing.
HTTPD_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/httpd/httpd.o
HTTPD_ELF  := $(BUILD)/userspace/httpd/httpd.elf
HTTPD_STRIPPED := $(BUILD)/userspace/httpd/httpd.stripped.elf

# chapter-106 looptest: forks into a TCP echo server (parent) and
# a client that dials 127.0.0.1 (child).  Demonstrates the new
# TCP loopback short-circuit -- without ch106 the child's SYN
# would die in SLIRP, with ch106 the handshake completes inside
# the guest kernel.
LOOPTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/looptest/looptest.o
LOOPTEST_ELF  := $(BUILD)/userspace/looptest/looptest.elf
LOOPTEST_STRIPPED := $(BUILD)/userspace/looptest/looptest.stripped.elf

# chapter-107 srvtest: forks into a /srv/echotest service (parent)
# and an IPC client (child) that round-trips one message via the
# named-IPC bus.  Smoke test for SYS_SRV_BIND/ACCEPT/CONNECT
# plus the FD_SRV_CONN read/write framing.
SRVTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/srvtest/srvtest.o
SRVTEST_ELF  := $(BUILD)/userspace/srvtest/srvtest.elf
SRVTEST_STRIPPED := $(BUILD)/userspace/srvtest/srvtest.stripped.elf

# chapter-114 clipboardd: the system clipboard, ported from the
# chapter-107 IPC bus to a chapter-114 userfs mount.  Now serves
# /clipboard with a single file /clipboard/text -- write to
# replace the payload, read to fetch it.  `cat`, `echo > ...`,
# and the GUI Ctrl-C/V keystrokes all hit the same code path.
# init's supervisor restarts it if it dies.
CLIPBOARDD_OBJS := $(BUILD)/userspace/crt/crt0.o \
                   $(LIBFS_OBJ) \
                   $(BUILD)/userspace/clipboardd/clipboardd.o
CLIPBOARDD_ELF  := $(BUILD)/userspace/clipboardd/clipboardd.elf
CLIPBOARDD_STRIPPED := $(BUILD)/userspace/clipboardd/clipboardd.stripped.elf

# chapter-114e procd: the /proc filesystem, evicted from the
# kernel (chapter-99 kernel/core/procfs.c is gone) and ported
# to a userfs daemon.  Same files (uptime / meminfo / cpuinfo /
# sched / <pid>/{status,cmdline,trace}) served from user space
# via SYS_KSTAT / SYS_THREAD_SNAPSHOT / SYS_STRACE_RENDER.
# init's supervisor restarts it if it dies.
PROCD_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(LIBFS_OBJ) \
              $(BUILD)/userspace/procd/procd.o
PROCD_ELF  := $(BUILD)/userspace/procd/procd.elf
PROCD_STRIPPED := $(BUILD)/userspace/procd/procd.stripped.elf

# chapter-108b fontd: the TrueType rasteriser, evicted from the
# kernel and turned into a long-running userspace daemon bound
# to /srv/font (chapter-107 IPC bus).  init's supervisor
# restarts it on crash.  The kernel-side WM reaches for it via
# kernel/core/wm_font.c when wm_draw_text needs a glyph; if the
# daemon is briefly unreachable (boot window, respawn) the WM
# falls back to the always-available bitmap font.
#
# DejaVuSans.ttf is embedded into fontd's ELF (no longer into
# the kernel) via the same objcopy-binary trick used for the
# ramfs payload.  The wrapper object exposes
# _binary_DejaVuSans_ttf_start / _end which userspace/fontd/ttf.c
# walks at startup.
FONTD_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/fontd/fontd.o \
              $(BUILD)/userspace/fontd/ttf.o \
              $(BUILD)/userspace/fontd/DejaVuSans.ttf.o
FONTD_ELF  := $(BUILD)/userspace/fontd/fontd.elf
FONTD_STRIPPED := $(BUILD)/userspace/fontd/fontd.stripped.elf

# chapter-108d wsd: the window-server daemon.  Phase A just
# claims the framebuffer via SYS_FB_MAP_SCANOUT and idles; the
# kernel WM still composes.  Subsequent phases replace
# kernel/core/wm.c with logic that lives in this binary.
# Bound to /srv/wm (chapter-107 IPC) in Phase B.
#
# chapter 108e -- wsd paints title bars and the cursor sprite
# using libgui/draw.h (draw_text -> fontd, draw_fill_rect,
# draw_blit_bgra), so DRAW_OBJ has to come along.  draw.o is
# declared later in this Makefile (WMCLIENT_OBJ block) but
# the := expansion of $(BUILD)/userspace/libgui/draw.o is a
# literal path -- works fine, no forward-reference issue.
WSD_OBJS := $(BUILD)/userspace/crt/crt0.o \
            $(BUILD)/userspace/wsd/wsd.o \
            $(BUILD)/userspace/libgui/draw.o
WSD_ELF  := $(BUILD)/userspace/wsd/wsd.elf
WSD_STRIPPED := $(BUILD)/userspace/wsd/wsd.stripped.elf

# chapter-108d Phase B smoke client.  One-shot CLI that
# connects to /srv/wm, exercises WM_HELLO + WM_LIST, and
# prints PASS/FAIL.  Driven by scripts/test_wsd_hello.py.
WMTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/wmtest/wmtest.o
WMTEST_ELF  := $(BUILD)/userspace/wmtest/wmtest.elf
WMTEST_STRIPPED := $(BUILD)/userspace/wmtest/wmtest.stripped.elf

# chapter-108d wmclient /srv/wm exhibit app.  First user of
# libgui/wmclient.h.  One-shot: HELLO, CREATE, MAP_FB paint,
# MOVE, DAMAGE, DESTROY, exit.  Driven by
# scripts/test_hellowsd.py.
#
# WMCLIENT_OBJ + DRAW_OBJ are hoisted to the top of the libgui
# group so that every later *_OBJS line (HELLOGUI, BROWSER,
# NOTEPAD, ...) can reference them under := immediate-expansion.
# Pattern-rule ordering trap recorded in
# /memories/repo/makefile-pattern-rule-ordering.md.
WMCLIENT_OBJ := $(BUILD)/userspace/libgui/wmclient.o
DRAW_OBJ     := $(BUILD)/userspace/libgui/draw.o
HELLOWSD_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/hellowsd/hellowsd.o \
                 $(WMCLIENT_OBJ)
HELLOWSD_ELF  := $(BUILD)/userspace/hellowsd/hellowsd.elf
HELLOWSD_STRIPPED := $(BUILD)/userspace/hellowsd/hellowsd.stripped.elf

# chapter-108 clip CLI: REMOVED in chapter 114.  The clipboard
# is now a userfs mount, so `echo X > /clipboard/text` and
# `cat /clipboard/text` from the shell do the same job that
# /bin/clip used to do.  See userspace/clip.deleted/ in git
# history if the old protocol-poking implementation is ever
# wanted as reference material.

# chapter-106b proxytest: orchestrates the in-guest proxy chain.
# Spawns /bin/httpd 8080 --once, sleeps briefly, then spawns
# /bin/browser <url>.  The browser's default BR_DEFAULT_PROXY
# now points at 127.0.0.1:8080, so a single `proxytest` invocation
# at the shell drives the whole chapter-106a+b flow.
PROXYTEST_OBJS := $(BUILD)/userspace/crt/crt0.o \
                  $(BUILD)/userspace/proxytest/proxytest.o
PROXYTEST_ELF  := $(BUILD)/userspace/proxytest/proxytest.elf
PROXYTEST_STRIPPED := $(BUILD)/userspace/proxytest/proxytest.stripped.elf

# milestone-59 htmltok: HTML5 tokenizer driver.  Reads a file (or
# /mnt/test.html by default), runs userspace/libc/html.h over it,
# and prints one [TYPE] line per token for the test harness to grep.
HTMLTOK_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/htmltok/htmltok.o
HTMLTOK_ELF  := $(BUILD)/userspace/htmltok/htmltok.elf
HTMLTOK_STRIPPED := $(BUILD)/userspace/htmltok/htmltok.stripped.elf

# milestone-60 htmldom: HTML DOM-builder driver.  Reads a file (or
# /mnt/test.html by default), runs userspace/libc/html.h followed
# by userspace/libc/dom.h over it, and walks the resulting tree
# depth-first.  One [TYPE] line per node, indented two spaces per
# level, for the test harness to grep.
HTMLDOM_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/htmldom/htmldom.o
HTMLDOM_ELF  := $(BUILD)/userspace/htmldom/htmldom.elf
HTMLDOM_STRIPPED := $(BUILD)/userspace/htmldom/htmldom.stripped.elf

# milestone-61 cssparse: CSS parser + selector matcher.  Reads a
# CSS file (default /mnt/test.css) and prints one indented block
# per parsed rule.  If a second arg is given (an HTML file path) it
# also builds a DOM and prints which rules match each element.
CSSPARSE_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/cssparse/cssparse.o
CSSPARSE_ELF  := $(BUILD)/userspace/cssparse/cssparse.elf
CSSPARSE_STRIPPED := $(BUILD)/userspace/cssparse/cssparse.stripped.elf

# milestone-62 layout: full layout pipeline driver.  Reads an HTML
# file (default /mnt/test_layout.html) at a given viewport width
# (default 800), runs html.h -> dom.h -> css.h cascade -> layout.h
# block + inline layout, and prints the box tree plus paint command
# stream in document/painting order.
LAYOUT_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/layout/layout.o
LAYOUT_ELF  := $(BUILD)/userspace/layout/layout.elf
LAYOUT_STRIPPED := $(BUILD)/userspace/layout/layout.stripped.elf

# milestone-63 browser: full browser pipeline.  Fetches an HTML
# document (file path or http:// URL), runs the M59-M62 stack, and
# either dumps the paint stream (--paint) or renders it onto an
# 8x16-px-cell character grid for stdout (default = plain ASCII
# with box-drawing borders; --ansi adds 24-bit colour + underline
# escapes).  https:// is rejected (no TLS yet).
# Chapter 108d: --gui mode now uses the wsd-backed
# wmclient + draw.o (each frame composes into the mapped FB and
# pushes one wm_window_dirty) instead of the chapter-94 kernel-WM
# syscalls (gui_create_window / gui_fill_rect / gui_present /
# gui_flush).  Non-GUI modes (--ansi, --paint, --bench-resize,
# headless plain) don't touch wmclient.o but link it anyway --
# the dead-code is < 4 KiB and lets the same browser binary do
# both paths without per-mode link variants.
#
# Chapter 112d: the browser now speaks TLS natively via the same
# BearSSL static library that backs tlstest + httpsd.  We pull in
# TLS_SOCKET_OBJ (the BearSSL engine + br_sslio_ wrapper),
# TEST_CHAIN_OBJ (the sample CN=localhost RSA-2048 chain whose
# intermediate doubles as our hardcoded trust anchor until 112e
# loads a real store), and CSTRING_OBJ (the freestanding mem*
# bridge libbearssl.a needs).  The full set is link-grouped
# against $(BEARSSL_LIB) below so members can resolve each other
# in one pass.
BROWSER_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/browser/browser.o \
                $(DRAW_OBJ) \
                $(WMCLIENT_OBJ) \
                $(TLS_SOCKET_OBJ) \
                $(TEST_CHAIN_OBJ) \
                $(CSTRING_OBJ)
BROWSER_ELF  := $(BUILD)/userspace/browser/browser.elf
BROWSER_STRIPPED := $(BUILD)/userspace/browser/browser.stripped.elf

# milestone-40 GUI demo: opens a window, paints a gradient + text,
# accepts keystrokes routed through the in-kernel WM.
# Chapter 108c: now links against libgui/draw.o for the in-process
# software rasteriser, so the cold paint + per-keystroke repaint
# both go through direct pixel writes + one gui_window_dirty
# instead of the legacy gui_present / gui_draw_text syscalls.
# DRAW_OBJ is defined at the top of the libgui group (next to
# WMCLIENT_OBJ) so every := user gets the right path.  Pattern-
# rule ordering trap recorded in
# /memories/repo/makefile-pattern-rule-ordering.md.
HELLOGUI_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/hellogui/hellogui.o \
                 $(DRAW_OBJ)
HELLOGUI_ELF  := $(BUILD)/userspace/hellogui/hellogui.elf
HELLOGUI_STRIPPED := $(BUILD)/userspace/hellogui/hellogui.stripped.elf

# chapter-108a GUI demo: opens a window, asks the kernel for direct
# access to its pixel buffer via SYS_GUI_MAP_WINDOW, paints a
# horizontal red->blue gradient by writing the BGRA bytes
# directly (no per-primitive syscalls), then DAMAGEs the whole
# window once.  Contrast with HELLOGUI, which uses one syscall
# per primitive.
PIXAPP_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/pixapp/pixapp.o \
               $(WMCLIENT_OBJ)
PIXAPP_ELF  := $(BUILD)/userspace/pixapp/pixapp.elf
PIXAPP_STRIPPED := $(BUILD)/userspace/pixapp/pixapp.stripped.elf

# milestone-41 mouse demo: paints colour squares wherever the user
# clicks, with right-click cycling palette + close-button support.
# Chapter 108c: links against libgui/draw.o so each brush stamp is
# an in-process pixel write + one gui_window_dirty over the stamp
# rect, instead of one gui_fill_rect + gui_flush syscall pair per
# stamp on the drag hot path.
# Chapter 108d: also pulls in wmclient.o so paint can
# create its window through wsd and pull mouse events via
# wm_poll_event (instead of the kernel-WM gui_* syscalls).
PAINT_OBJS := $(BUILD)/userspace/crt/crt0.o \
              $(BUILD)/userspace/paint/paint.o \
              $(DRAW_OBJ) \
              $(WMCLIENT_OBJ)
PAINT_ELF  := $(BUILD)/userspace/paint/paint.elf
PAINT_STRIPPED := $(BUILD)/userspace/paint/paint.stripped.elf

# milestone-42 GUI terminal: spawn_pipe() child binaries and render
# their stdout into the window scrollback.
GUI_TERM_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/gui_term/gui_term.o \
                 $(DRAW_OBJ) \
                 $(WMCLIENT_OBJ)
GUI_TERM_ELF  := $(BUILD)/userspace/gui_term/gui_term.elf
GUI_TERM_STRIPPED := $(BUILD)/userspace/gui_term/gui_term.stripped.elf

# milestone-43 GUI text editor: open / edit / save text files in a
# real window.  Uses Ctrl-S to save, Ctrl-Q / ESC to quit.
#
# Chapter 84: notepad is now the first multi-object userspace app
# in the tree — the Save As dialog lives in userspace/libgui/ as
# a separately-compiled translation unit (`save_dialog.o`) so it
# can be reused by future GUI apps without copy-pasting.  The
# build system has supported multi-object apps from day one (see
# the `*_OBJS` lists), but this is the first time we use it.
#
# Chapter 108c adds draw.o — the software rasteriser + fontd
# client every chapter-108c-mapped app links against.  Chapter
# Chapter 108d ported notepad off the kernel-WM syscalls onto
# the wsd-backed libgui primitives (DRAW_OBJ + WMCLIENT_OBJ);
# save_dialog.o also moved to draw/wmclient so it can be linked
# in alongside.  draw.o has no dependency on save_dialog.o, so
# the LIBGUI_OBJS group is just the save_dialog widget; users
# add DRAW_OBJ + WMCLIENT_OBJ themselves.
LIBGUI_OBJS  := $(BUILD)/userspace/libgui/save_dialog.o
NOTEPAD_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/notepad/notepad.o \
                $(LIBGUI_OBJS) \
                $(DRAW_OBJ) \
                $(WMCLIENT_OBJ)
NOTEPAD_ELF  := $(BUILD)/userspace/notepad/notepad.elf
NOTEPAD_STRIPPED := $(BUILD)/userspace/notepad/notepad.stripped.elf

# milestone-44 GUI app launcher: small floating window with three
# buttons that spawn() the matching binary on click.
# Chapter 108c: linked against libgui/draw.o so the per-hover
# repaint is a handful of in-process pixel writes + one
# gui_window_dirty, rather than eight syscalls per button.
LAUNCHER_OBJS := $(BUILD)/userspace/crt/crt0.o \
                 $(BUILD)/userspace/launcher/launcher.o \
                 $(DRAW_OBJ) \
                 $(WMCLIENT_OBJ)
LAUNCHER_ELF  := $(BUILD)/userspace/launcher/launcher.elf
LAUNCHER_STRIPPED := $(BUILD)/userspace/launcher/launcher.stripped.elf

# milestone-47 desktop taskbar: borderless always-on-top strip pinned
# to the bottom of the framebuffer; one cell per non-pinned window.
# Chapter 108c: linked against libgui/draw.o so the per-second
# clock tick + per-event cell repaint are pure in-process pixel
# writes plus a tight gui_window_dirty over the rect that
# actually changed.
TASKBAR_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/taskbar/taskbar.o \
                $(DRAW_OBJ) \
                $(WMCLIENT_OBJ)
TASKBAR_ELF  := $(BUILD)/userspace/taskbar/taskbar.elf
TASKBAR_STRIPPED := $(BUILD)/userspace/taskbar/taskbar.stripped.elf

# milestone-49 toast notifications.  /bin/notify pops up a brief
# borderless always-on-top window with a message and auto-dismisses.
# Chapter 108d: ported to /srv/wm via wmclient; the
# kernel-WM gui_fill_rect / gui_draw_text syscalls are stubs now,
# so notify rasterises its own pixels via libgui/draw.[hc] into
# the wsd-mapped framebuffer and DAMAGEs through WM_WIN_DAMAGE.
NOTIFY_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/notify/notify.o \
               $(DRAW_OBJ) \
               $(WMCLIENT_OBJ)
NOTIFY_ELF  := $(BUILD)/userspace/notify/notify.elf
NOTIFY_STRIPPED := $(BUILD)/userspace/notify/notify.stripped.elf

# milestone-50 desktop environment.  /bin/desktop owns the
# wallpaper: it reads /wallpaper.bgra from disk, creates a screen-
# sized PIN_TO_BOTTOM window, and blits the pixels in via the
# chapter-108c draw_blit_bgra helper.  Pre-108c this was a
# gui_present syscall — a memcpy of the whole 1920x1080x4 = 8 MB
# wallpaper across the EL0/EL1 boundary.  Now the kernel hands
# us the mapped framebuffer once and we copy in directly.
DESKTOP_OBJS := $(BUILD)/userspace/crt/crt0.o \
                $(BUILD)/userspace/desktop/desktop.o \
                $(DRAW_OBJ) \
                $(WMCLIENT_OBJ)
DESKTOP_ELF  := $(BUILD)/userspace/desktop/desktop.elf
DESKTOP_STRIPPED := $(BUILD)/userspace/desktop/desktop.stripped.elf

$(BUILD)/userspace/%.o: userspace/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/userspace/%.o: userspace/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# Vendored BearSSL: separate pattern rule with BEARSSL_CFLAGS
# (USER_CFLAGS has -Werror which we don't want for third-party
# code).  Output mirrors the src layout under build/vendor/bearssl/.
$(BUILD)/vendor/bearssl/%.o: vendor/bearssl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

# Archive all BearSSL objects into libbearssl.a.  Using `r` (replace)
# instead of `c` (create) so partial rebuilds don't drop members;
# `s` writes an in-archive index so the linker can pull individual
# .o files lazily without scanning the whole archive.
$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(CROSS)ar rcs $@ $(BEARSSL_OBJS)

# tlstest.o needs both BearSSL header paths (the umbrella bearssl.h
# pulls in bearssl_hash.h which #includes <string.h>) on top of
# USER_CFLAGS.  Generic userspace pattern rule above doesn't supply
# these, so override here.
$(BUILD)/userspace/tlstest/tlstest.o: userspace/tlstest/tlstest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I vendor/bearssl-shim -I vendor/bearssl/inc -c $< -o $@

# Chapter 112b: tls_socket.c is libc-adjacent code that uses
# BearSSL.  Same header-path override as tlstest.o.
$(BUILD)/userspace/libc/tls_socket.o: userspace/libc/tls_socket.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I vendor/bearssl-shim -I vendor/bearssl/inc -c $< -o $@

# Chapter 112d: browser.c now includes bearssl.h so it can read
# the engine's last-error code after a BR_SSLIO clean close and
# tell EOF apart from a real TLS error.  Same header-path
# override as tls_socket.o.  This pattern-specific rule must
# live AFTER the generic $(BUILD)/userspace/%.o rule (above)
# for Make's "most specific wins" tiebreaker to pick it up --
# trap recorded in /memories/repo/makefile-pattern-rule-ordering.md.
$(BUILD)/userspace/browser/browser.o: userspace/browser/browser.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I vendor/bearssl-shim -I vendor/bearssl/inc -c $< -o $@

# Chapter 112b: httpsd.c uses BearSSL too.
$(BUILD)/userspace/httpsd/httpsd.o: userspace/httpsd/httpsd.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I vendor/bearssl-shim -I vendor/bearssl/inc -c $< -o $@

# Chapter 112b: test_chain.c re-exports the BearSSL sample chain.
# Needs vendor/bearssl/samples/ on the include path so the sample
# headers (chain-rsa.h, key-rsa.h) resolve.  The sample headers
# come from BearSSL 0.6 (MIT) and are unmodified.
$(BUILD)/vendor/testcerts/test_chain.o: vendor/testcerts/test_chain.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I vendor/bearssl-shim -I vendor/bearssl/inc \
	    -I vendor/bearssl/samples -c $< -o $@

# Chapter 112e: same shape for the EC sample chain (chain-ec.h /
# key-ec.h).  Separate TU because the upstream headers declare
# their CERT0/CERT1/CHAIN as `static const` and would collide if
# included in the same translation unit as the RSA chain.
$(BUILD)/vendor/testcerts/test_chain_ec.o: vendor/testcerts/test_chain_ec.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I vendor/bearssl-shim -I vendor/bearssl/inc \
	    -I vendor/bearssl/samples -c $< -o $@

# Chapter 130a — DoomGeneric vendor sources.
#
# Pattern rule mirrors the BearSSL pattern: separate from the
# generic userspace rule because (a) third-party code can't be
# held to -Werror, (b) DG expects -DNORMALUNIX to gate its
# POSIX-y system calls, and (c) it #includes its own headers
# with bare names like "doomgeneric.h" so the src dir must be
# on the include path.
#
# Warnings disabled: this is a 25-year-old codebase originally
# written for K&R/early-C89.  The disabled set covers the
# warnings every recent gcc/clang emits for the upstream tree.
# Keep our own port (`userspace/doom/*.c`) on the strict
# USER_CFLAGS rule so any regression in OUR code surfaces.
DOOM_VENDOR_CFLAGS := -ffreestanding -nostdlib -nostartfiles \
                      -mcpu=cortex-a72 \
                      -fno-stack-protector -fno-pie -fno-pic \
                      -fno-asynchronous-unwind-tables \
                      -Wall -Os -g \
                      -Wno-unused-parameter -Wno-unused-variable \
                      -Wno-unused-function -Wno-unused-but-set-variable \
                      -Wno-sign-compare -Wno-missing-braces \
                      -Wno-format -Wno-format-security \
                      -Wno-implicit-fallthrough -Wno-misleading-indentation \
                      -Wno-array-bounds -Wno-stringop-overflow \
                      -Wno-stringop-truncation -Wno-discarded-qualifiers \
                      -Wno-pointer-sign -Wno-int-conversion \
                      -Wno-incompatible-pointer-types \
                      -DNORMALUNIX \
                      -DOSDEV_LIBC_NO_GLOBAL_DEFS \
                      -I vendor/doomgeneric/src -I userspace/libc \
                      -MMD -MP

$(BUILD)/vendor/doomgeneric/%.o: vendor/doomgeneric/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(DOOM_VENDOR_CFLAGS) -c $< -o $@

# Our own Doom shim — held to strict USER_CFLAGS but needs
# `-I userspace/libc` for vendor angle-bracket includes
# (doomgeneric.h pulls <stdint.h>/<stdlib.h>).  Generic
# userspace/%.o rule doesn't add it.
$(BUILD)/userspace/doom/doomgeneric_osdev.o: userspace/doom/doomgeneric_osdev.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I userspace/libc -c $< -o $@

$(HELLO_ELF): $(HELLO_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HELLO_OBJS)

$(CAT_ELF): $(CAT_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(CAT_OBJS)

$(INIT_ELF): $(INIT_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(INIT_OBJS)

$(SH_ELF): $(SH_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SH_OBJS)

$(BADPOKE_ELF): $(BADPOKE_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(BADPOKE_OBJS)

$(BADPTR_ELF): $(BADPTR_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(BADPTR_OBJS)

$(STACKBOMB_ELF): $(STACKBOMB_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(STACKBOMB_OBJS)

$(HEAPTEST_ELF): $(HEAPTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HEAPTEST_OBJS)

$(MMAPTEST_ELF): $(MMAPTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(MMAPTEST_OBJS)

$(THREADTEST_ELF): $(THREADTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(THREADTEST_OBJS)

$(THREADTEST2_ELF): $(THREADTEST2_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(THREADTEST2_OBJS)

$(THREADTEST3_ELF): $(THREADTEST3_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(THREADTEST3_OBJS)

$(ECHO_ELF): $(ECHO_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ECHO_OBJS)

$(PRINTFTEST_ELF): $(PRINTFTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PRINTFTEST_OBJS)

$(SETJMPTEST_ELF): $(SETJMPTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SETJMPTEST_OBJS)

$(SIGTEST2_ELF): $(SIGTEST2_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SIGTEST2_OBJS)

$(ABORTTEST_ELF): $(ABORTTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ABORTTEST_OBJS)

$(STRTEST_ELF): $(STRTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(STRTEST_OBJS)

$(ASSERTFAIL_ELF): $(ASSERTFAIL_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ASSERTFAIL_OBJS)

$(TIMETEST_ELF): $(TIMETEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(TIMETEST_OBJS)

$(STDLIBTEST_ELF): $(STDLIBTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(STDLIBTEST_OBJS)

$(PRINTFTEST2_ELF): $(PRINTFTEST2_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PRINTFTEST2_OBJS)

$(FPTEST_ELF): $(FPTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(FPTEST_OBJS)

# Chapter 130a — Doom link rule.  Wrap the objects in
# --start-group / --end-group so the ld can resolve the
# circular dependencies that DoomGeneric's modules have
# with each other (e.g. p_setup → r_main → g_game → p_setup).
# 83 vendor .o + our shim = ~3 MB of unstripped ELF.
$(DOOM_ELF): $(DOOM_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ --start-group $(DOOM_OBJS) --end-group

# Chapter 133e — Doom runtime archive shipped on the OSFS image.
# Bundles the 5 non-vendor objects (crt0, doomgeneric_osdev shim,
# setjmp, cstring, wmclient) into a single static .a so the
# in-guest link step is one /bin/ld invocation that takes the 80
# vendor .o files (built by /bin/make -f /bin/doom_full.mk) plus
# this one archive.  See assets/osfs/doom_link.mk + doom_link.args.
DOOMRT_OBJS := $(BUILD)/userspace/crt/crt0.o \
               $(BUILD)/userspace/doom/doomgeneric_osdev.o \
               $(BUILD)/userspace/libc/setjmp.o \
               $(CSTRING_OBJ) \
               $(WMCLIENT_OBJ)
DOOMRT_LIB  := $(BUILD)/userspace/doom/libdoomrt.a

$(DOOMRT_LIB): $(DOOMRT_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(CROSS)ar rcs $@ $(DOOMRT_OBJS)

$(ERRNOTEST_ELF): $(ERRNOTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ERRNOTEST_OBJS)

$(STDIOTEST_ELF): $(STDIOTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(STDIOTEST_OBJS)

$(ENVTEST_ELF): $(ENVTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ENVTEST_OBJS)

$(STATTEST_ELF): $(STATTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(STATTEST_OBJS)

$(AS_ELF): $(AS_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(AS_OBJS)

$(LD_ELF): $(LD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(LD_OBJS)

$(AR_ELF): $(AR_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(AR_OBJS)

$(ATEXITTEST_ELF): $(ATEXITTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ATEXITTEST_OBJS)

$(CC_ELF): $(CC_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(CC_OBJS)

$(GCCW_ELF): $(GCCW_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(GCCW_OBJS)

$(MAKE_ELF): $(MAKE_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(MAKE_OBJS)

$(LS_ELF): $(LS_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(LS_OBJS)

$(UPTIME_ELF): $(UPTIME_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(UPTIME_OBJS)

$(PS_ELF): $(PS_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PS_OBJS)

$(TOP_ELF): $(TOP_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(TOP_OBJS)

$(STRACE_ELF): $(STRACE_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(STRACE_OBJS)

$(DATE_ELF): $(DATE_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(DATE_OBJS)

$(BEEP_ELF): $(BEEP_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(BEEP_OBJS)

$(GETRAND_ELF): $(GETRAND_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(GETRAND_OBJS)

$(MOUNT_ELF): $(MOUNT_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(MOUNT_OBJS)

# chapter-112a tlstest: link order matters — the archive must come
# AFTER the .o files that reference it (TLSTEST_OBJS contains
# tlstest.o which calls br_sha256_*) so the linker can resolve
# forward references in one pass.  --start-group/--end-group lets
# libbearssl members re-resolve mem* etc. from cstring.o which is
# itself part of TLSTEST_OBJS.
$(TLSTEST_ELF): $(TLSTEST_OBJS) $(BEARSSL_LIB) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ \
	    --start-group $(TLSTEST_OBJS) $(BEARSSL_LIB) --end-group

# chapter-112b httpsd: same link discipline as tlstest -- libbearssl
# pulls members for both the server engine (br_ssl_server_*) and
# the RSA-PKCS1v1.5 + AES-GCM + SHA-256 transcript, all of which
# need to back-reference cstring.o's mem* implementations.
$(HTTPSD_ELF): $(HTTPSD_OBJS) $(BEARSSL_LIB) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ \
	    --start-group $(HTTPSD_OBJS) $(BEARSSL_LIB) --end-group

$(PNGDEC_ELF): $(PNGDEC_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PNGDEC_OBJS)

$(ENV_ELF): $(ENV_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ENV_OBJS)

$(GREP_ELF): $(GREP_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(GREP_OBJS)

$(WC_ELF): $(WC_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(WC_OBJS)

$(HEAD_ELF): $(HEAD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HEAD_OBJS)

$(TAIL_ELF): $(TAIL_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(TAIL_OBJS)

$(TAR_ELF): $(TAR_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(TAR_OBJS)

$(BF_ELF): $(BF_OBJS) userspace/linker_user.ld $(XGCC_SYS_LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(BF_OBJS) \
	    -L$(XGCC_SYSROOT_LIB) --start-group -losdevc --end-group

$(SLEEP_ELF): $(SLEEP_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SLEEP_OBJS)

$(SYNC_ELF): $(SYNC_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SYNC_OBJS)

$(PIPETEST_ELF): $(PIPETEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PIPETEST_OBJS)

$(FORKTEST_ELF): $(FORKTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(FORKTEST_OBJS)

$(SIGTEST_ELF): $(SIGTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SIGTEST_OBJS)

$(CHLDTEST_ELF): $(CHLDTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(CHLDTEST_OBJS)

$(COWTEST_ELF): $(COWTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(COWTEST_OBJS)

$(MIXTEST_ELF): $(MIXTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(MIXTEST_OBJS)

$(HTTPGET_ELF): $(HTTPGET_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HTTPGET_OBJS)

$(COOKIES_ELF): $(COOKIES_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(COOKIES_OBJS)

$(ECHOD_ELF): $(ECHOD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ECHOD_OBJS)

$(ECHOFS_ELF): $(ECHOFS_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ECHOFS_OBJS)

$(HANGFS_ELF): $(HANGFS_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HANGFS_OBJS)

$(HTTPD_ELF): $(HTTPD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HTTPD_OBJS)

$(LOOPTEST_ELF): $(LOOPTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(LOOPTEST_OBJS)

$(SRVTEST_ELF): $(SRVTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(SRVTEST_OBJS)

$(CLIPBOARDD_ELF): $(CLIPBOARDD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(CLIPBOARDD_OBJS)

$(PROCD_ELF): $(PROCD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PROCD_OBJS)

$(FONTD_ELF): $(FONTD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(FONTD_OBJS)

$(WSD_ELF): $(WSD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(WSD_OBJS)

$(WMTEST_ELF): $(WMTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(WMTEST_OBJS)

$(HELLOWSD_ELF): $(HELLOWSD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HELLOWSD_OBJS)

$(PROXYTEST_ELF): $(PROXYTEST_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PROXYTEST_OBJS)

$(HTMLTOK_ELF): $(HTMLTOK_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HTMLTOK_OBJS)

$(HTMLDOM_ELF): $(HTMLDOM_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HTMLDOM_OBJS)

$(CSSPARSE_ELF): $(CSSPARSE_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(CSSPARSE_OBJS)

$(LAYOUT_ELF): $(LAYOUT_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(LAYOUT_OBJS)

$(BROWSER_ELF): $(BROWSER_OBJS) $(BEARSSL_LIB) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ \
	    --start-group $(BROWSER_OBJS) $(BEARSSL_LIB) --end-group

$(HELLOGUI_ELF): $(HELLOGUI_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(HELLOGUI_OBJS)

$(PIXAPP_ELF): $(PIXAPP_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PIXAPP_OBJS)

$(PAINT_ELF): $(PAINT_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(PAINT_OBJS)

$(GUI_TERM_ELF): $(GUI_TERM_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(GUI_TERM_OBJS)

$(NOTEPAD_ELF): $(NOTEPAD_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(NOTEPAD_OBJS)

$(LAUNCHER_ELF): $(LAUNCHER_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(LAUNCHER_OBJS)

$(TASKBAR_ELF): $(TASKBAR_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(TASKBAR_OBJS)

$(NOTIFY_ELF): $(NOTIFY_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(NOTIFY_OBJS)

$(DESKTOP_ELF): $(DESKTOP_OBJS) userspace/linker_user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(DESKTOP_OBJS)

# Strip debug + symbol info before embedding to keep the kernel
# image small.  The ELF headers themselves stay intact — they are
# what the in-kernel loader walks.
$(HELLO_STRIPPED): $(HELLO_ELF)
	$(OBJCOPY) --strip-all $< $@

$(CAT_STRIPPED): $(CAT_ELF)
	$(OBJCOPY) --strip-all $< $@

$(INIT_STRIPPED): $(INIT_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SH_STRIPPED): $(SH_ELF)
	$(OBJCOPY) --strip-all $< $@

$(BADPOKE_STRIPPED): $(BADPOKE_ELF)
	$(OBJCOPY) --strip-all $< $@

$(BADPTR_STRIPPED): $(BADPTR_ELF)
	$(OBJCOPY) --strip-all $< $@

$(STACKBOMB_STRIPPED): $(STACKBOMB_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HEAPTEST_STRIPPED): $(HEAPTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(MMAPTEST_STRIPPED): $(MMAPTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(THREADTEST_STRIPPED): $(THREADTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(THREADTEST2_STRIPPED): $(THREADTEST2_ELF)
	$(OBJCOPY) --strip-all $< $@

$(THREADTEST3_STRIPPED): $(THREADTEST3_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ECHO_STRIPPED): $(ECHO_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PRINTFTEST_STRIPPED): $(PRINTFTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SETJMPTEST_STRIPPED): $(SETJMPTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SIGTEST2_STRIPPED): $(SIGTEST2_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ABORTTEST_STRIPPED): $(ABORTTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(STRTEST_STRIPPED): $(STRTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ASSERTFAIL_STRIPPED): $(ASSERTFAIL_ELF)
	$(OBJCOPY) --strip-all $< $@

$(TIMETEST_STRIPPED): $(TIMETEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(STDLIBTEST_STRIPPED): $(STDLIBTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PRINTFTEST2_STRIPPED): $(PRINTFTEST2_ELF)
	$(OBJCOPY) --strip-all $< $@

$(FPTEST_STRIPPED): $(FPTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(DOOM_STRIPPED): $(DOOM_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ERRNOTEST_STRIPPED): $(ERRNOTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(STDIOTEST_STRIPPED): $(STDIOTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ENVTEST_STRIPPED): $(ENVTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(STATTEST_STRIPPED): $(STATTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(AS_STRIPPED): $(AS_ELF)
	$(OBJCOPY) --strip-all $< $@

$(LD_STRIPPED): $(LD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(AR_STRIPPED): $(AR_ELF)
	$(OBJCOPY) --strip-all $< $@

# chapter-131f: cross-build GNU binutils gas + ld for our
# aarch64-osdev target via scripts/test_guest_ld.py.  The
# script wipes $(BINUTILS_GUEST_BUILD) on every invocation
# (idempotent rebuild from source), so the BINUTILS_AS_NEW
# rule's prerequisite list is the union of inputs the script
# actually depends on — the script itself, the libc bridge
# .c that gets compiled into the bridge .o, and cstring.o /
# crt0.o which are linked into both as-new and ld-new.  Any
# change to those re-runs the (slow) binutils build.  Both
# binaries fall out of the same script run; LD_NEW depends on
# AS_NEW to serialise (one script invocation produces both).
$(BINUTILS_AS_NEW): scripts/test_guest_ld.py \
                    userspace/libc/cstring.c \
                    userspace/libc/wchar.h \
                    $(CSTRING_OBJ) \
                    $(BUILD)/userspace/crt/crt0.o
	python3 scripts/test_guest_ld.py
	@test -f $@ || { echo "ERROR: test_guest_ld.py did not produce $@"; exit 1; }

$(BINUTILS_LD_NEW): $(BINUTILS_AS_NEW)
	@test -f $@ || { echo "ERROR: $@ missing after binutils build"; exit 1; }

$(BINUTILS_AS_STRIPPED): $(BINUTILS_AS_NEW)
	@mkdir -p $(dir $@)
	aarch64-elf-strip --strip-all -o $@ $<

$(BINUTILS_LD_STRIPPED): $(BINUTILS_LD_NEW)
	@mkdir -p $(dir $@)
	aarch64-elf-strip --strip-all -o $@ $<

$(ATEXITTEST_STRIPPED): $(ATEXITTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(CC_STRIPPED): $(CC_ELF)
	$(OBJCOPY) --strip-all $< $@

$(GCCW_STRIPPED): $(GCCW_ELF)
	$(OBJCOPY) --strip-all $< $@

# Strip xgcc and cc1 down by ~25 % each (debug info dominates).
# Both inputs are produced out-of-tree by
# scripts/test_guest_gcc.py.  The real build is wired in via the
# $(GCC_GUEST_STAMP) rule near the bottom of this Makefile, so
# `make build/disk.img` automatically triggers the cross-build
# when the binaries are missing.  The if-test below is a belt-
# and-braces guard: if the user explicitly bypasses (e.g. by
# touching the stamp), the strip step still won't crash.
$(XGCC_GUEST_STRIPPED): $(XGCC_GUEST_BIN)
	@mkdir -p $(dir $@)
	@if [ -f $(XGCC_GUEST_BIN) ]; then \
	    aarch64-elf-strip --strip-all -o $@ $(XGCC_GUEST_BIN); \
	else \
	    echo "xgcc-stub: $(XGCC_GUEST_BIN) missing -- shipping placeholder"; \
	    printf 'gcc not built; run scripts/test_guest_gcc.py\n' > $@; \
	fi

$(CC1_GUEST_STRIPPED): $(CC1_GUEST_BIN)
	@mkdir -p $(dir $@)
	@if [ -f $(CC1_GUEST_BIN) ]; then \
	    aarch64-elf-strip --strip-all -o $@ $(CC1_GUEST_BIN); \
	else \
	    echo "cc1-stub: $(CC1_GUEST_BIN) missing -- shipping placeholder"; \
	    printf 'cc1 not built; run scripts/test_guest_gcc.py\n' > $@; \
	fi

# (The $(XGCC_GUEST_BIN) / $(CC1_GUEST_BIN) targets themselves
# are defined further down in this Makefile, after the variables
# their build rule references are all in scope.)

$(MAKE_STRIPPED): $(MAKE_ELF)
	$(OBJCOPY) --strip-all $< $@

$(LS_STRIPPED): $(LS_ELF)
	$(OBJCOPY) --strip-all $< $@

$(UPTIME_STRIPPED): $(UPTIME_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PS_STRIPPED): $(PS_ELF)
	$(OBJCOPY) --strip-all $< $@

$(TOP_STRIPPED): $(TOP_ELF)
	$(OBJCOPY) --strip-all $< $@

$(STRACE_STRIPPED): $(STRACE_ELF)
	$(OBJCOPY) --strip-all $< $@

$(DATE_STRIPPED): $(DATE_ELF)
	$(OBJCOPY) --strip-all $< $@

$(BEEP_STRIPPED): $(BEEP_ELF)
	$(OBJCOPY) --strip-all $< $@

$(GETRAND_STRIPPED): $(GETRAND_ELF)
	$(OBJCOPY) --strip-all $< $@

$(MOUNT_STRIPPED): $(MOUNT_ELF)
	$(OBJCOPY) --strip-all $< $@

$(TLSTEST_STRIPPED): $(TLSTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HTTPSD_STRIPPED): $(HTTPSD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PNGDEC_STRIPPED): $(PNGDEC_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ENV_STRIPPED): $(ENV_ELF)
	$(OBJCOPY) --strip-all $< $@

$(GREP_STRIPPED): $(GREP_ELF)
	$(OBJCOPY) --strip-all $< $@

$(WC_STRIPPED): $(WC_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HEAD_STRIPPED): $(HEAD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(TAIL_STRIPPED): $(TAIL_ELF)
	$(OBJCOPY) --strip-all $< $@

$(TAR_STRIPPED): $(TAR_ELF)
	$(OBJCOPY) --strip-all $< $@

$(BF_STRIPPED): $(BF_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SLEEP_STRIPPED): $(SLEEP_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SYNC_STRIPPED): $(SYNC_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PIPETEST_STRIPPED): $(PIPETEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(FORKTEST_STRIPPED): $(FORKTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SIGTEST_STRIPPED): $(SIGTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(CHLDTEST_STRIPPED): $(CHLDTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(COWTEST_STRIPPED): $(COWTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(MIXTEST_STRIPPED): $(MIXTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HTTPGET_STRIPPED): $(HTTPGET_ELF)
	$(OBJCOPY) --strip-all $< $@

$(COOKIES_STRIPPED): $(COOKIES_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ECHOD_STRIPPED): $(ECHOD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(ECHOFS_STRIPPED): $(ECHOFS_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HANGFS_STRIPPED): $(HANGFS_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HTTPD_STRIPPED): $(HTTPD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(LOOPTEST_STRIPPED): $(LOOPTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(SRVTEST_STRIPPED): $(SRVTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(CLIPBOARDD_STRIPPED): $(CLIPBOARDD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PROCD_STRIPPED): $(PROCD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(FONTD_STRIPPED): $(FONTD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(WSD_STRIPPED): $(WSD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(WMTEST_STRIPPED): $(WMTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HELLOWSD_STRIPPED): $(HELLOWSD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PROXYTEST_STRIPPED): $(PROXYTEST_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HTMLTOK_STRIPPED): $(HTMLTOK_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HTMLDOM_STRIPPED): $(HTMLDOM_ELF)
	$(OBJCOPY) --strip-all $< $@

$(CSSPARSE_STRIPPED): $(CSSPARSE_ELF)
	$(OBJCOPY) --strip-all $< $@

$(LAYOUT_STRIPPED): $(LAYOUT_ELF)
	$(OBJCOPY) --strip-all $< $@

$(BROWSER_STRIPPED): $(BROWSER_ELF)
	$(OBJCOPY) --strip-all $< $@

$(HELLOGUI_STRIPPED): $(HELLOGUI_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PIXAPP_STRIPPED): $(PIXAPP_ELF)
	$(OBJCOPY) --strip-all $< $@

$(PAINT_STRIPPED): $(PAINT_ELF)
	$(OBJCOPY) --strip-all $< $@

$(GUI_TERM_STRIPPED): $(GUI_TERM_ELF)
	$(OBJCOPY) --strip-all $< $@

$(NOTEPAD_STRIPPED): $(NOTEPAD_ELF)
	$(OBJCOPY) --strip-all $< $@

$(LAUNCHER_STRIPPED): $(LAUNCHER_ELF)
	$(OBJCOPY) --strip-all $< $@

$(TASKBAR_STRIPPED): $(TASKBAR_ELF)
	$(OBJCOPY) --strip-all $< $@

$(NOTIFY_STRIPPED): $(NOTIFY_ELF)
	$(OBJCOPY) --strip-all $< $@

$(DESKTOP_STRIPPED): $(DESKTOP_ELF)
	$(OBJCOPY) --strip-all $< $@

# Wrap the stripped ELF as a raw blob into an aarch64 ELF object
# exposing `_binary_<name>_elf_bin_start/_end/_size`.
$(HELLO_EMBED): $(HELLO_STRIPPED)
	cd $(dir $<) && cp $(notdir $<) hello.elf.bin && \
	    $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	        --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	        hello.elf.bin $(notdir $@) && \
	    rm hello.elf.bin

$(CAT_EMBED): $(CAT_STRIPPED)
	cd $(dir $<) && cp $(notdir $<) cat.elf.bin && \
	    $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	        --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	        cat.elf.bin $(notdir $@) && \
	    rm cat.elf.bin

$(INIT_EMBED): $(INIT_STRIPPED)
	cd $(dir $<) && cp $(notdir $<) init.elf.bin && \
	    $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	        --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	        init.elf.bin $(notdir $@) && \
	    rm init.elf.bin

$(SH_EMBED): $(SH_STRIPPED)
	cd $(dir $<) && cp $(notdir $<) sh.elf.bin && \
	    $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	        --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	        sh.elf.bin $(notdir $@) && \
	    rm sh.elf.bin

# ----------------------------------------------------------------------
# Embedded ramfs files
#
# Each file under assets/ramfs/ becomes an embedded blob in the
# kernel image with `_binary_<name>_<ext>_start/_end` symbols that
# kernel/core/vfs.c references.  Add a new file by:
#   1. dropping it under assets/ramfs/,
#   2. extending RAMFS_FILES below,
#   3. extending the table in kernel/core/vfs.c.
# ----------------------------------------------------------------------
RAMFS_SRCS := assets/ramfs/motd.txt assets/ramfs/README.txt
RAMFS_OBJS := $(patsubst assets/ramfs/%,$(BUILD)/ramfs/%.o,$(RAMFS_SRCS))

$(BUILD)/ramfs/%.o: assets/ramfs/%
	@mkdir -p $(dir $@)
	cd $(dir $<) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	    --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	    $(notdir $<) $(abspath $@)

# ----------------------------------------------------------------------
# Embedded TrueType font (chapter 102 -> chapter 108b)
#
# Pre-chapter-108b this was embedded into the kernel image because
# the in-kernel TTF rasteriser (kernel/device/ttf.c) needed the
# raw bytes.  Chapter 108b moved the rasteriser to /bin/fontd,
# so the TrueType payload moved with it: instead of linking into
# the kernel ELF it links into the fontd ELF.  Same objcopy-binary
# trick; the symbols are still _binary_DejaVuSans_ttf_start /
# _end, walked by userspace/fontd/ttf.c at daemon startup.
#
# DejaVu Sans 2.37 — Bitstream Vera + Arev licences, see
# assets/fonts/DejaVuSans.LICENSE.
# ----------------------------------------------------------------------
FONT_BLOB_SRC := assets/fonts/DejaVuSans.ttf
FONT_BLOB_OBJ := $(BUILD)/userspace/fontd/DejaVuSans.ttf.o

$(FONT_BLOB_OBJ): $(FONT_BLOB_SRC)
	@mkdir -p $(dir $@)
	cd $(dir $<) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	    --rename-section .data=.rodata.embedded_user,readonly,data,contents,alloc \
	    $(notdir $<) $(abspath $@)

# milestone-50 desktop wallpaper.  An arbitrary JPEG (today
# assets/backgrounds/flowers.jpg) gets cover-fitted to the
# framebuffer's native resolution at build time, then placed onto
# OSFS as /mnt/wallpaper.bgra.  A userspace `desktop` process
# reads it at boot and blits it into a PIN_TO_BOTTOM window via
# gui_present.  The kernel does NOT see the image — wallpaper
# ownership belongs in userspace, not in the kernel binary.
#
# WALLPAPER_W/H are pinned to FB_XRES/FB_YRES so the on-disk
# image is always exactly the right size for the scanout the
# user passes via FB_RES (default 1920x1080).  The desktop
# process reads an 8-byte header (w, h) from the file at
# runtime and tolerates a mismatch by clipping.
#
# FB_RES is also used further down by the QEMU run targets.
FB_RES   ?= 1920x1080
FB_XRES  := $(word 1,$(subst x, ,$(FB_RES)))
FB_YRES  := $(word 2,$(subst x, ,$(FB_RES)))

WALLPAPER_SRC  := assets/backgrounds/flowers.jpg
WALLPAPER_BIN  := $(BUILD)/wallpaper.bgra
WALLPAPER_W    := $(FB_XRES)
WALLPAPER_H    := $(FB_YRES)

$(WALLPAPER_BIN): $(WALLPAPER_SRC) scripts/img_to_bgra.py
	@mkdir -p $(dir $@)
	python3 scripts/img_to_bgra.py $< $@ $(WALLPAPER_W) $(WALLPAPER_H)

# Chapter 133a: doomgeneric source tarball, shipped on /bin so
# the in-guest /bin/tar can extract it onto /data and the
# in-guest /bin/gcc can rebuild Doom from source.
DOOMGENERIC_TAR := $(BUILD)/doomgeneric.tar
DOOMGENERIC_SRCS := $(shell find vendor/doomgeneric/src -type f 2>/dev/null)

$(DOOMGENERIC_TAR): scripts/mktar.py $(DOOMGENERIC_SRCS)
	@mkdir -p $(dir $@)
	python3 scripts/mktar.py $@ vendor/doomgeneric/src src

# Chapter 133e: tarball of the host-cross-built Doom vendor .o
# files.  Shipped at /bin/doomobjs.tar so the link-only
# regression test (scripts/test_doom_link.py) can populate
# /data/src/ in ~5 seconds instead of running the full 20-minute
# in-guest compile.  Entries are prefixed `src/`, matching
# doomgeneric.tar's layout, so `/bin/tar xf
# /bin/doomobjs.tar -C /data` lands them at /data/src/.
# (Name is constrained to 19 bytes by the OSFS-1 directory format,
# so we abbreviate `doomgeneric_objs.tar` to `doomobjs.tar`.)
DOOMGENERIC_OBJS_TAR := $(BUILD)/doomobjs.tar

$(DOOMGENERIC_OBJS_TAR): scripts/mktar.py $(DOOM_VENDOR_OBJS)
	@mkdir -p $(dir $@)
	python3 scripts/mktar.py $@ $(BUILD)/vendor/doomgeneric src

OBJS     := $(S_OBJS) $(C_OBJS) $(RAMFS_OBJS)

# Disk images (definitions early so `all:` can depend on them).
# The actual rules are further down where the run targets live.
#
#   $(DISK)      — virtio-blk hd0, OSFS-1 (read-only, kernel + assets).
#   $(DATA_DISK) — virtio-blk hd1, OSFS-2 (writable, chapter 81+).
#
# OSFS-2 is formatted empty by default; chapter 84 will start seeding
# it with notepad documents and shell history that need to survive a
# reboot.
DISK      := $(BUILD)/disk.img
DATA_DISK := $(BUILD)/data.img

# ----------------------------------------------------------------------
# Default target
# ----------------------------------------------------------------------
#
# `$(GCC_MARKER)` is the chapter-132a vendored gcc source tree.  It's
# not yet used to build any disk artefact (that lands in 132c when
# xgcc is cross-built), but we list it here so a fresh clone running
# `make` / `make run-graphical` automatically fetches + patches the
# source.  Otherwise a reader following chapter 132a would have to
# know about `bash scripts/fetch_gcc.sh` out-of-band.  The marker
# rule is idempotent — once the .patched-osdev file exists, make
# does nothing on subsequent invocations.
#
# `$(GCC_PREREQS_MARKER)` is chapter 132b's GMP/MPFR/MPC in-tree
# symlinks.  Same auto-fetch reasoning.  Depends on $(GCC_MARKER)
# at the rule level so the order is always extract-gcc → drop-in
# the math libs.
# ----------------------------------------------------------------------
.PHONY: all
all: $(KERNEL) $(DISK) $(DATA_DISK) $(GCC_MARKER) $(GCC_PREREQS_MARKER)

$(KERNEL): $(OBJS) linker/kernel.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "linked: $@"
	@$(OBJDUMP) -h $@ | sed -n '1,16p'

# Raw image (no ELF header) — useful for U-Boot booti / bare-metal flash.
$(KIMG): $(KERNEL)
	$(OBJCOPY) -O binary $< $@

.PHONY: image
image: $(KIMG)

# ----------------------------------------------------------------------
# Compile rules
# ----------------------------------------------------------------------
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# ----------------------------------------------------------------------
# Auto-generated header dependencies
#
# `-MMD -MP` (in CFLAGS / USER_CFLAGS) tells gcc to emit a .d
# alongside every .o, listing the headers it transitively included.
# Including those .d files here means a header change to e.g.
# kernel/core/vfs.h re-triggers every .c that ultimately includes
# it — without this, growing a struct embedded in struct thread
# silently produced .o files compiled against TWO different
# layouts of the same struct, with predictably weird runtime
# corruption (chapter 81 post-mortem).
#
# `-include` (lower-case) means make does not error on first build
# when no .d files exist yet.
# ----------------------------------------------------------------------
DEP_FILES := $(shell find $(BUILD) -name '*.d' 2>/dev/null)
-include $(DEP_FILES)

# ----------------------------------------------------------------------
# Run targets
#
# `make run` is the daily-driver target on Apple Silicon: HVF gives
# native ARM speed.  `-cpu host` is required for HVF.  `gic-version=3`
# matches what HVF exposes.
#
# `make run-tcg` is the portable fallback that works on any host.
#
# `-nographic` routes UART0 to the controlling terminal AND wires
# Ctrl-A X as the QEMU exit shortcut.  We do not pass `-display`
# yet — that lands in milestone 6 when virtio-gpu shows up.
# ----------------------------------------------------------------------
# `assets/virt.dtb` is the auto-generated DTB extracted once via
# `qemu-system-aarch64 -M virt,...,dumpdtb=assets/virt.dtb`.  We
# load it at 0x44000000 (well past the 1 MiB kernel image) so
# milestone-6+ kernels can scan it for the physical memory map.
DTB      := assets/virt.dtb
DTB_ADDR := 0x44000000

# RAM size handed to QEMU.  Override with `make run QEMU_MEM=16G` to
# stress the dynamic L1 mapper and the page allocator.  After
# changing this you must re-run `scripts/build_dtb.sh` so the on-disk
# DTB's /memory node matches the new size.
QEMU_MEM ?= 8G

# Chapter 86 — number of cores to expose to the guest.  The
# kernel's smp_init() reads /cpus from the DTB and wakes every
# secondary via PSCI CPU_ON.  Default 2 (boot CPU + one
# secondary, the chapter-86 minimum); override with
# `make run QEMU_SMP=4` to stress.  After changing this you must
# also re-run `scripts/build_dtb.sh QEMU_SMP=N` so the dumped DTB
# enumerates the matching number of cpu nodes.
QEMU_SMP ?= 2

# Disk image attached as the virtio-blk backing store.  Built by
# scripts/mkosfs.py from the files under assets/osfs/ and the
# stripped user binaries under build/userspace/<prog>/.  1 MiB raw,
# OSFS-1 layout (see kernel/core/osfs.h).  The kernel mounts this
# at /mnt at boot, and looks up /bin/<name> from it as well.
# (DISK is defined earlier so all: can depend on it.)
OSFS_FILES := assets/osfs/hello.txt assets/osfs/poem.txt assets/osfs/test.html assets/osfs/test.css assets/osfs/test_layout.html assets/osfs/hn.html assets/osfs/forms.html assets/osfs/onclick.html assets/osfs/icon.png assets/osfs/icon_palette.png assets/osfs/icon_gray.png assets/osfs/icon_large.png assets/osfs/img_test.html assets/osfs/intrinsic.html assets/osfs/ca.bundle assets/osfs/osdev.ld assets/osfs/hello.bf assets/osfs/stdio_test.c assets/osfs/mk_test.mk assets/osfs/mk_helloA.c assets/osfs/mk_helloB.c assets/osfs/doom_pilot.mk assets/osfs/doom_full.mk assets/osfs/doom_link.mk assets/osfs/doom_link.args $(WALLPAPER_BIN)

# Chapter 132i + 132j: the user-facing libc headers shipped on the
# OSFS image so the in-guest /bin/gcc can resolve `#include
# <stdio.h>` and friends.  Two sets:
#
#  - LIBC_TOP_HEADERS: top-level headers, copied verbatim to /bin.
#    Chapter 132j added `unistd.h` (was excluded in 132i because it
#    pulls in `sys/stat.h`; now that we ship sys/ this works).
#
#  - LIBC_SYS_HEADERS: headers shipped as literal-named entries
#    `sys/foo.h` in the flat OSFS-1 directory.  cpp finds them via
#    `<sys/foo.h>` because `-isystem /bin` + literal name match.
#    They are STAGED through scripts/stage_libc_headers.py first
#    to rewrite `"../foo.h"` -> `<foo.h>` so cpp resolves siblings
#    through `-isystem /bin` instead of `/bin/sys/../foo.h` (which
#    our kernel doesn't normalise).
#
# `termios.h`, `poll.h`, `sys/mman.h`, `sys/ioctl.h`, `sys/select.h`
# are still missing.  Doom's xlib backend wants them; our
# `doomgeneric_osdev.c` shim doesn't.  Add on demand.
LIBC_TOP_HEADERS := assert.h atexit.h ctype.h dirent.h env.h errno.h \
                    fcntl.h inttypes.h locale.h malloc.h math.h printf.h \
                    scanf.h setjmp.h signal.h stdio.h stdlib.h string.h \
                    strings.h syscall.h thread.h time.h unistd.h wchar.h \
                    zlib.h

LIBC_SYS_HEADERS := stat.h types.h time.h times.h wait.h param.h

STAGED_LIBC_DIR := $(BUILD)/staged-libc-headers

# Pattern rule: stage a sys/*.h through stage_libc_headers.py,
# rewriting `"../foo.h"` -> `<foo.h>` so cpp finds siblings via
# `-isystem /bin` rather than parent-relative traversal.
$(STAGED_LIBC_DIR)/sys/%.h: userspace/libc/sys/%.h scripts/stage_libc_headers.py
	@mkdir -p $(@D)
	python3 scripts/stage_libc_headers.py $< $@

# Legacy alias retained for any external make-graph consumers; new
# code should reference LIBC_TOP_HEADERS.
LIBC_HEADERS := $(LIBC_TOP_HEADERS)

# Chapter 132i: GCC's own freestanding headers (stdint.h, stddef.h,
# stdarg.h, ...).  These are built as part of the xgcc cross-build
# and live under build/gcc-build-guest/gcc/gcc/include/.  Our libc
# headers `#include <stdint.h>` etc., so without these on disk cpp
# fails with "stdint.h: No such file or directory" on the first
# `#include <stdio.h>` (chapter 132i bring-up bug).  arm_*, tgmath
# and the heavier SIMD headers are deliberately skipped -- 16 ISO C
# headers / ~90 KB is enough for hello-world-shaped programs.
GCC_FREESTANDING_DIR := build/gcc-build-guest/gcc/gcc/include
GCC_FREESTANDING_HEADERS := stdint.h stdint-gcc.h stddef.h stdarg.h \
                            stdbool.h float.h limits.h iso646.h \
                            stdalign.h stdnoreturn.h syslimits.h \
                            stdatomic.h stdckdint.h stdfix.h tgmath.h \
                            varargs.h
# (The rule that actually populates $(GCC_FREESTANDING_DIR) is
# the $(GCC_GUEST_STAMP) rule defined near the bottom of this
# Makefile, alongside the rules for $(XGCC_GUEST_BIN) and
# $(CC1_GUEST_BIN).  Placed there so all variables it depends on
# -- GCC_XGCC, OSDEV_CC, GCC_PREREQS_MARKER -- are in scope at
# parse time.)
OSFS_BIN_FILES := $(INIT_STRIPPED) $(SH_STRIPPED) $(CAT_STRIPPED) $(HELLO_STRIPPED) $(BADPOKE_STRIPPED) $(BADPTR_STRIPPED) $(HEAPTEST_STRIPPED) $(MMAPTEST_STRIPPED) $(THREADTEST_STRIPPED) $(THREADTEST2_STRIPPED) $(THREADTEST3_STRIPPED) $(ECHO_STRIPPED) $(PRINTFTEST_STRIPPED) $(LS_STRIPPED) $(UPTIME_STRIPPED) $(PS_STRIPPED) $(TOP_STRIPPED) $(DATE_STRIPPED) $(BEEP_STRIPPED) $(GETRAND_STRIPPED) $(MOUNT_STRIPPED) $(TLSTEST_STRIPPED) $(HTTPSD_STRIPPED) $(PNGDEC_STRIPPED) $(ENV_STRIPPED) $(GREP_STRIPPED) $(WC_STRIPPED) $(HEAD_STRIPPED) $(TAIL_STRIPPED) $(SLEEP_STRIPPED) $(SYNC_STRIPPED) $(PIPETEST_STRIPPED) $(HTTPGET_STRIPPED) $(COOKIES_STRIPPED) $(ECHOD_STRIPPED) $(ECHOFS_STRIPPED) $(HTTPD_STRIPPED) $(LOOPTEST_STRIPPED) $(SRVTEST_STRIPPED) $(CLIPBOARDD_STRIPPED) $(PROCD_STRIPPED) $(FONTD_STRIPPED) $(PROXYTEST_STRIPPED) $(HTMLTOK_STRIPPED) $(HTMLDOM_STRIPPED) $(CSSPARSE_STRIPPED) $(LAYOUT_STRIPPED) $(BROWSER_STRIPPED) $(HELLOGUI_STRIPPED) $(PIXAPP_STRIPPED) $(PAINT_STRIPPED) $(GUI_TERM_STRIPPED) $(NOTEPAD_STRIPPED) $(LAUNCHER_STRIPPED) $(TASKBAR_STRIPPED) $(NOTIFY_STRIPPED) $(DESKTOP_STRIPPED) $(FORKTEST_STRIPPED) $(SIGTEST_STRIPPED) $(CHLDTEST_STRIPPED) $(COWTEST_STRIPPED) $(MIXTEST_STRIPPED) $(STRACE_STRIPPED) $(STACKBOMB_STRIPPED) $(WSD_STRIPPED) $(WMTEST_STRIPPED) $(HELLOWSD_STRIPPED) $(HANGFS_STRIPPED) $(ERRNOTEST_STRIPPED) $(STDIOTEST_STRIPPED) $(ENVTEST_STRIPPED) $(STATTEST_STRIPPED) $(BINUTILS_AS_STRIPPED) $(BINUTILS_LD_STRIPPED) $(AR_STRIPPED) $(ATEXITTEST_STRIPPED) $(CC_STRIPPED) $(MAKE_STRIPPED) $(SETJMPTEST_STRIPPED) $(SIGTEST2_STRIPPED) $(ABORTTEST_STRIPPED) $(STRTEST_STRIPPED) $(ASSERTFAIL_STRIPPED) $(TIMETEST_STRIPPED) $(STDLIBTEST_STRIPPED) $(PRINTFTEST2_STRIPPED) $(FPTEST_STRIPPED) $(DOOM_STRIPPED) $(GCCW_STRIPPED) $(XGCC_GUEST_STRIPPED) $(CC1_GUEST_STRIPPED) $(BF_STRIPPED) $(TAR_STRIPPED)

# Bake the chapter-97 test PNG (16x16 RGBA with a known pixel
# pattern) at build time.  See scripts/make_test_png.py for the
# layout and the rationale for not committing a binary.
assets/osfs/icon.png: scripts/make_test_png.py
	python3 scripts/make_test_png.py $@

# Chapter 98 added palette (colour type 3) and 8-bit grayscale
# (colour type 0) sister images for the extended decoder tests.
assets/osfs/icon_palette.png: scripts/make_test_png.py
	python3 scripts/make_test_png.py --kind=palette $@

assets/osfs/icon_gray.png: scripts/make_test_png.py
	python3 scripts/make_test_png.py --kind=gray $@

# Chapter 98b adds a larger (64x64) palette image used by
# test_browser_intrinsic_size.py to exercise the layout-pass
# intrinsic-size hook: each 32x32 quadrant = 1024 same-colour
# pixels, more than the layout's 16x16 fallback could produce.
assets/osfs/icon_large.png: scripts/make_test_png.py
	python3 scripts/make_test_png.py --kind=large_palette $@

# Chapter 112e + 112f CA bundle.  Framed "CAB1" format ingested
# by tls_socket_init_chain_from_bundle in the guest.  Two
# anchors, both ROOT certs (not intermediates):
#
#   cert-root-rsa.pem -- BearSSL sample RSA root CA
#   cert-root-ec.pem  -- BearSSL sample ECDSA P-256 root CA
#
# httpsd presents the full leaf+intermediate chain (CHAIN_LEN=2
# in chain-{rsa,ec}.h).  With the ROOT in the trust store the
# validator does the recursive walk: leaf sig vs intermediate
# pubkey (off the wire) -> intermediate sig vs root pubkey (off
# the anchor list) -> accept.  In chapter 112e we trusted the
# intermediate directly, which only exercised the first link.
#
# Ingest is via the chapter-112f PEM mode -- the same code path
# you'd point at a Mozilla NSS root list.
# Chapter 112g: fetch the host's public CA list (Mozilla NSS-derived
# on most systems) into vendor/testcerts/ on first build, then fold
# it into the framed bundle alongside the BearSSL sample roots.  The
# .pem is .gitignored and rebuilt on demand.
vendor/testcerts/public-roots.pem: scripts/fetch_public_roots.sh
	bash scripts/fetch_public_roots.sh $@

assets/osfs/ca.bundle: scripts/mkcabundle.py \
                      vendor/bearssl/samples/cert-root-rsa.pem \
                      vendor/bearssl/samples/cert-root-ec.pem \
                      vendor/testcerts/public-roots.pem
	python3 scripts/mkcabundle.py $@ \
	    --pem vendor/bearssl/samples/cert-root-rsa.pem \
	    --pem vendor/bearssl/samples/cert-root-ec.pem \
	    --pem vendor/testcerts/public-roots.pem

$(DISK): scripts/mkosfs.py $(OSFS_FILES) $(OSFS_BIN_FILES) \
         $(BUILD)/userspace/crt/crt0.o userspace/linker_user.ld \
         $(XGCC_SYS_LIBC) \
         $(DOOMGENERIC_TAR) \
         $(DOOMGENERIC_OBJS_TAR) \
         $(DOOMRT_LIB) \
         $(addprefix userspace/libc/,$(LIBC_TOP_HEADERS)) \
         $(addprefix $(STAGED_LIBC_DIR)/sys/,$(LIBC_SYS_HEADERS)) \
         $(addprefix $(GCC_FREESTANDING_DIR)/,$(GCC_FREESTANDING_HEADERS))
	@mkdir -p $(BUILD)
	python3 scripts/mkosfs.py $(DISK) \
	    $(foreach h,$(LIBC_TOP_HEADERS),$(h)=userspace/libc/$(h)) \
	    $(foreach h,$(LIBC_SYS_HEADERS),sys/$(h)=$(STAGED_LIBC_DIR)/sys/$(h)) \
	    $(foreach h,$(GCC_FREESTANDING_HEADERS),$(h)=$(GCC_FREESTANDING_DIR)/$(h)) \
	    hello.txt=assets/osfs/hello.txt \
	    hello.bf=assets/osfs/hello.bf \
	    stdio_test.c=assets/osfs/stdio_test.c \
	    sys_stat_test.c=assets/osfs/sys_stat_test.c \
	    poem.txt=assets/osfs/poem.txt \
	    test.html=assets/osfs/test.html \
	    test.css=assets/osfs/test.css \
	    test_layout.html=assets/osfs/test_layout.html \
	    hn.html=assets/osfs/hn.html \
	    forms.html=assets/osfs/forms.html \
	    onclick.html=assets/osfs/onclick.html \
	    icon.png=assets/osfs/icon.png \
	    icon_palette.png=assets/osfs/icon_palette.png \
	    icon_gray.png=assets/osfs/icon_gray.png \
	    icon_large.png=assets/osfs/icon_large.png \
	    img_test.html=assets/osfs/img_test.html \
	    intrinsic.html=assets/osfs/intrinsic.html \
	    ca.bundle=assets/osfs/ca.bundle \
	    osdev.ld=assets/osfs/osdev.ld \
	    init=$(INIT_STRIPPED) \
	    sh=$(SH_STRIPPED) \
	    cat=$(CAT_STRIPPED) \
	    hello=$(HELLO_STRIPPED) \
	    badpoke=$(BADPOKE_STRIPPED) \
	    badptr=$(BADPTR_STRIPPED) \
	    heaptest=$(HEAPTEST_STRIPPED) \
	    mmaptest=$(MMAPTEST_STRIPPED) \
	    threadtest=$(THREADTEST_STRIPPED) \
	    threadtest2=$(THREADTEST2_STRIPPED) \
	    threadtest3=$(THREADTEST3_STRIPPED) \
	    echo=$(ECHO_STRIPPED) \
	    printftest=$(PRINTFTEST_STRIPPED) \
	    setjmptest=$(SETJMPTEST_STRIPPED) \
	    sigtest2=$(SIGTEST2_STRIPPED) \
	    aborttest=$(ABORTTEST_STRIPPED) \
	    strtest=$(STRTEST_STRIPPED) \
	    assertfail=$(ASSERTFAIL_STRIPPED) \
	    timetest=$(TIMETEST_STRIPPED) \
	    stdlibtest=$(STDLIBTEST_STRIPPED) \
	    printftest2=$(PRINTFTEST2_STRIPPED) \
	    fptest=$(FPTEST_STRIPPED) \
	    doom=$(DOOM_STRIPPED) \
	    errnotest=$(ERRNOTEST_STRIPPED) \
	    stdiotest=$(STDIOTEST_STRIPPED) \
	    envtest=$(ENVTEST_STRIPPED) \
	    stattest=$(STATTEST_STRIPPED) \
	    as=$(BINUTILS_AS_STRIPPED) \
	    ld=$(BINUTILS_LD_STRIPPED) \
	    ar=$(AR_STRIPPED) \
	    atexittest=$(ATEXITTEST_STRIPPED) \
	    cc=$(CC_STRIPPED) \
	    make=$(MAKE_STRIPPED) \
	    gcc=$(GCCW_STRIPPED) \
	    xgcc=$(XGCC_GUEST_STRIPPED) \
	    cc1=$(CC1_GUEST_STRIPPED) \
	    crt0.o=$(BUILD)/userspace/crt/crt0.o \
	    linker_user.ld=userspace/linker_user.ld \
	    libosdevc.a=$(XGCC_SYS_LIBC) \
	    ls=$(LS_STRIPPED) \
	    uptime=$(UPTIME_STRIPPED) \
	    ps=$(PS_STRIPPED) \
	    top=$(TOP_STRIPPED) \
	    strace=$(STRACE_STRIPPED) \
	    stackbomb=$(STACKBOMB_STRIPPED) \
	    date=$(DATE_STRIPPED) \
	    beep=$(BEEP_STRIPPED) \
	    getrand=$(GETRAND_STRIPPED) \
	    mount=$(MOUNT_STRIPPED) \
	    tlstest=$(TLSTEST_STRIPPED) \
	    httpsd=$(HTTPSD_STRIPPED) \
	    pngdec=$(PNGDEC_STRIPPED) \
	    env=$(ENV_STRIPPED) \
	    grep=$(GREP_STRIPPED) \
	    wc=$(WC_STRIPPED) \
	    head=$(HEAD_STRIPPED) \
	    tail=$(TAIL_STRIPPED) \
	    bf=$(BF_STRIPPED) \
	    bf.c=userspace/bf/bf.c \
	    tar=$(TAR_STRIPPED) \
	    doomgeneric.tar=$(DOOMGENERIC_TAR) \
	    doomobjs.tar=$(DOOMGENERIC_OBJS_TAR) \
	    mk_test.mk=assets/osfs/mk_test.mk \
	    mk_helloA.c=assets/osfs/mk_helloA.c \
	    mk_helloB.c=assets/osfs/mk_helloB.c \
	    doom_pilot.mk=assets/osfs/doom_pilot.mk \
	    doom_full.mk=assets/osfs/doom_full.mk \
	    doom_link.mk=assets/osfs/doom_link.mk \
	    doom_link.args=assets/osfs/doom_link.args \
	    libdoomrt.a=$(DOOMRT_LIB) \
	    sleep=$(SLEEP_STRIPPED) \
	    sync=$(SYNC_STRIPPED) \
	    pipetest=$(PIPETEST_STRIPPED) \
	    httpget=$(HTTPGET_STRIPPED) \
	    cookies=$(COOKIES_STRIPPED) \
	    echod=$(ECHOD_STRIPPED) \
	    echofs=$(ECHOFS_STRIPPED) \
	    hangfs=$(HANGFS_STRIPPED) \
	    httpd=$(HTTPD_STRIPPED) \
	    looptest=$(LOOPTEST_STRIPPED) \
	    srvtest=$(SRVTEST_STRIPPED) \
	    clipboardd=$(CLIPBOARDD_STRIPPED) \
	    procd=$(PROCD_STRIPPED) \
	    fontd=$(FONTD_STRIPPED) \
	    wsd=$(WSD_STRIPPED) \
	    wmtest=$(WMTEST_STRIPPED) \
	    hellowsd=$(HELLOWSD_STRIPPED) \
	    proxytest=$(PROXYTEST_STRIPPED) \
	    htmltok=$(HTMLTOK_STRIPPED) \
	    htmldom=$(HTMLDOM_STRIPPED) \
	    cssparse=$(CSSPARSE_STRIPPED) \
	    layout=$(LAYOUT_STRIPPED) \
	    browser=$(BROWSER_STRIPPED) \
	    hellogui=$(HELLOGUI_STRIPPED) \
	    pixapp=$(PIXAPP_STRIPPED) \
	    paint=$(PAINT_STRIPPED) \
	    gui_term=$(GUI_TERM_STRIPPED) \
	    notepad=$(NOTEPAD_STRIPPED) \
	    launcher=$(LAUNCHER_STRIPPED) \
	    taskbar=$(TASKBAR_STRIPPED) \
	    notify=$(NOTIFY_STRIPPED) \
	    desktop=$(DESKTOP_STRIPPED) \
	    forktest=$(FORKTEST_STRIPPED) \
	    sigtest=$(SIGTEST_STRIPPED) \
	    chldtest=$(CHLDTEST_STRIPPED) \
	    cowtest=$(COWTEST_STRIPPED) \
	    mixtest=$(MIXTEST_STRIPPED) \
	    wallpaper.bgra=$(WALLPAPER_BIN)

# OSFS-2 disk: empty 64 MiB OSFS-2 image, formatted by mkosfs2.py.
# A `make clean` always wipes it; chapter 81 explicitly tests against
# the freshly-formatted state.  Chapter 84 will switch this to a
# preserve-across-clean policy once notepad/sh-history live here.
#
# Chapter 130b — if assets/wads/doom1.wad is present, seed it into the
# OSFS-2 root so `doom` (which looks at /data/doom1.wad by default)
# can launch a full title-screen game without the user having to type
# `-iwad <path>`.  The file is .gitignored (4 MiB shareware binary)
# and considered an optional asset — without it the OSFS-2 image is
# still formatted, but doom will exit with "Game mode indeterminate".
DOOM_WAD := assets/wads/doom1.wad
DOOM_WAD_SEED := $(wildcard $(DOOM_WAD))

$(DATA_DISK): scripts/mkosfs2.py $(DOOM_WAD_SEED)
	@mkdir -p $(BUILD)
	python3 scripts/mkosfs2.py $(DATA_DISK) \
	    $(if $(DOOM_WAD_SEED),doom1.wad=$(DOOM_WAD_SEED))

QEMU_BLK := -drive if=none,file=$(DISK),format=raw,id=hd0 \
            -device virtio-blk-device,drive=hd0 \
            -drive if=none,file=$(DATA_DISK),format=raw,id=hd1 \
            -device virtio-blk-device,drive=hd1

# Graphical run target uses an emulated virtio-gpu and routes the
# display to a Cocoa window.  `xres` / `yres` request a specific
# scanout resolution (the default is 1024x768; we pick 1920x1080 to
# show the framebuffer pipeline working at a real desktop resolution).
# Override with `make run-graphical FB_RES=1280x800`.
# (FB_RES / FB_XRES / FB_YRES are defined earlier alongside the
# wallpaper rule, since the wallpaper image must match the scanout
# resolution at build time.)
QEMU_GPU := -device virtio-gpu-device,xres=$(FB_XRES),yres=$(FB_YRES)
QEMU_KBD := -device virtio-keyboard-device
QEMU_TBL := -device virtio-tablet-device
# Milestone 52: SLIRP user-mode networking attached to a virtio-net
# device on the virtio-mmio bus.  Default: guest is 10.0.2.15/24,
# gateway is 10.0.2.2, no host-side port forwarding.  Override
# QEMU_NETDEV to add hostfwd=tcp::5555-:5555 etc.
QEMU_NETDEV ?= user,id=n0
QEMU_NET := -netdev $(QEMU_NETDEV) -device virtio-net-device,netdev=n0
QEMU_DISPLAY ?= cocoa

# Chapter 96: virtio-sound on the virtio-mmio bus.  The default
# `none` audiodev makes QEMU consume samples without playing
# them through any host backend (great for `make run` and tests
# — no audible boot chime in a terminal session).  For
# `make run-graphical` we override to `coreaudio` so the chime
# actually plays through the host speakers.  Override
# QEMU_AUDIO_BACKEND to use a different backend (`wav`, `dbus`,
# etc.) — see `qemu-system-aarch64 -audiodev help`.
QEMU_AUDIO_BACKEND ?= none
QEMU_SND := -audiodev $(QEMU_AUDIO_BACKEND),id=audio0 \
            -device virtio-sound-device,audiodev=audio0

# Chapter 112 — hardware RNG fed from the host's /dev/urandom.
# Without this option the kernel's virtio_rng_init() finds nothing
# and random_init() falls back to a CNTVCT-seeded ChaCha20 PRNG
# with a loud warning on the boot log; that path is fine for tests
# but is NOT safe to use for TLS (chapter 114+).  Every regular
# `run*` target now hands the device to the guest by default; pass
# `QEMU_RNG=` on the command line to suppress it and exercise the
# fallback path explicitly.
QEMU_RNG ?= -object rng-random,id=rng0,filename=/dev/urandom \
            -device virtio-rng-device,rng=rng0

# QEMU's `virt` machine defaults to legacy (v1) virtio-mmio.  We
# implement the modern (v2) transport, so force the bus to v2.
QEMU_VIRTIO_OPTS := -global virtio-mmio.force-legacy=off

.PHONY: run
run: $(KERNEL) $(DISK) $(DATA_DISK) $(GCC_MARKER) $(GCC_PREREQS_MARKER)
	@echo "Running under HVF (-m $(QEMU_MEM), -smp $(QEMU_SMP)) — Ctrl-A X to quit."
	$(QEMU) -M virt,gic-version=3 -cpu host -accel hvf \
	        -m $(QEMU_MEM) -smp $(QEMU_SMP) -nographic -serial mon:stdio \
	        $(QEMU_VIRTIO_OPTS) \
	        -device loader,file=$(DTB),addr=$(DTB_ADDR) \
	        $(QEMU_BLK) \
	        $(QEMU_NET) \
	        $(QEMU_SND) \
	        $(QEMU_RNG) \
	        -kernel $(KERNEL)

# `run-graphical` opens a Cocoa window and attaches a virtio-gpu so
# the kernel can paint the boot framebuffer.  Serial still goes to
# stdio so kernel logs continue to appear in the terminal alongside
# the graphical window.
#
# Chapter 105: this is the daily-driver target, so we bake a SLIRP
# hostfwd into its QEMU_NETDEV.  That way `httpd 8080 &` inside the
# guest is immediately reachable from the host as
# `curl http://127.0.0.1:18080/mnt/hello.txt`.  Without the hostfwd
# SLIRP silently drops inbound connections (NAT outbound only).
# Override HTTPD_HOST_PORT / HTTPD_GUEST_PORT if 18080/8080 clash.
HTTPD_HOST_PORT  ?= 18080
HTTPD_GUEST_PORT ?= 8080
.PHONY: run-graphical
run-graphical: QEMU_AUDIO_BACKEND := coreaudio
run-graphical: QEMU_SND := -audiodev $(QEMU_AUDIO_BACKEND),id=audio0 -device virtio-sound-device,audiodev=audio0
run-graphical: QEMU_NETDEV := user,id=n0,hostfwd=tcp::$(HTTPD_HOST_PORT)-:$(HTTPD_GUEST_PORT)
run-graphical: QEMU_NET    := -netdev $(QEMU_NETDEV) -device virtio-net-device,netdev=n0
run-graphical: $(KERNEL) $(DISK) $(DATA_DISK) $(GCC_MARKER) $(GCC_PREREQS_MARKER)
	@echo "Running graphical under HVF, $(FB_RES) virtio-gpu scanout, -smp $(QEMU_SMP)."
	@echo "Close the QEMU window or press Ctrl-A X in the terminal to quit."
	@echo "Inside guest:  httpd $(HTTPD_GUEST_PORT) &"
	@echo "From host:     curl http://127.0.0.1:$(HTTPD_HOST_PORT)/mnt/hello.txt"
	$(QEMU) -M virt,gic-version=3 -cpu host -accel hvf \
	        -m $(QEMU_MEM) -smp $(QEMU_SMP) -display $(QEMU_DISPLAY) -serial stdio \
	        $(QEMU_VIRTIO_OPTS) \
	        -device loader,file=$(DTB),addr=$(DTB_ADDR) \
	        $(QEMU_GPU) \
	        $(QEMU_KBD) \
	        $(QEMU_TBL) \
	        $(QEMU_BLK) \
	        $(QEMU_NET) \
	        $(QEMU_SND) \
	        $(QEMU_RNG) \
	        -kernel $(KERNEL)

.PHONY: run-tcg
run-tcg: $(KERNEL) $(DISK) $(DATA_DISK)
	@echo "Running under TCG (-m $(QEMU_MEM), -smp $(QEMU_SMP)) — Ctrl-A X to quit."
	$(QEMU) -M virt,gic-version=3 -cpu cortex-a72 -accel tcg \
	        -m $(QEMU_MEM) -smp $(QEMU_SMP) -nographic -serial mon:stdio \
	        $(QEMU_VIRTIO_OPTS) \
	        -device loader,file=$(DTB),addr=$(DTB_ADDR) \
	        $(QEMU_BLK) \
	        $(QEMU_NET) \
	        $(QEMU_SND) \
	        $(QEMU_RNG) \
	        -kernel $(KERNEL)

# Same as `run` but stops at first instruction and exposes a GDB
# stub on tcp::1234.  Use `aarch64-elf-gdb build/kernel.elf` then
# `target remote :1234` and `b kernel_main` then `c`.
.PHONY: debug
debug: $(KERNEL) $(DISK) $(DATA_DISK)
	@echo "GDB stub on :1234 (-m $(QEMU_MEM), -smp $(QEMU_SMP)), kernel paused at entry — Ctrl-A X to quit."
	$(QEMU) -M virt,gic-version=3 -cpu host -accel hvf \
	        -m $(QEMU_MEM) -smp $(QEMU_SMP) -nographic -s -S \
	        $(QEMU_VIRTIO_OPTS) \
	        -device loader,file=$(DTB),addr=$(DTB_ADDR) \
	        $(QEMU_BLK) \
	        -kernel $(KERNEL)

# ----------------------------------------------------------------------
# Diagnostics
# ----------------------------------------------------------------------
.PHONY: toolchain-check
toolchain-check:
	@command -v $(CC)   >/dev/null 2>&1 || { echo "missing $(CC)   (brew install aarch64-elf-gcc)"; exit 1; }
	@command -v $(LD)   >/dev/null 2>&1 || { echo "missing $(LD)   (brew install aarch64-elf-binutils)"; exit 1; }
	@command -v $(QEMU) >/dev/null 2>&1 || { echo "missing $(QEMU) (brew install qemu)"; exit 1; }
	@$(CC)   --version | head -1
	@$(LD)   --version | head -1
	@$(QEMU) --version | head -1
	@echo "toolchain ok — building for aarch64 virt under HVF"

.PHONY: disasm
disasm: $(KERNEL)
	$(OBJDUMP) -d -M no-aliases $(KERNEL) | sed -n '1,80p'

# ----------------------------------------------------------------------
# Chapter 131a: aarch64-osdev cross binutils.
#
# Source lives in vendor/binutils-2.44/, patched to know about the
# `aarch64-osdev` triple (cpu=aarch64, vendor=unknown, os=osdev).
# Both the source tree and the configure+build directory are
# .gitignore'd; scripts/fetch_binutils.sh re-creates the source
# from a sha256-pinned tarball.
#
# Why a new triple?  See book/chapters/.../131a-binutils-target.md.
# Short answer: when we later ship `aarch64-osdev-gcc` we want its
# default search paths and predefined macros (__osdev__) to be
# distinct from the host's aarch64-elf cross.
#
# Built artefacts land in build/toolchain/bin/ as:
#   aarch64-osdev-as, aarch64-osdev-ld, aarch64-osdev-ar, etc.
#
# `make clean` deliberately does NOT wipe the toolchain — it takes
# ~50 s on Apple Silicon to rebuild and isn't part of the kernel
# build loop.  Use `make clean-binutils` if you really want to nuke it.
# ----------------------------------------------------------------------
BINUTILS_SRC     := vendor/binutils-2.44
BINUTILS_MARKER  := $(BINUTILS_SRC)/.patched-osdev
BINUTILS_BUILD   := $(BUILD)/binutils-build
TOOLCHAIN_PREFIX := $(abspath $(BUILD)/toolchain)
BINUTILS_AS      := $(TOOLCHAIN_PREFIX)/bin/aarch64-osdev-as
BINUTILS_LD      := $(TOOLCHAIN_PREFIX)/bin/aarch64-osdev-ld

$(BINUTILS_MARKER):
	bash scripts/fetch_binutils.sh

$(BINUTILS_BUILD)/Makefile: $(BINUTILS_MARKER)
	mkdir -p $(BINUTILS_BUILD)
	cd $(BINUTILS_BUILD) && ../../$(BINUTILS_SRC)/configure \
	    --target=aarch64-osdev \
	    --prefix=$(TOOLCHAIN_PREFIX) \
	    --program-prefix=aarch64-osdev- \
	    --disable-nls --disable-gdb --disable-werror \
	    --disable-multilib --with-system-zlib

$(BINUTILS_AS) $(BINUTILS_LD): $(BINUTILS_BUILD)/Makefile
	$(MAKE) -C $(BINUTILS_BUILD) -j$(shell sysctl -n hw.ncpu) MAKEINFO=true
	$(MAKE) -C $(BINUTILS_BUILD) MAKEINFO=true \
	    install-binutils install-gas install-ld

.PHONY: binutils-osdev
binutils-osdev: $(BINUTILS_AS) $(BINUTILS_LD)
	@echo "binutils-osdev: installed $(BINUTILS_AS)"
	@echo "binutils-osdev: installed $(BINUTILS_LD)"

.PHONY: clean-binutils
clean-binutils:
	rm -rf $(BINUTILS_BUILD) $(TOOLCHAIN_PREFIX)

# ----------------------------------------------------------------------
# Chapter 132a: aarch64-osdev gcc source tree (fetch + patch only).
#
# This chapter ONLY teaches the gcc-14.2.0 source tree about the
# new `aarch64-osdev` triple — it does not build xgcc yet (that's
# chapter 132b, which also needs GMP/MPFR/MPC and a sibling build
# directory).  The patched source tree lives in vendor/gcc-14.2.0/
# alongside vendor/binutils-2.44/.  Both the tarball and the
# extracted tree are .gitignore'd; scripts/fetch_gcc.sh re-creates
# the source from a sha256-pinned tarball and applies our patch
# (vendor/gcc-aarch64-osdev.patch, committed).
#
# Use `make gcc-osdev-src` to fetch + patch on demand.  The smoke
# test scripts/test_gcc_target.py asserts the patch survived round
# trip and that config.sub canonicalises aarch64-osdev correctly.
#
# GCC_SRC / GCC_MARKER themselves are declared early in this
# Makefile (near the BINUTILS_* vars) so the default `all` target
# can list $(GCC_MARKER) as a prereq.
# ----------------------------------------------------------------------

$(GCC_MARKER):
	bash scripts/fetch_gcc.sh

.PHONY: gcc-osdev-src
gcc-osdev-src: $(GCC_MARKER)
	@echo "gcc-osdev-src: patched source ready at $(GCC_SRC)"

.PHONY: clean-gcc-src
clean-gcc-src:
	rm -rf $(GCC_SRC)

# ----------------------------------------------------------------------
# Chapter 132b: GMP / MPFR / MPC vendored as in-tree symlinks.
#
# GCC 14 needs all three at link time for arbitrary-precision
# arithmetic (constant folding, `__builtin_…` math, the
# overflow-in-integer-constant diagnostic).  GCC's own
# `contrib/download_prerequisites` script handles the same job;
# we don't run it because (a) we want sha256-pinned downloads
# captured in scripts/fetch_gcc_prereqs.sh, not a script that
# pulls whatever's at the upstream URL today, and (b) we want
# the downloads to be controllable from the same Makefile loop
# as everything else.
#
# The script downloads each tarball into vendor/, extracts it
# alongside gcc-14.2.0, and creates relative symlinks
# vendor/gcc-14.2.0/{gmp,mpfr,mpc} pointing at the extracted
# dirs.  When xgcc's configure (chapter 132c) sees those
# subdirs it switches into "in-tree" build mode and builds the
# math libs as part of `make all-gcc` itself.
#
# Variable $(GCC_PREREQS_MARKER) is declared early (near
# $(GCC_MARKER)) so the default `all` target can use it as a
# prereq.
# ----------------------------------------------------------------------
$(GCC_PREREQS_MARKER): $(GCC_MARKER)
	bash scripts/fetch_gcc_prereqs.sh

.PHONY: gcc-osdev-prereqs
gcc-osdev-prereqs: $(GCC_PREREQS_MARKER)
	@echo "gcc-osdev-prereqs: gmp/mpfr/mpc symlinked into $(GCC_SRC)"

.PHONY: clean-gcc-prereqs
clean-gcc-prereqs:
	rm -f $(GCC_PREREQS_MARKER)
	rm -f $(GCC_SRC)/gmp $(GCC_SRC)/mpfr $(GCC_SRC)/mpc
	rm -rf vendor/gmp-6.2.1 vendor/mpfr-4.1.0 vendor/mpc-1.2.1

# ----------------------------------------------------------------------
# Chapter 132c: cross-build aarch64-osdev-gcc.
#
# This is the actual compiler-build step.  We configure the patched
# gcc-14.2.0 source tree (chapter 132a) with our triple, point it at
# the binutils toolchain prefix (chapter 131a) so it picks up our
# aarch64-osdev-as / aarch64-osdev-ld, and run `make all-gcc` to
# produce a stage-1 host-built cross compiler.  We deliberately stop
# at `all-gcc` rather than going all the way to `make all` — the
# target libraries (libgcc, libstdc++, libatomic) need target headers
# and a working crt0 in places gcc's configure expects to find them,
# which is chapter 132d's job.  For now `aarch64-osdev-gcc -v` and
# `-dumpmachine` working is the whole contract.
#
# Configure flags worth calling out:
#   --enable-languages=c           we only need C right now; C++ is for
#                                   a later chapter when we want to
#                                   build C++ apps in-guest.
#   --disable-bootstrap            skip the 3-stage rebuild, just use
#                                   the host compiler once.  Cuts build
#                                   time by ~3x.  Fine for a cross.
#   --disable-multilib             only build aarch64; no 32-bit, no
#                                   different ABIs.
#   --disable-nls --disable-shared standard cross-toolchain hygiene.
#   --without-headers              tell gcc we don't have target libc
#                                   headers yet.  Combined with
#                                   --with-newlib it makes libgcc's
#                                   configure happy enough to generate
#                                   a Makefile (we don't actually build
#                                   libgcc this chapter).
#   --enable-checking=release      no assertions inside gcc itself.
#                                   ~20% faster build, same correctness
#                                   for our use case.
#
# Build dir lives at $(GCC_BUILD_HOST), separate from the source tree
# so we can `make clean-xgcc` without disturbing the patched source.
# Bulk of the disk hit is here (~2.5 GiB of intermediate objects after
# a fresh build).
# ----------------------------------------------------------------------
GCC_BUILD_HOST := $(BUILD)/gcc-build-host
GCC_XGCC       := $(TOOLCHAIN_PREFIX)/bin/aarch64-osdev-gcc

$(GCC_BUILD_HOST)/Makefile: $(GCC_PREREQS_MARKER) $(BINUTILS_AS) $(BINUTILS_LD)
	mkdir -p $(GCC_BUILD_HOST)
	cd $(GCC_BUILD_HOST) && \
	    PATH="$(TOOLCHAIN_PREFIX)/bin:$$PATH" \
	    ../../$(GCC_SRC)/configure \
	        --target=aarch64-osdev \
	        --prefix=$(TOOLCHAIN_PREFIX) \
	        --program-prefix=aarch64-osdev- \
	        --with-sysroot=$(TOOLCHAIN_PREFIX)/aarch64-osdev \
	        --enable-languages=c \
	        --disable-bootstrap \
	        --disable-multilib \
	        --disable-nls --disable-shared \
	        --without-headers --with-newlib \
	        --disable-libssp --disable-libquadmath \
	        --enable-checking=release \
	        --with-system-zlib

$(GCC_XGCC): $(GCC_BUILD_HOST)/Makefile
	@echo "xgcc: building (this takes ~5-10 minutes; log at $(GCC_BUILD_HOST)/.build.log)"
	PATH="$(TOOLCHAIN_PREFIX)/bin:$$PATH" \
	    $(MAKE) -C $(GCC_BUILD_HOST) -j$(shell sysctl -n hw.ncpu) all-gcc MAKEINFO=true \
	    > $(GCC_BUILD_HOST)/.build.log 2>&1
	$(MAKE) -C $(GCC_BUILD_HOST) MAKEINFO=true install-gcc \
	    >> $(GCC_BUILD_HOST)/.build.log 2>&1
	@test -x $@ || (echo "xgcc: build claimed success but $@ missing"; tail -40 $(GCC_BUILD_HOST)/.build.log; exit 1)
	@echo "xgcc: installed $@"

.PHONY: gcc-osdev
gcc-osdev: $(GCC_XGCC)
	@$(GCC_XGCC) -dumpmachine
	@$(GCC_XGCC) -dumpversion

.PHONY: clean-xgcc
clean-xgcc:
	rm -rf $(GCC_BUILD_HOST)
	rm -f $(GCC_XGCC)
	rm -rf $(TOOLCHAIN_PREFIX)/libexec/gcc/aarch64-osdev
	rm -rf $(TOOLCHAIN_PREFIX)/lib/gcc/aarch64-osdev
	rm -rf $(TOOLCHAIN_PREFIX)/aarch64-osdev

# ----------------------------------------------------------------------
# Chapter 132d: xgcc sysroot install.
#
# The real `aarch64-osdev-gcc` looks up `crt0.o` via STARTFILE_SPEC
# and `linker_user.ld` via LINK_SPEC (defined in
# vendor/gcc-14.2.0/gcc/config/aarch64/aarch64-osdev.h).  Both names
# resolve through gcc's standard cross-compiler search path, which
# includes $(TOOLCHAIN_PREFIX)/aarch64-osdev/lib/ — so we just copy
# the inputs there.  Libc headers go in aarch64-osdev/include/ which
# gcc adds to the default `-isystem` chain for cross builds.
#
# Same layout will be mirrored under /aarch64-osdev/{lib,include}/
# inside the guest when chapter 132e ships xgcc as /bin/gcc, so a
# single set of specs works on both host and target.
# ----------------------------------------------------------------------
XGCC_SYSROOT     := $(TOOLCHAIN_PREFIX)/aarch64-osdev
XGCC_SYSROOT_LIB := $(XGCC_SYSROOT)/lib
XGCC_SYSROOT_INC := $(XGCC_SYSROOT)/include
XGCC_SYSROOT_BIN := $(XGCC_SYSROOT)/bin

XGCC_SYS_CRT0    := $(XGCC_SYSROOT_LIB)/crt0.o
XGCC_SYS_LDS     := $(XGCC_SYSROOT_LIB)/linker_user.ld
XGCC_SYS_INC_MARKER := $(XGCC_SYSROOT_INC)/.osdev-libc-stamp
XGCC_SYS_BIN_MARKER := $(XGCC_SYSROOT_BIN)/.osdev-tools-stamp
XGCC_SYS_LIBC    := $(XGCC_SYSROOT_LIB)/libosdevc.a

$(XGCC_SYS_CRT0): $(BUILD)/userspace/crt/crt0.o | $(GCC_XGCC)
	@mkdir -p $(XGCC_SYSROOT_LIB)
	cp $< $@

$(XGCC_SYS_LDS): userspace/linker_user.ld | $(GCC_XGCC)
	@mkdir -p $(XGCC_SYSROOT_LIB)
	cp $< $@

# Glob libc headers at rule-eval time so adding/removing one is
# picked up next make.  The stamp file keeps make from re-copying
# on every invocation; touching any .h invalidates it.
$(XGCC_SYS_INC_MARKER): $(wildcard userspace/libc/*.h) | $(GCC_XGCC)
	@mkdir -p $(XGCC_SYSROOT_INC)
	cp userspace/libc/*.h $(XGCC_SYSROOT_INC)/
	@touch $@

# xgcc's tool search adds $prefix/aarch64-osdev/bin/ but looks for
# unprefixed names (as, ld, ar, ...) there.  Our binutils install
# lands as aarch64-osdev-as et al. in $prefix/bin/.  Symlink the
# unprefixed names into the target bin dir so gcc finds them
# without -B.  Same layout cross-binutils makes by default when
# building libbfd/libopcodes target-side; we don't, so we synth.
$(XGCC_SYS_BIN_MARKER): $(BINUTILS_AS) $(BINUTILS_LD) | $(GCC_XGCC)
	@mkdir -p $(XGCC_SYSROOT_BIN)
	@for tool in as ld ar nm objcopy objdump ranlib strip; do \
	    if [ -x $(TOOLCHAIN_PREFIX)/bin/aarch64-osdev-$$tool ]; then \
	        ln -sf ../../bin/aarch64-osdev-$$tool \
	            $(XGCC_SYSROOT_BIN)/$$tool; \
	    fi; \
	done
	@touch $@

# Chapter 132e: bundle libc extern wrappers (memcpy / memset /
# memmove / malloc / strlen / strerror / ... — all the symbols
# cstring.c defines non-static) into a sysroot archive so xgcc
# autoconf link tests (gmp, mpfr, mpc, gcc itself) resolve them.
# Plain .o linking pulls every symbol; an archive lets ld pick
# only the ones each conftest references.  Built with our own
# binutils ar to keep the object format compatible with the
# aarch64-osdev linker.
$(XGCC_SYS_LIBC): $(BUILD)/userspace/libc/cstring.o \
                  $(BUILD)/userspace/libc/cxxabi.o | $(GCC_XGCC)
	@mkdir -p $(XGCC_SYSROOT_LIB)
	$(TOOLCHAIN_PREFIX)/bin/aarch64-osdev-ar rcs $@ \
	    $(BUILD)/userspace/libc/cstring.o \
	    $(BUILD)/userspace/libc/cxxabi.o

.PHONY: xgcc-sysroot
xgcc-sysroot: $(XGCC_SYS_CRT0) $(XGCC_SYS_LDS) $(XGCC_SYS_INC_MARKER) \
              $(XGCC_SYS_BIN_MARKER) $(XGCC_SYS_LIBC)
	@echo "xgcc-sysroot: $(XGCC_SYSROOT_LIB)/{crt0.o,linker_user.ld,libosdevc.a} + $(XGCC_SYSROOT_INC)/*.h + $(XGCC_SYSROOT_BIN)/{as,ld,...} installed"

# Make `gcc-osdev` install the sysroot too, so `make gcc-osdev`
# leaves you with a self-contained compiler that can build hello.c
# end-to-end without the chapter-131b wrapper.
gcc-osdev: xgcc-sysroot

# ----------------------------------------------------------------------
# Chapter 131b: aarch64-osdev-cc target compiler wrapper.
#
# Bridges the gap until chapter 132 lands a real aarch64-osdev-gcc.
# Wraps the host's aarch64-elf-gcc with three overrides:
#   - -B points at our chapter 131a toolchain bin/, with `as` and
#     `ld` symlinked to aarch64-osdev-as / aarch64-osdev-ld so gcc
#     dispatches to our binutils, not its bundled aarch64-elf ones.
#   - -nostdlibinc kills aarch64-elf-gcc's bundled newlib system
#     headers.
#   - -isystem points header lookup at our libc (chapters 116-128).
#
# Why it's a generated file:  the wrapper needs an absolute path to
# the OSDEV_ROOT so it works no matter where in the workspace make
# is invoked from.  The template at scripts/aarch64-osdev-cc.in
# carries `@OSDEV_ROOT@` which `sed` substitutes at install time.
# ----------------------------------------------------------------------
OSDEV_CC          := $(TOOLCHAIN_PREFIX)/bin/aarch64-osdev-cc
OSDEV_CC_TEMPLATE := scripts/aarch64-osdev-cc.in
OSDEV_AS_SYMLINK  := $(TOOLCHAIN_PREFIX)/bin/as
OSDEV_LD_SYMLINK  := $(TOOLCHAIN_PREFIX)/bin/ld

$(OSDEV_CC): $(OSDEV_CC_TEMPLATE) | $(BINUTILS_AS)
	sed 's|@OSDEV_ROOT@|$(abspath .)|g' $(OSDEV_CC_TEMPLATE) > $@.tmp
	chmod +x $@.tmp
	mv $@.tmp $@

# Symlinks live next to the prefixed binaries.  GCC's -B prefix
# lookup tries unprefixed `as` and `ld` first; that's what these
# satisfy.  We use the prefixed versions as the symlink targets so
# `aarch64-osdev-as` and `aarch64-osdev-ld` remain the canonical
# names for direct invocation.
$(OSDEV_AS_SYMLINK): $(BINUTILS_AS)
	ln -sf aarch64-osdev-as $@

$(OSDEV_LD_SYMLINK): $(BINUTILS_LD)
	ln -sf aarch64-osdev-ld $@

.PHONY: aarch64-osdev-cc-install
aarch64-osdev-cc-install: $(OSDEV_CC) $(OSDEV_AS_SYMLINK) $(OSDEV_LD_SYMLINK)
	@echo "aarch64-osdev-cc-install: installed $(OSDEV_CC)"
	@echo "aarch64-osdev-cc-install:   as -> $$(readlink $(OSDEV_AS_SYMLINK))"
	@echo "aarch64-osdev-cc-install:   ld -> $$(readlink $(OSDEV_LD_SYMLINK))"

# ----------------------------------------------------------------------
# Guest GCC cross-build (chapter 132c-132i)
#
# The build/gcc-build-guest/ tree contains three things we ship
# onto /bin in the OSFS image:
#
#   * xgcc                              -> /bin/gcc
#   * cc1                               -> /bin/cc1
#   * gcc/gcc/include/{stdint,stddef,..} -> /bin/{stdint,stddef,..}
#
# All three are produced by `make all-gcc` inside the cross-build
# tree.  scripts/test_guest_gcc.py drives that build (Phases 1-4
# from chapters 132c-132h), so we route the make-side dependency
# through a single stamp file and let the script do the work.
#
# KEEP=1 is critical: without it the script wipes its build tree
# every run, turning every `make build/disk.img` into a multi-hour
# rebuild loop.  With it, an unchanged tree is a near-no-op (the
# script still walks its phases but each one finds its outputs
# already present and skips ahead).
#
# `make clean` preserves $(BUILD)/gcc-build-guest/ (parity with
# binutils-build, see the clean: rule below), so this stamp rule
# normally fires exactly once on a fresh checkout; subsequent
# `make clean run-graphical` cycles re-use the existing tree.
#
# Placement: this block lives here, not next to GCC_FREESTANDING_*
# at line ~2290, because make expands $(...) in prerequisite
# lists at PARSE time (chapter `makefile-pattern-rule-ordering`
# in repo memory), so the rule must come after every variable it
# references (GCC_XGCC, OSDEV_CC, GCC_PREREQS_MARKER) is defined.
# ----------------------------------------------------------------------
GCC_GUEST_BUILD := $(BUILD)/gcc-build-guest
GCC_GUEST_STAMP := $(GCC_GUEST_BUILD)/.osdev-built-stamp

$(GCC_GUEST_STAMP): $(GCC_XGCC) $(OSDEV_CC) $(GCC_PREREQS_MARKER) \
                    $(BUILD)/userspace/crt/crt0.o
	@# If a pre-existing $(GCC_GUEST_BUILD) tree has no real xgcc
	@# inside it (typical after a Makefile change wrote placeholder
	@# stub headers into gcc/gcc/include/), it's not a usable
	@# resumable build -- wipe so test_guest_gcc.py KEEP=1 starts
	@# clean.  We check XGCC presence rather than directory
	@# existence so a real partial build (e.g. interrupted Phase 4)
	@# still gets resumed.
	@if [ -d $(GCC_GUEST_BUILD) ] && [ ! -f $(XGCC_GUEST_BIN) ]; then \
	    echo "gcc-build-guest: pre-existing tree has no xgcc; wiping for clean build"; \
	    rm -rf $(GCC_GUEST_BUILD); \
	fi
	@echo "gcc-build-guest: cross-building xgcc + cc1 + freestanding headers"
	@echo "gcc-build-guest:   (first run is ~hours; subsequent runs are incremental)"
	KEEP=1 python3 scripts/test_guest_gcc.py
	@test -f $(XGCC_GUEST_BIN) || \
	    (echo "gcc-build-guest: $(XGCC_GUEST_BIN) missing after build"; exit 1)
	@test -f $(CC1_GUEST_BIN) || \
	    (echo "gcc-build-guest: $(CC1_GUEST_BIN) missing after build"; exit 1)
	@test -f $(GCC_FREESTANDING_DIR)/stdint.h || \
	    (echo "gcc-build-guest: $(GCC_FREESTANDING_DIR)/stdint.h missing after build"; exit 1)
	@touch $@

# All three sets of outputs are produced by the stamp rule above;
# empty recipes here tell make "this file is produced as a side
# effect of $(GCC_GUEST_STAMP)'s recipe -- don't try to remake it
# individually".
$(XGCC_GUEST_BIN): $(GCC_GUEST_STAMP) ;
$(CC1_GUEST_BIN):  $(GCC_GUEST_STAMP) ;
$(GCC_FREESTANDING_DIR)/%.h: $(GCC_GUEST_STAMP) ;

.PHONY: gcc-build-guest
gcc-build-guest: $(GCC_GUEST_STAMP)
	@echo "gcc-build-guest: $(GCC_GUEST_BUILD) up to date"

# ----------------------------------------------------------------------
# Clean
#
# Wipes the kernel/userspace/ramfs build outputs but preserves the
# host cross toolchain (build/toolchain/) and the binutils build
# directory (build/binutils-build/) — both are expensive to rebuild
# and aren't part of the inner kernel-iteration loop.  Use
# `make clean-binutils` to nuke those explicitly.
#
# Also preserved: the xgcc cross-build (build/gcc-build-guest/),
# produced by scripts/test_guest_gcc.py.  Same rationale — multi-
# hour rebuild, supplies xgcc + cc1 + freestanding headers shipped
# onto /bin (chapter 132i).  Use `rm -rf build/gcc-build-guest` to
# nuke it explicitly.
# ----------------------------------------------------------------------
.PHONY: clean
clean:
	@find $(BUILD) -mindepth 1 -maxdepth 1 \
	    ! -name toolchain ! -name binutils-build ! -name gcc-build-guest \
	    -exec rm -rf {} + 2>/dev/null || true
