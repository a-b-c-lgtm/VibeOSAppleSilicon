# Chapter 1 — Why Apple Silicon, why aarch64, why now

> **Milestone in this chapter:** none (orientation only).
> **Code referenced:** none yet — the first lines of code appear in
> chapter 3.
>
> **At the end of this chapter** you will understand the choices that
> shape every later one: why the target is AArch64, why the host is
> Apple Silicon, what the book commits to teach, and what it
> explicitly does not.

## What this book is

This book is a guide. By the time you reach the last chapter you will
have written — yourself, line by line — an AArch64 operating system
that boots on a QEMU virtual machine accelerated by Apple's
hypervisor, mounts a real filesystem, runs a desktop with its own
window manager and a small collection of applications, and includes a
working web browser that fetches pages over its own TCP/IP stack and
renders them into its own framebuffer. By Part XVIII you will also
have a real GCC running inside the guest, rebuilding Doom from source
without leaving QEMU.

The kernel is monolithic, written in C, and weighs in around twelve
thousand lines once it has grown a scheduler, an MMU, a VFS, virtio
drivers, a window manager, and a TCP stack. It is not Linux, it is
not BSD, it is not Minix. It is its own system, designed to be small
enough to read in a weekend but real enough to host its own
toolchain.

## Why AArch64

Most existing "build a hobby OS" books target x86-64. There are good
historical reasons for that — x86-64 was for a long time the only
architecture you could realistically emulate with full fidelity on
commodity hardware, and most of the existing teaching material lives
in that ecosystem.

But x86-64 carries thirty-five years of legacy decisions in its boot
flow alone. The simplest "hello, world" kernel on that platform has
to:

- Be loaded by a BIOS or UEFI firmware blob whose interface is
  effectively undocumented in modern terms.
- Start in 16-bit real mode and be transitioned manually through
  protected mode and long mode.
- Set up the A20 line, the GDT, the TSS, and the IDT.
- Talk to the 8259 PIC, a chip designed in 1978, before any
  reasonable interrupt model can be brought online.
- Discover memory through ACPI, a specification large enough to fill
  a small library, even though the only thing the OS needs to know
  is "where is RAM".

None of that hardware exists in any conceptual sense on a modern
ARMv8-A core. AArch64 boots already in 64-bit mode, has no segments,
has no separate IDT or GDT (just a single vector-table register), and
discovers memory and devices through a small flat blob of data called
a *device tree* that is typically a few kilobytes. The first
"hello, world" kernel in this book is around fifty lines of assembly
and seventy lines of C.

## Why Apple Silicon

The other reason AArch64 makes sense in 2026 is that the modal laptop
a hobbyist OS developer is sitting in front of is now an Apple
Silicon Mac. M-series chips are AArch64 cores, so when you run an
AArch64 guest on them you can use Apple's hypervisor framework
(`Hypervisor.framework`, abbreviated HVF) to execute every guest
instruction directly on the host CPU, with no software emulation.

The performance difference is substantial. On a 2023 MacBook Pro M2:

| Workload                          | TCG (x86-64 guest) | HVF (AArch64 guest) | Speedup |
|-----------------------------------|--------------------|---------------------|---------|
| `make all` for a small kernel     | 2.1 s              | 0.2 s               | ~10×    |
| Boot to login prompt              | 4.3 s              | 0.4 s               | ~10×    |
| HTML parse + layout (40 KiB page) | 8.7 s              | 0.3 s               | ~30×    |
| GCC self-host (Part XVIII)        | impractical        | minutes             | ∞       |

Beyond raw speed, HVF gives you usable SMP. The QEMU x86-64-on-ARM
TCG path can technically use multiple host threads but loses most of
the win to atomic-operation translation overhead; HVF passes ARM
atomics straight through.

The cost is that you must build the kernel for AArch64. That cost,
as it happens, is also the benefit that removes most of the legacy
hardware tax above.

## What this book is not

A few clarifications, so you do not feel mid-reading that something
was promised that you are not getting.

This book does **not** teach:

- 32-bit ARM (AArch32). It is its own large topic and is no longer
  the architecture used by any new Apple Silicon device.
- AArch64 EL2 hypervisor work. EL2 is reserved for HVF on Apple
  hardware, and learning EL2 requires either a non-Apple host or a
  nested-virtualization path, which is fragile.
- TrustZone, secure-world programming, or anything in EL3.
- Microkernels, capability systems, or formally verified designs.
  Those are real and important, but the contrast they offer is best
  appreciated *after* you have built a monolithic kernel and felt
  where its design pulls hardest on you.

The early chapters also do not write a complete libc. The first
userspace program in chapter 15 uses the same inline-asm syscall
stubs as the milestone-0 kernel's UART driver. A real libc grows
organically — `printf` arrives in chapter 28 when the shell demands
formatted output, `strtod` and friends arrive when the browser
chapter starts parsing CSS lengths, and a POSIX-shaped libc with
`stdio`, `errno`, `stat`, and the rest lands across chapters
116–117 once we begin building a compiler.

## How to read this book

Three reading modes:

1. **Linear, with the codebase.** Read each chapter, build the
   matching milestone tag of the kernel, run it under QEMU, and
   confirm you see what the chapter says you should. Move on. This
   is the intended path.
2. **Skim then deep-dive.** Read Part I in full, skim Parts II–IV,
   then dive into the part that interests you. The book's parts are
   deliberately self-contained: networking does not depend on
   graphics, the browser depends on most things but is its own
   readable arc, and the compiler in Part XVII can be read after
   anything in Parts IV–XII.
3. **As a reference.** Use the [book index](../../INDEX.md) to jump
   directly to a topic. Each chapter opens with a self-contained
   summary callout and lists the source files it touches.

## The codebase contract

For every milestone, the codebase guarantees:

- A clean build under `make all` with `-Werror` on macOS Apple
  Silicon with the Homebrew `aarch64-elf` cross toolchain.
- A working `make run` under HVF that exhibits the behaviour the
  corresponding chapter promises.
- A `make run-tcg` fallback so you can build and run on non-Apple
  hosts (slower, but useful for CI).
- A `make debug` target that exposes a GDB stub on `tcp::1234`.

If a chapter and the codebase ever disagree, the codebase is
authoritative.

## What you need

Before reading chapter 2, you need:

- A Mac with Apple Silicon (M1 or later) running macOS 13 or newer.
- Homebrew installed.
- Around 10 GB of free disk space (more for Part XVIII).
- Familiarity with C, with reading assembly given a reference, and
  with `gdb`.
- Patience for one slow chapter (chapter 6, the MMU) and one fiddly
  chapter (chapter 11, the context switch). Everything else is
  easier than it looks from outside.

You do **not** need:

- An existing OS-development background.
- A printed copy of the ARMv8-A architecture reference manual. The
  chapters cite the relevant subsections inline, and the manual is
  freely downloadable from arm.com when you want it.
- Real ARM hardware. Everything in the book runs under QEMU; the
  appendix has notes for those who want to cross-flash to a
  Raspberry Pi 4 or 5 later.

Turn the page when you are ready.
