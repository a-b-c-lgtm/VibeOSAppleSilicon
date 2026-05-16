# Building a Hobby Operating System on Apple Silicon

This repository is the source code and book for an AArch64 hobby
operating system that boots natively on Apple Silicon Macs (M1, M2,
M3, M4 and later) under QEMU's HVF accelerator. It runs as a
monolithic kernel on QEMU's `virt` machine, talks to the world over
virtio-mmio devices, and ships with a GUI desktop, a TCP/IP stack,
and a small web browser — all written in C and a little AArch64
assembly, all built in this tree.

The accompanying book — *Building a Hobby Operating System on Apple
Silicon* — walks through every milestone in the order it was
implemented, with each chapter pointing at the exact files and
regression test that prove the milestone works. Read it as a tour
of the codebase, or use it as a recipe to build the same OS yourself.

![Desktop screenshot: launcher, notepad editing a text file, gui_term
running ps, and two browser windows on Hacker News and a PNG colour-swatch
test page, over a flowers wallpaper with a taskbar and live clock.](assets/screenshots/final_screenshot.png)

The screenshot above is `make run-graphical` on an M2: the launcher
in the top-left, `notepad` editing a tmpfs file, `gui_term` running
`ps`, and two `/bin/browser` windows — one on Hacker News, one on a
PNG colour-swatch test page — all composited by the in-kernel WM
over a userspace-rendered wallpaper, with the taskbar and live
clock along the bottom.

## Why AArch64? Why HVF?

Most "build a hobby OS" books target x86-64 because that is what the
authors learned on. On a MacBook Pro M2 (and every other Apple
Silicon Mac), running an x86-64 guest means running every guest
instruction through QEMU's TCG software translator — so even the
fastest part of your kernel runs perhaps 1/20th the speed of the
hardware sitting an inch away. Worse, you cannot use SMP usefully,
graphics is slow, and the boot pipeline is dominated by 1990s-era
legacy decisions (BIOS, A20, GDT, TSS, real mode, the 8259 PIC) that
have nothing to teach a modern OS engineer.

This book takes a different bet: build an aarch64 kernel from the
start, run it on QEMU's `virt` machine, and accelerate every guest
instruction through Apple's `Hypervisor.framework` (HVF). The result
is a kernel that boots in under a second on an M2, can usefully
schedule real SMP, draws into a high-resolution framebuffer through
virtio-gpu, and has zero legacy 1980s baggage.

It is also, accidentally, the better way to teach the subject. There
are no segments, no real-mode dance, no GDT or TSS, no 8259 PIC, no
A20 line, and no ACPI tangle. Every chapter teaches a feature of
modern systems hardware, not a workaround for legacy hardware.

## Reading mode

This is a book and a codebase that move together. The expected
workflow is:

1. Read a chapter.
2. Build the corresponding milestone tag of the kernel.
3. Run it under QEMU and confirm the behaviour the chapter promises.
4. Move on.

You do not need prior operating-systems-development experience, but
you should be comfortable reading C, reading assembly with a
reference open, and using a debugger.

## Quick start

```bash
brew install aarch64-elf-gcc aarch64-elf-binutils qemu

make toolchain-check
make all

# Text-mode (serial only):
make run

# Graphical (Cocoa window + virtio-gpu, 1920x1080 by default):
make run-graphical
# Or pick a smaller framebuffer:
make run-graphical FB_RES=1280x800

# Force the TCG software interpreter (e.g. on a non-Apple-Silicon Mac):
make run-tcg
```

Press `Ctrl-A X` (in the terminal hosting the serial) or close the QEMU
window to quit.

The regression sweep that gates every chapter lives in `scripts/`:

```bash
for t in scripts/test_*.py; do python3 "$t" || break; done
```

## Where to start reading

- [Book index](book/INDEX.md) — master table of contents and status
  table, broken into fifteen parts.
- [Chapter 1: Why Apple Silicon, why aarch64, why now](book/chapters/01-foundations/01-why-apple-silicon.md)
  — the design argument in long form.
- [Chapter 3: First boot](book/chapters/01-foundations/03-first-boot.md)
  — the smallest thing that prints to the UART.

## What works today

Booting `make run-graphical` drops you onto a desktop with a
wallpaper, a taskbar with a live clock, and a launcher you can click
to open windowed apps:

- **GUI desktop** — in-kernel window manager, virtio-gpu framebuffer,
  virtio-keyboard and virtio-tablet input, focus/drag/close/minimize,
  taskbar with window list, toast notifications, userspace wallpaper.
- **GUI applications** — `notepad` (text editor with writable
  tmpfs save), `paint`, `gui_term` (a terminal in a window),
  `launcher`, `taskbar`, `browser`.
- **Shell and userland** — a line-editor shell with kill-ring
  readline keys, env vars, quoting, `cd`/`pwd`, pipelines,
  redirection (`<`, `>`, `>>`), background jobs, and the usual
  small tools (`ls`, `cat`, `grep`, `wc`, `head`, `tail`, `echo`,
  `env`, `sleep`, `uptime`, `time`).
- **Filesystems** — a read-only on-disk OSFS plus a writable
  in-memory `tmpfs`, fronted by a block cache over virtio-blk.
- **Networking** — virtio-net, full Ethernet/ARP/IPv4 stack,
  ICMP echo, UDP, a DHCP client, a TCP client with a real state
  machine, BSD-shaped socket syscalls, a DNS resolver, and a URL
  / HTTP/1.1 parser. `/bin/httpget http://example.com/` works.
- **Browser** — `/bin/browser` parses HTML, builds a DOM, parses
  CSS, runs a CSS-driven layout engine, and renders the result
  in four modes: `paint` (raw paint commands), `plain`, `ANSI`,
  and `GUI` (resizable window, address bar, back/forward,
  click-to-navigate).
- **SMP and memory** — two cores brought up via PSCI, atomics
  and spinlocks, IPIs through GICv3 SGIs, a per-CPU runqueue,
  `mmap` over a unified page cache, userspace threads via
  `clone`, `CLONE_FILES` with a refcounted FD table, and a
  dedicated browser parser thread that takes HTML/CSS/layout
  off the GUI core.
- **System services** — a real RTC and wall-clock time,
  virtio-snd boot chime and `beep`, PNG decoding (truecolour,
  palette, grayscale) with a content-type sniffer feeding the
  browser image cache, a `/proc`-shaped pseudo-filesystem with
  `ps` and `top`, and `strace` over `/proc/<pid>/trace`.

## Project status

The codebase tracks the book chapter by chapter. As of chapter 100
(`/bin/strace` and `/proc/<pid>/trace`), every part through
XII — System Services and Polish — is shipped end to end and
gated by a regression test in `scripts/`. The headline gaps still
open on the roadmap are:

- **Part IX — Process Model.** A POSIX-shaped `fork`/`exec`/COW
  pair with signals and job control. The kernel already does
  enough of `fork` for `/bin/strace` to attach to a child via
  fork-then-`SYS_TRACE_ME`-then-`execv`; the chapter sequence
  for the full process model and the retirement of `spawn` is
  still being written.
- **Part X — Persistence.** A writable on-disk filesystem with a
  small journal so `notepad` saves, shell history, and browser
  cookies survive reboot. Today writes go to in-memory `tmpfs`.
- **Part XIII — TCP server and `/bin/httpd`.** Passive open,
  `accept()`, and the browser fetching from its own kernel.
- **Part XIV — Browser Maturation.** HTML forms, cookies,
  same-origin policy, a pocket-sized JS, and a TLS bridge so
  the browser can speak `https://`.
- **Part XV — Filesystem Architecture.** A mount table and
  `struct fs_ops` vtable, then a 9P-shaped userspace filesystem
  server protocol.

See the [project status snapshot](book/INDEX.md#project-status-snapshot)
in the book index for the canonical chapter-by-chapter table.
