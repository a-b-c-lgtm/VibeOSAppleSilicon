#!/usr/bin/env python3
"""scripts/test_directories.py — chapter 85 subdirectory smoke test.

Exercises the new path-walking OSFS-2 syscalls and shell builtins:

  1. mkdir /data/notes
  2. ls /data/ should show "notes" with the <DIR> marker
  3. mkdir /data/notes/personal  (nested mkdir)
  4. echo hello > /data/notes/personal/hello.txt  (write into subdir)
  5. cat /data/notes/personal/hello.txt           (read it back)
  6. ls /data/notes/personal                      (lists the file)
  7. reboot, repeat ls + cat to verify persistence
  8. unlink the file, rmdir-by-shell the directory chain,
     verify it's gone (rmdir is not exposed yet so we just
     leave the empty dir)

Same boot/serial harness as test_journal.py so the two stay in
sync.  Designed to run in the regression sweep alongside
test_journal.py and test_notepad_save_as.py.
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-dirs.sock"
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

def boot_to_shell():
    q = boot()
    s = conn()
    wait_for(s, PROMPT, timeout=15.0)
    return q, s

def shutdown(q, s):
    try: s.close()
    except Exception: pass
    hard_kill(q)

# ---------- the test ----------

def main():
    print("[chapter 85] subdirectory smoke test")
    reformat_data()

    q, s = boot_to_shell()
    try:
        # 1. mkdir /data/notes
        out = send_cmd(s, "mkdir /data/notes")
        expect(b"errno" not in out,
               "mkdir /data/notes succeeds")

        # 2. ls /data shows the new dir with <DIR> marker.
        out = send_cmd(s, "ls /data")
        expect(b"<DIR>" in out and b"notes" in out,
               "ls /data shows notes/ with <DIR> marker")

        # 3. nested mkdir.
        out = send_cmd(s, "mkdir /data/notes/personal")
        expect(b"errno" not in out,
               "mkdir /data/notes/personal succeeds (nested)")

        out = send_cmd(s, "ls /data/notes")
        expect(b"<DIR>" in out and b"personal" in out,
               "ls /data/notes shows personal/")

        # 4. write a file into the subdir.
        out = send_cmd(s, "echo hello > /data/notes/personal/hi.txt")
        expect(b"errno" not in out,
               "write to /data/notes/personal/hi.txt succeeds")

        # 5. read it back.
        out = send_cmd(s, "cat /data/notes/personal/hi.txt")
        expect(b"hello" in out,
               "cat /data/notes/personal/hi.txt round-trips")

        # 6. ls the deep dir.
        out = send_cmd(s, "ls /data/notes/personal")
        expect(b"hi.txt" in out,
               "ls /data/notes/personal shows hi.txt")

        # 7. fsync to make sure the data actually hits disk.
        send_cmd(s, "/bin/sync")
    finally:
        shutdown(q, s)

    # 8. reboot and verify persistence.
    print("\n[chapter 85] verifying persistence across reboot")
    q, s = boot_to_shell()
    try:
        out = send_cmd(s, "ls /data")
        expect(b"<DIR>" in out and b"notes" in out,
               "after reboot: notes/ still present")
        out = send_cmd(s, "ls /data/notes")
        expect(b"<DIR>" in out and b"personal" in out,
               "after reboot: personal/ still present")
        out = send_cmd(s, "cat /data/notes/personal/hi.txt")
        expect(b"hello" in out,
               "after reboot: hi.txt content survives")

        # 9. unlink in subdir, then verify gone.
        send_cmd(s, "rm /data/notes/personal/hi.txt")
        out = send_cmd(s, "ls /data/notes/personal")
        expect(b"hi.txt" not in out,
               "after rm: hi.txt no longer listed")

        # 10. mkdir error path: parent doesn't exist.
        out = send_cmd(s, "mkdir /data/no/such/path")
        expect(b"errno" in out,
               "mkdir with missing parent fails with errno")

        # 11. mkdir error path: leaf already exists.
        out = send_cmd(s, "mkdir /data/notes")
        expect(b"errno" in out,
               "mkdir of existing dir fails with errno")
    finally:
        shutdown(q, s)

    print(f"\n{len(PASSES)} PASS / {len(FAILS)} FAIL")
    if FAILS:
        print("FAILED:")
        for f in FAILS:
            print(f"  - {f}")
        sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
