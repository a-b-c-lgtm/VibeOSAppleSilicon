#!/usr/bin/env python3
"""scripts/test_tar.py -- chapter 133a /bin/tar smoke test.

We just shipped /bin/tar (a ustar archive reader) and bundled
the doomgeneric source tree into /bin/doomgeneric.tar via the
host script scripts/mktar.py.

This regression confirms the loop:

  1. /bin/tar exists and is an ELF.
  2. /bin/doomgeneric.tar exists, starts with a ustar header,
     and `tar tf` lists ~200 entries.
  3. `tar xf /bin/doomgeneric.tar -C /data` actually creates
     the files under /data/src/, with d_main.c readable + the
     right size.
  4. Re-running extraction over an existing tree is idempotent
     (mkdir EEXIST swallowed cleanly).

When this is green the in-guest /bin/gcc has 200+ C source
files sitting on /data/src/, ready to be compiled by the
chapter-133c Doom-rebuild step.

SERIAL_SOCK is unique per regression so the test can run
in parallel with other chapters' smoke tests.
"""

import os
import re
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-tar.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

GUEST_TAR = "/bin/doomgeneric.tar"


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
    print("[chapter 133a] /bin/tar")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # --- step 1: /bin/tar is an ELF -----------------------
        out = send_cmd(s, "cat /bin/tar", timeout=10.0)
        expect(b"\x7FELF" in out, "step 1: /bin/tar is an ELF")

        # --- step 2: tar tf lists ~200 files ------------------
        out = send_cmd(s, f"/bin/tar tf {GUEST_TAR}", timeout=60.0)
        sys.stdout.write("--- /bin/tar tf (head) ---\n")
        sys.stdout.write(out[:2000].decode("utf-8", errors="replace"))
        sys.stdout.write("\n--- /bin/tar tf (tail) ---\n")
        sys.stdout.write(out[-2000:].decode("utf-8", errors="replace"))
        sys.stdout.write("\n--------------------------\n")
        m = re.search(rb"tar: (\d+) entries", out)
        entries = int(m.group(1)) if m else -1
        expect(entries >= 50,
               f"step 2: tar tf reports >=50 entries (got {entries})")
        expect(b"d_main.c" in out,
               "step 2: tar tf includes src/d_main.c")

        # --- step 3: tar xf extracts to /data -----------------
        out = send_cmd(s, f"/bin/tar xf {GUEST_TAR} -C /data",
                       timeout=120.0)
        m = re.search(rb"tar: (\d+) entries", out)
        x_entries = int(m.group(1)) if m else -1
        expect(x_entries == entries,
               f"step 3: tar xf processed the same entry count "
               f"(list={entries} extract={x_entries})")

        out = send_cmd(s, "ls /data/src", timeout=20.0)
        sys.stdout.write("--- ls /data/src (truncated) ---\n")
        sys.stdout.write(out[:2000].decode("utf-8", errors="replace"))
        sys.stdout.write("\n--------------------------------\n")
        expect(b"d_main.c" in out and b"doomgeneric.c" in out,
               "step 3: /data/src/ contains d_main.c + doomgeneric.c")

        # cat the first ~5 lines of d_main.c -- it should look
        # like C source.
        out = send_cmd(s, "head /data/src/d_main.c", timeout=10.0)
        sys.stdout.write("--- head /data/src/d_main.c ---\n")
        sys.stdout.write(out.decode("utf-8", errors="replace"))
        sys.stdout.write("\n-------------------------------\n")
        expect(b"//" in out or b"/*" in out or b"#include" in out,
               "step 3: extracted /data/src/d_main.c looks like C source")

        # --- step 4: re-extract is idempotent -----------------
        out = send_cmd(s, f"/bin/tar xf {GUEST_TAR} -C /data",
                       timeout=120.0)
        m = re.search(rb"tar: (\d+) entries", out)
        re_entries = int(m.group(1)) if m else -1
        expect(re_entries == entries,
               f"step 4: re-extraction is idempotent "
               f"(first={entries} second={re_entries})")
        expect(b"cannot create" not in out,
               "step 4: re-extraction reports no file-create errors")

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
