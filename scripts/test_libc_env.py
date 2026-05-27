#!/usr/bin/env python3
"""scripts/test_libc_env.py -- chapter 151 env arena smoke test.

Boots the OS and runs `/bin/envtest`, then asserts the new
POSIX-shaped env API behaves correctly:

    T1  getenv("PATH") returns non-NULL (init seeded "/bin").
    T2  getenv("ZZZ_NEVER_SET") returns NULL.
    T3  setenv("FOO","bar",1) + getenv returns "bar".
    T4  setenv("FOO","baz",0) leaves "bar" in place.
    T5  setenv("FOO","baz",1) overwrites to "baz".
    T6  unsetenv("FOO") clears it; getenv == NULL.
    T7  putenv("MUTEX=42") + getenv returns "42".
    T8  environ[] iteration finds the live entries.
    T9  setenv with '=' in the name returns -1 with errno=EINVAL.
    T10 __sys_getenv("MUTEX") returns "42" too (kernel write-through).

Run from the workspace root:

    python3 scripts/test_libc_env.py
"""

import os, signal, socket, subprocess, sys, time, select, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-libc-env.sock"
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


PASS_RE = re.compile(rb"\[envtest\]\s+(T\d+)\s+PASS")
FAIL_RE = re.compile(rb"\[envtest\]\s+(T\d+)\s+FAIL")


def main():
    print("[chapter 151] env.h POSIX surface (getenv/setenv/unsetenv/putenv/environ)")
    reformat_data()

    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=15.0)
        out = send_cmd(s, "/bin/envtest", timeout=15.0)

        passes = {m.group(1).decode() for m in PASS_RE.finditer(out)}
        fails  = {m.group(1).decode() for m in FAIL_RE.finditer(out)}

        for t, label in [
            ("T1",  "getenv(PATH) returns non-NULL (init seeded /bin)"),
            ("T2",  "getenv(ZZZ_NEVER_SET) returns NULL"),
            ("T3",  "setenv(FOO,bar,1) + getenv roundtrip"),
            ("T4",  "setenv(FOO,baz,0) preserves existing value"),
            ("T5",  "setenv(FOO,baz,1) overwrites"),
            ("T6",  "unsetenv(FOO) clears; getenv returns NULL"),
            ("T7",  "putenv(MUTEX=42) installs"),
            ("T8",  "environ[] iteration finds PATH and MUTEX"),
            ("T9",  "setenv with '=' in name -> EINVAL"),
            ("T10", "kernel-side __sys_getenv reflects write-through"),
        ]:
            expect(t in passes and t not in fails, label)

        expect(b"[envtest] ALL PASS" in out,
               "binary printed ALL PASS marker")

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
