#!/usr/bin/env python3
"""scripts/test_userfs_timeout.py -- chapter 114f deadline test.

Boots the OS, spawns `/bin/hangfs &`, then opens a file under
the deliberately-wedged mount and verifies that the call
returns -ETIMEDOUT_VFS (-110) within a bounded wall-clock
budget instead of hanging the calling client forever.

What this proves:
    1. The kernel's per-request 5 s deadline in
       `userfs_call` actually fires (the daemon will never
       reply -- it sleeps in 1 s chunks and never touches
       its request pipe).
    2. The deadline is *bounded* -- the open call returns
       between 5 s (best case) and ~7 s (worst case, allowing
       for tick granularity + shell echo).  We allow up to
       15 s end-to-end before declaring a hang.
    3. The shell prompt comes back after the failed open --
       the calling process did NOT get killed; cat just
       exits non-zero and the shell prints the next prompt.

The hangfs binary itself never closes its fds and never
calls `userfs_serve`.  See userspace/hangfs/hangfs.c for the
3-line implementation; see book/chapters/16-filesystem-
architecture/114f-userfs-timeouts.md for the design
rationale.

Run from the workspace root:

    python3 scripts/test_userfs_timeout.py
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-userfs-timeout.sock"
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
    raise RuntimeError(f"no serial socket: {SERIAL_SOCK}")


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


def send_cmd(s, cmd, timeout=10.0):
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
    print("[chapter 114f] userfs per-request deadline (hangfs)")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=15.0)

        # Spawn the deliberately-wedged daemon and wait for
        # its mount confirmation.  hangfs uses mount_kernel
        # directly (no libfs) so it prints its own banner.
        s.sendall(b"/bin/hangfs &\n")
        log = wait_for(s, b"hangfs: mounted /hang", timeout=10.0)
        expect(b"hangfs: mounted /hang" in log,
               "hangfs reported its mount line")

        # Drain to the next prompt before issuing the request
        # we expect to time out.  Same race-window guard as
        # test_userfs_echo.
        wait_for(s, PROMPT, timeout=5.0)

        # Open a file under /hang.  The daemon will never reply,
        # so cat should sit for ~5 s then print
        #   "cat: cannot open /hang/anything: errno=110"
        # and the shell prompt should follow.  We allow up to
        # 15 s before declaring a hang.
        t0 = time.time()
        out = send_cmd(s, "/bin/cat /hang/anything", timeout=15.0).decode(
            "utf-8", "replace")
        elapsed = time.time() - t0

        expect("errno=110" in out,
               f"cat /hang/anything returns errno=110 (got {out!r})")

        # The deadline is 5 s on the kernel side; the call
        # round-trip should be in [5, 10] s once shell echo and
        # tick granularity are counted.  Loose upper bound so
        # this test stays robust on slow CI hosts.
        expect(4.5 <= elapsed <= 12.0,
               f"cat timed out within bounded window "
               f"(elapsed={elapsed:.2f}s, expected 5..12 s)")

        # After the failed open, the shell prompt MUST come back
        # (the calling process wasn't killed -- cat just exited
        # non-zero).  Send a trivial command and check it works.
        out = send_cmd(s, "/bin/echo alive-after-timeout").decode(
            "utf-8", "replace")
        expect("alive-after-timeout" in out,
               f"shell still responsive after timeout (got {out!r})")

        # Second attempt against the same broken mount should
        # short-circuit: once userfs_call marked the channel
        # dead, subsequent opens return -EIO (-5) immediately
        # (no 5 s wait).  This proves the dead-channel
        # short-circuit also works.
        t0 = time.time()
        out = send_cmd(s, "/bin/cat /hang/another", timeout=10.0).decode(
            "utf-8", "replace")
        elapsed2 = time.time() - t0
        expect("errno=" in out and "errno=110" not in out and
               "cannot open" in out,
               f"second open returns a different errno (got {out!r})")
        expect(elapsed2 < 4.0,
               f"second open short-circuits (elapsed={elapsed2:.2f}s)")

    finally:
        try:
            s.close()
        except Exception:
            pass
        hard_kill(q)

    print(f"\n{len(PASSES)} PASS / {len(FAILS)} FAIL")
    if FAILS:
        print("FAILED:")
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
