#!/usr/bin/env python3
"""scripts/test_mount_ro.py — chapter 132 step 7: MOUNT_RO + EROFS_VFS
hardening verification.

For each MOUNT_RO mount registered by vfs_init() —

    /        (root ramfs catchall)
    /proc    (procfs)
    /mnt     (osfs1)
    /bin     (osfs1, second alias)

— we expect every mutation attempt to return -EROFS_VFS (errno 30),
NOT -ENOENT_VFS (errno 2) or -EINVAL_VFS (errno 22).  The check is
performed by vfs_open / sys_unlink / sys_mkdir BEFORE the driver's
op_open / op_unlink / op_mkdir method runs, so even RO mounts whose
driver lacks a write method respond with EROFS rather than some
downstream "operation not supported" code.

This test is the contract enforcement for chapter-113's MOUNT_RO
flag.  If a future refactor reorders or removes the RO check,
this test fails and points at the regression.
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-mount-ro.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

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
            if not c: break
            out += c
        elif out:
            break
    return out

def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf

def send_cmd(s, cmd, timeout=10.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out

PASSES, FAILS = [], []

def expect(cond, msg, extra=None):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        if extra:
            print(f"      raw: {extra!r}")
        FAILS.append(msg)

def main():
    print("[chapter 132 step 7] MOUNT_RO + EROFS_VFS hardening")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=15.0)

        # The shell prints "errno=30" for EROFS_VFS.  We assert
        # that literal substring rather than "errno=" alone so a
        # different errno (ENOENT=2, EINVAL=22) doesn't sneak by.
        RO = b"errno=30"

        # ---- mkdir on each MOUNT_RO mount ----
        out = send_cmd(s, "mkdir /proc/foo")
        expect(RO in out, "mkdir /proc/foo => EROFS_VFS", out)

        out = send_cmd(s, "mkdir /bin/foo")
        expect(RO in out, "mkdir /bin/foo  => EROFS_VFS", out)

        out = send_cmd(s, "mkdir /mnt/foo")
        expect(RO in out, "mkdir /mnt/foo  => EROFS_VFS", out)

        out = send_cmd(s, "mkdir /motd")
        expect(RO in out, "mkdir /motd     => EROFS_VFS (root ramfs)", out)

        # ---- unlink on each MOUNT_RO mount ----
        # /proc/<pid>/cmdline always exists for self == pid 1 or
        # higher; we don't care if the unlink "succeeds" in some
        # weird future world — we care that EROFS fires first.
        out = send_cmd(s, "rm /proc/self/cmdline")
        expect(RO in out, "rm /proc/self/cmdline => EROFS_VFS", out)

        # /bin and /mnt have real OSFS-1 files: pick anything we
        # know is there.  ls and cat are part of the standard
        # init image (see Makefile OSFS_BIN_FILES).
        out = send_cmd(s, "rm /bin/ls")
        expect(RO in out, "rm /bin/ls => EROFS_VFS", out)

        out = send_cmd(s, "rm /mnt/init")
        # /mnt/init may not exist but we still want EROFS first
        # rather than ENOENT — the RO check runs before the
        # driver's op_unlink does any lookup.
        expect(RO in out, "rm /mnt/init => EROFS_VFS", out)

        out = send_cmd(s, "rm /motd")
        expect(RO in out, "rm /motd => EROFS_VFS (root ramfs)", out)

        # ---- redirect-write to a path on a RO mount ----
        # The shell opens the redirect target with O_WRONLY|O_CREAT
        # |O_TRUNC, so this exercises the open-time check too.
        out = send_cmd(s, "echo hi > /proc/foo")
        expect(b"errno=30" in out,
               "echo > /proc/foo => EROFS_VFS at open", out)

        out = send_cmd(s, "echo hi > /motd")
        expect(b"errno=30" in out,
               "echo > /motd => EROFS_VFS at open (root ramfs)", out)

        # ---- sanity: /tmp and /data remain writable ----
        out = send_cmd(s, "echo hi > /tmp/probe.txt")
        expect(b"errno" not in out, "echo > /tmp/probe.txt succeeds", out)

        out = send_cmd(s, "echo hi > /data/probe.txt")
        expect(b"errno" not in out, "echo > /data/probe.txt succeeds", out)
    finally:
        try: s.close()
        except Exception: pass
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
