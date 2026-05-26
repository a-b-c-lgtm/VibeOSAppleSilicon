#!/usr/bin/env python3
"""Boot the OS and use /bin/echo to verify argv layout for several
argument shapes — including the exact one xgcc receives.

The chapter-132f xgcc bring-up showed argv[0] (printed as the
COLLECT_GCC env value) and the path that follows -o coming back
mangled.  This probe isolates whether the mangling is on the
kernel argv-copy side (in which case /bin/echo will see it too)
or specific to xgcc's argv processing.

Uses the same unix-socket serial harness as test_gcc_hello.py
so the boot reliably reaches a shell prompt.
"""
import os
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = os.path.join(tempfile.gettempdir(), "osdev_argv_probe.sock")
DATA_IMG = os.path.join(tempfile.gettempdir(), "osdev_argv_probe_data.img")
PROMPT = b"/$ "


def cleanup_sock():
    try:
        os.unlink(SERIAL_SOCK)
    except FileNotFoundError:
        pass


def reformat_data():
    if not os.path.exists(DATA_IMG):
        with open(DATA_IMG, "wb") as f:
            f.truncate(64 * 1024 * 1024)
    subprocess.run(
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


def send_cmd(s, cmd, timeout=30.0):
    if isinstance(cmd, str):
        cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


def main():
    reformat_data()
    q = boot()
    s = None
    try:
        s = conn()
        wait_for(s, PROMPT, timeout=30.0)

        cases = [
            "/bin/echo hello world",
            "/bin/echo -v -S -o /tmp/hello.s /tmp/hello.c",
            "/bin/echo a b c d e",
            "/bin/echo /tmp/hello.s",
            "/bin/echo -o /tmp/hello.s",
            "/bin/echo /tmp/hello.s -o",
            "/bin/echo -v -c -o /tmp/hello.o /tmp/hello.c",
        ]
        for cmd in cases:
            print(f"\n========= {cmd}")
            out = send_cmd(s, cmd, timeout=30.0)
            sys.stdout.write(out.decode("utf-8", "replace"))
            print("=========")
    finally:
        if s is not None:
            try:
                s.close()
            except Exception:
                pass
        try:
            q.send_signal(signal.SIGKILL)
            q.wait(timeout=3)
        except Exception:
            pass
        cleanup_sock()


if __name__ == "__main__":
    main()
