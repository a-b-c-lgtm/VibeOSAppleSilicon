# Summary

[Book Index](INDEX.md)

# Part I — Foundations

- [Why Apple Silicon, why aarch64, why now](chapters/01-foundations/001-why-apple-silicon.md)
- [Toolchain and host setup](chapters/01-foundations/002-toolchain-and-host-setup.md)
- [First boot: QEMU virt, the boot stub, and PL011 UART](chapters/01-foundations/003-first-boot.md)

# Part II — Memory and Exceptions

- [The AArch64 execution environment (exception levels, registers, PSTATE)](chapters/02-memory/004-execution-environment.md)
- [Exception vectors, ESR, FAR, and synchronous fault handling](chapters/02-memory/005-exception-vectors.md)
- [The MMU, translation tables, and MAIR](chapters/02-memory/006-mmu-and-page-tables.md)
- [Physical memory and the device tree](chapters/02-memory/007-physical-memory-and-device-tree.md)

# Part III — Time and Concurrency

- [GIC v3 fundamentals](chapters/03-time-and-concurrency/008-gic-v3.md)
- [The ARM generic timer](chapters/03-time-and-concurrency/009-arm-generic-timer.md)
- [Threads and the AArch64 context switch](chapters/03-time-and-concurrency/010-threads-and-context-switch.md)
- [Preemptive scheduling](chapters/03-time-and-concurrency/011-preemption.md)

# Part IV — Userspace

- [The kernel heap](chapters/04-userspace/012-kernel-heap.md)
- [SVC and the syscall ABI](chapters/04-userspace/013-svc-and-syscalls.md)
- [ELF loading and the first user program](chapters/04-userspace/014-elf-and-first-user-program.md)
- [Files, VFS, and a tiny ramfs](chapters/04-userspace/015-files-and-vfs.md)
- [init, spawn, wait: the simplest process model that works](chapters/04-userspace/016-init-spawn-wait.md)
- [Console keyboard input and a line-mode shell](chapters/04-userspace/017-keyboard-and-shell.md)

# Part V — Devices

- [virtio-mmio: bus, queues, and the modern transport](chapters/05-devices/018-virtio-mmio.md)
- [virtio-blk and persistent storage](chapters/05-devices/019-virtio-blk.md)
- [A read-only on-disk filesystem and cat /mnt/...](chapters/05-devices/020-osfs-and-mount.md)
- [Loading user binaries from disk](chapters/05-devices/021-binaries-on-disk.md)
- [A block cache in front of virtio-blk](chapters/05-devices/022-block-cache.md)
- [Per-process address spaces](chapters/05-devices/023-per-process-address-spaces.md)
- [Hardening the kernel/user boundary](chapters/05-devices/024-kernel-user-boundary.md)
- [A user heap via sbrk](chapters/05-devices/025-user-heap.md)
- [argc, argv, and the user stack](chapters/05-devices/026-argc-argv.md)
- [A printf for the user libc](chapters/05-devices/027-printf.md)
- [Browsing the namespace: SYS_LISTDIR and ls](chapters/05-devices/028-listdir-and-ls.md)
- [uptime and a real shell PATH](chapters/05-devices/029-uptime-and-path.md)
- [A time builtin for the shell](chapters/05-devices/030-time-builtin.md)
- [Per-process cwd: cd, pwd, dynamic prompt](chapters/05-devices/031-cwd-cd-pwd.md)
- [Environment variables and a real PATH walk](chapters/05-devices/032-env-vars-and-path.md)
- [Variable expansion and ./prog](chapters/05-devices/033-var-expansion-and-relative-paths.md)
- [Shell quoting](chapters/05-devices/034-shell-quoting.md)
- [Bigger filesystem and four classic tools](chapters/05-devices/035-bigger-fs-and-four-tools.md)
- [Input redirection](chapters/05-devices/036-input-redirection.md)
- [Sleep and the THREAD_SLEEPING state](chapters/05-devices/037-sleep-and-blocking.md)
- [Kernel pipes, dup2, and THREAD_BLOCKED](chapters/05-devices/038-pipes.md)
- [Shell pipelines: cat | grep | wc](chapters/05-devices/039-shell-pipelines.md)
- [Writable tmpfs and > output redirection](chapters/05-devices/040-writable-tmpfs-and-output-redirection.md)
- [tmpfs polish: >>, ls /tmp/, rm](chapters/05-devices/041-tmpfs-polish.md)
- [Raw TTY mode and the shell line editor](chapters/05-devices/042-raw-tty-and-line-editor.md)
- [Cursor movement and readline keybindings](chapters/05-devices/043-cursor-and-readline-keys.md)
- [Kill ring: Ctrl-K, Ctrl-U, Ctrl-W, Ctrl-Y](chapters/05-devices/044-kill-ring-and-yank.md)
- [virtio-gpu: a framebuffer at native resolution](chapters/05-devices/045-virtio-gpu-framebuffer.md)
- [virtio-input: an evdev keyboard for the GUI](chapters/05-devices/046-virtio-input-keyboard.md)

# Part VI — GUI

- [An in-kernel window manager and seven GUI syscalls](chapters/06-gui/047-window-manager-and-gui-syscalls.md)
- [virtio-tablet, mouse focus, drag, and close](chapters/06-gui/048-virtio-tablet-and-wm-mouse.md)
- [gui_term: a terminal in a window, and the synchronous-pipe spawn pattern](chapters/06-gui/049-gui-terminal-and-pipe-spawn.md)
- [notepad: a real text editor in a window, and the writable-tmpfs round-trip](chapters/06-gui/050-notepad-and-tmpfs-roundtrip.md)
- [launcher: clicking is the new typing](chapters/06-gui/051-launcher-and-click-to-spawn.md)
- [A WM rendering bug, surfaced by the launcher](chapters/06-gui/052-wm-z-order-bug.md)
- [Boot to desktop: auto-spawn the launcher and a gradient wallpaper](chapters/06-gui/053-boot-to-desktop.md)
- [A taskbar, three new GUI syscalls, and a real desktop](chapters/06-gui/054-taskbar-and-window-list.md)
- [A clock in the taskbar](chapters/06-gui/055-clock-in-taskbar.md)
- [Toast notifications and proper child reaping](chapters/06-gui/056-toast-notifications.md)
- [A userspace wallpaper, and the yield/IRQ race it uncovered](chapters/06-gui/057-userspace-wallpaper-and-yield-race.md)
- [Window minimize and restore](chapters/06-gui/058-window-minimize-restore.md)

# Part VII — Networking

- [virtio-net: getting bytes on and off the wire](chapters/07-networking/059-virtio-net.md)
- [Ethernet, ARP, and IPv4](chapters/07-networking/060-ethernet-arp-ipv4.md)
- [ICMP, UDP, and DHCP](chapters/07-networking/061-icmp-udp-dhcp.md)
- [TCP and a kernel-side socket API](chapters/07-networking/062-tcp-and-sockets.md)
- [Socket syscalls and a userspace httpget](chapters/07-networking/063-socket-syscalls-and-httpget.md)
- [DNS resolver](chapters/07-networking/064-dns-resolver.md)
- [URL and HTTP parser](chapters/07-networking/065-url-and-http-parser.md)

# Part VIII — Browser

- [The HTML tokenizer](chapters/08-browser/066-html-tokenizer.md)
- [DOM construction](chapters/08-browser/067-dom-construction.md)
- [A tiny CSS parser](chapters/08-browser/068-css-parser.md)
- [Block and inline layout](chapters/08-browser/069-block-and-inline-layout.md)
- [/bin/browser — paint, plain, ANSI, GUI](chapters/08-browser/070-bin-browser.md)

# Part IX — Finishing the Process Model

- [Why fork (and not just spawn)](chapters/09-process-model/071-why-fork-vs-spawn.md)
- [fork on AArch64: the address-space copy](chapters/09-process-model/072-aarch64-fork-and-as-copy.md)
- [exec: tearing down and rebuilding an AS in place](chapters/09-process-model/073-exec-and-as-rebuild.md)
- [Copy-on-write: making fork cheap](chapters/09-process-model/074-copy-on-write.md)
- [Signals, starting with SIGINT](chapters/09-process-model/075-signals-sigint.md)
- [Catching signals: sigaction, masks, EINTR](chapters/09-process-model/076-sigaction-and-eintr.md)
- [SIGCHLD and waitpid: parent-child plumbing](chapters/09-process-model/077-sigchld-and-waitpid.md)
- [Job control in the shell](chapters/09-process-model/078-job-control.md)
- [gui_term gets real processes, signals, and Ctrl-C](chapters/09-process-model/079-gui-term-real-processes.md)
- [PLAN: retiring spawn in favour of fork+exec everywhere](chapters/09-process-model/080-retiring-spawn.md)

# Part X — Persistence and a Real Filesystem

- [Why we need a writable filesystem](chapters/10-filesystem/081-writable-fs-design.md)
- [Inodes, dirents, and the free-space bitmap](chapters/10-filesystem/082-inodes-and-bitmap.md)
- [Write-back, fsync, and the durability gap](chapters/10-filesystem/083-write-back-and-fsync.md)
- [A tiny journal: crash-consistency on a budget](chapters/10-filesystem/084-tiny-journal.md)
- [Save As: dialogs, libraries, and the first widget toolkit](chapters/10-filesystem/085-persistence-in-practice.md)
- [Subdirectories: a path walker, mkdir, and a navigable Save As](chapters/10-filesystem/086-subdirectories.md)

# Part XI — Multiprocessing and Memory

- [The second core: PSCI and secondary boot](chapters/11-smp-and-memory/087-second-core-psci.md)
- [Atomics and spinlocks on AArch64](chapters/11-smp-and-memory/088-atomics-and-spinlocks.md)
- [IPIs through GICv3 and TLB shootdown](chapters/11-smp-and-memory/089-ipis-via-gicv3.md)
- [An SMP runqueue and basic load balance](chapters/11-smp-and-memory/090-smp-runqueue.md)
- [mmap and a unified page cache](chapters/11-smp-and-memory/091-mmap-and-page-cache.md)
- [Userspace threads (clone-shaped)](chapters/11-smp-and-memory/092-userspace-threads.md)
- [Real SMP scheduling: per-CPU timers, locked sleeper walks, and CLONE_CPU](chapters/11-smp-and-memory/093-real-smp-scheduling.md)
- [Sharing the FD table: CLONE_FILES and refcounted fd_table](chapters/11-smp-and-memory/094-clone-files.md)
- [The browser parser thread: HTML/CSS/layout off the GUI core](chapters/11-smp-and-memory/095-browser-parser-thread.md)

# Part XII — System Services and Polish

- [A real RTC and wall-clock time](chapters/12-system-services/096-rtc-and-wallclock.md)
- [virtio-snd: a boot chime and beep](chapters/12-system-services/097-virtio-snd.md)
- [PNG decoding and the browser image cache](chapters/12-system-services/098-png-and-image-cache.md)
- [Extending PNG: palette, grayscale, and content-type sniffing](chapters/12-system-services/099-png-extended.md)
- [Intrinsic image sizing and the resize race](chapters/12-system-services/100-intrinsic-image-sizing.md)
- [A /proc-shaped filesystem, ps, and top](chapters/12-system-services/101-procfs-ps-top.md)
- [strace: a syscall tracer in 200 lines](chapters/12-system-services/102-strace.md)
- [Guard pages and a friendlier stack overflow](chapters/12-system-services/103-guard-pages.md)
- [TrueType fonts in the kernel](chapters/12-system-services/104-truetype-fonts.md)

# Part XIII — TCP Server and httpd

- [Passive open: LISTEN, SYN_RECEIVED, the backlog](chapters/13-tcp-server/105-passive-open-listen.md)
- [accept() and a server socket API](chapters/13-tcp-server/106-accept-and-server-sockets.md)
- [/bin/httpd: serve /mnt and /data over HTTP](chapters/13-tcp-server/107-bin-httpd.md)
- [TCP loopback (lo0 and 127.0.0.0/8)](chapters/13-tcp-server/108-tcp-loopback.md)
- [httpd as a forwarding proxy (TLS bridge)](chapters/13-tcp-server/109-httpd-tls-bridge.md)
- [The browser uses the in-guest httpd as its proxy](chapters/13-tcp-server/110-browser-uses-in-guest-httpd.md)
- [End to end: the browser fetches from its own kernel](chapters/13-tcp-server/111-end-to-end-loop.md)

# Part XIV — Userspace Services

- [IPC: a tiny message bus for long-running services](chapters/14-userspace-services/112-ipc.md)
- [The system clipboard, as a userspace service](chapters/14-userspace-services/113-clipboard.md)
- [Userspace access to window pixel buffers](chapters/14-userspace-services/114-userspace-window-buffers.md)
- [Moving font rendering into userspace](chapters/14-userspace-services/115-userspace-font-server.md)
- [Moving the GUI SDK into userspace](chapters/14-userspace-services/116-gui-sdk-userspace-drawing.md)
- [The window server moves to userspace](chapters/14-userspace-services/117-userspace-window-server.md)
- [Decoration, the cursor, input routing, and resize](chapters/14-userspace-services/118-userspace-decoration-input-resize.md)

# Part XV — Browser Maturation

- [HTML forms: input, button, submit](chapters/15-browser-maturation/119-html-forms.md)
- [Cookies and the Same-Origin Policy](chapters/15-browser-maturation/120-cookies-and-sop.md)
- [Cross-origin form submission blocking](chapters/15-browser-maturation/121-cross-origin-form-blocking.md)
- [A pocket JavaScript: expression evaluator for onclick](chapters/15-browser-maturation/122-pocket-javascript.md)
- [Entropy: virtio-rng and a kernel CSPRNG](chapters/15-browser-maturation/123-entropy-and-csprng.md)
- [BearSSL builds for our userspace](chapters/15-browser-maturation/124-bearssl-build.md)
- [In-guest TLS handshake](chapters/15-browser-maturation/125-tls-handshake.md)
- [X.509 chain validation](chapters/15-browser-maturation/126-chain-validation.md)
- [Browser https:// (native TLS)](chapters/15-browser-maturation/127-browser-https.md)
- [TLS trust store (multi-anchor)](chapters/15-browser-maturation/128-trust-store.md)
- [PEM ingest and the recursive chain walk](chapters/15-browser-maturation/129-pem-ingest.md)
- [Direct outbound HTTPS: real public CAs](chapters/15-browser-maturation/130-public-trust.md)
- [URL-bar UX after native TLS](chapters/15-browser-maturation/131-url-bar-relatives.md)

# Part XVI — Filesystem Architecture

- [A real VFS — mount table and struct fs_ops](chapters/16-filesystem-architecture/132-mount-table-and-vtable.md)
- [Step 1 — mount table types + vfs_resolve](chapters/16-filesystem-architecture/133-mount-table-types.md)
- [Step 2 — porting procfs onto fs_ops](chapters/16-filesystem-architecture/134-procfs-port.md)
- [Step 3 — porting tmpfs onto fs_ops](chapters/16-filesystem-architecture/135-tmpfs-port.md)
- [Step 4 — porting OSFS-1 and OSFS-2](chapters/16-filesystem-architecture/136-osfs-port.md)
- [Step 5 — embedded ramfs as a root mount](chapters/16-filesystem-architecture/137-ramfs-as-root.md)
- [Step 6 — SYS_MOUNTS and /bin/mount](chapters/16-filesystem-architecture/138-sys-mounts.md)
- [Step 7 — MOUNT_RO + EROFS_VFS hardening](chapters/16-filesystem-architecture/139-mount-ro-hardening.md)
- [PLAN: User-space filesystem servers (9P-shaped)](chapters/16-filesystem-architecture/140-userspace-filesystem-servers.md)
- [Step 1 — kernel userfs module + FD_USERFS_FILE](chapters/16-filesystem-architecture/141-kernel-userfs-module.md)
- [Step 2 — SYS_MOUNT / SYS_UMOUNT](chapters/16-filesystem-architecture/142-sys-mount-umount.md)
- [Step 3 — libfs + /bin/echofs](chapters/16-filesystem-architecture/143-libfs-and-echofs.md)
- [Step 4 — porting clipboardd to userfs](chapters/16-filesystem-architecture/144-clipboardd-port.md)
- [Step 5 — porting procfs to /bin/procd](chapters/16-filesystem-architecture/145-procd-port.md)
- [Step 6 — per-request timeouts and deadlock detection](chapters/16-filesystem-architecture/146-timeouts-and-deadlock.md)

# Part XVII — A C compiler on the OS

- [PLAN: A C compiler that runs on the OS](chapters/17-a-c-compiler-on-the-os/147-c-compiler-strategy.md)
- [A POSIX-ish libc, part 1: stdio, errno, env](chapters/17-a-c-compiler-on-the-os/148-libc-stdio-and-env.md)
  - [Step 1 — errno populated by every syscall wrapper](chapters/17-a-c-compiler-on-the-os/149-errno.md)
  - [Step 2 — FILE *, fopen, fread, fwrite, fseek, fprintf](chapters/17-a-c-compiler-on-the-os/150-stdio.md)
  - [Step 4 — POSIX errno convention + strerror + cat/wc/head/tail on FILE *](chapters/17-a-c-compiler-on-the-os/152-errno-convention.md)
- [A POSIX-ish libc, part 2: stat, fstat, fcntl, dirent](chapters/17-a-c-compiler-on-the-os/153-libc-stat-fcntl-dirent.md)
- [An AArch64 assembler: /bin/as](chapters/17-a-c-compiler-on-the-os/154-bin-as-assembler.md)
- [An AArch64 linker: /bin/ld and /bin/ar](chapters/17-a-c-compiler-on-the-os/155-bin-ld-linker.md)
- [Bootstrap glue: crt0, crti, crtn, libgcc-style stubs](chapters/17-a-c-compiler-on-the-os/156-crt0-and-libgcc-stubs.md)
- [/bin/cc: a one-chapter native C compiler](chapters/17-a-c-compiler-on-the-os/157-bin-cc.md)
- [The host cross-toolchain contract](chapters/17-a-c-compiler-on-the-os/158-cross-toolchain-contract.md)
- [/bin/cc grows variables and arithmetic](chapters/17-a-c-compiler-on-the-os/159-cc-variables-and-arithmetic.md)
- [The first native compile from disk](chapters/17-a-c-compiler-on-the-os/160-first-native-compile.md)
- [The self-hosting gap, and why we don't close it here](chapters/17-a-c-compiler-on-the-os/161-self-hosting-bootstrap.md)
- [A tiny build driver: /bin/make](chapters/17-a-c-compiler-on-the-os/162-make-port.md)
- [Notepad gets a Build button: an in-OS dev loop](chapters/17-a-c-compiler-on-the-os/163-notepad-build-button.md)

# Part XVIII — Real GCC, real software, real Doom

- [PLAN: Real GCC on the OS, and a playable Doom](chapters/18-real-gcc-and-real-software/164-plan-real-gcc-and-doom.md)
  - [165 — setjmp / longjmp](chapters/18-real-gcc-and-real-software/165-setjmp-longjmp.md)
  - [166 — raise / abort / full POSIX signal table](chapters/18-real-gcc-and-real-software/166-signal-and-raise.md)
  - [167 — ctype / assert / str* family](chapters/18-real-gcc-and-real-software/167-ctype-assert-string.md)
  - [168 — POSIX <time.h> (struct tm, gmtime_r, strftime)](chapters/18-real-gcc-and-real-software/168-posix-time.md)
  - [169 — <stdlib.h>: qsort, bsearch, strtol, getopt](chapters/18-real-gcc-and-real-software/169-stdlib-getopt.md)
  - [170 — real printf + scanf (%o, precision, scanf)](chapters/18-real-gcc-and-real-software/170-real-printf-scanf.md)
  - [171 — FP / SIMD at EL0](chapters/18-real-gcc-and-real-software/171-fp-simd-at-el0.md)
  - [172 — Doom (the port)](chapters/18-real-gcc-and-real-software/172-doomgeneric-port.md)
  - [173 — Staging the WAD (so Doom actually plays)](chapters/18-real-gcc-and-real-software/173-doom-wad-staging.md)
  - [174 — Doom plays (closing Phase 1)](chapters/18-real-gcc-and-real-software/174-doom-plays.md)
  - [175 — Binutils with an aarch64-osdev target](chapters/18-real-gcc-and-real-software/175-binutils-target.md)
  - [176 — aarch64-osdev-cc: the target-compiler seam](chapters/18-real-gcc-and-real-software/176-osdev-cc-wrapper.md)
  - [177 — Cross-build seam: link-mode wrapper and the libc-gap catalog](chapters/18-real-gcc-and-real-software/177-cross-build-seam.md)
  - [178 — Closing the libc gap for cross-built libiberty](chapters/18-real-gcc-and-real-software/178-libc-gaps.md)
  - [179 — ld in-guest: cross-building binutils' linker](chapters/18-real-gcc-and-real-software/179-binutils-ld-in-guest.md)
  - [180 — Replacing /bin/as and /bin/ld with the real binutils](chapters/18-real-gcc-and-real-software/180-replace-bin-as-ld.md)
  - [181 — GCC with an aarch64-osdev target](chapters/18-real-gcc-and-real-software/181-gcc-target.md)
  - [182 — GMP, MPFR, MPC as in-tree prerequisites](chapters/18-real-gcc-and-real-software/182-gcc-prereqs.md)
  - [183 — Cross-building aarch64-osdev-gcc](chapters/18-real-gcc-and-real-software/183-cross-build-xgcc.md)
  - [184 — Real cross-compiler specs: retiring the wrapper](chapters/18-real-gcc-and-real-software/184-real-cross-specs.md)
  - [185 — Cross-building GMP/MPFR/MPC for the guest sysroot](chapters/18-real-gcc-and-real-software/185-gcc-prereqs-for-guest.md)
  - [186 — gcc hello.c works on the OS](chapters/18-real-gcc-and-real-software/186-gcc-runs-in-guest.md)
  - [187 — gcc hello.c -o hello (no escape-hatch flags)](chapters/18-real-gcc-and-real-software/187-gcc-default-specs.md)
  - [188 — /bin/gcc builds a medium real program (bf)](chapters/18-real-gcc-and-real-software/188-gcc-builds-real-program.md)
  - [189 — #include <stdio.h> works in the guest](chapters/18-real-gcc-and-real-software/189-libc-headers-on-disk.md)
  - [190 — sys/ headers without a hierarchical filesystem](chapters/18-real-gcc-and-real-software/190-osfs1-subdirs.md)
  - [191 — /bin/tar (ustar reader)](chapters/18-real-gcc-and-real-software/191-tar.md)
  - [192 — expanding /bin/make for a real multi-file build](chapters/18-real-gcc-and-real-software/192-make-expansion.md)
  - [193 — in-guest Doom rebuild, pilot (three real vendor files)](chapters/18-real-gcc-and-real-software/193-doom-pilot.md)
  - [194 — in-guest Doom rebuild, full vendor compile (77 files)](chapters/18-real-gcc-and-real-software/194-full-doom-compile.md)
  - [195 — in-guest Doom link (binutils @file response files)](chapters/18-real-gcc-and-real-software/195-link-doomgeneric-in-guest.md)
  - [196 — the rebuilt Doom plays](chapters/18-real-gcc-and-real-software/196-rebuilt-doom-plays.md)
  - [197 — Real key-up events (retiring the doom timed-release shim)](chapters/18-real-gcc-and-real-software/197-real-key-up-events.md)
