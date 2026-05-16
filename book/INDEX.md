# Book Index

This is the master table of contents for *Building a Hobby Operating
System on Apple Silicon*. Chapters are grouped into eight parts; read
them in order on your first pass, then use the index as a reference.

## Reader contract

This book teaches one operating system, not a family of alternatives:

- **Guest architecture:** AArch64 (ARMv8-A, little-endian, EL1 only).
- **Host system:** macOS on Apple Silicon (M1 or later).
- **Emulator:** QEMU's `virt` machine.
- **Accelerator:** Apple's `Hypervisor.framework` (HVF), giving native
  ARM speed for the guest.
- **Boot path:** QEMU `-kernel` ELF protocol; later, U-Boot or Limine
  for booting on real hardware.
- **Implementation language:** C and a small amount of AArch64
  assembly.
- **Kernel design:** Monolithic, incremental, single-source.
- **Devices:** virtio-mmio everywhere we can — virtio-blk for
  persistent storage, virtio-net for networking, virtio-gpu for the
  framebuffer, virtio-keyboard and virtio-tablet for input. No
  legacy bus emulation (no PS/2, no AHCI, no PCI, no PIC).

The book deliberately avoids 32-bit ARM, AArch64-EL2 hypervisor
work, and TrustZone. Those are good follow-ups, not fundamentals.

## Parts

### Part I — Foundations

The first three chapters track milestone 0 of the codebase: boot to a
serial banner under HVF.

1. [Why Apple Silicon, why aarch64, why now](chapters/01-foundations/01-why-apple-silicon.md)
2. [Toolchain and host setup](chapters/01-foundations/02-toolchain-and-host-setup.md)
3. [First boot: QEMU virt, the boot stub, and PL011 UART](chapters/01-foundations/03-first-boot.md)

### Part II — Memory and Exceptions

Milestone 1 of the codebase. AArch64 forces these to land together
because you cannot run real C with the MMU off (see the milestone-0
caveat in `kernel/core/main.c`).

4. [The AArch64 execution environment (exception levels, registers, PSTATE)](chapters/02-memory/04-execution-environment.md)
5. [Exception vectors, ESR, FAR, and synchronous fault handling](chapters/02-memory/05-exception-vectors.md)
6. [The MMU, translation tables, and MAIR](chapters/02-memory/06-mmu-and-page-tables.md)
7. [Physical memory and the device tree](chapters/02-memory/07-physical-memory-and-device-tree.md)
8. Higher-half kernel and TTBR1 — *deferred until needed; see ch 7 epilogue*

### Part III — Time and Concurrency

Milestone 2. The GIC and the generic timer let us take interrupts
and tick.

9. [GIC v3 fundamentals](chapters/03-time-and-concurrency/09-gic-v3.md)
10. [The ARM generic timer](chapters/03-time-and-concurrency/10-arm-generic-timer.md)
11. [Threads and the AArch64 context switch](chapters/03-time-and-concurrency/11-threads-and-context-switch.md)
12. [Preemptive scheduling](chapters/03-time-and-concurrency/12-preemption.md)

### Part IV — Userspace

Milestones 3 and 4. SVC syscalls, ELF loading, the first user
program, the file system, and the shell.

13. [The kernel heap](chapters/04-userspace/13-kernel-heap.md)
14. [SVC and the syscall ABI](chapters/04-userspace/14-svc-and-syscalls.md)
15. [ELF loading and the first user program](chapters/04-userspace/15-elf-and-first-user-program.md)
16. [Files, VFS, and a tiny ramfs](chapters/04-userspace/16-files-and-vfs.md)
17. [`init`, `spawn`, `wait`: the simplest process model that works](chapters/04-userspace/17-init-spawn-wait.md)
18. [Console keyboard input and a line-mode shell](chapters/04-userspace/18-keyboard-and-shell.md)

### Part V — Devices

Milestones 5 and 6. virtio-mmio for everything.

19. [virtio-mmio: bus, queues, and the modern transport](chapters/05-devices/19-virtio-mmio.md)
20. [virtio-blk and persistent storage](chapters/05-devices/20-virtio-blk.md)
21. [A read-only on-disk filesystem and `cat /mnt/...`](chapters/05-devices/21-osfs-and-mount.md)
22. [Loading user binaries from disk](chapters/05-devices/22-binaries-on-disk.md)
23. [A block cache in front of virtio-blk](chapters/05-devices/23-block-cache.md)
24. [Per-process address spaces](chapters/05-devices/24-per-process-address-spaces.md)
25. [Hardening the kernel/user boundary](chapters/05-devices/25-kernel-user-boundary.md)
26. [A user heap via sbrk](chapters/05-devices/26-user-heap.md)
27. [argc, argv, and the user stack](chapters/05-devices/27-argc-argv.md)
28. [A printf for the user libc](chapters/05-devices/28-printf.md)
29. [Browsing the namespace: SYS_LISTDIR and ls](chapters/05-devices/29-listdir-and-ls.md)
30. [uptime and a real shell PATH](chapters/05-devices/30-uptime-and-path.md)
31. [A `time` builtin for the shell](chapters/05-devices/31-time-builtin.md)
32. [Per-process cwd: cd, pwd, dynamic prompt](chapters/05-devices/32-cwd-cd-pwd.md)
33. [Environment variables and a real PATH walk](chapters/05-devices/33-env-vars-and-path.md)
34. [Variable expansion and ./prog](chapters/05-devices/34-var-expansion-and-relative-paths.md)
35. [Shell quoting](chapters/05-devices/35-shell-quoting.md)
36. [Bigger filesystem and four classic tools](chapters/05-devices/36-bigger-fs-and-four-tools.md)
37. [Input redirection](chapters/05-devices/37-input-redirection.md)
38. [Sleep and the THREAD_SLEEPING state](chapters/05-devices/38-sleep-and-blocking.md)
39. [Kernel pipes, dup2, and THREAD_BLOCKED](chapters/05-devices/39-pipes.md)
40. [Shell pipelines: cat | grep | wc](chapters/05-devices/40-shell-pipelines.md)
41. [Writable tmpfs and `>` output redirection](chapters/05-devices/41-writable-tmpfs-and-output-redirection.md)
42. [tmpfs polish: `>>`, `ls /tmp/`, `rm`](chapters/05-devices/42-tmpfs-polish.md)
43. [Raw TTY mode and the shell line editor](chapters/05-devices/43-raw-tty-and-line-editor.md)
44. [Cursor movement and readline keybindings](chapters/05-devices/44-cursor-and-readline-keys.md)
45. [Kill ring: Ctrl-K, Ctrl-U, Ctrl-W, Ctrl-Y](chapters/05-devices/45-kill-ring-and-yank.md)
46. [virtio-gpu: a framebuffer at native resolution](chapters/05-devices/46-virtio-gpu-framebuffer.md)
47. [virtio-input: an evdev keyboard for the GUI](chapters/05-devices/47-virtio-input-keyboard.md)

### Part VI — GUI

Milestone 7. The compositor, window server, toolkit, and desktop.

48. [An in-kernel window manager and seven GUI syscalls](chapters/06-gui/48-window-manager-and-gui-syscalls.md)
49. [virtio-tablet, mouse focus, drag, and close](chapters/06-gui/49-virtio-tablet-and-wm-mouse.md)
50. [gui_term: a terminal in a window, and the synchronous-pipe spawn pattern](chapters/06-gui/50-gui-terminal-and-pipe-spawn.md)
51. [notepad: a real text editor in a window, and the writable-tmpfs round-trip](chapters/06-gui/51-notepad-and-tmpfs-roundtrip.md)
52. [launcher: clicking is the new typing](chapters/06-gui/52-launcher-and-click-to-spawn.md)
53. [A WM rendering bug, surfaced by the launcher](chapters/06-gui/53-wm-z-order-bug.md)
54. [Boot to desktop: auto-spawn the launcher and a gradient wallpaper](chapters/06-gui/54-boot-to-desktop.md)
55. [A taskbar, three new GUI syscalls, and a real desktop](chapters/06-gui/55-taskbar-and-window-list.md)
56. [A clock in the taskbar](chapters/06-gui/56-clock-in-taskbar.md)
57. [Toast notifications and proper child reaping](chapters/06-gui/57-toast-notifications.md)
58. [A userspace wallpaper, and the yield/IRQ race it uncovered](chapters/06-gui/58-userspace-wallpaper-and-yield-race.md)
59. [Window minimize and restore](chapters/06-gui/59-window-minimize-restore.md)

### Part VII — Networking

Milestone 8. virtio-net, then a vertical slice through Ethernet,
IPv4, ICMP/UDP/DHCP, and TCP with a BSD-shaped socket API.

60. [virtio-net: getting bytes on and off the wire](chapters/07-networking/60-virtio-net.md)
61. [Ethernet, ARP, and IPv4](chapters/07-networking/61-ethernet-arp-ipv4.md)
62. [ICMP, UDP, and DHCP](chapters/07-networking/62-icmp-udp-dhcp.md)
63. [TCP and a kernel-side socket API](chapters/07-networking/63-tcp-and-sockets.md)
64. [Socket syscalls and a userspace `httpget`](chapters/07-networking/64-socket-syscalls-and-httpget.md)
65. [DNS resolver](chapters/07-networking/65-dns-resolver.md)
66. [URL and HTTP parser](chapters/07-networking/66-url-and-http-parser.md)

### Part VIII — Browser

Milestone 9. The capstone: a working text-mode browser inside our
own kernel, on our own GUI, over our own TCP/IP stack.

67. [The HTML tokenizer](chapters/08-browser/67-html-tokenizer.md)
68. [DOM construction](chapters/08-browser/68-dom-construction.md)
69. [A tiny CSS parser](chapters/08-browser/69-css-parser.md)
70. [Block and inline layout](chapters/08-browser/70-block-and-inline-layout.md)
71. [/bin/browser — paint, plain, ANSI, GUI](chapters/08-browser/71-bin-browser.md)
72. Host-side TLS bridge — *deferred indefinitely; insecure http:// only for now*

### Part IX — Finishing the Process Model

Signals, fork, exec, and job control — the Unix process model the
book has been deferring since chapter 17.

72. [Why fork (and not just spawn)](chapters/09-process-model/72-why-fork-vs-spawn.md)
73. [fork on AArch64: the address-space copy](chapters/09-process-model/73-aarch64-fork-and-as-copy.md)
74. [exec: tearing down and rebuilding an AS in place](chapters/09-process-model/74-exec-and-as-rebuild.md)
75. [Copy-on-write: making fork cheap](chapters/09-process-model/75-copy-on-write.md)
76. [Signals, starting with SIGINT](chapters/09-process-model/76-signals-sigint.md)
77. [Catching signals: sigaction, masks, EINTR](chapters/09-process-model/77-sigaction-and-eintr.md)
78. [SIGCHLD and waitpid: parent-child plumbing](chapters/09-process-model/78-sigchld-and-waitpid.md)
79. [Job control in the shell](chapters/09-process-model/79-job-control.md)
79b. [gui_term gets real processes, signals, and Ctrl-C](chapters/09-process-model/79b-gui-term-real-processes.md)
79c. [PLAN: retiring `spawn` in favour of fork+exec everywhere](chapters/09-process-model/79c-retiring-spawn.md) *(plan only; implement after chapter 84)*

### Part X — Persistence and a Real Filesystem

A writable on-disk filesystem with a small journal so that
Notepad saves, shell history, and browser cookies survive reboot.

80. [Why we need a writable filesystem](chapters/10-filesystem/80-writable-fs-design.md)
81. [Inodes, dirents, and the free-space bitmap](chapters/10-filesystem/81-inodes-and-bitmap.md)
82. [Write-back, fsync, and the durability gap](chapters/10-filesystem/82-write-back-and-fsync.md)
83. [A tiny journal: crash-consistency on a budget](chapters/10-filesystem/83-tiny-journal.md)
84. [Save As: dialogs, libraries, and the first widget toolkit](chapters/10-filesystem/84-persistence-in-practice.md)
85. [Subdirectories: a path walker, mkdir, and a navigable Save As](chapters/10-filesystem/85-subdirectories.md)

### Part XI — Multiprocessing and Memory

The second core, atomics, IPIs, an SMP scheduler, mmap with a
unified page cache, and userspace threads.

86. [The second core: PSCI and secondary boot](chapters/11-smp-and-memory/86-second-core-psci.md)
87. [Atomics and spinlocks on AArch64](chapters/11-smp-and-memory/87-atomics-and-spinlocks.md)
88. [IPIs through GICv3 and TLB shootdown](chapters/11-smp-and-memory/88-ipis-via-gicv3.md)
89. [An SMP runqueue and basic load balance](chapters/11-smp-and-memory/89-smp-runqueue.md)
90. [mmap and a unified page cache](chapters/11-smp-and-memory/90-mmap-and-page-cache.md)
91. [Userspace threads (clone-shaped)](chapters/11-smp-and-memory/91-userspace-threads.md)
92. [Real SMP scheduling: per-CPU timers, locked sleeper walks, and CLONE_CPU](chapters/11-smp-and-memory/92-real-smp-scheduling.md)
93. [Sharing the FD table: CLONE_FILES and refcounted fd_table](chapters/11-smp-and-memory/93-clone-files.md)
94. [The browser parser thread: HTML/CSS/layout off the GUI core](chapters/11-smp-and-memory/94-browser-parser-thread.md)

### Part XII — System Services and Polish

The smaller features that round out a usable system: an RTC,
audio, PNGs, scalable fonts, observability, and a friendlier
stack-overflow message.

95. [A real RTC and wall-clock time](chapters/12-system-services/95-rtc-and-wallclock.md)
96. [virtio-snd: a boot chime and beep](chapters/12-system-services/96-virtio-snd.md)
97. [PNG decoding and the browser image cache](chapters/12-system-services/97-png-and-image-cache.md)
98. [Extending PNG: palette, grayscale, and content-type sniffing](chapters/12-system-services/98-png-extended.md)
98b. [Intrinsic image sizing and the resize race](chapters/12-system-services/98b-intrinsic-image-sizing.md)
99. [A /proc-shaped filesystem, ps, and top](chapters/12-system-services/99-procfs-ps-top.md)
100. [strace: a syscall tracer in 200 lines](chapters/12-system-services/100-strace.md)
101. [Guard pages and a friendlier stack overflow](chapters/12-system-services/101-guard-pages.md)
102. [TrueType fonts in the kernel](chapters/12-system-services/102-truetype-fonts.md)

### Part XIII — TCP Server and httpd

Closing the loop on TCP: passive open, accept, a tiny static
file server, and the browser fetching from its own kernel.

103. [Passive open: LISTEN, SYN_RECEIVED, the backlog](chapters/13-tcp-server/103-passive-open-listen.md)
104. [accept() and a server socket API](chapters/13-tcp-server/104-accept-and-server-sockets.md)
105. [/bin/httpd: serve /mnt and /data over HTTP](chapters/13-tcp-server/105-bin-httpd.md)
106. [End to end: the browser fetches from its own kernel](chapters/13-tcp-server/106-end-to-end-loop.md)

### Part XIV — Userspace Services

Part XIII taught `bind`/`listen`/`accept` as the foundational
idiom of inbound connections, on IPv4. This part reuses the
same shape — minus the network — to build a named-IPC bus,
and then puts the first long-running userspace service (the
clipboard) on top of it. The pattern generalises: every
future daemon (audio mixer, TLS proxy, the 9P-shaped
filesystem servers from Part XVI) sits on the same primitive.

107. [IPC: a tiny message bus for long-running services](chapters/14-userspace-services/107-ipc.md)
108. [The system clipboard, as a userspace service](chapters/14-userspace-services/108-clipboard.md)
108a. [Userspace access to window pixel buffers](chapters/14-userspace-services/108a-userspace-window-buffers.md)
108b. [Moving font rendering into userspace](chapters/14-userspace-services/108b-userspace-font-server.md)

### Part XV — Browser Maturation

Forms, cookies + Same-Origin Policy, a tiny scriptable
interpreter, and an honest TLS discussion.

109. [HTML forms: input, button, submit](chapters/15-browser-maturation/109-html-forms.md)
110. [Cookies and the Same-Origin Policy](chapters/15-browser-maturation/110-cookies-and-sop.md)
111. [A pocket JavaScript: expression evaluator for onclick](chapters/15-browser-maturation/111-pocket-javascript.md)
112. [TLS options: honest proxy, or BearSSL](chapters/15-browser-maturation/112-tls-options.md)

### Part XVI — Filesystem Architecture

The VFS grew six prefix-special-cased mounts (root ramfs, `/mnt/`,
`/bin/`, `/tmp/`, `/data/`, `/proc/`) over the course of the book.
This section retires the prefix ladder in favour of a classical
Unix VFS — a mount table dispatching through a `struct fs_ops`
vtable — and then uses that abstraction to support user-space
filesystem servers (a 9P-shaped RPC, so any userspace daemon can
become a mountable filesystem).

Both chapters are currently **PLAN** documents: they nail down
the design so the implementation work that follows is mechanical.
The userspace application updates (`ls`, `notepad` save dialog,
`init`, `sh`) are folded into each plan rather than split out.

113. [PLAN: A real VFS — mount table and `struct fs_ops`](chapters/16-filesystem-architecture/113-mount-table-and-vtable.md) *(plan only; prerequisite for chapter 114)*
114. [PLAN: User-space filesystem servers (9P-shaped)](chapters/16-filesystem-architecture/114-userspace-filesystem-servers.md) *(plan only; implement after chapter 113)*

## Appendices

Appendices fill in reference material the chapters cite but do not
fully expand. Some are imported and updated from the x86-64 edition;
others are new for the aarch64 edition.

- A. QEMU command reference for aarch64 virt — *to be written*
- B. GDB workflows for early kernel debugging — *to be written*
- C. AArch64 calling convention and register reference — *to be written*
- D. Linker script reference for the project — *to be written*
- E. Device tree quick reference — *to be written*
- F. virtio queue and descriptor reference — *to be written*
- G. Common boot failures and HVF assertions — *to be written*
- H. Glossary — *to be written*

## Dependency map

Read in order at the beginning:

1. Chapter 1 — orientation
2. Chapter 2 — toolchain
3. Chapter 3 — first boot

Then the critical path through the kernel is:

```
exception vectors → MMU → physical memory → heap
                        → GIC → timer → threads → scheduler
                                                → syscalls → ELF
                                                          → files
                                                          → fork/exec
```

Devices, GUI, networking, and browser are independent acceleration
tracks layered on top of "syscalls work."

## Project status snapshot

| Part | Milestone | Code state | Book state |
|------|-----------|------------|------------|
| I    | 0 — boot + UART | Done   | Chapters 1–3 written |
| II   | 1 — vectors + MMU + GIC | Done | Stubs |
| III  | 2 — timer + threads + scheduler | Done | Stubs |
| IV   | 3 — heap + syscalls + ELF + VFS | Done | Stubs |
| IV   | 4 — userspace + shell | Done | Stubs |
| V    | 5–7 — virtio-blk + OSFS + cache | Done | Stubs |
| V    | 8–37 — process model, libc, tools, signals | Done | Stubs |
| V    | **38 — virtio-gpu framebuffer** | **Done** | [Chapter 46](chapters/05-devices/46-virtio-gpu-framebuffer.md) |
| V    | **39 — virtio-input keyboard** | **Done** | [Chapter 47](chapters/05-devices/47-virtio-input-keyboard.md) |
| VI   | **40 — window manager + GUI syscalls** | **Done** | [Chapter 48](chapters/06-gui/48-window-manager-and-gui-syscalls.md) |
| VI   | **41 — virtio-tablet mouse + WM focus/drag/close** | **Done** | [Chapter 49](chapters/06-gui/49-virtio-tablet-and-wm-mouse.md) |
| VI   | **42 — gui_term: terminal-in-a-window via pipe+spawn** | **Done** | [Chapter 50](chapters/06-gui/50-gui-terminal-and-pipe-spawn.md) |
| VI   | **43 — notepad: GUI text editor + writable-tmpfs save** | **Done** | [Chapter 51](chapters/06-gui/51-notepad-and-tmpfs-roundtrip.md) |
| VI   | **44 — launcher: clickable mouse-driven app launcher** | **Done** | [Chapter 52](chapters/06-gui/52-launcher-and-click-to-spawn.md) |
| VI   | **45 — WM z-order bug fix + painter's-algorithm hardening** | **Done** | [Chapter 53](chapters/06-gui/53-wm-z-order-bug.md) |
| VI   | **46 — boot to desktop: auto-spawn launcher + gradient wallpaper** | **Done** | [Chapter 54](chapters/06-gui/54-boot-to-desktop.md) |
| VI   | **47 — taskbar + window list / raise syscalls + always-on-top** | **Done** | [Chapter 55](chapters/06-gui/55-taskbar-and-window-list.md) |
| VI   | **48 — clock in the taskbar** | **Done** | [Chapter 56](chapters/06-gui/56-clock-in-taskbar.md) |
| VI   | **49 — toast notifications + child-reap fix** | **Done** | [Chapter 57](chapters/06-gui/57-toast-notifications.md) |
| VI   | **50 — userspace wallpaper + yield/IRQ race fix** | **Done** | [Chapter 58](chapters/06-gui/58-userspace-wallpaper-and-yield-race.md) |
| VI   | **51 — window minimize / restore** | **Done** | [Chapter 59](chapters/06-gui/59-window-minimize-restore.md) |
| VI   | 52+ — menus + window snapping | Not started | Stubs |
| VII  | **52 — virtio-net + in-kernel ARP self-test** | **Done** | [Chapter 60](chapters/07-networking/60-virtio-net.md) |
| VII  | **53 — Ethernet + ARP cache + IPv4 stack** | **Done** | [Chapter 61](chapters/07-networking/61-ethernet-arp-ipv4.md) |
| VII  | **54 — ICMP echo + UDP + DHCP client** | **Done** | [Chapter 62](chapters/07-networking/62-icmp-udp-dhcp.md) |
| VII  | **55 — TCP client (state machine + buffered conn) + boot HTTP self-test** | **Done** | [Chapter 63](chapters/07-networking/63-tcp-and-sockets.md) |
| VII  | **56 — socket syscalls (FD_SOCKET) + /bin/httpget userspace tool** | **Done** | [Chapter 64](chapters/07-networking/64-socket-syscalls-and-httpget.md) |
| VII  | **57 — DHCP option-6 capture + DNS resolver + SYS_RESOLVE + httpget hostnames** | **Done** | [Chapter 65](chapters/07-networking/65-dns-resolver.md) |
| VII  | **58 — URL / HTTP parser + httpget URL form (one-hop redirect)** | **Done** | [Chapter 66](chapters/07-networking/66-url-and-http-parser.md) |
| VIII | **59 — HTML5 tokenizer (header-only, /bin/htmltok driver)** | **Done** | [Chapter 67](chapters/08-browser/67-html-tokenizer.md) |
| VIII | **60 — DOM (header-only, /bin/htmldom driver)** | **Done** | [Chapter 68](chapters/08-browser/68-dom-construction.md) |
| VIII | **61 — CSS parser + selector matcher (/bin/cssparse driver)** | **Done** | [Chapter 69](chapters/08-browser/69-css-parser.md) |
| VIII | **62 — CSS-driven layout engine + /bin/layout driver** | **Done** | [Chapter 70](chapters/08-browser/70-block-and-inline-layout.md) |
| VIII | **63 — /bin/browser: paint/plain/ANSI/GUI + resizable windows + horizontal scroll** | **Done** | [Chapter 71](chapters/08-browser/71-bin-browser.md) |
| VIII | **64 — browser UX: toolbar, address bar, click-to-navigate, back/forward + TCP perf + user-stack bump** | **Done** | Postscripts in [27](chapters/05-devices/27-argc-argv.md), [63](chapters/07-networking/63-tcp-and-sockets.md), [71](chapters/08-browser/71-bin-browser.md) |
| IX   | 65 — fork (eager copy) + exec | Planned | Stubs [73](chapters/09-process-model/73-aarch64-fork-and-as-copy.md), [74](chapters/09-process-model/74-exec-and-as-rebuild.md) |
| IX   | 66 — copy-on-write | Planned | Stub [75](chapters/09-process-model/75-copy-on-write.md) |
| IX   | 67 — signals (SIGINT → sigaction → SIGCHLD/waitpid) | Planned | Stubs [76](chapters/09-process-model/76-signals-sigint.md)–[78](chapters/09-process-model/78-sigchld-and-waitpid.md) |
| IX   | 68 — job control in the shell | Planned | Stub [79](chapters/09-process-model/79-job-control.md) |
| X    | 69–71 — OSFS-2: writable FS + journal | Planned | Stubs [80](chapters/10-filesystem/80-writable-fs-design.md)–[85](chapters/10-filesystem/85-subdirectories.md) |
| XI   | **72 — SMP boot: PSCI + secondary CPU** | **Done** | [Chapter 86](chapters/11-smp-and-memory/86-second-core-psci.md) |
| XI   | **72b — Atomics, barriers, and the audit of every shared global** | **Done** | [Chapter 87](chapters/11-smp-and-memory/87-atomics-and-spinlocks.md) |
| XI   | **72c — IPIs through GICv3 SGIs** | **Done** | [Chapter 88](chapters/11-smp-and-memory/88-ipis-via-gicv3.md) |
| XI   | **73 — SMP runqueue (per-CPU current + runq, no migration)** | **Done** | [Chapter 89](chapters/11-smp-and-memory/89-smp-runqueue.md) |
| XI   | **74 — mmap + unified page cache** | **Done** | [Chapter 90](chapters/11-smp-and-memory/90-mmap-and-page-cache.md) |
| XI   | **75 — userspace threads (clone)** | **Done** | [Chapter 91](chapters/11-smp-and-memory/91-userspace-threads.md) |
| XI   | **75b — real SMP scheduling: per-CPU timers, locked sleeper walks, CLONE_CPU** | **Done** | [Chapter 92](chapters/11-smp-and-memory/92-real-smp-scheduling.md) |
| XI   | **75c — sharing the FD table: CLONE_FILES + refcounted fd_table** | **Done** | [Chapter 93](chapters/11-smp-and-memory/93-clone-files.md) |
| XI   | **75d — browser parser thread: HTML/CSS/layout off the GUI core** | **Done** | [Chapter 94](chapters/11-smp-and-memory/94-browser-parser-thread.md) |
| XII  | **76 — RTC + wall-clock time** | **Done** | [Chapter 95](chapters/12-system-services/95-rtc-and-wallclock.md) |
| XII  | **77 — virtio-snd + boot chime** | **Done** | [Chapter 96](chapters/12-system-services/96-virtio-snd.md) |
| XII  | **78 — PNG + image cache** | **Done** | [Chapter 97](chapters/12-system-services/97-png-and-image-cache.md) |
| XII  | **78b — PNG: palette / gray + content-type sniff** | **Done** | [Chapter 98](chapters/12-system-services/98-png-extended.md) |
| XII  | **78c — intrinsic image sizing + resize race** | **Done** | [Chapter 98b](chapters/12-system-services/98b-intrinsic-image-sizing.md) |
| XII  | **79 — procfs + ps + top** | **Done** | [Chapter 99](chapters/12-system-services/99-procfs-ps-top.md) |
| XII  | **80 — strace via /proc/&lt;pid&gt;/trace** | **Done** | [Chapter 100](chapters/12-system-services/100-strace.md) |
| XII  | **80a — guard pages** | **Done** | [Chapter 101](chapters/12-system-services/101-guard-pages.md) |
| XII  | **80b -- TrueType fonts in the kernel** | **Done** | [Chapter 102](chapters/12-system-services/102-truetype-fonts.md) |
| XIV  | 80b — named IPC + service supervisor | Planned | Stub [Chapter 107](chapters/14-userspace-services/107-ipc.md) |
| XIV  | 80c — clipboard (userspace, over IPC) | Planned | Stub [Chapter 108](chapters/14-userspace-services/108-clipboard.md) |
| XIV  | 90d — userspace window-buffer access | Planned | Stub [Chapter 108a](chapters/14-userspace-services/108a-userspace-window-buffers.md) |
| XIV  | 90e — font rendering moves to userspace | Planned | Stub [Chapter 108b](chapters/14-userspace-services/108b-userspace-font-server.md) |
| XVI  | 84a — VFS refactor: mount table + `struct fs_ops` vtable | Planned | Design [Chapter 113](chapters/16-filesystem-architecture/113-mount-table-and-vtable.md) |
| XVI  | 84b — User-space filesystem servers (9P-shaped) | Planned | Design [Chapter 114](chapters/16-filesystem-architecture/114-userspace-filesystem-servers.md) |
| XIII | 81 — TCP passive open + accept + httpd + loopback | Planned | Stubs [103](chapters/13-tcp-server/103-passive-open-listen.md)–[106](chapters/13-tcp-server/106-end-to-end-loop.md) |
| XV   | 82 — HTML forms + cookies + SOP + pocket-JS | Planned | Stubs [109](chapters/15-browser-maturation/109-html-forms.md)–[111](chapters/15-browser-maturation/111-pocket-javascript.md) |
| XV   | 83 — TLS options (host proxy or BearSSL port) | Planned | Stub [Chapter 112](chapters/15-browser-maturation/112-tls-options.md) |
| VIII | 65+ — book polish, more sites, TLS bridge | Not started | Stubs |
