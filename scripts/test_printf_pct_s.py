#!/usr/bin/env python3
"""scripts/test_printf_pct_s.py — minimal repro for the
printf("%s\\n", ptr) truncation we hit in milestone 58.

We boot, drop to a shell, and run two tools that exercise
printf with %s on heap- and stack-resident strings:

    printftest          (prints "hello %s, you are %d ...")
    env                 (prints each NUL-delimited env entry as "%s\\n")

Records the full transcript so we can see if any single-shot
"%s\\n" line ever survives the entire string.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-printf-pct-s.sock"


def boot():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
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
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def read_until(ser, needles, timeout, prior=b""):
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles): return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if any(n in buf for n in needles): return bytes(buf)
    return bytes(buf)


def wait_for_new(ser, needles, timeout, log):
    """Wait for any needle to appear in serial bytes received AFTER
    the current end-of-`log`. Returns the new accumulated buffer.

    Use this instead of `read_until(..., prior=log)` whenever the
    needle (e.g. the shell '$ ' prompt) might already be present in
    `log` from a previous step.
    """
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    cutoff = len(log)
    buf = bytearray(log)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(buf.find(n, cutoff) >= 0 for n in needles):
            return bytes(buf)
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: shell prompt"); return 1
        print("PASS: shell")

        # 1) printftest — known string literal in code
        ser.sendall(b"printftest\n")
        log = wait_for_new(ser, [b"all checks passed", b"PANIC"], 15.0, log)
        print("--- printftest output: ---")
        # print just the printftest section
        idx = log.rfind(b"printftest\r\n")
        if idx < 0: idx = log.rfind(b"printftest\n")
        if idx >= 0:
            section = log[idx:].decode("ascii", "replace")
            for line in section.split("\n"):
                print("  ", repr(line))
        if b"all checks passed" in log:
            print("PASS: printftest reached final line")
        else:
            print("FAIL: printftest did NOT reach final line")
            return 1

        # 2) Set an env var and dump it via 'env' (uses printf("%s\n"))
        # Use wait_for_new so each '$ ' wait blocks for the NEW prompt
        # produced by the just-sent command, not the stale one already
        # in `log`.
        log = wait_for_new(ser, [b"$ "], 5.0, log)
        ser.sendall(b"export MSG=this-should-survive-fully\n")
        log = wait_for_new(ser, [b"$ "], 5.0, log)
        ser.sendall(b"env\n")
        log = wait_for_new(ser, [b"$ "], 5.0, log)
        print("--- env output (last 1KB): ---")
        print(log[-1000:].decode("ascii", "replace"))
        if b"MSG=this-should-survive-fully" in log:
            print("PASS: env preserved the full %s string")
        else:
            print("FAIL: env did NOT preserve full %s string")
            return 1

        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())
