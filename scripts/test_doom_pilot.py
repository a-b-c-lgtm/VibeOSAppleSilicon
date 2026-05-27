#!/usr/bin/env python3
"""scripts/test_doom_pilot.py -- chapter 193 first doom .o
in-guest.

Phase 6 of guest-gcc bring-up.  The earlier chapters proved
each piece individually:

    132h    /bin/gcc compiles a multi-file C program (bf)
    132i+j  libc + sys/ headers on /bin
    133a    /bin/tar extracts a 1.9 MB source tarball
    133b    /bin/make handles variables / patterns / autos

This chapter wires them together.  In-guest sequence:

    /bin/tar xf /bin/doomgeneric.tar -C /data
    cd /data/src
    /bin/make -f /bin/doom_pilot.mk

…producing three vendor DoomGeneric object files
(m_random.o, m_bbox.o, m_fixed.o) compiled by the in-guest
toolchain from the in-guest-extracted source.

We pick three small files with low coupling rather than
attempting the full 82-file set in one shot:

    m_random.c   — 65 LoC, zero #includes.  Smoke test:
                   does /bin/gcc even handle a vendor file?
    m_bbox.c     — uses <limits.h> (GCC freestanding) and
                   <stdbool.h>.  Smoke test: cpp finds
                   bracket-include headers.
    m_fixed.c    — uses "stdlib.h" (our libc), and via
                   doomtype.h pulls <inttypes.h> +
                   <strings.h>.  Smoke test: full libc
                   include chain works for a vendor file.

Test ladder (4 steps, 8 expectations):

    1. /bin/tar is shipped + the pilot Makefile is shipped
    2. tar xf populates /data/src/m_*.c
    3. /bin/make -f /bin/doom_pilot.mk produces all three .o
       files inside /data/src/
    4. Each .o is an ELF AArch64 object (cat the first
       4 bytes, look for 0x7F'E''L''F')

If any step fails: that's the libc gap to fix.  The error
message printed by /bin/gcc inside the guest tells us which
header / symbol is missing.

Next chapter (133d) scales OBJS in the Makefile to all 82
vendor files and adds whatever libc fillers come up along
the way.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-doom-pilot.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "


def cleanup_sock():
    try:
        os.unlink(SERIAL_SOCK)
    except FileNotFoundError:
        pass


def reformat_data():
    subprocess.check_call(
        ["python3", f"{ROOT}/scripts/mkosfs2.py", DATA_IMG],
        stdout=subprocess.DEVNULL,
    )


def boot():
    cleanup_sock()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def hard_kill(q):
    try:
        q.send_signal(signal.SIGKILL)
        q.wait(timeout=3)
    except Exception:
        pass
    cleanup_sock()


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SERIAL_SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(65536)
            if not c:
                break
            out += c
        elif out:
            break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def send_cmd(s, cmd, timeout=60.0):
    if isinstance(cmd, str):
        cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


PASSES, FAILS = [], []


def expect(cond, msg):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        FAILS.append(msg)


def main():
    print("[chapter 193] first doom .o files compiled in-guest")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # --- step 1: prereqs shipped ------------------------
        out = send_cmd(s, "cat /bin/tar", timeout=15.0)
        expect(b"\x7FELF" in out,
               "step 1a: /bin/tar shipped as ELF")

        out = send_cmd(s, "cat /bin/doom_pilot.mk", timeout=10.0)
        expect(b"m_random.o" in out and b"%.o:" in out
               and b"/data/src" in out,
               "step 1b: /bin/doom_pilot.mk shipped on OSFS-1")

        # --- step 2: extract source tarball -----------------
        out = send_cmd(s, "/bin/tar xf /bin/doomgeneric.tar -C /data",
                       timeout=120.0)
        sys.stdout.write("--- tar xf output ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n---------------------\n")
        # No "cannot create" errors expected:
        expect(b"cannot create" not in out and b"errno=" not in out,
               "step 2a: /bin/tar extracted without errors")

        # All three sources should be on /data/src/.  /bin/ls
        # only honors argv[1] so we have to stat each one
        # individually.
        out = b""
        for src in (b"m_random.c", b"m_bbox.c", b"m_fixed.c"):
            out += send_cmd(s, "/bin/ls /data/src/" + src.decode(),
                            timeout=10.0)
        expect(b"m_random.c" in out and b"m_bbox.c" in out
               and b"m_fixed.c" in out,
               "step 2b: pilot sources extracted to /data/src/")

        # --- step 3: run the pilot Makefile -----------------
        # All paths in the Makefile are absolute -- /bin/make
        # doesn't propagate cwd (chapter-133c finding) so we
        # can't rely on `cd /data/src && make` working as it
        # would on POSIX.  See note in /bin/doom_pilot.mk.
        out = send_cmd(s, "/bin/make -f /bin/doom_pilot.mk",
                       timeout=300.0)
        sys.stdout.write("--- /bin/make output ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n------------------------\n")
        expect(b"make: built 'all'" in out,
               "step 3a: /bin/make ran the pilot Makefile to "
               "completion")
        # And there should be no compile errors (look for
        # gcc's typical patterns):
        expect(b"error:" not in out and b"undefined reference"
               not in out and b"fatal error" not in out,
               "step 3b: /bin/gcc reported no compile errors")

        # --- step 4: produced .o files are ELF objects ------
        all_elf = True
        for of in (b"m_random.o", b"m_bbox.o", b"m_fixed.o"):
            r = send_cmd(s,
                         "cat /data/src/" + of.decode(),
                         timeout=10.0)
            ok = b"\x7FELF" in r
            print(f"  {of.decode()}: "
                  f"{'ELF' if ok else 'NOT-ELF'}")
            all_elf = all_elf and ok
        expect(all_elf,
               "step 4: all three .o files exist on "
               "/data/src/ as ELF objects (m_random.o + "
               "m_bbox.o + m_fixed.o)")

        # --- bonus check: the smallest one, m_random.o ------
        # should contain ELF e_type=ET_REL (1) and machine
        # AArch64 (0xB7).  Walking the header by string match
        # to keep the test simple.
        out = send_cmd(s, "cat /data/src/m_random.o",
                       timeout=10.0)
        # Bytes 18-19 of an ELF64 header are e_machine little-
        # endian.  After 0x7FELF (4) + 1+1+1+1+8+8+2 = bytes
        # 18-19 within the first 64.  Just look for 0xB7 0x00
        # near the start (machine=EM_AARCH64).
        head = out[:200] if out else b""
        expect(b"\xB7\x00" in head[:30],
               "bonus: m_random.o's ELF header says AArch64 "
               "(EM_AARCH64=0xB7)")

    finally:
        hard_kill(q)

    return _report()


def _report():
    print()
    print(f"PASS: {len(PASSES)}")
    print(f"FAIL: {len(FAILS)}")
    if FAILS:
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)


if __name__ == "__main__":
    main()
