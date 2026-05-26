#!/usr/bin/env python3
"""scripts/_dbg_ld_argv.py -- Diagnose /bin/ld argv corruption.

Chapter 131f bring-up: scripts/test_bin_ld_ar.py passes 8/12.
/bin/ld receives a corrupted `-o` argument and reports
'cannot open output file <garbage>: Read-only file system'.

This script boots the OS and runs a series of /bin/ld
invocations with increasing argc so we can see exactly when
the corruption starts and what the bytes look like.

Per debug-scripts-policy this stays in scripts/ permanently.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-dbg-ld-argv.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

SOURCE = r""".text
.global _start
_start:
    mov x0, #42
    mov x8, #2
    svc #0
"""


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
    return subprocess.Popen(
        [
            "qemu-system-aarch64",
            "-M", "virt,gic-version=3",
            "-cpu", "host",
            "-accel", "hvf",
            "-m", "8G",
            "-smp", "2",
            "-display", "none",
            "-serial", f"unix:{SERIAL_SOCK},server,nowait",
            "-global", "virtio-mmio.force-legacy=off",
            "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
            "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
            "-device", "virtio-blk-device,drive=hd0",
            "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
            "-device", "virtio-blk-device,drive=hd1",
            "-kernel", f"{ROOT}/build/kernel.elf",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


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
            c = s.recv(8192)
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


def send_cmd(s, cmd, timeout=15.0):
    if isinstance(cmd, str):
        cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


def main():
    print("[_dbg_ld_argv] booting OS")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        send_cmd(s, "rm /tmp/hello.s 2>/dev/null; true")
        for line in SOURCE.strip("\n").split("\n"):
            send_cmd(s, f"echo '{line}' >> /tmp/hello.s")

        out = send_cmd(
            s, "/bin/as /tmp/hello.s -o /tmp/hello.o", timeout=30.0
        )
        print("===AS===")
        print(repr(out))

        out = send_cmd(s, "ls -l /tmp/hello.o", timeout=10.0)
        print("===LS hello.o===")
        print(repr(out))

        cases = [
            ("A", "/bin/ld --version"),
            ("B", "/bin/ld -o /tmp/x1 /tmp/hello.o"),
            ("C", "/bin/ld -e _start -o /tmp/x2 /tmp/hello.o"),
            ("D", "/bin/ld -T /bin/osdev.ld -o /tmp/x3 /tmp/hello.o"),
            ("E", "/bin/ld -T /bin/osdev.ld -e _start -o /tmp/x4 /tmp/hello.o"),
        ]
        for tag, cmd in cases:
            print(f"===CASE {tag}: {cmd}===")
            out = send_cmd(s, cmd, timeout=40.0)
            print(repr(out))
            ls = send_cmd(s, "ls -l /tmp/ 2>&1 | head -20", timeout=10.0)
            print(f"===LS after {tag}===")
            print(repr(ls))
    finally:
        try:
            s.close()
        except Exception:
            pass
        hard_kill(q)


if __name__ == "__main__":
    main()
