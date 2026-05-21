#!/usr/bin/env python3
"""
_dbg_external_cookie.py -- probe what actually happens when the
guest fetches a real external host that emits Set-Cookie.

The user asked "can I test cookies against httpbin.org/response-
headers?Set-Cookie=foo%3Dbar?" and the previous answer was
speculation.  This script answers it for real.

Test fixture:
  1. Reformat /data (so we know the jar starts empty).
  2. Boot with virtio-net + virtio-blk hd1 (data disk).
  3. From the in-guest shell, run httpget against the external
     httpbin endpoint.
  4. Inspect /data and /data/cookies/ -- did anything land?
  5. Re-run the request and watch for an outgoing Cookie: header.

Captures the full serial transcript so we can see which of these
fired:
  [browser] resolved httpbin.org -> X.X.X.X
  [browser] HTTP/1.1 200 OK ...
  [browser] stored N cookie(s) from httpbin.org
  [browser] sending N cookie(s) to httpbin.org   (on the retry)
"""
import os, socket, subprocess, signal, select, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-ext-cookie.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

URL = "http://httpbin.org/response-headers?Set-Cookie=foo%3Dbar"


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
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
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


def send_cmd(s, cmd, timeout=30.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out


def section(title):
    print()
    print("=" * 70)
    print(title)
    print("=" * 70)


def main():
    print("[+] reformatting /data ...")
    reformat_data()
    print("[+] booting ...")
    q = boot()
    try:
        s = conn()
        buf = wait_for(s, PROMPT, timeout=180.0)
        if PROMPT not in buf:
            print("FAIL: never reached shell")
            print(buf[-2000:].decode("ascii", "replace"))
            return 1
        print("[+] shell up")

        section("baseline: /data should be empty")
        out = send_cmd(s, "ls /data")
        print(out.decode("ascii", "replace"))

        section(f"httpget {URL}")
        out = send_cmd(s, f"httpget {URL}", timeout=60.0)
        print(out.decode("ascii", "replace"))

        section("/data after the request")
        out = send_cmd(s, "ls /data")
        print(out.decode("ascii", "replace"))
        out = send_cmd(s, "ls /data/cookies")
        print(out.decode("ascii", "replace"))
        out = send_cmd(s, "cat /data/cookies/httpbin.org")
        print(out.decode("ascii", "replace"))

        section(f"httpget {URL} (second hit -- should send Cookie:)")
        out = send_cmd(s, f"httpget {URL}", timeout=60.0)
        print(out.decode("ascii", "replace"))

        section("now try the SAME url through the browser binary "
                "(--paint forces a headless render)")
        out = send_cmd(s, f"browser --paint {URL}", timeout=90.0)
        print(out.decode("ascii", "replace"))

        section("/data again")
        out = send_cmd(s, "ls /data/cookies")
        print(out.decode("ascii", "replace"))
        out = send_cmd(s, "cat /data/cookies/httpbin.org")
        print(out.decode("ascii", "replace"))

        return 0
    finally:
        hard_kill(q)


if __name__ == "__main__":
    sys.exit(main())
