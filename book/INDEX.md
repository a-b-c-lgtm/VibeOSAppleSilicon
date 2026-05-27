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

The first three chapters cover the boot to a serial banner under HVF.

1. [Why Apple Silicon, why aarch64, why now](chapters/01-foundations/001-why-apple-silicon.md)
2. [Toolchain and host setup](chapters/01-foundations/002-toolchain-and-host-setup.md)
3. [First boot: QEMU virt, the boot stub, and PL011 UART](chapters/01-foundations/003-first-boot.md)

### Part II — Memory and Exceptions

AArch64 forces these to land together
because you cannot run real C with the MMU off (see the early-boot
caveat in `kernel/core/main.c`).

4. [The AArch64 execution environment (exception levels, registers, PSTATE)](chapters/02-memory/004-execution-environment.md)
5. [Exception vectors, ESR, FAR, and synchronous fault handling](chapters/02-memory/005-exception-vectors.md)
6. [The MMU, translation tables, and MAIR](chapters/02-memory/006-mmu-and-page-tables.md)
7. [Physical memory and the device tree](chapters/02-memory/007-physical-memory-and-device-tree.md)
- Higher-half kernel and TTBR1 — *deferred until needed; see ch 7 epilogue*

### Part III — Time and Concurrency

The GIC and the generic timer let us take interrupts
and tick.

8. [GIC v3 fundamentals](chapters/03-time-and-concurrency/008-gic-v3.md)
9. [The ARM generic timer](chapters/03-time-and-concurrency/009-arm-generic-timer.md)
10. [Threads and the AArch64 context switch](chapters/03-time-and-concurrency/010-threads-and-context-switch.md)
11. [Preemptive scheduling](chapters/03-time-and-concurrency/011-preemption.md)

### Part IV — Userspace

SVC syscalls, ELF loading, the first user
program, the file system, and the shell.

12. [The kernel heap](chapters/04-userspace/012-kernel-heap.md)
13. [SVC and the syscall ABI](chapters/04-userspace/013-svc-and-syscalls.md)
14. [ELF loading and the first user program](chapters/04-userspace/014-elf-and-first-user-program.md)
15. [Files, VFS, and a tiny ramfs](chapters/04-userspace/015-files-and-vfs.md)
16. [`init`, `spawn`, `wait`: the simplest process model that works](chapters/04-userspace/016-init-spawn-wait.md)
17. [Console keyboard input and a line-mode shell](chapters/04-userspace/017-keyboard-and-shell.md)

### Part V — Devices

virtio-mmio for everything.

18. [virtio-mmio: bus, queues, and the modern transport](chapters/05-devices/018-virtio-mmio.md)
19. [virtio-blk and persistent storage](chapters/05-devices/019-virtio-blk.md)
20. [A read-only on-disk filesystem and `cat /mnt/...`](chapters/05-devices/020-osfs-and-mount.md)
21. [Loading user binaries from disk](chapters/05-devices/021-binaries-on-disk.md)
22. [A block cache in front of virtio-blk](chapters/05-devices/022-block-cache.md)
23. [Per-process address spaces](chapters/05-devices/023-per-process-address-spaces.md)
24. [Hardening the kernel/user boundary](chapters/05-devices/024-kernel-user-boundary.md)
25. [A user heap via sbrk](chapters/05-devices/025-user-heap.md)
26. [argc, argv, and the user stack](chapters/05-devices/026-argc-argv.md)
27. [A printf for the user libc](chapters/05-devices/027-printf.md)
28. [Browsing the namespace: SYS_LISTDIR and ls](chapters/05-devices/028-listdir-and-ls.md)
29. [uptime and a real shell PATH](chapters/05-devices/029-uptime-and-path.md)
30. [A `time` builtin for the shell](chapters/05-devices/030-time-builtin.md)
31. [Per-process cwd: cd, pwd, dynamic prompt](chapters/05-devices/031-cwd-cd-pwd.md)
32. [Environment variables and a real PATH walk](chapters/05-devices/032-env-vars-and-path.md)
33. [Variable expansion and ./prog](chapters/05-devices/033-var-expansion-and-relative-paths.md)
34. [Shell quoting](chapters/05-devices/034-shell-quoting.md)
35. [Bigger filesystem and four classic tools](chapters/05-devices/035-bigger-fs-and-four-tools.md)
36. [Input redirection](chapters/05-devices/036-input-redirection.md)
37. [Sleep and the THREAD_SLEEPING state](chapters/05-devices/037-sleep-and-blocking.md)
38. [Kernel pipes, dup2, and THREAD_BLOCKED](chapters/05-devices/038-pipes.md)
39. [Shell pipelines: cat | grep | wc](chapters/05-devices/039-shell-pipelines.md)
40. [Writable tmpfs and `>` output redirection](chapters/05-devices/040-writable-tmpfs-and-output-redirection.md)
41. [tmpfs polish: `>>`, `ls /tmp/`, `rm`](chapters/05-devices/041-tmpfs-polish.md)
42. [Raw TTY mode and the shell line editor](chapters/05-devices/042-raw-tty-and-line-editor.md)
43. [Cursor movement and readline keybindings](chapters/05-devices/043-cursor-and-readline-keys.md)
44. [Kill ring: Ctrl-K, Ctrl-U, Ctrl-W, Ctrl-Y](chapters/05-devices/044-kill-ring-and-yank.md)
45. [virtio-gpu: a framebuffer at native resolution](chapters/05-devices/045-virtio-gpu-framebuffer.md)
46. [virtio-input: an evdev keyboard for the GUI](chapters/05-devices/046-virtio-input-keyboard.md)

### Part VI — GUI

The compositor, window server, toolkit, and desktop.

47. [An in-kernel window manager and seven GUI syscalls](chapters/06-gui/047-window-manager-and-gui-syscalls.md)
48. [virtio-tablet, mouse focus, drag, and close](chapters/06-gui/048-virtio-tablet-and-wm-mouse.md)
49. [gui_term: a terminal in a window, and the synchronous-pipe spawn pattern](chapters/06-gui/049-gui-terminal-and-pipe-spawn.md)
50. [notepad: a real text editor in a window, and the writable-tmpfs round-trip](chapters/06-gui/050-notepad-and-tmpfs-roundtrip.md)
51. [launcher: clicking is the new typing](chapters/06-gui/051-launcher-and-click-to-spawn.md)
52. [A WM rendering bug, surfaced by the launcher](chapters/06-gui/052-wm-z-order-bug.md)
53. [Boot to desktop: auto-spawn the launcher and a gradient wallpaper](chapters/06-gui/053-boot-to-desktop.md)
54. [A taskbar, three new GUI syscalls, and a real desktop](chapters/06-gui/054-taskbar-and-window-list.md)
55. [A clock in the taskbar](chapters/06-gui/055-clock-in-taskbar.md)
56. [Toast notifications and proper child reaping](chapters/06-gui/056-toast-notifications.md)
57. [A userspace wallpaper, and the yield/IRQ race it uncovered](chapters/06-gui/057-userspace-wallpaper-and-yield-race.md)
58. [Window minimize and restore](chapters/06-gui/058-window-minimize-restore.md)

### Part VII — Networking

virtio-net, then a vertical slice through Ethernet,
IPv4, ICMP/UDP/DHCP, and TCP with a BSD-shaped socket API.

59. [virtio-net: getting bytes on and off the wire](chapters/07-networking/059-virtio-net.md)
60. [Ethernet, ARP, and IPv4](chapters/07-networking/060-ethernet-arp-ipv4.md)
61. [ICMP, UDP, and DHCP](chapters/07-networking/061-icmp-udp-dhcp.md)
62. [TCP and a kernel-side socket API](chapters/07-networking/062-tcp-and-sockets.md)
63. [Socket syscalls and a userspace `httpget`](chapters/07-networking/063-socket-syscalls-and-httpget.md)
64. [DNS resolver](chapters/07-networking/064-dns-resolver.md)
65. [URL and HTTP parser](chapters/07-networking/065-url-and-http-parser.md)

### Part VIII — Browser

The capstone: a working text-mode browser inside our
own kernel, on our own GUI, over our own TCP/IP stack.

66. [The HTML tokenizer](chapters/08-browser/066-html-tokenizer.md)
67. [DOM construction](chapters/08-browser/067-dom-construction.md)
68. [A tiny CSS parser](chapters/08-browser/068-css-parser.md)
69. [Block and inline layout](chapters/08-browser/069-block-and-inline-layout.md)
70. [/bin/browser — paint, plain, ANSI, GUI](chapters/08-browser/070-bin-browser.md)
- Host-side TLS bridge — *delivered in Part XIII as
    [Chapter 109](chapters/13-tcp-server/109-httpd-tls-bridge.md);
    `/bin/httpd` runs an in-guest forwarding proxy that splices
    HTTPS requests through `scripts/https_proxy.py` on the host.*

### Part IX — Finishing the Process Model

Signals, fork, exec, and job control — the Unix process model the
book has been deferring since chapter 16.

71. [Why fork (and not just spawn)](chapters/09-process-model/071-why-fork-vs-spawn.md)
72. [fork on AArch64: the address-space copy](chapters/09-process-model/072-aarch64-fork-and-as-copy.md)
73. [exec: tearing down and rebuilding an AS in place](chapters/09-process-model/073-exec-and-as-rebuild.md)
74. [Copy-on-write: making fork cheap](chapters/09-process-model/074-copy-on-write.md)
75. [Signals, starting with SIGINT](chapters/09-process-model/075-signals-sigint.md)
76. [Catching signals: sigaction, masks, EINTR](chapters/09-process-model/076-sigaction-and-eintr.md)
77. [SIGCHLD and waitpid: parent-child plumbing](chapters/09-process-model/077-sigchld-and-waitpid.md)
78. [Job control in the shell](chapters/09-process-model/078-job-control.md)
79. [gui_term gets real processes, signals, and Ctrl-C](chapters/09-process-model/079-gui-term-real-processes.md)
80. [PLAN: retiring `spawn` in favour of fork+exec everywhere](chapters/09-process-model/080-retiring-spawn.md) *(plan only; implement after chapter 85)*

### Part X — Persistence and a Real Filesystem

A writable on-disk filesystem with a small journal so that
Notepad saves, shell history, and browser cookies survive reboot.

81. [Why we need a writable filesystem](chapters/10-filesystem/081-writable-fs-design.md)
82. [Inodes, dirents, and the free-space bitmap](chapters/10-filesystem/082-inodes-and-bitmap.md)
83. [Write-back, fsync, and the durability gap](chapters/10-filesystem/083-write-back-and-fsync.md)
84. [A tiny journal: crash-consistency on a budget](chapters/10-filesystem/084-tiny-journal.md)
85. [Save As: dialogs, libraries, and the first widget toolkit](chapters/10-filesystem/085-persistence-in-practice.md)
86. [Subdirectories: a path walker, mkdir, and a navigable Save As](chapters/10-filesystem/086-subdirectories.md)

### Part XI — Multiprocessing and Memory

The second core, atomics, IPIs, an SMP scheduler, mmap with a
unified page cache, and userspace threads.

87. [The second core: PSCI and secondary boot](chapters/11-smp-and-memory/087-second-core-psci.md)
88. [Atomics and spinlocks on AArch64](chapters/11-smp-and-memory/088-atomics-and-spinlocks.md)
89. [IPIs through GICv3 and TLB shootdown](chapters/11-smp-and-memory/089-ipis-via-gicv3.md)
90. [An SMP runqueue and basic load balance](chapters/11-smp-and-memory/090-smp-runqueue.md)
91. [mmap and a unified page cache](chapters/11-smp-and-memory/091-mmap-and-page-cache.md)
92. [Userspace threads (clone-shaped)](chapters/11-smp-and-memory/092-userspace-threads.md)
93. [Real SMP scheduling: per-CPU timers, locked sleeper walks, and CLONE_CPU](chapters/11-smp-and-memory/093-real-smp-scheduling.md)
94. [Sharing the FD table: CLONE_FILES and refcounted fd_table](chapters/11-smp-and-memory/094-clone-files.md)
95. [The browser parser thread: HTML/CSS/layout off the GUI core](chapters/11-smp-and-memory/095-browser-parser-thread.md)

### Part XII — System Services and Polish

The smaller features that round out a usable system: an RTC,
audio, PNGs, scalable fonts, observability, and a friendlier
stack-overflow message.

96. [A real RTC and wall-clock time](chapters/12-system-services/096-rtc-and-wallclock.md)
97. [virtio-snd: a boot chime and beep](chapters/12-system-services/097-virtio-snd.md)
98. [PNG decoding and the browser image cache](chapters/12-system-services/098-png-and-image-cache.md)
99. [Extending PNG: palette, grayscale, and content-type sniffing](chapters/12-system-services/099-png-extended.md)
100. [Intrinsic image sizing and the resize race](chapters/12-system-services/100-intrinsic-image-sizing.md)
101. [A /proc-shaped filesystem, ps, and top](chapters/12-system-services/101-procfs-ps-top.md)
102. [strace: a syscall tracer in 200 lines](chapters/12-system-services/102-strace.md)
103. [Guard pages and a friendlier stack overflow](chapters/12-system-services/103-guard-pages.md)
104. [TrueType fonts in the kernel](chapters/12-system-services/104-truetype-fonts.md)

### Part XIII — TCP Server and httpd

Closing the loop on TCP: passive open, accept, a tiny static
file server, and the browser fetching from its own kernel.

105. [Passive open: LISTEN, SYN_RECEIVED, the backlog](chapters/13-tcp-server/105-passive-open-listen.md)
106. [accept() and a server socket API](chapters/13-tcp-server/106-accept-and-server-sockets.md)
107. [/bin/httpd: serve /mnt and /data over HTTP](chapters/13-tcp-server/107-bin-httpd.md)
108. [TCP loopback (lo0 and 127.0.0.0/8)](chapters/13-tcp-server/108-tcp-loopback.md)
109. [httpd as a forwarding proxy (TLS bridge)](chapters/13-tcp-server/109-httpd-tls-bridge.md)
110. [The browser uses the in-guest httpd as its proxy](chapters/13-tcp-server/110-browser-uses-in-guest-httpd.md)
111. [End to end: the browser fetches from its own kernel](chapters/13-tcp-server/111-end-to-end-loop.md)

### Part XIV — Userspace Services

Part XIII taught `bind`/`listen`/`accept` as the foundational
idiom of inbound connections, on IPv4. This part reuses the
same shape — minus the network — to build a named-IPC bus,
and then puts the first long-running userspace service (the
clipboard) on top of it. The pattern generalises: every
future daemon (audio mixer, TLS proxy, the 9P-shaped
filesystem servers from Part XVI) sits on the same primitive.

112. [IPC: a tiny message bus for long-running services](chapters/14-userspace-services/112-ipc.md)
113. [The system clipboard, as a userspace service](chapters/14-userspace-services/113-clipboard.md)
114. [Userspace access to window pixel buffers](chapters/14-userspace-services/114-userspace-window-buffers.md)
115. [Moving font rendering into userspace](chapters/14-userspace-services/115-userspace-font-server.md)
116. [Moving the GUI SDK into userspace](chapters/14-userspace-services/116-gui-sdk-userspace-drawing.md)
117. [The window server moves to userspace](chapters/14-userspace-services/117-userspace-window-server.md)
118. [Decoration, the cursor, input routing, and resize](chapters/14-userspace-services/118-userspace-decoration-input-resize.md)

### Part XV — Browser Maturation

Forms, cookies + Same-Origin Policy, a tiny scriptable
interpreter, and an honest TLS discussion.

119. [HTML forms: input, button, submit](chapters/15-browser-maturation/119-html-forms.md)
120. [Cookies and the Same-Origin Policy](chapters/15-browser-maturation/120-cookies-and-sop.md)
121. [Cross-origin form submission blocking](chapters/15-browser-maturation/121-cross-origin-form-blocking.md)
122. [A pocket JavaScript: expression evaluator for onclick](chapters/15-browser-maturation/122-pocket-javascript.md)
123. [Entropy: virtio-rng and a kernel CSPRNG](chapters/15-browser-maturation/123-entropy-and-csprng.md)
124. [BearSSL builds for our userspace](chapters/15-browser-maturation/124-bearssl-build.md)
125. [In-guest TLS handshake](chapters/15-browser-maturation/125-tls-handshake.md)
126. [X.509 chain validation](chapters/15-browser-maturation/126-chain-validation.md)
127. [Browser https:// (native TLS)](chapters/15-browser-maturation/127-browser-https.md)
128. [TLS trust store (multi-anchor)](chapters/15-browser-maturation/128-trust-store.md)
129. [PEM ingest and the recursive chain walk](chapters/15-browser-maturation/129-pem-ingest.md)
130. [Direct outbound HTTPS: real public CAs](chapters/15-browser-maturation/130-public-trust.md)
131. [URL-bar UX after native TLS](chapters/15-browser-maturation/131-url-bar-relatives.md)

### Part XVI — Filesystem Architecture

The VFS grew six prefix-special-cased mounts (root ramfs, `/mnt/`,
`/bin/`, `/tmp/`, `/data/`, `/proc/`) over the course of the book.
This section retires the prefix ladder in favour of a classical
Unix VFS — a mount table dispatching through a `struct fs_ops`
vtable — and then uses that abstraction to support user-space
filesystem servers (a 9P-shaped RPC, so any userspace daemon can
become a mountable filesystem).

Chapter 132 (the mount-table refactor) is **Done**, landed across
seven incremental steps (113a–113g), each one sweep-green before
the next began. Chapter 140 (userspace filesystem servers) is the
next milestone and still **PLAN**.

132. [A real VFS — mount table and `struct fs_ops`](chapters/16-filesystem-architecture/132-mount-table-and-vtable.md) *(plan + design contract)*
133. [Step 1 — mount table types + `vfs_resolve`](chapters/16-filesystem-architecture/133-mount-table-types.md)
134. [Step 2 — porting procfs onto `fs_ops`](chapters/16-filesystem-architecture/134-procfs-port.md)
135. [Step 3 — porting tmpfs onto `fs_ops`](chapters/16-filesystem-architecture/135-tmpfs-port.md)
136. [Step 4 — porting OSFS-1 and OSFS-2](chapters/16-filesystem-architecture/136-osfs-port.md)
137. [Step 5 — embedded ramfs as a root mount](chapters/16-filesystem-architecture/137-ramfs-as-root.md)
138. [Step 6 — `SYS_MOUNTS` and `/bin/mount`](chapters/16-filesystem-architecture/138-sys-mounts.md)
139. [Step 7 — `MOUNT_RO` + `EROFS_VFS` hardening](chapters/16-filesystem-architecture/139-mount-ro-hardening.md)
140. [PLAN: User-space filesystem servers (9P-shaped)](chapters/16-filesystem-architecture/140-userspace-filesystem-servers.md) *(plan; implementation in 114a–114f)*
141. [Step 1 — kernel userfs module + `FD_USERFS_FILE`](chapters/16-filesystem-architecture/141-kernel-userfs-module.md)
142. [Step 2 — `SYS_MOUNT` / `SYS_UMOUNT`](chapters/16-filesystem-architecture/142-sys-mount-umount.md)
143. [Step 3 — `libfs` + `/bin/echofs`](chapters/16-filesystem-architecture/143-libfs-and-echofs.md)
144. [Step 4 — porting `clipboardd` to userfs](chapters/16-filesystem-architecture/144-clipboardd-port.md)
145. [Step 5 — porting `procfs` to `/bin/procd`](chapters/16-filesystem-architecture/145-procd-port.md)
146. [Step 6 — per-request timeouts and deadlock detection](chapters/16-filesystem-architecture/146-timeouts-and-deadlock.md)

### Part XVII — A C compiler on the OS

After Part XVI the OS has the syscalls and filesystem
shape a compiler needs. Part XVII stands up a tiny
native C toolchain — libc, assembler, linker, compiler,
build driver — so that source files in `/data/src/`
can be compiled to binaries in `/tmp/` from inside the
running OS itself.

The compiler that ships is `/bin/cc`: a from-scratch,
under-1000-line tool that accepts a deliberately small
subset of C (literals, locals, `int` arithmetic, calls
to a fixed-shape `printf`, `return`/`exit`). It is not
GCC, it is not TinyCC, and it does not self-host. What
it does do is close the loop end-to-end — source on
disk, compiler in the guest, executable in the guest,
all without leaving QEMU. Chapter 161 spells out what
the gap between `/bin/cc` and a self-hosting compiler
actually looks like; chapter 163 closes the loop with
a Build button in notepad.

A real GCC port is left for Part XVIII (planned). Part
XVII deliberately ships the smallest thing that proves
the OS can host its own toolchain, then stops.

147. [PLAN: A C compiler that runs on the OS](chapters/17-a-c-compiler-on-the-os/147-c-compiler-strategy.md) *(strategy doc for the whole section)*
148. [A POSIX-ish libc, part 1: stdio, errno, env](chapters/17-a-c-compiler-on-the-os/148-libc-stdio-and-env.md)
     149. [Step 1 — errno populated by every syscall wrapper](chapters/17-a-c-compiler-on-the-os/149-errno.md)
     150. [Step 2 — FILE *, fopen, fread, fwrite, fseek, fprintf](chapters/17-a-c-compiler-on-the-os/150-stdio.md)
     151. [Step 3 — environ[], getenv, setenv, unsetenv, putenv](chapters/17-a-c-compiler-on-the-os/151-env-arena.md)
     152. [Step 4 — POSIX errno convention + strerror + cat/wc/head/tail on FILE *](chapters/17-a-c-compiler-on-the-os/152-errno-convention.md)
153. [A POSIX-ish libc, part 2: stat, fstat, fcntl, dirent](chapters/17-a-c-compiler-on-the-os/153-libc-stat-fcntl-dirent.md)
154. [An AArch64 assembler: /bin/as](chapters/17-a-c-compiler-on-the-os/154-bin-as-assembler.md)
155. [An AArch64 linker: /bin/ld and /bin/ar](chapters/17-a-c-compiler-on-the-os/155-bin-ld-linker.md)
156. [Bootstrap glue: crt0, crti, crtn, libgcc-style stubs](chapters/17-a-c-compiler-on-the-os/156-crt0-and-libgcc-stubs.md)
157. [/bin/cc: a one-chapter native C compiler](chapters/17-a-c-compiler-on-the-os/157-bin-cc.md)
158. [The host cross-toolchain contract](chapters/17-a-c-compiler-on-the-os/158-cross-toolchain-contract.md)
159. [/bin/cc grows variables and arithmetic](chapters/17-a-c-compiler-on-the-os/159-cc-variables-and-arithmetic.md)
160. [The first native compile from disk](chapters/17-a-c-compiler-on-the-os/160-first-native-compile.md)
161. [The self-hosting gap, and why we don't close it here](chapters/17-a-c-compiler-on-the-os/161-self-hosting-bootstrap.md)
162. [A tiny build driver: /bin/make](chapters/17-a-c-compiler-on-the-os/162-make-port.md)
163. [Notepad gets a Build button: an in-OS dev loop](chapters/17-a-c-compiler-on-the-os/163-notepad-build-button.md)

### Part XVIII — Real GCC, real software, real Doom

Part XVII shipped a deliberately tiny `/bin/cc` and an
honest accounting of the self-hosting gap it doesn't
close. Part XVIII closes that gap the practical way:
cross-build real GCC + GNU binutils on the host
targeting our OS, ship the binaries as `/bin/gcc`,
`/bin/as`, `/bin/ld`, and grow libc until real upstream
software compiles and runs in-guest.

The section is shaped around a concrete demo: download
the Doom source over HTTP, untar it, build it with
`/bin/gcc`, and play it on the framebuffer. Phase 1
(chapters 165–174) cross-builds Doom on the host and
proves the platform integration. Phase 2 (chapters
131a–133f) brings up real GCC + binutils in-guest and
rebuilds Doom from source on the booted OS.

The section is multi-month. The chapter sequence is the
plan; deviations get written down as they happen.

164. [PLAN: Real GCC on the OS, and a playable Doom](chapters/18-real-gcc-and-real-software/164-plan-real-gcc-and-doom.md) *(plan; implementation in 128a–133g)*
   - [165 — setjmp / longjmp](chapters/18-real-gcc-and-real-software/165-setjmp-longjmp.md)
   - [166 — raise / abort / full POSIX signal table](chapters/18-real-gcc-and-real-software/166-signal-and-raise.md)
   - [167 — ctype / assert / str* family](chapters/18-real-gcc-and-real-software/167-ctype-assert-string.md)
   - [168 — POSIX `<time.h>` (struct tm, gmtime_r, strftime)](chapters/18-real-gcc-and-real-software/168-posix-time.md)
   - [169 — `<stdlib.h>`: qsort, bsearch, strtol, getopt](chapters/18-real-gcc-and-real-software/169-stdlib-getopt.md)
   - [170 — real printf + scanf (%o, precision, scanf)](chapters/18-real-gcc-and-real-software/170-real-printf-scanf.md)
   - [171 — FP / SIMD at EL0](chapters/18-real-gcc-and-real-software/171-fp-simd-at-el0.md)
   - [172 — Doom (the port)](chapters/18-real-gcc-and-real-software/172-doomgeneric-port.md)
   - [173 — Staging the WAD (so Doom actually plays)](chapters/18-real-gcc-and-real-software/173-doom-wad-staging.md)
   - [174 — Doom plays (closing Phase 1)](chapters/18-real-gcc-and-real-software/174-doom-plays.md)
   - [175 — Binutils with an `aarch64-osdev` target](chapters/18-real-gcc-and-real-software/175-binutils-target.md)
   - [176 — `aarch64-osdev-cc`: the target-compiler seam](chapters/18-real-gcc-and-real-software/176-osdev-cc-wrapper.md)
   - [177 — Cross-build seam: link-mode wrapper and the libc-gap catalog](chapters/18-real-gcc-and-real-software/177-cross-build-seam.md)
   - [178 — Closing the libc gap for cross-built `libiberty`](chapters/18-real-gcc-and-real-software/178-libc-gaps.md)
   - [179 — `ld` in-guest: cross-building binutils' linker](chapters/18-real-gcc-and-real-software/179-binutils-ld-in-guest.md)
   - [180 — Replacing `/bin/as` and `/bin/ld` with the real binutils](chapters/18-real-gcc-and-real-software/180-replace-bin-as-ld.md)
   - [181 — GCC with an `aarch64-osdev` target](chapters/18-real-gcc-and-real-software/181-gcc-target.md)
   - [182 — GMP, MPFR, MPC as in-tree prerequisites](chapters/18-real-gcc-and-real-software/182-gcc-prereqs.md)
   - [183 — Cross-building `aarch64-osdev-gcc`](chapters/18-real-gcc-and-real-software/183-cross-build-xgcc.md)
   - [184 — Real cross-compiler specs: retiring the wrapper](chapters/18-real-gcc-and-real-software/184-real-cross-specs.md)
   - [185 — Cross-building GMP/MPFR/MPC for the guest sysroot](chapters/18-real-gcc-and-real-software/185-gcc-prereqs-for-guest.md)
   - [186 — `gcc hello.c` works on the OS](chapters/18-real-gcc-and-real-software/186-gcc-runs-in-guest.md)
   - [187 — `gcc hello.c -o hello` (no escape-hatch flags)](chapters/18-real-gcc-and-real-software/187-gcc-default-specs.md)
   - [188 — `/bin/gcc` builds a medium real program (`bf`)](chapters/18-real-gcc-and-real-software/188-gcc-builds-real-program.md)
   - [189 — `#include <stdio.h>` works in the guest](chapters/18-real-gcc-and-real-software/189-libc-headers-on-disk.md)
   - [190 — `sys/` headers without a hierarchical filesystem](chapters/18-real-gcc-and-real-software/190-osfs1-subdirs.md)
   - [191 — `/bin/tar` (ustar reader)](chapters/18-real-gcc-and-real-software/191-tar.md)
   - [192 — expanding `/bin/make` for a real multi-file build](chapters/18-real-gcc-and-real-software/192-make-expansion.md)
   - [193 — in-guest Doom rebuild, pilot (three real vendor files)](chapters/18-real-gcc-and-real-software/193-doom-pilot.md)
   - [194 — in-guest Doom rebuild, full vendor compile (77 files)](chapters/18-real-gcc-and-real-software/194-full-doom-compile.md)
   - [195 — in-guest Doom link (binutils @file response files)](chapters/18-real-gcc-and-real-software/195-link-doomgeneric-in-guest.md)
   - [196 — the rebuilt Doom plays](chapters/18-real-gcc-and-real-software/196-rebuilt-doom-plays.md)
   - [197 — Real key-up events (retiring the doom timed-release shim)](chapters/18-real-gcc-and-real-software/197-real-key-up-events.md)

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
| V    | **38 — virtio-gpu framebuffer** | **Done** | [Chapter 45](chapters/05-devices/045-virtio-gpu-framebuffer.md) |
| V    | **39 — virtio-input keyboard** | **Done** | [Chapter 46](chapters/05-devices/046-virtio-input-keyboard.md) |
| VI   | **40 — window manager + GUI syscalls** | **Done** | [Chapter 47](chapters/06-gui/047-window-manager-and-gui-syscalls.md) |
| VI   | **41 — virtio-tablet mouse + WM focus/drag/close** | **Done** | [Chapter 48](chapters/06-gui/048-virtio-tablet-and-wm-mouse.md) |
| VI   | **42 — gui_term: terminal-in-a-window via pipe+spawn** | **Done** | [Chapter 49](chapters/06-gui/049-gui-terminal-and-pipe-spawn.md) |
| VI   | **43 — notepad: GUI text editor + writable-tmpfs save** | **Done** | [Chapter 50](chapters/06-gui/050-notepad-and-tmpfs-roundtrip.md) |
| VI   | **44 — launcher: clickable mouse-driven app launcher** | **Done** | [Chapter 51](chapters/06-gui/051-launcher-and-click-to-spawn.md) |
| VI   | **45 — WM z-order bug fix + painter's-algorithm hardening** | **Done** | [Chapter 52](chapters/06-gui/052-wm-z-order-bug.md) |
| VI   | **46 — boot to desktop: auto-spawn launcher + gradient wallpaper** | **Done** | [Chapter 53](chapters/06-gui/053-boot-to-desktop.md) |
| VI   | **47 — taskbar + window list / raise syscalls + always-on-top** | **Done** | [Chapter 54](chapters/06-gui/054-taskbar-and-window-list.md) |
| VI   | **48 — clock in the taskbar** | **Done** | [Chapter 55](chapters/06-gui/055-clock-in-taskbar.md) |
| VI   | **49 — toast notifications + child-reap fix** | **Done** | [Chapter 56](chapters/06-gui/056-toast-notifications.md) |
| VI   | **50 — userspace wallpaper + yield/IRQ race fix** | **Done** | [Chapter 57](chapters/06-gui/057-userspace-wallpaper-and-yield-race.md) |
| VI   | **51 — window minimize / restore** | **Done** | [Chapter 58](chapters/06-gui/058-window-minimize-restore.md) |
| VI   | 52+ — menus + window snapping | Not started | Stubs |
| VII  | **52 — virtio-net + in-kernel ARP self-test** | **Done** | [Chapter 59](chapters/07-networking/059-virtio-net.md) |
| VII  | **53 — Ethernet + ARP cache + IPv4 stack** | **Done** | [Chapter 60](chapters/07-networking/060-ethernet-arp-ipv4.md) |
| VII  | **54 — ICMP echo + UDP + DHCP client** | **Done** | [Chapter 61](chapters/07-networking/061-icmp-udp-dhcp.md) |
| VII  | **55 — TCP client (state machine + buffered conn) + boot HTTP self-test** | **Done** | [Chapter 62](chapters/07-networking/062-tcp-and-sockets.md) |
| VII  | **56 — socket syscalls (FD_SOCKET) + /bin/httpget userspace tool** | **Done** | [Chapter 63](chapters/07-networking/063-socket-syscalls-and-httpget.md) |
| VII  | **57 — DHCP option-6 capture + DNS resolver + SYS_RESOLVE + httpget hostnames** | **Done** | [Chapter 64](chapters/07-networking/064-dns-resolver.md) |
| VII  | **58 — URL / HTTP parser + httpget URL form (one-hop redirect)** | **Done** | [Chapter 65](chapters/07-networking/065-url-and-http-parser.md) |
| VIII | **59 — HTML5 tokenizer (header-only, /bin/htmltok driver)** | **Done** | [Chapter 66](chapters/08-browser/066-html-tokenizer.md) |
| VIII | **60 — DOM (header-only, /bin/htmldom driver)** | **Done** | [Chapter 67](chapters/08-browser/067-dom-construction.md) |
| VIII | **61 — CSS parser + selector matcher (/bin/cssparse driver)** | **Done** | [Chapter 68](chapters/08-browser/068-css-parser.md) |
| VIII | **62 — CSS-driven layout engine + /bin/layout driver** | **Done** | [Chapter 69](chapters/08-browser/069-block-and-inline-layout.md) |
| VIII | **63 — /bin/browser: paint/plain/ANSI/GUI + resizable windows + horizontal scroll** | **Done** | [Chapter 70](chapters/08-browser/070-bin-browser.md) |
| VIII | **64 — browser UX: toolbar, address bar, click-to-navigate, back/forward + TCP perf + user-stack bump** | **Done** | Postscripts in [27](chapters/05-devices/026-argc-argv.md), [63](chapters/07-networking/062-tcp-and-sockets.md), [71](chapters/08-browser/070-bin-browser.md) |
| IX   | 65 — fork (eager copy) + exec | Planned | Stubs [73](chapters/09-process-model/072-aarch64-fork-and-as-copy.md), [74](chapters/09-process-model/073-exec-and-as-rebuild.md) |
| IX   | 66 — copy-on-write | Planned | Stub [75](chapters/09-process-model/074-copy-on-write.md) |
| IX   | 67 — signals (SIGINT → sigaction → SIGCHLD/waitpid) | Planned | Stubs [76](chapters/09-process-model/075-signals-sigint.md)–[78](chapters/09-process-model/077-sigchld-and-waitpid.md) |
| IX   | 68 — job control in the shell | Planned | Stub [79](chapters/09-process-model/078-job-control.md) |
| X    | 69–71 — OSFS-2: writable FS + journal | Planned | Stubs [80](chapters/10-filesystem/081-writable-fs-design.md)–[85](chapters/10-filesystem/086-subdirectories.md) |
| XI   | **72 — SMP boot: PSCI + secondary CPU** | **Done** | [Chapter 87](chapters/11-smp-and-memory/087-second-core-psci.md) |
| XI   | **72b — Atomics, barriers, and the audit of every shared global** | **Done** | [Chapter 88](chapters/11-smp-and-memory/088-atomics-and-spinlocks.md) |
| XI   | **72c — IPIs through GICv3 SGIs** | **Done** | [Chapter 89](chapters/11-smp-and-memory/089-ipis-via-gicv3.md) |
| XI   | **73 — SMP runqueue (per-CPU current + runq, no migration)** | **Done** | [Chapter 90](chapters/11-smp-and-memory/090-smp-runqueue.md) |
| XI   | **74 — mmap + unified page cache** | **Done** | [Chapter 91](chapters/11-smp-and-memory/091-mmap-and-page-cache.md) |
| XI   | **75 — userspace threads (clone)** | **Done** | [Chapter 92](chapters/11-smp-and-memory/092-userspace-threads.md) |
| XI   | **75b — real SMP scheduling: per-CPU timers, locked sleeper walks, CLONE_CPU** | **Done** | [Chapter 93](chapters/11-smp-and-memory/093-real-smp-scheduling.md) |
| XI   | **75c — sharing the FD table: CLONE_FILES + refcounted fd_table** | **Done** | [Chapter 94](chapters/11-smp-and-memory/094-clone-files.md) |
| XI   | **75d — browser parser thread: HTML/CSS/layout off the GUI core** | **Done** | [Chapter 95](chapters/11-smp-and-memory/095-browser-parser-thread.md) |
| XII  | **76 — RTC + wall-clock time** | **Done** | [Chapter 96](chapters/12-system-services/096-rtc-and-wallclock.md) |
| XII  | **77 — virtio-snd + boot chime** | **Done** | [Chapter 97](chapters/12-system-services/097-virtio-snd.md) |
| XII  | **78 — PNG + image cache** | **Done** | [Chapter 98](chapters/12-system-services/098-png-and-image-cache.md) |
| XII  | **78b — PNG: palette / gray + content-type sniff** | **Done** | [Chapter 99](chapters/12-system-services/099-png-extended.md) |
| XII  | **78c — intrinsic image sizing + resize race** | **Done** | [Chapter 100](chapters/12-system-services/100-intrinsic-image-sizing.md) |
| XII  | **79 — procfs + ps + top** | **Done** | [Chapter 101](chapters/12-system-services/101-procfs-ps-top.md) |
| XII  | **80 — strace via /proc/&lt;pid&gt;/trace** | **Done** | [Chapter 102](chapters/12-system-services/102-strace.md) |
| XII  | **80a — guard pages** | **Done** | [Chapter 103](chapters/12-system-services/103-guard-pages.md) |
| XII  | **80b -- TrueType fonts in the kernel** | **Done** | [Chapter 104](chapters/12-system-services/104-truetype-fonts.md) |
| XIII | **92 -- TCP passive open (LISTEN, SYN_RECEIVED)** | **Done** | [Chapter 105](chapters/13-tcp-server/105-passive-open-listen.md) |
| XIII | **93 -- accept() syscall + /bin/echod** | **Done** | [Chapter 106](chapters/13-tcp-server/106-accept-and-server-sockets.md) |
| XIII | **94 -- /bin/httpd static-file HTTP server** | **Done** | [Chapter 107](chapters/13-tcp-server/107-bin-httpd.md) |
| XIII | 95 -- TCP loopback (lo0, 127.0.0.0/8) | **Done** | [Chapter 108](chapters/13-tcp-server/108-tcp-loopback.md) |
| XIII | **96 -- httpd as forwarding proxy (TLS bridge)** | **Done** | [Chapter 109](chapters/13-tcp-server/109-httpd-tls-bridge.md) |
| XIII | **97 -- browser uses in-guest httpd as its proxy** | **Done** | [Chapter 110](chapters/13-tcp-server/110-browser-uses-in-guest-httpd.md) |
| XIII | **98 -- end-to-end loop (browser <-> in-guest httpd via lo0)** | **Done** | [Chapter 111](chapters/13-tcp-server/111-end-to-end-loop.md) |
| XIV  | 80b — named IPC + service supervisor | Done | [Chapter 112](chapters/14-userspace-services/112-ipc.md) |
| XIV  | 80c — clipboard (userspace, over IPC) | Done | [Chapter 113](chapters/14-userspace-services/113-clipboard.md) |
| XIV  | 90d — userspace window-buffer access | Planned | Stub [Chapter 114](chapters/14-userspace-services/114-userspace-window-buffers.md) |
| XIV  | 90e — font rendering moves to userspace | Planned | Stub [Chapter 115](chapters/14-userspace-services/115-userspace-font-server.md) |
| XIV  | 90f — GUI SDK rasterisation moves to userspace | Done | [Chapter 116](chapters/14-userspace-services/116-gui-sdk-userspace-drawing.md) |
| XIV  | 90g — window server moves to userspace | Done | [Chapter 117](chapters/14-userspace-services/117-userspace-window-server.md) — kernel compositor retired (`compose_all`, `blit_window`, `blit_cursor`, `paint_wallpaper`, `wm_draw_text_fb`, `wm_blend_pixel`, the cursor sprite — all deleted from `kernel/core/wm.c`); wsd is now the sole owner of the scanout, painting wallpaper + every mapped window + flushing via new `SYS_FB_PRESENT = 88` syscall; legacy render syscalls (`wm_present`, `wm_fill_rect`, `wm_draw_text`, `wm_flush`) stubbed to no-op success so legacy apps don't crash but draw nothing until C.5 ports them to `wmclient` one-by-one |
| XIV  | 90h — wsd-owned decoration, cursor, input routing, resize | Done | [Chapter 118](chapters/14-userspace-services/118-userspace-decoration-input-resize.md) — `wsd` paints title bars, close/minimize buttons, and the cursor sprite from `libgui/draw.h`; new kernel surface `SYS_POINTER_STATE = 89`, `SYS_GUI_MOVE_WINDOW = 90`, `SYS_GUI_DELIVER_EVENT = 91`, `SYS_GUI_SET_INPUT_PASSTHROUGH = 92` plus `SYS_WIN_FB_RESIZE` lets `wsd` own pointer hit-testing via the **input shadow** pattern (each `wsd` window is paired with a hidden, passthrough-flagged kernel WM window that owns the event queue `gui_poll_event` reads from); 60 Hz input poller in `wsd` does drag, raise, close-button delivery, minimize, hover, and live resize (kernel `win_fb` reallocates pages, client remaps via `wm_window_remap_fb`, `GUI_EVENT_RESIZE` delivered to the shadow's queue) |
| XVI  | 84a — VFS refactor: mount table + `struct fs_ops` vtable | Done | [Chapter 132](chapters/16-filesystem-architecture/132-mount-table-and-vtable.md) — landed across seven steps (113a–113g) per the plan; new types `struct fs_ops` + `struct mount` + `MOUNT_RO` flag + `EROFS_VFS=30` / `ENOSYS_VFS=38` errnos in [`kernel/core/vfs.h`](kernel/core/vfs.h); longest-prefix `vfs_resolve` with a "/" catchall that loses every tie ([Chapter 133](chapters/16-filesystem-architecture/133-mount-table-types.md)); five drivers ported onto the vtable with a uniform `_strip_slash` adapter pattern — [procfs](chapters/16-filesystem-architecture/134-procfs-port.md) ([`kernel/core/procfs.c`](kernel/core/procfs.c)), [tmpfs](chapters/16-filesystem-architecture/135-tmpfs-port.md) ([`kernel/core/tmpfs.c`](kernel/core/tmpfs.c)), [OSFS-1+OSFS-2](chapters/16-filesystem-architecture/136-osfs-port.md) ([`kernel/core/osfs.c`](kernel/core/osfs.c) + [`kernel/core/osfs2.c`](kernel/core/osfs2.c)) — OSFS-1 registers TWICE with the same `fs_ops`/`NULL` cookie to serve both `/mnt` and `/bin`; [embedded ramfs as the root mount](chapters/16-filesystem-architecture/137-ramfs-as-root.md) at `"/"` with `MOUNT_RO` so the dispatcher in `vfs_open`/`vfs_open_into`/`vfs_load` finally has no legacy fallback; new `SYS_MOUNTS = 95` syscall + `/bin/mount` user-visible tool + libc `mounts()` wrapper + `struct mount_info { char prefix[32]; unsigned flags; }` ABI ([Chapter 138](chapters/16-filesystem-architecture/138-sys-mounts.md), [`userspace/mount/mount.c`](userspace/mount/mount.c), [`userspace/libc/syscall.h`](userspace/libc/syscall.h)); MOUNT_RO+EROFS_VFS hardening pass ([Chapter 139](chapters/16-filesystem-architecture/139-mount-ro-hardening.md)) fixes a real bug where mutations against RO mounts whose driver lacked the op pointer returned EINVAL_VFS instead of EROFS_VFS — sys_unlink and sys_mkdir now check `MOUNT_RO` BEFORE checking `m->ops->{unlink,mkdir}`; per-step regression sweeps green at 113a/build, 113b/test_procfs, 113c/5 tests, 113d/7 tests, 113e/10 tests, 113f/test_mounts (7 PASS), 113g/test_mount_ro (12 PASS); save_dialog mount-table iteration deferred to a future UI-redesign chapter per the 113f writeup; userspace fs servers + `SYS_MOUNT`/`SYS_UMOUNT` reserved at 96/97 for chapter 140 |
| XVI  | 84b — User-space filesystem servers (9P-shaped) | Planned | Design [Chapter 140](chapters/16-filesystem-architecture/140-userspace-filesystem-servers.md) |
| XV   | 82 — HTML forms + cookies + SOP + pocket-JS | In progress | Done: [109](chapters/15-browser-maturation/119-html-forms.md), [110](chapters/15-browser-maturation/120-cookies-and-sop.md), [110a](chapters/15-browser-maturation/121-cross-origin-form-blocking.md), [111](chapters/15-browser-maturation/122-pocket-javascript.md) — pocket JS in [`userspace/libc/pocketjs.h`](userspace/libc/pocketjs.h) (~750 lines) + DOM bridge in [`userspace/browser/jsdom.h`](userspace/browser/jsdom.h); `onclick` fires via MOUSE_DOWN hook in [`userspace/browser/browser.c`](userspace/browser/browser.c); [`browser --check-js`](scripts/test_browser_js.py) regression at 20/20 |
| XV   | 83 — TLS prerequisite: entropy (virtio-rng + CSPRNG + getrandom) | Done | [Chapter 123](chapters/15-browser-maturation/123-entropy-and-csprng.md) — virtio-rng driver ([`kernel/device/virtio_rng.c`](kernel/device/virtio_rng.c)), ChaCha20 CSPRNG with virtio-rng reseed every 256 KiB ([`kernel/core/random.c`](kernel/core/random.c)), `SYS_GETRANDOM = 94`, `/bin/getrand` userspace tool, regression [`test_getrand.py`](scripts/test_getrand.py); BearSSL port (chapters 124–128) builds on this |
| XV   | 83a — BearSSL freestanding build + SHA-256 KAT | Done | [Chapter 124](chapters/15-browser-maturation/124-bearssl-build.md) — vendored BearSSL 0.6 ([`vendor/bearssl/`](vendor/bearssl/)) builds into [`build/vendor/bearssl/libbearssl.a`](Makefile) with a 5-symbol `<string.h>` shim ([`vendor/bearssl-shim/string.h`](vendor/bearssl-shim/string.h)) and extern mem*/strlen/time in [`userspace/libc/cstring.c`](userspace/libc/cstring.c); `/bin/tlstest` ([`userspace/tlstest/tlstest.c`](userspace/tlstest/tlstest.c)) verifies SHA-256("") and SHA-256("abc") against NIST vectors at boot ([`scripts/test_tlstest.py`](scripts/test_tlstest.py)) |
| XV   | 83b — In-guest TLS handshake (knownkey, loopback) | Done | [Chapter 125](chapters/15-browser-maturation/125-tls-handshake.md) — `tls_socket_*` client surface in [`userspace/libc/tls_socket.c`](userspace/libc/tls_socket.c) wraps BearSSL's `br_sslio_*` over our chapter-104 TCP sockets, seeds via `SYS_GETRANDOM`, pins the leaf cert's RSA-2048 public key via `br_x509_knownkey_*` (extracted at startup with `br_x509_decoder_*`); new `/bin/httpsd` ([`userspace/httpsd/httpsd.c`](userspace/httpsd/httpsd.c)) spawned on port 8443 by [`init.c`](userspace/init/init.c) serves a marker body inside the TLS record stream; BearSSL sample CN=localhost cert chain re-exported via [`vendor/testcerts/test_chain.c`](vendor/testcerts/test_chain.c) so no host openssl tooling is required; `tlstest --handshake 127.0.0.1 8443` regression ([`scripts/test_tls_handshake.py`](scripts/test_tls_handshake.py)) passes end-to-end |
| XV   | 83c — X.509 chain validation (BearSSL minimal + wallclock) | Done | [Chapter 126](chapters/15-browser-maturation/126-chain-validation.md) — `tls_socket_init_chain_from_anchor` ([`userspace/libc/tls_socket.c`](userspace/libc/tls_socket.c)) bakes a `br_x509_trust_anchor` at runtime from a DER CA cert (DN bytes accumulated via `br_x509_decoder_*`'s `append_dn` callback, RSA `n`/`e` copied out of the decoder pad), wires it into BearSSL's `br_x509_minimal_*` validator, and seeds the validator's clock from `SYS_GETTIMEOFDAY` (chapter 96) with the engine-internal offset of 719528 days for the Unix epoch; `/bin/tlstest --handshake-ca 127.0.0.1 8443` exercises end-to-end chain validation against the sample intermediate CA, regression in [`scripts/test_tls_chain.py`](scripts/test_tls_chain.py) — first userspace consumer of RTC outside `/bin/date` |
| XV   | 83d — Browser https:// (native TLS, in-guest anchor) | Done | [Chapter 127](chapters/15-browser-maturation/127-browser-https.md) — five-call `br_conn_t` abstraction in [`userspace/browser/browser.c`](userspace/browser/browser.c) tags a fetch as TCP or TLS so `http_fetch_one` / `drain_conn` carry both with one body; `br_conn_open` heap-allocates the ~42 KB `tls_socket_t`, initialises it with the chapter-112c sample intermediate as the trust anchor, and runs the chain-validating handshake; `canonicalize_url` `https://` case gated on `g_proxy_was_set` so the legacy chapter-106b proxy rewrite stays alive for `proxytest`/`test_browser_hn_*` callers that explicitly set `BROWSER_PROXY` while the bare browser binary uses real TLS; literal-`localhost` shortcut in the resolver skips `SYS_RESOLVE` so the sample chain's CN=localhost leaf passes SNI; Makefile gains `BEARSSL_INC` per-file override for `browser.o` and pulls `libbearssl.a` into the `BROWSER_ELF` `--start-group` window; regression [`scripts/test_browser_https.py`](scripts/test_browser_https.py) drives `browser https://localhost:8443/` and asserts the chapter-112d TLS-OK line plus the decrypted body reaching the renderer; `scripts/https_proxy.py` retired from the boot path but kept on disk per `/memories/debug-scripts-policy.md` |
| XV   | 83e — TLS trust store (multi-anchor, RSA + ECDSA) | Done | [Chapter 128](chapters/15-browser-maturation/128-trust-store.md) — `tls_socket_t` gains an 8-slot `anchors[]` array with per-row backing buffers for RSA `n`/`e` AND ECDSA `q` ([`userspace/libc/tls_socket.h`](userspace/libc/tls_socket.h)); `tls_bake_anchor_into` ([`userspace/libc/tls_socket.c`](userspace/libc/tls_socket.c)) branches on `pk->key_type` so EC pubkeys land in `anchor_pk_q[i]` with `pk->key.ec.curve` recorded; new `tls_socket_init_chain_multi` + `tls_socket_init_chain_from_bundle` APIs (chapter-112d's `_from_anchor` is now a wrapper over `_multi(1)`); `/mnt/ca.bundle` ships in OSFS as a framed `"CAB1"` blob generated at build time by [`scripts/mkcabundle.py`](scripts/mkcabundle.py) from BearSSL's `chain-rsa.h` and `chain-ec.h` headers (~1.3 KiB, 2 anchors); browser ([`userspace/browser/browser.c`](userspace/browser/browser.c)) slurps the bundle at first `https://` fetch via `load_ca_bundle_once`, prefers it over the in-binary fallback, and prints `source=bundle` in the TLS-OK line; `/bin/httpsd` ([`userspace/httpsd/httpsd.c`](userspace/httpsd/httpsd.c)) gains a `--ec` flag that flips to `br_ssl_server_init_full_ec` with the new [`vendor/testcerts/test_chain_ec.c`](vendor/testcerts/test_chain_ec.c) (BearSSL `chain-ec.h` re-export, separate TU to avoid `CERT0/CERT1` `static const` collision); [`init.c`](userspace/init/init.c) spawns a second httpsd as `--ec 8444` alongside the original 8443; regression [`scripts/test_browser_https_multi.py`](scripts/test_browser_https_multi.py) fetches both ports back-to-back asserting `source=bundle` for each |
| XV   | 83f — PEM ingest + recursive chain walk | Done | [Chapter 129](chapters/15-browser-maturation/129-pem-ingest.md) — [`scripts/mkcabundle.py`](scripts/mkcabundle.py) grows a `--pem PATH` mode (regex-extracted `-----BEGIN CERTIFICATE-----` blocks, whitespace-collapsed base64 decode via `base64.b64decode(..., validate=True)`); Makefile retargets the `assets/osfs/ca.bundle` rule at the BearSSL sample ROOT PEMs (`vendor/bearssl/samples/cert-root-{rsa,ec}.pem`) instead of the intermediate-from-header path so `httpsd`'s served leaf+ica chain now forces the validator's full root → intermediate → leaf walk in both algorithms; `TLS_MAX_ANCHORS` raised from 8 to 32 ([`userspace/libc/tls_socket.h`](userspace/libc/tls_socket.h)) and `BR_CA_BUNDLE_MAX` from 32 KiB to 256 KiB ([`userspace/browser/browser.c`](userspace/browser/browser.c)) to fit a future Mozilla NSS drop without a recompile; regression [`scripts/test_tls_pem_bundle.py`](scripts/test_tls_pem_bundle.py) asserts the host-side `CAB1`-framed bundle layout (magic + count + 1208 bytes for both BearSSL roots) AND end-to-end 2-link chain validation against both `:8443` (RSA) and `:8444` (ECDSA) |
| XV   | 83g — Direct outbound HTTPS to real public sites | Done | [Chapter 130](chapters/15-browser-maturation/130-public-trust.md) — new [`scripts/fetch_public_roots.sh`](scripts/fetch_public_roots.sh) cross-platform-probes the host's system CA store (`/etc/ssl/cert.pem` on macOS, `/etc/ssl/certs/ca-certificates.crt` on Debian, `/etc/pki/tls/certs/ca-bundle.crt` on RHEL, Homebrew openssl@3 paths) and atomic-copies it to `vendor/testcerts/public-roots.pem` (.gitignored, fetched on first build); Makefile [`assets/osfs/ca.bundle`](Makefile) rule folds the resulting ~140 KiB / 128-anchor PEM in alongside the BearSSL sample roots via a third `--pem` argument so the production bundle now ships 130 anchors / 142,861 bytes; capacity bumps `TLS_MAX_ANCHORS` 32→256 ([`userspace/libc/tls_socket.h`](userspace/libc/tls_socket.h)) and `BR_CA_BUNDLE_MAX` 256 KiB→512 KiB ([`userspace/browser/browser.c`](userspace/browser/browser.c)) with no logic change; [`scripts/test_tls_pem_bundle.py`](scripts/test_tls_pem_bundle.py) loosens the count assertion from `== 2` to `>= 2` while keeping the end-to-end recursive-walk check; new manual probe [`scripts/_dbg_tls_outbound.py`](scripts/_dbg_tls_outbound.py) (per debug-scripts policy, not in regression sweep) drives `browser https://example.com/` and `browser https://news.ycombinator.com/` end-to-end through SLIRP → DNS → real public CA chain validation → SAN/CN match against an in-guest-unknown hostname, both confirmed working; nothing in `tls_socket.c` / `browser.c`'s TLS path needed to change — SNI, recursive walk, hostname check, and wall-clock validity were all already wired since chapters 125–129, the only blocker was the trust list itself |
| XV   | 83h — URL-bar UX after native TLS | Done | [Chapter 131](chapters/15-browser-maturation/131-url-bar-relatives.md) — `canonicalize_url` in [`userspace/browser/browser.c`](userspace/browser/browser.c) gains a new case (5a) that resolves path-relative references against an http(s):// current page via the existing chapter-110a `resolve_url` helper, gated on a "looks like a host" heuristic (input contains a `.` or `:` before the first `/` or `?` ⇒ host, otherwise relative); case (6) default flipped from "prepend `g_proxy_prefix`" to "prepend `https://`" so typing `news.ycombinator.com` (no scheme) goes through native TLS instead of the chapter-106b in-guest proxy; both changes gated on `g_proxy_was_set` so `BROWSER_PROXY=...` callers ([`scripts/test_browser_proxy.py`](scripts/test_browser_proxy.py), the `test_browser_hn_*` GUI suite) keep the legacy rewrite untouched; the first live HN load uncovered and fixed a latent bug in `resolve_url`'s path-relative branch where `last_slash` was pre-initialised to `path_start` so the `>=` test was always true and the "no path in base" else branch was unreachable — for a base URL like `https://news.ycombinator.com` (no trailing `/`) the merge planted a NUL byte at `out[host_len]` and every relative ref (`news.css?...`, `y18.gif`, `item?id=...`) silently truncated to the bare host, manifesting as three apparently unrelated symptoms (stylesheet skip-https log line with no path, png_decode failure decoding the HTML body as PNG, comment-link click reloading the front page), all collapsed by initialising `last_slash = 0` instead; the latent bug had survived the entire 109a→112g arc because no prior fetched page had a no-path base URL and link clicks had never gone through `resolve_url` before case (5a); follow-on cleanup deletes the vestigial `if (br_starts(abs, "https://")) { skip }` guard in `apply_link_sheets` since chapter 127 already made `http_fetch` → `br_conn_open` transport-agnostic, so HN's `news.css` now fetches over its own native-TLS handshake instead of being skipped; new manual probes [`scripts/_dbg_bare_host.py`](scripts/_dbg_bare_host.py) and [`scripts/_dbg_hn_resources.py`](scripts/_dbg_hn_resources.py) (per debug-scripts policy, not in regression sweep) assert the bare-host TLS path and the resolve_url/stylesheet-fetch fix respectively against live HN, the latter requiring TWO `TLS handshake OK with news.ycombinator.com:443` lines (page + stylesheet) and a `fetching stylesheet https://news.ycombinator.com/news.css...` line; fixes the user-reported bug that comment-link clicks from `https://news.ycombinator.com/` were navigating to `http://127.0.0.1:80/item?id=...` because HN's `<a href="item?id=...">` hrefs are relative path-references, not absolute URLs, AND the follow-on bug that those refs still re-loaded the front page after case (5a) landed because `resolve_url` was truncating them to the bare host |
| XVII | 100 — POSIX-ish libc growth for hosting a compiler | **Done** | [116a](chapters/17-a-c-compiler-on-the-os/149-errno.md)–[116d](chapters/17-a-c-compiler-on-the-os/152-errno-convention.md), [117](chapters/17-a-c-compiler-on-the-os/153-libc-stat-fcntl-dirent.md) |
| XVII | 101 — /bin/as + /bin/ld + /bin/ar + crt0 + libgcc-style stubs | **Done** | [118](chapters/17-a-c-compiler-on-the-os/154-bin-as-assembler.md)–[120](chapters/17-a-c-compiler-on-the-os/156-crt0-and-libgcc-stubs.md) |
| XVII | 102 — /bin/cc: one-chapter native C compiler (first compiler on the OS) | **Done** | [121](chapters/17-a-c-compiler-on-the-os/157-bin-cc.md) |
| XVII | 103 — Cross-toolchain contract + /bin/cc grows locals & arithmetic + first on-disk native compile + honest self-hosting-gap chapter | **Done** | [122](chapters/17-a-c-compiler-on-the-os/158-cross-toolchain-contract.md), [123](chapters/17-a-c-compiler-on-the-os/159-cc-variables-and-arithmetic.md), [124](chapters/17-a-c-compiler-on-the-os/160-first-native-compile.md), [125](chapters/17-a-c-compiler-on-the-os/161-self-hosting-bootstrap.md) |
| XVII | 104 — /bin/make + notepad "Build" button | **Done** | [126](chapters/17-a-c-compiler-on-the-os/162-make-port.md), [127](chapters/17-a-c-compiler-on-the-os/163-notepad-build-button.md) |
| XVIII | 105 — Real GCC on the OS + playable Doom | PLAN | [Chapter 164](chapters/18-real-gcc-and-real-software/164-plan-real-gcc-and-doom.md) — strategy: cross-build `aarch64-osdev-gcc` and `aarch64-osdev-binutils` on the host (do not bootstrap GCC on the OS), grow libc to host real upstream software (setjmp, signal, ctype, time, qsort, full printf, getopt), turn on FP/SIMD at EL0 (chapter 171; lift `-mgeneral-regs-only`, extend context switch with `q0..q31`+`fpsr`+`fpcr`), validate platform integration with a cross-built Doom (Phase 1: chapters 165–174) before sinking chapters into the toolchain port (Phase 2: chapters 175–196), end-state is `httpget <doom.tar.gz>` + `tar -xzf` + `make` + `./doomgeneric -iwad doom1.wad` all on the booted OS with no host involvement past "booted QEMU"; multi-month section, ~20 chapters, biggest unknowns are FP-at-EL0 context-switch interactions and the binutils/GCC libc-gap long tail |
| VIII | 65+ — book polish, more sites, TLS bridge | Not started | Stubs |
