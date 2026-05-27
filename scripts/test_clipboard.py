#!/usr/bin/env python3
"""scripts/test_clipboard.py -- chapter 140 clipboard test.

Boots the kernel headless, waits for the shell prompt, and
drives the clipboard via plain file I/O on /clipboard/text.
Since chapter 140, the clipboard daemon is a userfs mount
rather than an IPC service: write to a file to copy, read
the same file to paste, truncate it to clear.  That means
this test uses nothing but `echo`, `cat`, and shell
redirection -- the same tools an end user would use, and
the very thing the userfs port was designed to unlock.

What this exercises:

  - chapter 140 kernel side: SYS_MOUNT installed clipboardd's
    userfs handle into the kernel namespace at /clipboard,
    so VFS lookups under /clipboard route to the daemon.
  - chapter 140 client side: open / write / close on
    /clipboard/text travels through the kernel userfs glue
    to clipboardd's on_open / on_write handlers.
  - chapter 140 round-trip: cat /clipboard/text drains the
    daemon's static g_data buffer back to stdout, verifying
    on_read returns the just-written bytes.
  - chapter 140 init wiring: init still supervises
    /bin/clipboardd; the very fact that the file exists at
    the prompt proves the userfs_serve() call succeeded
    ahead of the shell.

Roughly 6 assertions, ~10 s wall.

Why no GUI events: the clipboard itself is GUI-agnostic.
Cross-app paste (notepad Ctrl-V, browser Ctrl-V) is covered
by scripts/test_clipboard_paste.py.  This regression keeps
to the shell so it stays fast and hermetic.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-clipboard.sock"


def cleanup():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass


def boot():
    """Launch QEMU headless.  Same shape as test_ipc.py."""
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=n0",
        "-device", "virtio-net-device,netdev=n0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c:
                break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def main():
    q = boot()
    try:
        ser = conn()

        # 1. Reach the shell prompt.  Boot also brings up
        #    /bin/clipboardd via init's supervisor.
        log = wait_for(ser, b"$ ", 60.0)
        if b"$ " not in log:
            print("FAIL: shell prompt not reached")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. Supervisor brought up clipboardd before the prompt.
        #    libfs's userfs_serve() prints "/clipboard mounted
        #    as id N" once SYS_MOUNT succeeds; that's the
        #    chapter-114 readiness signal.
        if b"/clipboard mounted" not in log:
            print("FAIL: clipboardd never mounted /clipboard")
            print(log[-3000:].decode("ascii", "replace"))
            return 1
        print("PASS: /clipboard is mounted")

        # 3. Write a payload via shell `>` redirection.  This
        #    exercises sh's open(O_TRUNC) path, which fans out
        #    to clipboardd's on_open + on_write.  Plain `echo`
        #    appends a trailing newline; we'll account for it
        #    in step 4.
        ser.sendall(b"echo Hello chapter 140 > /clipboard/text\n")
        log = wait_for(ser, b"$ ", 5.0)
        print("PASS: shell write to /clipboard/text returned")

        # 4. Read it back via `cat`.  We expect to see the
        #    exact line we just wrote, framed by the next
        #    prompt.
        ser.sendall(b"cat /clipboard/text\n")
        log = wait_for(ser, b"Hello chapter 140", 5.0)
        if b"Hello chapter 140" not in log:
            print("FAIL: cat /clipboard/text did not echo back")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: cat /clipboard/text returned the payload")

        # 5. Replace the payload.  on_open with O_TRUNC must
        #    reset g_len to 0 so the second cat returns only
        #    the new bytes, not "world" + leftover "Hello...".
        ser.sendall(b"echo world > /clipboard/text\n")
        log = wait_for(ser, b"$ ", 5.0)
        ser.sendall(b"cat /clipboard/text\n")
        log = wait_for(ser, b"world", 5.0)
        if b"world" not in log:
            print("FAIL: second write did not replace the payload")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        # The "Hello" of the first write must NOT be visible
        # in the cat output that follows the second write.
        # Slice from the last 'cat' invocation forward.
        tail = log.split(b"cat /clipboard/text")[-1]
        if b"Hello chapter 140" in tail:
            print("FAIL: stale bytes from the first write leaked through")
            print(tail[:500].decode("ascii", "replace"))
            return 1
        print("PASS: O_TRUNC reset the payload (no stale 'Hello' bytes)")

        # 6. Clear via `:` (the shell null command, which still
        #    triggers O_TRUNC on the redirection target).  After
        #    this, cat must NOT return "world" -- any other
        #    serial-log noise is fine, but if the payload from
        #    step 5 is still readable then the clear didn't land.
        ser.sendall(b": > /clipboard/text\n")
        log = wait_for(ser, b"$ ", 5.0)
        ser.sendall(b"cat /clipboard/text\n")
        log = wait_for(ser, b"$ ", 5.0)
        tail = log.split(b"cat /clipboard/text")[-1]
        if b"world" in tail:
            print("FAIL: clear left the 'world' payload in /clipboard/text:")
            print(tail[:400].decode("ascii", "replace"))
            return 1
        if b"Hello chapter 140" in tail:
            print("FAIL: clear resurrected the 'Hello' payload:")
            print(tail[:400].decode("ascii", "replace"))
            return 1
        print("PASS: : > /clipboard/text cleared the payload")

        print("\nCHAPTER 114 (clipboard as userfs): ALL TESTS PASSED")
        return 0
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()


if __name__ == "__main__":
    sys.exit(main())
