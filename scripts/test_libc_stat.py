#!/usr/bin/env python3
"""scripts/test_libc_stat.py -- chapter 153 stat / fstat / dirent / access smoke test.

Boots the OS, runs /bin/stattest, and asserts the new POSIX
file-metadata surface:

    A1  stat("/mnt/hello.txt") returns 0 with S_IFREG and nonzero size.
    A2  stat("/data") and stat("/") report S_IFDIR.
    A3  stat("/does/not/exist") returns -1 with errno=ENOENT.
    A4  fstat() of an opened file matches stat() of its path.
    A5  opendir("/mnt") + readdir() yields at least one DT_REG.
    A6  access("/bin/cat", R_OK) succeeds; access(missing) -> ENOENT.

Run:  python3 scripts/test_libc_stat.py
"""

import os, signal, socket, subprocess, sys, time, select, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-libc-stat.sock"
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


def send_cmd(s, cmd, timeout=15.0):
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


PASS_RE = re.compile(rb"\[stattest\]\s+PASS:\s+(.+)")
FAIL_RE = re.compile(rb"\[stattest\]\s+FAIL:\s+(.+)")


def main():
    print("[chapter 153] stat/fstat/dirent/access POSIX surface")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        out = send_cmd(s, "/bin/stattest", timeout=20.0)

        pass_msgs = [m.group(1).strip().decode(errors="replace")
                     for m in PASS_RE.finditer(out)]
        fail_msgs = [m.group(1).strip().decode(errors="replace")
                     for m in FAIL_RE.finditer(out)]

        expect(len(pass_msgs) >= 18,
               f"binary reports >=18 PASS lines (got {len(pass_msgs)})")
        expect(len(fail_msgs) == 0,
               f"binary reports no FAIL lines (got {len(fail_msgs)})")
        expect(b"[stattest] ALL PASS" in out,
               "binary printed ALL PASS marker")

        # Spot-check a few specific assertions made it through as PASSes
        # (substring inside the PASS message list, not the raw output —
        # otherwise a FAIL line with the same text would still match).
        wanted = [
            "stat(/mnt/hello.txt) reports a regular file",
            "stat(/data) reports a directory",
            "stat(missing) sets errno=ENOENT",
            "fstat reports a regular file",
            "readdir(/mnt) yields at least one DT_REG",
            "access(/bin/cat, R_OK) succeeds",
        ]
        for w in wanted:
            expect(w in pass_msgs, f"PASS: {w}")

        if fail_msgs:
            print("\nbinary FAIL lines:")
            for f in fail_msgs:
                print(f"  - {f}")

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
