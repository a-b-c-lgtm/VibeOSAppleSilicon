#!/usr/bin/env python3
"""scripts/test_userfs_echo.py -- chapter 140 user-space filesystem
smoke test, end-to-end.

Boots the OS, spawns `/bin/echofs &`, then exercises the new
SYS_MOUNT / SYS_UMOUNT plumbing by:

    1. `mount` -- verifies `/echo` appears in the kernel mount
       table after the daemon installed itself.
    2. `cat /echo/hello` -- verifies the canned "hello from
       echofs" string round-trips through the daemon (open,
       read, close on the userfs path).
    3. Write-then-read on `/echo/buf` -- verifies the write
       path stores bytes and a subsequent read returns them.
    4. `ls /echo` -- verifies on_listdir reports the three
       child entries.

The echofs binary itself is the smallest possible client of
`userspace/libfs/userfs.c`; passing this test means every
piece of the user-space filesystem RPC -- sys_mount, the pipe
pair, the p9_msg encoder/decoder, the libfs serve loop, the
g_userfs_ops vtable, vfs_resolve's longest-prefix match --
works end-to-end on a real `/echo/...` path.

Run from the workspace root:

    python3 scripts/test_userfs_echo.py
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-userfs-echo.sock"
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
    print("[chapter 140] userfs end-to-end (echofs)")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=15.0)

        # Spawn the daemon in the background and wait for libfs's
        # "mounted as id" log line to confirm the channel is live
        # before we issue any /echo/ ops.
        s.sendall(b"/bin/echofs &\n")
        log = wait_for(s, b"/echo mounted as id", timeout=10.0)
        expect(b"/echo mounted as id" in log,
               "echofs reported its libfs mount line")

        # Race-window guard: even after the mount log line, the
        # kernel's mount-table insert is sequenced with the
        # daemon's first pipe_read, which races the next shell
        # prompt by a few hundred microseconds.  Drain to prompt
        # before issuing any /echo/ command.
        wait_for(s, PROMPT, timeout=5.0)

        # 1. `mount` lists /echo alongside the chapter-113 mounts.
        out = send_cmd(s, "/bin/mount").decode("utf-8", "replace")
        lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
        expect(any(ln == "/echo" or ln.startswith("/echo ") for ln in lines),
               f"/echo appears in mount table (saw {lines})")

        # 2. `cat /echo/hello` returns the canned greeting.  The
        # trailing newline is part of the payload (k_hello ends
        # in '\n'); the shell prompt follows.
        out = send_cmd(s, "/bin/cat /echo/hello").decode("utf-8", "replace")
        expect("hello from echofs" in out,
               f"cat /echo/hello returns canned text (got {out!r})")

        # 3. Write to /echo/buf via shell redirect, then read it
        # back via cat.  Using `echo` because it's the smallest
        # binary that can stuff bytes through `> /echo/buf`.
        send_cmd(s, "/bin/echo userfs-write-test > /echo/buf")
        out = send_cmd(s, "/bin/cat /echo/buf").decode("utf-8", "replace")
        expect("userfs-write-test" in out,
               f"write-then-read on /echo/buf round-trips "
               f"(got {out!r})")

        # 4. ls /echo lists the three children.  The order isn't
        # guaranteed by on_listdir's contract (only "idx in
        # range" is) but echofs returns them in declaration
        # order: hello, buf, echo.
        out = send_cmd(s, "/bin/ls /echo").decode("utf-8", "replace")
        for name in ("hello", "buf", "echo"):
            expect(name in out,
                   f"ls /echo includes {name!r}")

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
