#!/usr/bin/env python3
"""scripts/test_libc_errno.py -- chapter 149 errno smoke test.

Boots the OS and runs `/bin/errnotest`, then asserts that:

    1. open() of a missing path returns -1 (POSIX as of 116d)
       AND sets `errno=2` (ENOENT).
    2. close(-1) returns -1 AND sets `errno=9` (EBADF).
    3. read(-1, ...) returns -1 AND sets `errno=9` (EBADF).
    4. A successful syscall (getpid) does NOT clobber a stale
       errno written by the test (`errno=42` survives).

The point of (4) is to lock in the rule that `__svc_check`
only writes errno on failure -- the same shape POSIX requires.

Run from the workspace root:

    python3 scripts/test_libc_errno.py
"""

import os, signal, socket, subprocess, sys, time, select, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-libc-errno.sock"
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


LINE_RE = re.compile(
    rb"\[errnotest\]\s+(\S+)\s+rc=(-?\d+)\s+errno=(-?\d+)"
)


def parse(out):
    """Map tag -> (rc, errno) from errnotest's output lines."""
    rows = {}
    for tag, rc, err in LINE_RE.findall(out):
        rows[tag.decode()] = (int(rc), int(err))
    return rows


def main():
    print("[chapter 149] errno populated by syscall wrappers")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=15.0)
        out = send_cmd(s, "/bin/errnotest", timeout=10.0)

        rows = parse(out)

        # (1) open() of a missing path.
        rc_open, err_open = rows.get("open_missing", (None, None))
        expect(rc_open is not None and rc_open < 0,
               f"open() of missing path returns negative rc "
               f"(got rc={rc_open})")
        expect(err_open == 2,
               f"errno after open(missing) is ENOENT=2 "
               f"(got errno={err_open})")
        expect(rc_open == -1 if rc_open is not None else False,
               f"open() rc is -1 per POSIX convention (chapter 152) "
               f"(got rc={rc_open})")

        # (2) close(-1).
        rc_close, err_close = rows.get("close_badfd", (None, None))
        expect(rc_close is not None and rc_close < 0,
               f"close(-1) returns negative rc (got rc={rc_close})")
        expect(err_close == 9,
               f"errno after close(-1) is EBADF=9 "
               f"(got errno={err_close})")

        # (3) read(-1, ...).
        rc_read, err_read = rows.get("read_badfd", (None, None))
        expect(rc_read is not None and rc_read < 0,
               f"read(-1, ...) returns negative rc "
               f"(got rc={rc_read})")
        expect(err_read == 9,
               f"errno after read(-1) is EBADF=9 "
               f"(got errno={err_read})")

        # (4) A successful syscall must not stomp errno.
        rc_getpid, err_getpid = rows.get("getpid_ok", (None, None))
        expect(rc_getpid is not None and rc_getpid > 0,
               f"getpid() returns positive pid (got rc={rc_getpid})")
        expect(err_getpid == 42,
               f"successful syscall preserves pre-call errno "
               f"(set to 42; got errno={err_getpid})")

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
