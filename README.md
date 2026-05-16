# Building a Hobby Operating System on Apple Silicon

This repository is the source code and book for an aarch64 hobby
operating system that boots natively on Apple Silicon Macs (M1, M2,
M3, M4 and later) under QEMU's HVF accelerator.

## Why aarch64? Why HVF?

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
```

Press `Ctrl-A X` (in the terminal hosting the serial) or close the QEMU
window to quit.

## Where to start reading

- [Book index](book/INDEX.md)
- [Chapter 1: Why Apple Silicon, why aarch64, why now](book/chapters/01-foundations/01-why-apple-silicon.md)

## Project status

The project is mid-port from x86-64 to aarch64.  The original x86-64
codebase lives unchanged under [VibeOS/](VibeOS/) for reference.

- Milestone 0 (boot + UART) — complete
- Milestone 1 (exception vectors + MMU + GIC) — complete
- Milestone 2 (timer + threads + scheduler) — complete
- Milestone 3 (heap + syscalls + ELF + VFS) — complete
- Milestone 4 (init/spawn/wait + shell) — complete
- Milestone 5–7 (virtio-blk, OSFS, block cache) — complete
- Milestone 8–16 (per-process AS, user heap, argv, printf, ls) — complete
- Milestone 17–23 (uptime, time, cwd, env, var-expansion, quoting) — complete
- Milestone 24–37 (bigger fs + tools, redirection, pipes, tmpfs, raw
  TTY, readline, kill ring, signals) — complete
- **Milestone 38 (virtio-gpu framebuffer + 8x16 font)** — complete
- **Milestone 39 (virtio-input keyboard via evdev)** — complete
- **Milestone 40 (in-kernel WM + 7 GUI syscalls + hellogui demo)** — complete
- **Milestone 41 (virtio-tablet mouse + WM focus/drag/close + paint demo)** — complete
- **Milestone 42 (gui_term: terminal-in-a-window via pipe + spawn_pipe + wait)** — complete
- **Milestone 43 (notepad: GUI text editor + writable-tmpfs save)** — complete
- **Milestone 44 (launcher: clickable mouse-driven app launcher)** — complete
- **Milestone 45 (WM z-order bug fix + painter's algorithm hardening)** — complete
- **Milestone 46 (boot to desktop: auto-spawn launcher + gradient wallpaper)** — complete
- **Milestone 47 (taskbar + 3 GUI syscalls: list/raise/create-ex + always-on-top)** — complete
- **Milestone 48 (clock in the taskbar)** — complete
- **Milestone 49 (toast notifications + child-reap fix)** — complete
- **Milestone 50 (userspace wallpaper + yield/IRQ race fix)** — complete
- **Milestone 51 (window minimize / restore via title-bar button + taskbar toggle)** — complete
- **Milestone 52 (virtio-net driver + in-kernel ARP self-test against SLIRP gateway)** — complete
- **Milestone 53 (Ethernet + ARP cache + IPv4 framing in `kernel/core/net.{c,h}`)** — complete
- **Milestone 54 (ICMP echo + UDP + DHCP client; boots to a DHCP-acquired IP)** — complete
- **Milestone 55 (TCP client: state machine + buffered conn + boot HTTP self-test)** — complete
- **Milestone 56 (socket syscalls — `FD_SOCKET`, `SYS_SOCKET_CONNECT/STATE/SHUTDOWN` — and `/bin/httpget` userspace tool)** — complete
- **Milestone 57 (DNS resolver: DHCP option-6 capture, `dns_resolve`, `SYS_RESOLVE`, `httpget hostname` support)** — complete
- **Milestone 58 (URL + HTTP/1.1 parser in userspace libc + `httpget <url>` form with one-hop redirect; legacy 3-arg form preserved)** — complete
- **Milestone 59 (HTML5 tokenizer in userspace libc + `/bin/htmltok` driver; first browser brick)** — complete
- **Milestone 60 (DOM tree builder in userspace libc + `/bin/htmldom` driver; OSFS dir capacity 32 → 64)** — complete
- **Milestone 61 (CSS parser + selector matcher in userspace libc + `/bin/cssparse` driver)** — complete
- **Milestone 62 (CSS-driven layout engine in userspace libc + `/bin/layout` driver; cascade with three origins, anonymous-block wrap, margin collapsing, inline wrap with text-align justify, paint command stream)** — complete
- **Milestone 63 (`/bin/browser`: paint/plain/ANSI/GUI modes; resizable WM windows + `GUI_EVENT_RESIZE` + horizontal scroll for pages wider than the viewport; live re-layout on resize)** — complete
- Milestone 64+ (image decoding, hit-testing, host TLS bridge) — to port

See the [book index](book/INDEX.md) for the full roadmap.
