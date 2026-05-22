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
72. Host-side TLS bridge — *delivered in Part XIII as
    [Chapter 106a](chapters/13-tcp-server/106a-httpd-tls-bridge.md);
    `/bin/httpd` runs an in-guest forwarding proxy that splices
    HTTPS requests through `scripts/https_proxy.py` on the host.*

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
106. [TCP loopback (lo0 and 127.0.0.0/8)](chapters/13-tcp-server/106-tcp-loopback.md)
106a. [httpd as a forwarding proxy (TLS bridge)](chapters/13-tcp-server/106a-httpd-tls-bridge.md)
106b. [The browser uses the in-guest httpd as its proxy](chapters/13-tcp-server/106b-browser-uses-in-guest-httpd.md)
106c. [End to end: the browser fetches from its own kernel](chapters/13-tcp-server/106c-end-to-end-loop.md)

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
108c. [Moving the GUI SDK into userspace](chapters/14-userspace-services/108c-gui-sdk-userspace-drawing.md)
108d. [The window server moves to userspace](chapters/14-userspace-services/108d-userspace-window-server.md)
108e. [Decoration, the cursor, input routing, and resize](chapters/14-userspace-services/108e-userspace-decoration-input-resize.md)

### Part XV — Browser Maturation

Forms, cookies + Same-Origin Policy, a tiny scriptable
interpreter, and an honest TLS discussion.

109. [HTML forms: input, button, submit](chapters/15-browser-maturation/109-html-forms.md)
110. [Cookies and the Same-Origin Policy](chapters/15-browser-maturation/110-cookies-and-sop.md)
110a. [Cross-origin form submission blocking](chapters/15-browser-maturation/110a-cross-origin-form-blocking.md)
111. [A pocket JavaScript: expression evaluator for onclick](chapters/15-browser-maturation/111-pocket-javascript.md)
112. [Entropy: virtio-rng and a kernel CSPRNG](chapters/15-browser-maturation/112-entropy-and-csprng.md)
112a. [BearSSL builds for our userspace](chapters/15-browser-maturation/112a-bearssl-build.md)
112b. [In-guest TLS handshake](chapters/15-browser-maturation/112b-tls-handshake.md)
112c. [X.509 chain validation](chapters/15-browser-maturation/112c-chain-validation.md)
112d. [Browser https:// (native TLS)](chapters/15-browser-maturation/112d-browser-https.md)
112e. [TLS trust store (multi-anchor)](chapters/15-browser-maturation/112e-trust-store.md)
112f. [PEM ingest and the recursive chain walk](chapters/15-browser-maturation/112f-pem-ingest.md)
112g. [Direct outbound HTTPS: real public CAs](chapters/15-browser-maturation/112g-public-trust.md)
112h. [URL-bar UX after native TLS](chapters/15-browser-maturation/112h-url-bar-relatives.md)

### Part XVI — Filesystem Architecture

The VFS grew six prefix-special-cased mounts (root ramfs, `/mnt/`,
`/bin/`, `/tmp/`, `/data/`, `/proc/`) over the course of the book.
This section retires the prefix ladder in favour of a classical
Unix VFS — a mount table dispatching through a `struct fs_ops`
vtable — and then uses that abstraction to support user-space
filesystem servers (a 9P-shaped RPC, so any userspace daemon can
become a mountable filesystem).

Chapter 113 (the mount-table refactor) is **Done**, landed across
seven incremental steps (113a–113g), each one sweep-green before
the next began. Chapter 114 (userspace filesystem servers) is the
next milestone and still **PLAN**.

113. [A real VFS — mount table and `struct fs_ops`](chapters/16-filesystem-architecture/113-mount-table-and-vtable.md) *(plan + design contract)*
113a. [Step 1 — mount table types + `vfs_resolve`](chapters/16-filesystem-architecture/113a-mount-table-types.md)
113b. [Step 2 — porting procfs onto `fs_ops`](chapters/16-filesystem-architecture/113b-procfs-port.md)
113c. [Step 3 — porting tmpfs onto `fs_ops`](chapters/16-filesystem-architecture/113c-tmpfs-port.md)
113d. [Step 4 — porting OSFS-1 and OSFS-2](chapters/16-filesystem-architecture/113d-osfs-port.md)
113e. [Step 5 — embedded ramfs as a root mount](chapters/16-filesystem-architecture/113e-ramfs-as-root.md)
113f. [Step 6 — `SYS_MOUNTS` and `/bin/mount`](chapters/16-filesystem-architecture/113f-sys-mounts.md)
113g. [Step 7 — `MOUNT_RO` + `EROFS_VFS` hardening](chapters/16-filesystem-architecture/113g-mount-ro-hardening.md)
114. [PLAN: User-space filesystem servers (9P-shaped)](chapters/16-filesystem-architecture/114-userspace-filesystem-servers.md) *(plan; implementation in 114a–114f)*
114a. [Step 1 — kernel userfs module + `FD_USERFS_FILE`](chapters/16-filesystem-architecture/114a-kernel-userfs-module.md)
114b. [Step 2 — `SYS_MOUNT` / `SYS_UMOUNT`](chapters/16-filesystem-architecture/114b-sys-mount-umount.md)
114c. [Step 3 — `libfs` + `/bin/echofs`](chapters/16-filesystem-architecture/114c-libfs-and-echofs.md)
114d. [Step 4 — porting `clipboardd` to userfs](chapters/16-filesystem-architecture/114d-clipboardd-port.md)
114e. [Step 5 — porting `procfs` to `/bin/procd`](chapters/16-filesystem-architecture/114e-procd-port.md)
114f. [Step 6 — per-request timeouts and deadlock detection](chapters/16-filesystem-architecture/114f-timeouts-and-deadlock.md)

### Part XVII — A C compiler on the OS

After Part XVI the OS has the syscalls and filesystem
shape a compiler needs. Part XVII stands up a native
C toolchain — libc, assembler, linker, compiler,
build driver — so that source files in `/data/src/`
can be compiled to binaries in `/bin/` from inside
the running OS itself. Two compilers ship: TinyCC
(one chapter, proves the libc surface is complete)
and GCC (the headline, culminating in a self-hosting
bootstrap).

Every chapter in this part is a **stub or plan
document**. The implementation is the largest single
undertaking in the book; the chapters here lay out
the design so the work that follows is mechanical.

115. [PLAN: A C compiler that runs on the OS](chapters/17-self-hosting-gcc/115-c-compiler-strategy.md) *(strategy doc for the whole section)*
116. [A POSIX-ish libc, part 1: stdio, errno, env](chapters/17-self-hosting-gcc/116-libc-stdio-and-env.md)
117. [A POSIX-ish libc, part 2: stat, fcntl, dirent, getcwd](chapters/17-self-hosting-gcc/117-libc-stat-fcntl-dirent.md)
118. [An AArch64 assembler: /bin/as](chapters/17-self-hosting-gcc/118-bin-as-assembler.md)
119. [An AArch64 linker: /bin/ld and /bin/ar](chapters/17-self-hosting-gcc/119-bin-ld-linker.md)
120. [Bootstrap glue: crt0, crti, crtn, libgcc stubs](chapters/17-self-hosting-gcc/120-crt0-and-libgcc-stubs.md)
121. [TinyCC: a one-chapter native C compiler](chapters/17-self-hosting-gcc/121-tinycc-port.md)
122. [Cross-building a GCC that targets osdev](chapters/17-self-hosting-gcc/122-gcc-cross-build.md)
123. [Cross-building a GCC that runs on osdev](chapters/17-self-hosting-gcc/123-gcc-native-build.md)
124. [First native compile: hello.c on the desktop](chapters/17-self-hosting-gcc/124-first-native-compile.md)
125. [Self-hosting GCC: stage 2 builds stage 3](chapters/17-self-hosting-gcc/125-self-hosting-bootstrap.md)
126. [Porting GNU make](chapters/17-self-hosting-gcc/126-make-port.md)
127. [Notepad gets a Build button: an in-OS dev loop](chapters/17-self-hosting-gcc/127-notepad-build-button.md)

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
| XIII | **92 -- TCP passive open (LISTEN, SYN_RECEIVED)** | **Done** | [Chapter 103](chapters/13-tcp-server/103-passive-open-listen.md) |
| XIII | **93 -- accept() syscall + /bin/echod** | **Done** | [Chapter 104](chapters/13-tcp-server/104-accept-and-server-sockets.md) |
| XIII | **94 -- /bin/httpd static-file HTTP server** | **Done** | [Chapter 105](chapters/13-tcp-server/105-bin-httpd.md) |
| XIII | 95 -- TCP loopback (lo0, 127.0.0.0/8) | **Done** | [Chapter 106](chapters/13-tcp-server/106-tcp-loopback.md) |
| XIII | **96 -- httpd as forwarding proxy (TLS bridge)** | **Done** | [Chapter 106a](chapters/13-tcp-server/106a-httpd-tls-bridge.md) |
| XIII | **97 -- browser uses in-guest httpd as its proxy** | **Done** | [Chapter 106b](chapters/13-tcp-server/106b-browser-uses-in-guest-httpd.md) |
| XIII | **98 -- end-to-end loop (browser <-> in-guest httpd via lo0)** | **Done** | [Chapter 106c](chapters/13-tcp-server/106c-end-to-end-loop.md) |
| XIV  | 80b — named IPC + service supervisor | Done | [Chapter 107](chapters/14-userspace-services/107-ipc.md) |
| XIV  | 80c — clipboard (userspace, over IPC) | Done | [Chapter 108](chapters/14-userspace-services/108-clipboard.md) |
| XIV  | 90d — userspace window-buffer access | Planned | Stub [Chapter 108a](chapters/14-userspace-services/108a-userspace-window-buffers.md) |
| XIV  | 90e — font rendering moves to userspace | Planned | Stub [Chapter 108b](chapters/14-userspace-services/108b-userspace-font-server.md) |
| XIV  | 90f — GUI SDK rasterisation moves to userspace | Done | [Chapter 108c](chapters/14-userspace-services/108c-gui-sdk-userspace-drawing.md) |
| XIV  | 90g — window server moves to userspace | Done | [Chapter 108d](chapters/14-userspace-services/108d-userspace-window-server.md) — kernel compositor retired (`compose_all`, `blit_window`, `blit_cursor`, `paint_wallpaper`, `wm_draw_text_fb`, `wm_blend_pixel`, the cursor sprite — all deleted from `kernel/core/wm.c`); wsd is now the sole owner of the scanout, painting wallpaper + every mapped window + flushing via new `SYS_FB_PRESENT = 88` syscall; legacy render syscalls (`wm_present`, `wm_fill_rect`, `wm_draw_text`, `wm_flush`) stubbed to no-op success so legacy apps don't crash but draw nothing until C.5 ports them to `wmclient` one-by-one |
| XIV  | 90h — wsd-owned decoration, cursor, input routing, resize | Done | [Chapter 108e](chapters/14-userspace-services/108e-userspace-decoration-input-resize.md) — `wsd` paints title bars, close/minimize buttons, and the cursor sprite from `libgui/draw.h`; new kernel surface `SYS_POINTER_STATE = 89`, `SYS_GUI_MOVE_WINDOW = 90`, `SYS_GUI_DELIVER_EVENT = 91`, `SYS_GUI_SET_INPUT_PASSTHROUGH = 92` plus `SYS_WIN_FB_RESIZE` lets `wsd` own pointer hit-testing via the **input shadow** pattern (each `wsd` window is paired with a hidden, passthrough-flagged kernel WM window that owns the event queue `gui_poll_event` reads from); 60 Hz input poller in `wsd` does drag, raise, close-button delivery, minimize, hover, and live resize (kernel `win_fb` reallocates pages, client remaps via `wm_window_remap_fb`, `GUI_EVENT_RESIZE` delivered to the shadow's queue) |
| XVI  | 84a — VFS refactor: mount table + `struct fs_ops` vtable | Done | [Chapter 113](chapters/16-filesystem-architecture/113-mount-table-and-vtable.md) — landed across seven steps (113a–113g) per the plan; new types `struct fs_ops` + `struct mount` + `MOUNT_RO` flag + `EROFS_VFS=30` / `ENOSYS_VFS=38` errnos in [`kernel/core/vfs.h`](kernel/core/vfs.h); longest-prefix `vfs_resolve` with a "/" catchall that loses every tie ([Chapter 113a](chapters/16-filesystem-architecture/113a-mount-table-types.md)); five drivers ported onto the vtable with a uniform `_strip_slash` adapter pattern — [procfs](chapters/16-filesystem-architecture/113b-procfs-port.md) ([`kernel/core/procfs.c`](kernel/core/procfs.c)), [tmpfs](chapters/16-filesystem-architecture/113c-tmpfs-port.md) ([`kernel/core/tmpfs.c`](kernel/core/tmpfs.c)), [OSFS-1+OSFS-2](chapters/16-filesystem-architecture/113d-osfs-port.md) ([`kernel/core/osfs.c`](kernel/core/osfs.c) + [`kernel/core/osfs2.c`](kernel/core/osfs2.c)) — OSFS-1 registers TWICE with the same `fs_ops`/`NULL` cookie to serve both `/mnt` and `/bin`; [embedded ramfs as the root mount](chapters/16-filesystem-architecture/113e-ramfs-as-root.md) at `"/"` with `MOUNT_RO` so the dispatcher in `vfs_open`/`vfs_open_into`/`vfs_load` finally has no legacy fallback; new `SYS_MOUNTS = 95` syscall + `/bin/mount` user-visible tool + libc `mounts()` wrapper + `struct mount_info { char prefix[32]; unsigned flags; }` ABI ([Chapter 113f](chapters/16-filesystem-architecture/113f-sys-mounts.md), [`userspace/mount/mount.c`](userspace/mount/mount.c), [`userspace/libc/syscall.h`](userspace/libc/syscall.h)); MOUNT_RO+EROFS_VFS hardening pass ([Chapter 113g](chapters/16-filesystem-architecture/113g-mount-ro-hardening.md)) fixes a real bug where mutations against RO mounts whose driver lacked the op pointer returned EINVAL_VFS instead of EROFS_VFS — sys_unlink and sys_mkdir now check `MOUNT_RO` BEFORE checking `m->ops->{unlink,mkdir}`; per-step regression sweeps green at 113a/build, 113b/test_procfs, 113c/5 tests, 113d/7 tests, 113e/10 tests, 113f/test_mounts (7 PASS), 113g/test_mount_ro (12 PASS); save_dialog mount-table iteration deferred to a future UI-redesign chapter per the 113f writeup; userspace fs servers + `SYS_MOUNT`/`SYS_UMOUNT` reserved at 96/97 for chapter 114 |
| XVI  | 84b — User-space filesystem servers (9P-shaped) | Planned | Design [Chapter 114](chapters/16-filesystem-architecture/114-userspace-filesystem-servers.md) |
| XV   | 82 — HTML forms + cookies + SOP + pocket-JS | In progress | Done: [109](chapters/15-browser-maturation/109-html-forms.md), [110](chapters/15-browser-maturation/110-cookies-and-sop.md), [110a](chapters/15-browser-maturation/110a-cross-origin-form-blocking.md), [111](chapters/15-browser-maturation/111-pocket-javascript.md) — pocket JS in [`userspace/libc/pocketjs.h`](userspace/libc/pocketjs.h) (~750 lines) + DOM bridge in [`userspace/browser/jsdom.h`](userspace/browser/jsdom.h); `onclick` fires via MOUSE_DOWN hook in [`userspace/browser/browser.c`](userspace/browser/browser.c); [`browser --check-js`](scripts/test_browser_js.py) regression at 20/20 |
| XV   | 83 — TLS prerequisite: entropy (virtio-rng + CSPRNG + getrandom) | Done | [Chapter 112](chapters/15-browser-maturation/112-entropy-and-csprng.md) — virtio-rng driver ([`kernel/device/virtio_rng.c`](kernel/device/virtio_rng.c)), ChaCha20 CSPRNG with virtio-rng reseed every 256 KiB ([`kernel/core/random.c`](kernel/core/random.c)), `SYS_GETRANDOM = 94`, `/bin/getrand` userspace tool, regression [`test_getrand.py`](scripts/test_getrand.py); BearSSL port (chapters 112a–112e) builds on this |
| XV   | 83a — BearSSL freestanding build + SHA-256 KAT | Done | [Chapter 112a](chapters/15-browser-maturation/112a-bearssl-build.md) — vendored BearSSL 0.6 ([`vendor/bearssl/`](vendor/bearssl/)) builds into [`build/vendor/bearssl/libbearssl.a`](Makefile) with a 5-symbol `<string.h>` shim ([`vendor/bearssl-shim/string.h`](vendor/bearssl-shim/string.h)) and extern mem*/strlen/time in [`userspace/libc/cstring.c`](userspace/libc/cstring.c); `/bin/tlstest` ([`userspace/tlstest/tlstest.c`](userspace/tlstest/tlstest.c)) verifies SHA-256("") and SHA-256("abc") against NIST vectors at boot ([`scripts/test_tlstest.py`](scripts/test_tlstest.py)) |
| XV   | 83b — In-guest TLS handshake (knownkey, loopback) | Done | [Chapter 112b](chapters/15-browser-maturation/112b-tls-handshake.md) — `tls_socket_*` client surface in [`userspace/libc/tls_socket.c`](userspace/libc/tls_socket.c) wraps BearSSL's `br_sslio_*` over our chapter-104 TCP sockets, seeds via `SYS_GETRANDOM`, pins the leaf cert's RSA-2048 public key via `br_x509_knownkey_*` (extracted at startup with `br_x509_decoder_*`); new `/bin/httpsd` ([`userspace/httpsd/httpsd.c`](userspace/httpsd/httpsd.c)) spawned on port 8443 by [`init.c`](userspace/init/init.c) serves a marker body inside the TLS record stream; BearSSL sample CN=localhost cert chain re-exported via [`vendor/testcerts/test_chain.c`](vendor/testcerts/test_chain.c) so no host openssl tooling is required; `tlstest --handshake 127.0.0.1 8443` regression ([`scripts/test_tls_handshake.py`](scripts/test_tls_handshake.py)) passes end-to-end |
| XV   | 83c — X.509 chain validation (BearSSL minimal + wallclock) | Done | [Chapter 112c](chapters/15-browser-maturation/112c-chain-validation.md) — `tls_socket_init_chain_from_anchor` ([`userspace/libc/tls_socket.c`](userspace/libc/tls_socket.c)) bakes a `br_x509_trust_anchor` at runtime from a DER CA cert (DN bytes accumulated via `br_x509_decoder_*`'s `append_dn` callback, RSA `n`/`e` copied out of the decoder pad), wires it into BearSSL's `br_x509_minimal_*` validator, and seeds the validator's clock from `SYS_GETTIMEOFDAY` (chapter 95) with the engine-internal offset of 719528 days for the Unix epoch; `/bin/tlstest --handshake-ca 127.0.0.1 8443` exercises end-to-end chain validation against the sample intermediate CA, regression in [`scripts/test_tls_chain.py`](scripts/test_tls_chain.py) — first userspace consumer of RTC outside `/bin/date` |
| XV   | 83d — Browser https:// (native TLS, in-guest anchor) | Done | [Chapter 112d](chapters/15-browser-maturation/112d-browser-https.md) — five-call `br_conn_t` abstraction in [`userspace/browser/browser.c`](userspace/browser/browser.c) tags a fetch as TCP or TLS so `http_fetch_one` / `drain_conn` carry both with one body; `br_conn_open` heap-allocates the ~42 KB `tls_socket_t`, initialises it with the chapter-112c sample intermediate as the trust anchor, and runs the chain-validating handshake; `canonicalize_url` `https://` case gated on `g_proxy_was_set` so the legacy chapter-106b proxy rewrite stays alive for `proxytest`/`test_browser_hn_*` callers that explicitly set `BROWSER_PROXY` while the bare browser binary uses real TLS; literal-`localhost` shortcut in the resolver skips `SYS_RESOLVE` so the sample chain's CN=localhost leaf passes SNI; Makefile gains `BEARSSL_INC` per-file override for `browser.o` and pulls `libbearssl.a` into the `BROWSER_ELF` `--start-group` window; regression [`scripts/test_browser_https.py`](scripts/test_browser_https.py) drives `browser https://localhost:8443/` and asserts the chapter-112d TLS-OK line plus the decrypted body reaching the renderer; `scripts/https_proxy.py` retired from the boot path but kept on disk per `/memories/debug-scripts-policy.md` |
| XV   | 83e — TLS trust store (multi-anchor, RSA + ECDSA) | Done | [Chapter 112e](chapters/15-browser-maturation/112e-trust-store.md) — `tls_socket_t` gains an 8-slot `anchors[]` array with per-row backing buffers for RSA `n`/`e` AND ECDSA `q` ([`userspace/libc/tls_socket.h`](userspace/libc/tls_socket.h)); `tls_bake_anchor_into` ([`userspace/libc/tls_socket.c`](userspace/libc/tls_socket.c)) branches on `pk->key_type` so EC pubkeys land in `anchor_pk_q[i]` with `pk->key.ec.curve` recorded; new `tls_socket_init_chain_multi` + `tls_socket_init_chain_from_bundle` APIs (chapter-112d's `_from_anchor` is now a wrapper over `_multi(1)`); `/mnt/ca.bundle` ships in OSFS as a framed `"CAB1"` blob generated at build time by [`scripts/mkcabundle.py`](scripts/mkcabundle.py) from BearSSL's `chain-rsa.h` and `chain-ec.h` headers (~1.3 KiB, 2 anchors); browser ([`userspace/browser/browser.c`](userspace/browser/browser.c)) slurps the bundle at first `https://` fetch via `load_ca_bundle_once`, prefers it over the in-binary fallback, and prints `source=bundle` in the TLS-OK line; `/bin/httpsd` ([`userspace/httpsd/httpsd.c`](userspace/httpsd/httpsd.c)) gains a `--ec` flag that flips to `br_ssl_server_init_full_ec` with the new [`vendor/testcerts/test_chain_ec.c`](vendor/testcerts/test_chain_ec.c) (BearSSL `chain-ec.h` re-export, separate TU to avoid `CERT0/CERT1` `static const` collision); [`init.c`](userspace/init/init.c) spawns a second httpsd as `--ec 8444` alongside the original 8443; regression [`scripts/test_browser_https_multi.py`](scripts/test_browser_https_multi.py) fetches both ports back-to-back asserting `source=bundle` for each |
| XV   | 83f — PEM ingest + recursive chain walk | Done | [Chapter 112f](chapters/15-browser-maturation/112f-pem-ingest.md) — [`scripts/mkcabundle.py`](scripts/mkcabundle.py) grows a `--pem PATH` mode (regex-extracted `-----BEGIN CERTIFICATE-----` blocks, whitespace-collapsed base64 decode via `base64.b64decode(..., validate=True)`); Makefile retargets the `assets/osfs/ca.bundle` rule at the BearSSL sample ROOT PEMs (`vendor/bearssl/samples/cert-root-{rsa,ec}.pem`) instead of the intermediate-from-header path so `httpsd`'s served leaf+ica chain now forces the validator's full root → intermediate → leaf walk in both algorithms; `TLS_MAX_ANCHORS` raised from 8 to 32 ([`userspace/libc/tls_socket.h`](userspace/libc/tls_socket.h)) and `BR_CA_BUNDLE_MAX` from 32 KiB to 256 KiB ([`userspace/browser/browser.c`](userspace/browser/browser.c)) to fit a future Mozilla NSS drop without a recompile; regression [`scripts/test_tls_pem_bundle.py`](scripts/test_tls_pem_bundle.py) asserts the host-side `CAB1`-framed bundle layout (magic + count + 1208 bytes for both BearSSL roots) AND end-to-end 2-link chain validation against both `:8443` (RSA) and `:8444` (ECDSA) |
| XV   | 83g — Direct outbound HTTPS to real public sites | Done | [Chapter 112g](chapters/15-browser-maturation/112g-public-trust.md) — new [`scripts/fetch_public_roots.sh`](scripts/fetch_public_roots.sh) cross-platform-probes the host's system CA store (`/etc/ssl/cert.pem` on macOS, `/etc/ssl/certs/ca-certificates.crt` on Debian, `/etc/pki/tls/certs/ca-bundle.crt` on RHEL, Homebrew openssl@3 paths) and atomic-copies it to `vendor/testcerts/public-roots.pem` (.gitignored, fetched on first build); Makefile [`assets/osfs/ca.bundle`](Makefile) rule folds the resulting ~140 KiB / 128-anchor PEM in alongside the BearSSL sample roots via a third `--pem` argument so the production bundle now ships 130 anchors / 142,861 bytes; capacity bumps `TLS_MAX_ANCHORS` 32→256 ([`userspace/libc/tls_socket.h`](userspace/libc/tls_socket.h)) and `BR_CA_BUNDLE_MAX` 256 KiB→512 KiB ([`userspace/browser/browser.c`](userspace/browser/browser.c)) with no logic change; [`scripts/test_tls_pem_bundle.py`](scripts/test_tls_pem_bundle.py) loosens the count assertion from `== 2` to `>= 2` while keeping the end-to-end recursive-walk check; new manual probe [`scripts/_dbg_tls_outbound.py`](scripts/_dbg_tls_outbound.py) (per debug-scripts policy, not in regression sweep) drives `browser https://example.com/` and `browser https://news.ycombinator.com/` end-to-end through SLIRP → DNS → real public CA chain validation → SAN/CN match against an in-guest-unknown hostname, both confirmed working; nothing in `tls_socket.c` / `browser.c`'s TLS path needed to change — SNI, recursive walk, hostname check, and wall-clock validity were all already wired since chapters 112b–112f, the only blocker was the trust list itself |
| XV   | 83h — URL-bar UX after native TLS | Done | [Chapter 112h](chapters/15-browser-maturation/112h-url-bar-relatives.md) — `canonicalize_url` in [`userspace/browser/browser.c`](userspace/browser/browser.c) gains a new case (5a) that resolves path-relative references against an http(s):// current page via the existing chapter-110a `resolve_url` helper, gated on a "looks like a host" heuristic (input contains a `.` or `:` before the first `/` or `?` ⇒ host, otherwise relative); case (6) default flipped from "prepend `g_proxy_prefix`" to "prepend `https://`" so typing `news.ycombinator.com` (no scheme) goes through native TLS instead of the chapter-106b in-guest proxy; both changes gated on `g_proxy_was_set` so `BROWSER_PROXY=...` callers ([`scripts/test_browser_proxy.py`](scripts/test_browser_proxy.py), the `test_browser_hn_*` GUI suite) keep the legacy rewrite untouched; the first live HN load uncovered and fixed a latent bug in `resolve_url`'s path-relative branch where `last_slash` was pre-initialised to `path_start` so the `>=` test was always true and the "no path in base" else branch was unreachable — for a base URL like `https://news.ycombinator.com` (no trailing `/`) the merge planted a NUL byte at `out[host_len]` and every relative ref (`news.css?...`, `y18.gif`, `item?id=...`) silently truncated to the bare host, manifesting as three apparently unrelated symptoms (stylesheet skip-https log line with no path, png_decode failure decoding the HTML body as PNG, comment-link click reloading the front page), all collapsed by initialising `last_slash = 0` instead; the latent bug had survived the entire 109a→112g arc because no prior fetched page had a no-path base URL and link clicks had never gone through `resolve_url` before case (5a); follow-on cleanup deletes the vestigial `if (br_starts(abs, "https://")) { skip }` guard in `apply_link_sheets` since chapter 112d already made `http_fetch` → `br_conn_open` transport-agnostic, so HN's `news.css` now fetches over its own native-TLS handshake instead of being skipped; new manual probes [`scripts/_dbg_bare_host.py`](scripts/_dbg_bare_host.py) and [`scripts/_dbg_hn_resources.py`](scripts/_dbg_hn_resources.py) (per debug-scripts policy, not in regression sweep) assert the bare-host TLS path and the resolve_url/stylesheet-fetch fix respectively against live HN, the latter requiring TWO `TLS handshake OK with news.ycombinator.com:443` lines (page + stylesheet) and a `fetching stylesheet https://news.ycombinator.com/news.css...` line; fixes the user-reported bug that comment-link clicks from `https://news.ycombinator.com/` were navigating to `http://127.0.0.1:80/item?id=...` because HN's `<a href="item?id=...">` hrefs are relative path-references, not absolute URLs, AND the follow-on bug that those refs still re-loaded the front page after case (5a) landed because `resolve_url` was truncating them to the bare host |
| XVII | 100 — POSIX-ish libc growth for compiler hosting | Planned | Stubs [116](chapters/17-self-hosting-gcc/116-libc-stdio-and-env.md)–[117](chapters/17-self-hosting-gcc/117-libc-stat-fcntl-dirent.md) |
| XVII | 101 — /bin/as + /bin/ld + /bin/ar + crt0/libgcc | Planned | Stubs [118](chapters/17-self-hosting-gcc/118-bin-as-assembler.md)–[120](chapters/17-self-hosting-gcc/120-crt0-and-libgcc-stubs.md) |
| XVII | 102 — TinyCC native port (first compiler on the OS) | Planned | Stub [121](chapters/17-self-hosting-gcc/121-tinycc-port.md) |
| XVII | 103 — GCC native port + self-hosting bootstrap | Planned | Stubs [122](chapters/17-self-hosting-gcc/122-gcc-cross-build.md)–[125](chapters/17-self-hosting-gcc/125-self-hosting-bootstrap.md) |
| XVII | 104 — GNU make + notepad "Build" button | Planned | Stubs [126](chapters/17-self-hosting-gcc/126-make-port.md)–[127](chapters/17-self-hosting-gcc/127-notepad-build-button.md) |
| VIII | 65+ — book polish, more sites, TLS bridge | Not started | Stubs |
