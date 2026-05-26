#!/usr/bin/env python3
"""
scripts/test_atexit.py — chapter 120 smoke test.

Boots the OS, runs /bin/atexittest, and asserts the
expected output ordering:

  ctor1     ← __init_array walk (constructors)
  ctor2     ← __init_array walk
  main      ← main()
  exit2     ← atexit LIFO (last-registered first)
  exit1     ← atexit LIFO
  dtor      ← .fini_array (destructors run after atexit chain)

Also asserts exit code 7.
"""
import os
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_IMG = f"{ROOT}/build/data.img"
SERIAL_SOCK = "/tmp/osdev-atexit.sock"
PROMPT = b"/$ "


def cleanup_sock():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass


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
        q.send_signal(signal.SIGKILL); q.wait(timeout=3)
    except Exception: pass
    cleanup_sock()


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SERIAL_SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
        elif out: break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def send_cmd(s, cmd, timeout=15.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0: out = out[idx + len(cmd):]
    return out


PASSES, FAILS = [], []


def expect(cond, msg):
    if cond: print(f"PASS: {msg}"); PASSES.append(msg)
    else: print(f"FAIL: {msg}"); FAILS.append(msg)


def main():
    print("[chapter 120] /bin/atexittest smoke test")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)

        out = send_cmd(s, "/bin/atexittest", timeout=15.0)
        print("--- atexittest output ---")
        try: print(out.decode("utf-8", errors="replace"))
        except Exception: print(repr(out))
        print("-------------------------")

        txt = out.decode("utf-8", errors="replace")
        i_c1 = txt.find("ctor1")
        i_c2 = txt.find("ctor2")
        i_m  = txt.find("main\n")
        if i_m < 0: i_m = txt.find("main\r")
        i_e1 = txt.find("exit1")
        i_e2 = txt.find("exit2")
        i_d  = txt.find("dtor")
        i_ex = txt.find("exited with code 0x0000000000000007")

        expect(i_c1 >= 0, "ctor1 ran")
        expect(i_c2 >= 0, "ctor2 ran")
        expect(i_m  >= 0, "main ran")
        expect(i_e1 >= 0, "exit1 ran")
        expect(i_e2 >= 0, "exit2 ran")
        expect(i_d  >= 0, "dtor ran")
        expect(i_c1 >= 0 and i_c2 >= 0 and i_m >= 0
               and i_c1 < i_m and i_c2 < i_m,
               "both ctors ran BEFORE main (__init_array walk)")
        expect(i_m >= 0 and i_e1 >= 0 and i_e2 >= 0
               and i_m < i_e1 and i_m < i_e2,
               "main ran before atexit handlers")
        expect(i_e2 >= 0 and i_e1 >= 0 and i_e2 < i_e1,
               "exit2 ran before exit1 (atexit LIFO)")
        expect(i_e1 >= 0 and i_d >= 0 and i_e1 < i_d,
               "dtor ran AFTER atexit chain (.fini_array last)")
        expect(i_ex >= 0,
               "/bin/atexittest exited with code 7 via crt0 forwarder")

        print()
        print(f"{len(PASSES)} PASS / {len(FAILS)} FAIL")
        if FAILS:
            print("FAILED:")
            for f in FAILS: print(f"  - {f}")
            return 1
        return 0
    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)


if __name__ == "__main__":
    sys.exit(main())
