#!/usr/bin/env python3
"""Capture stdout of xgcc -v with -o, write to /tmp/x.out via shell
redirect, then `cat /tmp/x.out` and dump as hex on the host side.

Goal: pin down whether "COLLECT_GCC=???" is literally 4 garbage bytes,
or 9+ bytes of UTF-8 replacement chars, or something else entirely.
This will narrow down which subsystem (obstack/xmalloc/concat/argv copy)
is producing the wrong bytes.
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
SERIAL_SOCK = os.path.join(tempfile.gettempdir(), "osdev_hex_probe.sock")
DATA_IMG = os.path.join(tempfile.gettempdir(), "osdev_hex_probe_data.img")
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


def send_cmd(s, cmd, timeout=60.0):
    if isinstance(cmd, str):
        cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


def hexdump(label, raw):
    print(f"\n--- {label} ({len(raw)} bytes) ---")
    for i in range(0, len(raw), 16):
        chunk = raw[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        text = "".join(chr(b) if 0x20 <= b < 0x7f else "." for b in chunk)
        print(f"  {i:04x}  {hexs:<47}  {text}")


def main():
    reformat_data()
    q = boot()
    s = None
    try:
        s = conn()
        wait_for(s, PROMPT, timeout=30.0)

        # Stage source
        send_cmd(s, "rm /tmp/hello.c", timeout=10.0)
        for line in [
            "void _start(void) {",
            "register long n asm(\"x8\") = 2;",
            "register long c asm(\"x0\") = 42;",
            "asm volatile(\"svc #0\" :: \"r\"(n), \"r\"(c));",
            "}",
        ]:
            send_cmd(s, f"echo '{line}' >> /tmp/hello.c", timeout=10.0)

        # Capture xgcc -v -S -o output to /tmp/x.out
        send_cmd(s, "rm /tmp/x.out", timeout=10.0)
        send_cmd(s,
                 "/bin/xgcc -v -S -o /tmp/hello.s /tmp/hello.c "
                 "> /tmp/x.out",
                 timeout=60.0)
        out = send_cmd(s, "cat /tmp/x.out", timeout=20.0)

        # Find the COLLECT_GCC= line and the COLLECT_GCC_OPTIONS= line
        text = out
        for marker in (b"COLLECT_GCC=", b"COLLECT_GCC_OPTIONS="):
            i = text.find(marker)
            if i < 0:
                print(f"NOT FOUND: {marker!r}")
                continue
            # Take up to next newline
            j = text.find(b"\n", i)
            if j < 0:
                j = len(text)
            line = text[i:j]
            hexdump(marker.decode().rstrip("="), line)

        # Also dump the raw stderr (which has the warning)
        out2 = send_cmd(s,
                        "/bin/xgcc -v -S -o /tmp/hello.s /tmp/hello.c "
                        "> /tmp/x.out2",
                        timeout=60.0)
        # That stderr is on serial (since no 2> redirect).  Pull it
        # from the response buffer directly.
        hexdump("stderr-of-second-xgcc-run", out2)

        # And cat the second file content too for completeness
        out3 = send_cmd(s, "cat /tmp/x.out2", timeout=20.0)
        for marker in (b"COLLECT_GCC=", b"COLLECT_GCC_OPTIONS="):
            i = out3.find(marker)
            if i < 0:
                continue
            j = out3.find(b"\n", i)
            if j < 0:
                j = len(out3)
            hexdump(marker.decode().rstrip("=") + "-rerun", out3[i:j])
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
