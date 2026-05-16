# Chapter 2 — Toolchain and host setup

> **Milestone in this chapter:** prerequisites for milestone 0.
> **Code referenced:** [Makefile](../../../Makefile) (the
> `toolchain-check` target).

## What we are about to install

To build and run the kernel on a MacBook Pro M2 (or any other Apple
Silicon Mac), you need three pieces of software:

1. **A cross-compiler toolchain that targets bare-metal aarch64.**
   We use `aarch64-elf-gcc` and its sibling tools from binutils.
   The host's clang cannot do this — it produces Mach-O object
   files for macOS, not bare-metal ELF.
2. **QEMU with HVF support.** Homebrew's `qemu` formula builds with
   HVF enabled by default on Apple Silicon, so the standard
   `brew install qemu` is enough.
3. **GDB that understands aarch64 ELF.** The version that ships with
   Apple's developer tools (`lldb`) cannot debug bare-metal aarch64
   over the QEMU GDB stub. We install GNU `gdb` from Homebrew.

That's it. No Docker, no Vagrant, no pre-built virtual machine
image, no sysroot, no GRUB.

## Install

```bash
brew install aarch64-elf-gcc aarch64-elf-binutils qemu
```

That single line installs everything. The download is around
600 MiB and takes three to five minutes depending on your network.

Verify:

```bash
$ aarch64-elf-gcc --version | head -1
aarch64-elf-gcc (GCC) 14.2.0

$ aarch64-elf-ld --version | head -1
GNU ld (GNU Binutils) 2.44

$ qemu-system-aarch64 --version | head -1
QEMU emulator version 11.0.0

$ qemu-system-aarch64 -machine virt -accel help 2>&1 | head -10
Accelerators supported in QEMU binary:
hvf
tcg
```

If `qemu-system-aarch64 -accel help` does not list `hvf`, your QEMU
was built without HVF support. Reinstall:

```bash
brew reinstall qemu
```

If `aarch64-elf-gcc --version` reports an error about libisl or
libmpfr, run:

```bash
brew reinstall isl mpfr gmp mpc
```

Those are the GMP/MPFR/MPC stack that GCC depends on at runtime.

## What `aarch64-elf-` actually means

The cross-compiler is named `aarch64-elf-gcc`, not `aarch64-gcc`.
The `-elf-` part is the *target triple suffix* and matters: it
tells GCC that:

- The target processor is `aarch64`.
- The target operating system is *no operating system* (the empty
  middle slot, often shown as `none`).
- The target object-file format is ELF.

This is the right choice for a kernel: the compiler will not
assume any libc, will not emit calls to `__libc_start_main`, and
will produce ELF files that can be linked with our own linker
script and loaded by QEMU directly.

A different toolchain you may see online is `aarch64-linux-gnu-gcc`.
That one targets *Linux on aarch64* — it assumes a libc, emits
GOT/PLT relocations expecting a dynamic linker, and produces
binaries that QEMU's `-kernel` loader will not accept. Do not use
it.

## What QEMU is going to do

When we run `make run` later, the command we will be executing is:

```bash
qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu host \
    -accel hvf \
    -m 2G \
    -nographic \
    -kernel build/kernel.elf
```

Each flag matters:

- `-M virt` selects QEMU's "virt" machine model. This is a
  synthetic, paravirtualized aarch64 platform — no real hardware
  has this exact memory map. Its great virtue is that *every device
  on the bus is virtio-mmio*, which is the cleanest device-driver
  protocol there is. Most of the rest of this book is virtio.
- `gic-version=3` tells the virt machine to expose a GICv3
  interrupt controller, matching what M-series CPUs implement and
  what HVF will pass through.
- `-cpu host` tells QEMU to expose the host's actual CPU features
  to the guest. This is required for HVF; with TCG you would write
  `-cpu cortex-a72` instead.
- `-accel hvf` selects the Apple hypervisor as the execution
  backend. Without this flag QEMU uses TCG (software translation),
  which works but is much slower.
- `-m 2G` gives the guest 2 GiB of RAM. The virt machine maps RAM
  starting at physical address `0x40000000`.
- `-nographic` routes the guest's UART to your terminal and wires
  `Ctrl-A X` as the QEMU exit shortcut. We do not need a graphical
  window until milestone 6 when virtio-gpu lights up.
- `-kernel build/kernel.elf` tells QEMU to load our ELF directly,
  parse its program headers, place each PT_LOAD segment at the
  declared physical address, and jump to the ELF entry point with
  `x0 = DTB physical address`.

There is no bootloader in this picture. QEMU's `-kernel` loader
*is* our bootloader, and the book's first few chapters lean into
that simplicity. Real-hardware booting (where U-Boot or Limine
takes the bootloader role) is covered in Appendix A.

## Verifying the toolchain end-to-end

We have a tiny verification target in the Makefile:

```bash
$ make toolchain-check
aarch64-elf-gcc (GCC) 14.2.0
GNU ld (GNU Binutils) 2.44
QEMU emulator version 11.0.0
toolchain ok — building for aarch64 virt under HVF
```

If you see that, the rest of the book will work. If you do not,
the most common causes are:

| Error                                                | Cause                                                 |
|------------------------------------------------------|-------------------------------------------------------|
| `command not found: aarch64-elf-gcc`                 | `brew install aarch64-elf-gcc` failed silently        |
| `qemu-system-aarch64: invalid accelerator hvf`        | QEMU built without HVF — `brew reinstall qemu`        |
| `qemu-system-aarch64: HVF: ...`                      | macOS denied virtualization entitlement; reboot      |
| Slow compile, no errors                              | Rosetta — make sure your terminal is native arm64    |

The Rosetta one bites people. Run `arch` in your shell; it must
report `arm64`. If it reports `i386`, you have an x86-64 terminal
running under Rosetta and `brew install` will install the x86-64
versions of the tools, which then run under Rosetta themselves and
are roughly 5× slower than they need to be.

## What we have not installed

You may notice we did not install:

- A device-tree compiler (`dtc`). We do not need one until much
  later, when we want to inspect QEMU's DTB by name; for now the
  DTB is a black box that the kernel can either read or ignore.
- An emulator UI (`UTM`, `Parallels`, etc.). QEMU is the only
  emulator we use, and it does not need a wrapper.
- `clang-format` or any linter. Style is enforced lightly through
  the Makefile's `-Wall -Wextra -Werror`, and that is enough at
  this scale.

## Next chapter

In chapter 3 we write the first kernel. It is two source files
totalling about 130 lines, plus a 40-line linker script. By the
end of the chapter you will see this on your terminal:

```
============================================================
osdev aarch64 — milestone 0 (boot + PL011)
============================================================
kernel_main reached — boot path is alive
MMU off; deferring DTB hex-dump to milestone 1
entering wfe halt loop (Ctrl-A X to quit QEMU)
```

That is your first operating system.
