#!/usr/bin/env python3
"""scripts/_dbg_gcc_libc_probe.py — chapter 187 probe.

Step 8 of test_gcc_hello.py fails with

    ld: cannot find -losdevc: file format not recognized

Two suspects:
  (a) ld is finding /bin/libosdevc.a but the bytes are mangled
      somehow (mkosfs sector boundary, OSFS-1 padding, ...).
  (b) ld's search path isn't picking up /bin/, so it's
      hitting some OTHER libosdevc.a (or some other file
      called libosdevc on the OS) by mistake.

This probe:
  1. Boots the OS once.
  2. Dumps the first 64 bytes of /bin/libosdevc.a via
     `cat /bin/libosdevc.a | head -c 64` so we can see the
     `!<arch>\\n` header.
  3. Runs `/bin/gcc -v /tmp/hello2.c -o /tmp/hello2` to dump
     the spec expansion + the literal ld command line.
  4. Tries `/bin/ld -L/bin -losdevc -o /tmp/foo 2>&1` to
     isolate the linker behaviour without the gcc driver in
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
SOCK = "/tmp/osdev-gcc-probe.sock"
DATA = f"{ROOT}/build/data.img"
PROMPT = b"$ "


def _cleanup():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass


def boot():
    _cleanup()
    subprocess.check_call(
        ["python3", f"{ROOT}/scripts/mkosfs2.py", DATA],
        stdout=subprocess.DEVNULL,
    )
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={DATA},format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


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
    raise RuntimeError("no serial sock")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.2)
        if r:
            c = s.recv(65536)
            if not c:
                break
            out += c
        elif out:
            break
    return out


def wait_for(s, needle, timeout=30.0):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def cmd(s, c, timeout=120.0):
    if isinstance(c, str):
        c = c.encode()
    s.sendall(c + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(c)
    if idx >= 0:
        out = out[idx + len(c):]
    return out


def main():
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        print("=" * 60)
        print("Probe 1: file presence + size")
        print("=" * 60)
        print(cmd(s, "ls /bin", timeout=30.0).decode("utf-8",
                                                       errors="replace"))

        print("=" * 60)
        print("Probe 2: first 64 bytes of /bin/libosdevc.a")
        print("=" * 60)
        out = cmd(s, "cat /bin/libosdevc.a > /tmp/lib.cp",
                  timeout=30.0)
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        out = cmd(s, "cat /tmp/lib.cp", timeout=30.0)
        # show the raw bytes hex-encoded by us locally
        print(repr(out[:128]))

        print("=" * 60)
        print("Probe 3: cc1 banner (does gcc itself run?)")
        print("=" * 60)
        out = cmd(s, "/bin/gcc --version", timeout=60.0)
        sys.stdout.write(out.decode("utf-8", errors="replace"))

        print("=" * 60)
        print("Probe 4: write hello2.c, then gcc -v ...")
        print("=" * 60)
        cmd(s, "rm /tmp/hello2.c", timeout=10.0)
        cmd(s, "echo 'int main(void) { return 7; }' > /tmp/hello2.c",
            timeout=10.0)
        cmd(s, "cat /tmp/hello2.c", timeout=10.0)
        out = cmd(s,
                  "/bin/gcc -v /tmp/hello2.c -o /tmp/hello2",
                  timeout=180.0)
        sys.stdout.write(out.decode("utf-8", errors="replace"))

        print("=" * 60)
        print("Probe 5: try ld directly")
        print("=" * 60)
        out = cmd(s,
                  "/bin/ld -L/bin -losdevc -o /tmp/foo",
                  timeout=60.0)
        sys.stdout.write(out.decode("utf-8", errors="replace"))

    finally:
        try:
            q.send_signal(signal.SIGKILL)
            q.wait(timeout=3)
        except Exception:
            pass
        _cleanup()


if __name__ == "__main__":
    main()
