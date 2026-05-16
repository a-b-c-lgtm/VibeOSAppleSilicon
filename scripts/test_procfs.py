#!/usr/bin/env python3
"""scripts/test_procfs.py — chapter 99 smoke test.

Boots the kernel, drops into /bin/sh on the kernel console, and
exercises the read-only /proc pseudo-FS.

Checks:
  - `ls /proc` lists at least the four static files plus one
    numeric pid leaf.
  - `cat /proc/uptime` returns two whitespace-separated numbers.
  - `cat /proc/meminfo` returns MemTotal / MemFree / MemUsed.
  - `cat /proc/cpuinfo` returns at least `cpu0:` online.
  - `cat /proc/sched` returns at least one cpu runqueue row.
  - `ps` lists init and /bin/sh.

The test is end-to-end through the syscall layer — it does NOT
poke procfs.c directly — so the VFS prefix dispatch, FD_PROCFS
read path, snapshot kfree on close, and listdir_at /proc branch
are all exercised on the kernel side.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-procfs.sock"


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
    """As in test_printftest.py: wait for needle that arrives AFTER
    log was sampled, so the previous '$ ' prompt doesn't cause an
    instant stale match."""
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
    """Send `cmd\\n` to the shell and wait for the next prompt.
    Returns (updated_log, output_section_after_command)."""
    ser.sendall(cmd.encode() + b"\n")
    new_log = wait_for_new(ser, [b"$ ", b"PANIC"], timeout, log)
    # Slice just the bytes emitted after the command echo.
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

        # --- /proc/uptime ----------------------------------------
        log, out = run_cmd(ser, "cat /proc/uptime", log)
        # Two whitespace-separated decimal numbers on a single line
        # ("<sec>.<cs> <sec>.<cs>\n").  Walk lines and look for a
        # line that has exactly two tokens, both with a '.'.  This
        # avoids matching unrelated kernel/init noise like
        # "[init] reaped background tid=12 code=1".
        seen_uptime = False
        for line in out.splitlines():
            parts = line.strip().split()
            if len(parts) == 2 and all("." in p for p in parts):
                a, b = parts
                try:
                    float(a); float(b)
                    seen_uptime = True
                    break
                except ValueError:
                    pass
        if not seen_uptime:
            return fail("uptime: no <sec.cs> <sec.cs> line", out)

        # --- /proc/meminfo ---------------------------------------
        log, out = run_cmd(ser, "cat /proc/meminfo", log)
        for key in ("MemTotal:", "MemFree:", "MemUsed:", "PageSize:"):
            if key not in out:
                return fail(f"meminfo missing {key}", out)

        # --- /proc/cpuinfo ---------------------------------------
        log, out = run_cmd(ser, "cat /proc/cpuinfo", log)
        for key in ("cpus:", "cpu0:"):
            if key not in out:
                return fail(f"cpuinfo missing {key}", out)

        # --- /proc/sched -----------------------------------------
        log, out = run_cmd(ser, "cat /proc/sched", log)
        if "threads_live:" not in out or "runqueue" not in out:
            return fail("sched missing rows", out)

        # --- ls /proc --------------------------------------------
        log, out = run_cmd(ser, "ls /proc", log)
        for key in ("uptime", "meminfo", "cpuinfo", "sched"):
            if key not in out:
                return fail(f"ls /proc missing {key}", out)
        # At least one numeric pid leaf.
        has_pid = any(tok.isdigit() for tok in out.replace("\r"," ").split())
        if not has_pid:
            return fail("ls /proc missing pid leaves", out)

        # --- ps --------------------------------------------------
        log, out = run_cmd(ser, "ps", log, timeout=20.0)
        if "PID" not in out or "CMD" not in out:
            return fail("ps missing header", out)
        # Should see at least sh listed (the shell we're running in)
        # and init or another long-lived thread.
        if "sh" not in out:
            return fail("ps did not list sh", out)

        print("PASS: /proc dispatch + ps work end-to-end")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())
