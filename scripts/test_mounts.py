#!/usr/bin/env python3
"""scripts/test_mounts.py — chapter 132 mount-table syscall smoke test.

Boots the OS, runs `/bin/mount`, and verifies the output lists all
six kernel mounts with the expected MOUNT_RO flags:

    /         [ro]
    /proc     [ro]
    /tmp
    /mnt      [ro]
    /bin      [ro]
    /data

The exact registration order is set by vfs_init() in
kernel/core/vfs.c — root first (added by ramfs_register_root_mount
during vfs_init), then procfs, then tmpfs, then osfs1 twice
(/mnt then /bin), then osfs2 (/data).

This test exercises SYS_MOUNTS (#95) end-to-end: the userspace
`mounts()` wrapper packs the request, the kernel's sys_mounts
walks g_mounts[] via vfs_mount_count/vfs_mount_at, and the
results round-trip back through copy_to_user.
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-mounts.sock"
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

def expect(cond, msg):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        FAILS.append(msg)

def main():
    print("[chapter 132] SYS_MOUNTS smoke test")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=15.0)
        out = send_cmd(s, "/bin/mount")

        # Each line is "<prefix>" or "<prefix>  [ro]".  We don't
        # care about ordering between the entries — just that they
        # all show up with the right RO/RW disposition.
        lines = [ln.strip() for ln in out.decode("utf-8", "replace").splitlines()]
        # Drop the shell echo and any empty/prompt fragments.
        lines = [ln for ln in lines if ln and ln != "$"]

        def has(prefix, ro):
            tag = "  [ro]" if ro else ""
            return any(ln == prefix + tag for ln in lines)

        expect(has("/",     True),  "/ present with [ro]")
        expect(has("/proc", True),  "/proc present with [ro]")
        expect(has("/tmp",  False), "/tmp present (writable)")
        expect(has("/mnt",  True),  "/mnt present with [ro]")
        expect(has("/bin",  True),  "/bin present with [ro]")
        expect(has("/data", False), "/data present (writable)")

        # Sanity: at least six entries reported (could be more if a
        # future chapter mounts something else during boot, that's
        # fine — we only assert the six we shipped in chapter 132).
        expect(len(lines) >= 6,
               f"at least 6 mount entries reported (got {len(lines)})")
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
