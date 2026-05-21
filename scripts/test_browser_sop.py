#!/usr/bin/env python3
"""scripts/test_browser_sop.py -- chapter 110a same-origin policy.

Drives `browser --check-sop <page-url> <action> [resolved]` from
the in-guest shell to exercise every branch of the form-submission
SOP decision tree:

  - Same origin (scheme + host + port all match):    silent allow.
  - Cross origin, action attr is absolute (author):  allow + log.
  - Cross origin via a relative action attr:         BLOCK + log.

The third branch is currently unreachable in production code (no
<base href>, no JS to mutate form.action), so the test feeds an
override "resolved" URL to simulate what chapter 113's <base
href> work would surface: a relative action that resolves
cross-origin.  Same one-line decision; same test fixture exercises
all branches.

The boot harness mirrors test_browser_cookies.py but skips
virtio-net because --check-sop is pure userspace and never
touches a socket.
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-sop.sock"
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
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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


def send_cmd(s, cmd, timeout=20.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


PASSES, FAILS = [], []


def expect(cond, msg, ctx=b""):
    if cond:
        print(f"PASS: {msg}")
        PASSES.append(msg)
    else:
        print(f"FAIL: {msg}")
        if ctx:
            tail = ctx[-1500:].decode("ascii", "replace")
            print("--- ctx ---\n" + tail + "\n-----------")
        FAILS.append(msg)


def expect_contains(out, needle, msg):
    if isinstance(needle, str): needle = needle.encode()
    expect(needle in out, msg, ctx=out)


def main():
    reformat_data()
    q = boot()
    s = conn()
    try:
        if PROMPT not in wait_for(s, PROMPT, 120.0):
            raise RuntimeError("shell prompt never reached")

        page = "http://127.0.0.1:80/index.html"

        # 1. Relative action against same host -> same origin.
        out = send_cmd(s, f"browser --check-sop {page} /login")
        expect_contains(out, "SOP: same-origin",
                        "relative action is same-origin")

        # 2. Absolute action to same host:port -> same origin.
        out = send_cmd(s, f"browser --check-sop {page} "
                          f"http://127.0.0.1:80/login")
        expect_contains(out, "SOP: same-origin",
                        "absolute action to same host:port is same-origin")

        # 3. Absolute action to different host -> cross-origin allowed.
        out = send_cmd(s, f"browser --check-sop {page} "
                          f"http://other.example.com/login")
        expect_contains(out, "SOP: cross-origin allowed",
                        "absolute action to different host is cross-origin allowed")

        # 4. Absolute action to different port -> cross-origin allowed.
        out = send_cmd(s, f"browser --check-sop {page} "
                          f"http://127.0.0.1:8080/login")
        expect_contains(out, "SOP: cross-origin allowed",
                        "absolute action to different port is cross-origin allowed")

        # 5. Absolute action to different scheme -> cross-origin allowed.
        out = send_cmd(s, f"browser --check-sop {page} "
                          f"https://127.0.0.1/login")
        expect_contains(out, "SOP: cross-origin allowed",
                        "https:// action from http page is cross-origin allowed")

        # 6. Protocol-relative cross-host -> cross-origin allowed
        #    (author wrote "//host", which the SOP treats as
        #    author-declared cross-origin via the // prefix).
        out = send_cmd(s, f"browser --check-sop {page} "
                          f"//evil.example.com/login")
        expect_contains(out, "SOP: cross-origin allowed",
                        "//host protocol-relative is cross-origin allowed")

        # 7. The blocked branch (defense-in-depth, currently
        #    unreachable in production -- needs <base href> or JS
        #    to mutate form.action).  Use the resolved-override
        #    third arg to simulate what chapter 113's <base href>
        #    would surface: a /login action that resolves to a
        #    third-party host.
        out = send_cmd(s, f"browser --check-sop {page} /login "
                          f"http://evil.example.com/login")
        expect_contains(out, "SOP: blocked",
                        "relative action that resolved cross-origin is blocked")

        # 8. Same-origin with case-different host -- host strings
        #    compare case-insensitively per RFC.
        out = send_cmd(s, f"browser --check-sop "
                          f"http://Example.COM:80/index.html "
                          f"http://example.com:80/login")
        expect_contains(out, "SOP: same-origin",
                        "hosts compare case-insensitively")

    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)

    print(f"\nresults: {len(PASSES)} pass, {len(FAILS)} fail")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
