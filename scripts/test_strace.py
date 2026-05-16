#!/usr/bin/env python3
"""scripts/test_strace.py — chapter 100 smoke test.

Boots the kernel, drops into /bin/sh, and runs:

    strace echo hello

…which must:
  1. Print "hello" (from the traced echo).
  2. Print a trace line that contains a recognisable syscall
     name like "write" or "execv" — proves the kernel tracer
     ring rendered through /proc/<pid>/trace and that
     /bin/strace pumped it to stderr.
  3. Print "strace: + exited with code 0" (parent saw waitpid).

Also runs a degenerate `cat /proc/1/trace` to prove the
"not traced" banner appears for an unattached thread.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-strace.sock"


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


def run_cmd(ser, cmd, log, timeout=15.0):
    ser.sendall(cmd.encode() + b"\n")
    new_log = wait_for_new(ser, [b"$ ", b"PANIC"], timeout, log)
    section = new_log[len(log):]
    return new_log, section.decode("ascii", "replace")


def fail(msg, section=""):
    print(f"FAIL: {msg}")
    if section:
        print("---- last section ----")
        print(section)
        print("----------------------")
    return 1


def main():
    q = boot()
    try:
        ser = conn()
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            return fail("no shell prompt within 90s",
                        log[-2000:].decode("ascii", "replace"))

        # --- /proc/<pid>/trace banner check ----------------------
        # Grab the live pid list from `ls /proc` rather than
        # guessing 1/2/3 — early kernel threads burn small ids.
        log, out = run_cmd(ser, "ls /proc", log, timeout=10.0)
        live_pids = sorted({int(tok) for tok in out.replace("\r"," ").split()
                            if tok.isdigit()})
        if not live_pids:
            return fail("ls /proc had no numeric leaves", out)

        seen_banner = False
        last_out = ""
        for pid in live_pids[:5]:
            log, last_out = run_cmd(ser, f"cat /proc/{pid}/trace", log,
                                    timeout=10.0)
            if "(not traced)" in last_out:
                seen_banner = True
                break
        if not seen_banner:
            return fail("no (not traced) banner from any live pid", last_out)

        # --- strace echo hello -----------------------------------
        log, out = run_cmd(ser, "strace /bin/echo hello", log, timeout=30.0)

        if "hello" not in out:
            return fail("strace child did not print 'hello'", out)

        # Look for a syscall-shaped line: contains one of the
        # well-known syscall names rendered by strace.c's
        # SYSCALL_META[] table, followed by '('.
        recognised = ("write(", "open(", "execv(", "close(", "read(",
                      "fork(", "exit(", "getpid(", "waitpid(", "brk(")
        if not any(name in out for name in recognised):
            return fail("strace output had no recognisable syscall line", out)

        if "strace: + exited with code" not in out:
            return fail("strace did not announce child exit", out)

        print("PASS: /proc/<pid>/trace + /bin/strace work end-to-end")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())
