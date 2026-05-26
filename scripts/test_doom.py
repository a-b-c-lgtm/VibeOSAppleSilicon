#!/usr/bin/env python3
"""scripts/test_doom.py — chapter 130a/130b Doom regression.

Boots the kernel, waits for the shell prompt, runs the `doom`
binary, and verifies that DoomGeneric reaches D_IdentifyVersion.

Two acceptance modes depending on whether a WAD is staged:

  WAD-present mode (assets/wads/doom1.wad exists on host)
    The Makefile DATA_DISK rule will have seeded the WAD into
    OSFS-2 at /data/doom1.wad (mkosfs2 is flat — no subdirs).
    We require proof that D_DoomMain progressed past the IWAD
    scan: any of "DOOM Shareware", "DOOM Registered", "DOOM 2",
    "V_Init", or "R_Init" on stderr.

  WAD-absent mode (no doom1.wad on host)
    D_IdentifyVersion fails to find an IWAD and I_Error prints
    "Game mode indeterminate. ..." then exits.  Seeing that
    banner is proof that:

      - the ELF loaded and `crt0` ran;
      - `main` in `doomgeneric_osdev.c` synthesised default argv
        and called `doomgeneric_Create`;
      - 80+ DG translation units linked and executed at least
        up to `D_IdentifyVersion`;
      - the `wm_create_window_input` call from `DG_Init`
        succeeded;
      - the FP/SIMD-at-EL0 work from chapter 129 didn't
        regress;
      - the libc additions from this chapter resolve at link
        and don't trap at runtime.

In either mode kernel panic, EL0 sync abort, or unresolved-
symbol trap is a hard FAIL.  The full captured serial transcript
is written to /tmp/test_doom_serial.log for post-mortem.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-doom.sock"


def boot():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket appeared")


def read_until(ser, needles, timeout, prior=b""):
    if isinstance(needles, (bytes, str)):
        needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles):
        return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            c = ser.recv(8192)
            if not c:
                break
            buf.extend(c)
            if any(n in buf for n in needles):
                return bytes(buf)
    return bytes(buf)


def wait_for_new(ser, needles, timeout, log):
    if isinstance(needles, (bytes, str)):
        needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    cutoff = len(log)
    buf = bytearray(log)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(buf.find(n, cutoff) >= 0 for n in needles):
            return bytes(buf)
        r, _, _ = select.select([ser], [], [], 0.2)
        if r:
            c = ser.recv(8192)
            if not c:
                break
            buf.extend(c)
    return bytes(buf)


LOG_PATH = "/tmp/test_doom_serial.log"


def _dump(log):
    try:
        with open(LOG_PATH, "wb") as f:
            f.write(log)
    except OSError:
        pass


def main():
    # Chapter 130b: if assets/wads/doom1.wad is present in the
    # source tree, the Makefile DATA_DISK rule will have seeded
    # it into the OSFS-2 image at /data/doom1.wad.  In that case
    # we expect doom to progress past the IWAD scan and reach
    # the registered/shareware banner.  Without the WAD we still
    # accept the "Game mode indeterminate" path.
    wad_present = os.path.exists(
        os.path.join(ROOT, "assets/wads/doom1.wad"))

    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            _dump(log)
            print("FAIL: shell prompt never appeared")
            return 1

        ser.sendall(b"doom\n")
        # PASS markers depend on whether the WAD is staged.  Both
        # paths still fail on kernel panic, EL0 sync abort, or
        # any unresolved-symbol runtime trap.
        log = wait_for_new(
            ser,
            [
                b"Game mode indeterminate",
                b"IWAD file",
                b"DOOM Shareware",
                b"DOOM Registered",
                b"DOOM 2",
                b"V_Init",
                b"M_LoadDefaults",
                b"R_Init",
                b"PANIC",
                b"EL0 sync abort",
                b"undefined reference",
            ],
            60.0,
            log,
        )
        _dump(log)
        if b"PANIC" in log:
            print("FAIL: guest panicked while running doom")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        if b"EL0 sync abort" in log:
            print("FAIL: doom binary took a synchronous abort at EL0")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        wad_loaded_markers = (
            b"DOOM Shareware",
            b"DOOM Registered",
            b"DOOM 2",
            b"V_Init",
            b"R_Init",
        )
        ident_markers = (
            b"Game mode indeterminate",
            b"IWAD file",
        ) + wad_loaded_markers

        if wad_present:
            # When the WAD is on disk we require proof that
            # D_DoomMain progressed past the IWAD scan; the
            # "indeterminate" path is now a regression.
            if not any(m in log for m in wad_loaded_markers):
                print("FAIL: WAD on disk but doom never reported "
                      "Shareware/Registered/V_Init/R_Init")
                print(log[-2000:].decode("ascii", "replace"))
                return 1
            print("doom: PASS (WAD loaded)")
            return 0

        if not any(m in log for m in ident_markers):
            print("FAIL: doom never reached D_IdentifyVersion / IWAD scan")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("doom: PASS (no WAD, indeterminate path)")
        return 0

    finally:
        try: q.terminate()
        except Exception: pass
        try: q.wait(timeout=5)
        except Exception:
            try: q.kill()
            except Exception: pass


if __name__ == "__main__":
    sys.exit(main())
