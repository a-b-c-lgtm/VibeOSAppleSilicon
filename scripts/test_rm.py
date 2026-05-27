#!/usr/bin/env python3
# scripts/test_rm.py
# ─────────────────────────────────────────────────────────────────────────────
# Smoke test for /bin/rm.
#
# The doom_link.mk `clean` recipe (and any future Makefile we ship
# on the OS) wants `rm -f $(OUTPUT)`.  Before this binary existed
# the shell had a built-in `rm` but no spawnable /bin/rm, so make
# would fail.  This test boots the OS, drops a file into /data,
# removes it via /bin/rm, and covers the three cases the recipe
# needs: successful removal, silent -f on missing, and the
# diagnostic-on-missing default.
# ─────────────────────────────────────────────────────────────────────────────
import os, sys, time, socket, subprocess, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SERIAL_SOCK = "/tmp/osdev-rm.sock"
DISK_IMG    = f"{ROOT}/build/disk.img"
DATA_IMG    = f"{ROOT}/build/data.img"

QEMU = [
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-smp", "2", "-display", "none",
    "-serial", f"unix:{SERIAL_SOCK},server,nowait",
    "-global", "virtio-mmio.force-legacy=off",
    "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive", f"if=none,file={DISK_IMG},format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-kernel", f"{ROOT}/build/kernel.elf",
]

PASS, FAIL = 0, 0
def expect(cond, label):
    global PASS, FAIL
    if cond:
        print(f"PASS: {label}"); PASS += 1
    else:
        print(f"FAIL: {label}"); FAIL += 1

def reformat_data():
    subprocess.check_call(
        [sys.executable, f"{ROOT}/scripts/mkosfs2.py", DATA_IMG],
        cwd=ROOT)

def send_cmd(sock, cmd, timeout=10.0, idle=1.5):
    sock.sendall((cmd + "\n").encode())
    out = b""
    deadline = time.time() + timeout
    last = time.time()
    sock.settimeout(0.3)
    while time.time() < deadline:
        try:
            chunk = sock.recv(8192)
            if chunk:
                out += chunk
                last = time.time()
            else:
                break
        except socket.timeout:
            if time.time() - last >= idle:
                break
    return out

def wait_for_prompt(sock, deadline_seconds=90.0):
    out = b""
    deadline = time.time() + deadline_seconds
    sock.settimeout(1.0)
    while time.time() < deadline:
        try:
            chunk = sock.recv(8192)
            if chunk:
                out += chunk
                if b"/$ " in out or b"$ " in out:
                    return out
        except socket.timeout:
            pass
    return out

def main():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass

    reformat_data()

    qemu = subprocess.Popen(QEMU, cwd=ROOT,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)

    sock = None
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(SERIAL_SOCK)
                break
            except OSError:
                sock = None
                time.sleep(0.1)
        else:
            time.sleep(0.1)
    if sock is None:
        try: os.killpg(qemu.pid, signal.SIGKILL)
        except Exception: pass
        raise RuntimeError("could not connect to QEMU serial socket")

    try:
        boot = wait_for_prompt(sock, 90.0)
        expect(b"/$ " in boot or b"$ " in boot,
               "boot: reached shell prompt")

        out = send_cmd(sock, "/bin/ls /bin/rm")
        expect(b"rm" in out and b"no such" not in out.lower(),
               "step 1: /bin/rm shipped on OSFS-1")

        send_cmd(sock, "/bin/echo hello > /data/rmtest.txt")
        out = send_cmd(sock, "/bin/ls /data/rmtest.txt")
        expect(b"rmtest.txt" in out and b"no such" not in out.lower(),
               "step 2: created /data/rmtest.txt")

        out = send_cmd(sock, "/bin/rm /data/rmtest.txt")
        expect(b"cannot remove" not in out and b"missing operand" not in out,
               "step 3a: /bin/rm produced no error on extant file")

        out = send_cmd(sock, "/bin/ls /data/rmtest.txt")
        expect(b"no such" in out.lower() or b"enoent" in out.lower()
               or b"rmtest.txt" not in out,
               "step 3b: /data/rmtest.txt is gone")

        out = send_cmd(sock, "/bin/rm -f /data/does-not-exist.txt")
        expect(b"cannot remove" not in out and b"missing operand" not in out,
               "step 4: /bin/rm -f silent on missing file")

        out = send_cmd(sock, "/bin/rm /data/does-not-exist.txt")
        expect(b"cannot remove" in out
               and b"No such file or directory" in out,
               "step 5: /bin/rm (no -f) prints error for missing file")
    finally:
        try: sock.close()
        except Exception: pass
        try: os.killpg(qemu.pid, signal.SIGTERM)
        except Exception: pass
        time.sleep(0.5)
        try: os.killpg(qemu.pid, signal.SIGKILL)
        except Exception: pass
        try: os.unlink(SERIAL_SOCK)
        except FileNotFoundError: pass

    print(f"\nPASS={PASS} FAIL={FAIL}")
    sys.exit(0 if FAIL == 0 else 1)

if __name__ == "__main__":
    main()
