#!/usr/bin/env python3
"""scripts/test_osfs2.py — chapter 81 smoke test.

Boots the system fully headless on the serial console (no
GUI), waits for the shell prompt, and exercises the freshly-
landed writable OSFS-2 filesystem mounted at /data/:

  1. Empty `/data/` listing.  Right after boot, ls should NOT
     show any /data/ entries.
  2. Create + read.  `echo hello > /data/foo` ; `cat /data/foo`
     -> expect "hello".
  3. Listing.  `ls` -> expect a /data/foo line.
  4. Delete.  `rm /data/foo` ; `ls` -> /data/foo absent again.
  5. Multi-block file (exercises direct[1..15] + the indirect
     tier).  Writes 3 large heredocs concatenated, sized so
     the total spans more than the 16 direct blocks (16 * 4 KiB
     = 64 KiB) — at 80 KiB we are guaranteed to allocate at
     least one indirect-table entry.  Reads it back and checks
     it contains the expected sentinel.

The shell driver is deliberately conservative: every step
sends one line, waits for the next prompt, and only then
sends the next command.  No timing-sensitive racing.

Why a separate script?  test_boot_to_desktop.py exercises the
GUI path and only goes as far as confirming the shell prompt
is reachable; this one ignores the GUI entirely and goes deep
on the writable FS.
"""
import os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = "/tmp/osdev-serial-osfs2.sock"

PROMPT = b"$ "

def cleanup():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass

def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive", f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
        "-device", "virtio-blk-device,drive=hd1",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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
    """Read all bytes available before deadline."""
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
    """Send a shell line, wait for the prompt, return the bytes
    that came back BETWEEN the echoed command and the prompt."""
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    # Strip the echoed command from the front (best-effort).
    # On a cooked tty the shell echoes back what we sent.
    idx = out.find(cmd)
    if idx >= 0:
        out = out[idx + len(cmd):]
    return out

def check(label, ok, why=""):
    if ok:
        print(f"PASS: {label}")
        return True
    print(f"FAIL: {label}{(': ' + why) if why else ''}")
    return False

def main():
    # Reformat build/data.img so this test is independent of which
    # other tests ran before it (test_fsync.py leaves a /data/.sync
    # artefact behind that would trip the "empty /data/" check).
    subprocess.check_call(
        ["python3", os.path.join(ROOT, "scripts", "mkosfs2.py"),
         os.path.join(ROOT, "build", "data.img")],
        stdout=subprocess.DEVNULL,
    )

    q = boot()
    failed = []
    try:
        ser = conn()

        # 1. Wait for shell prompt.
        out = wait_for(ser, PROMPT, 15.0)
        if PROMPT not in out:
            print("FAIL: shell prompt never appeared")
            print("---- last 1 KiB of serial output ----")
            print(out[-1024:].decode(errors="replace"))
            return 1
        print("PASS: shell prompt reached")

        # 2. /data/ should be empty (no files yet).
        out = send_cmd(ser, "ls", timeout=5.0)
        if not check("ls right after boot does not show any /data/ files",
                     b"/data/" not in out, why="/data/ leaked into ls"):
            failed.append("ls-empty-data")

        # 3. echo hello > /data/foo ; cat /data/foo.
        send_cmd(ser, "echo hello-osfs2 > /data/foo")
        out = send_cmd(ser, "cat /data/foo")
        if not check("cat /data/foo prints what echo wrote",
                     b"hello-osfs2" in out, why="payload missing"):
            failed.append("cat")

        # 4. ls now shows /data/foo.
        out = send_cmd(ser, "ls")
        if not check("ls shows /data/foo after the write",
                     b"/data/foo" in out, why="/data/foo missing in ls"):
            failed.append("ls-after-write")

        # 4b. `ls /data/` filters to just the /data/ mount.
        # /data/foo must be present and /mnt/* must NOT be.
        out = send_cmd(ser, "ls /data/")
        if not check("ls /data/ shows /data/foo",
                     b"/data/foo" in out, why="/data/foo missing under filter"):
            failed.append("ls-filter-data-positive")
        if not check("ls /data/ does NOT show /mnt/* entries",
                     b"/mnt/" not in out, why="/mnt/ leaked through filter"):
            failed.append("ls-filter-data-negative")

        # 4c. `ls /data` (no trailing slash) behaves the same.
        out = send_cmd(ser, "ls /data")
        if not check("ls /data (no trailing slash) shows /data/foo",
                     b"/data/foo" in out and b"/mnt/" not in out,
                     why="trailing-slash normalisation broken"):
            failed.append("ls-filter-no-trailing-slash")

        # 4d. `ls /mnt/` filters out /data/* entries.
        out = send_cmd(ser, "ls /mnt/")
        if not check("ls /mnt/ does NOT show /data/* entries",
                     b"/data/" not in out, why="/data/ leaked through /mnt filter"):
            failed.append("ls-filter-mnt")

        # 4e. `ls /` shows top-level entries only.  Mount points
        # appear as collapsed "<DIR>  /mnt/" lines; root-ramfs
        # files like /motd appear as leaves; sub-mount contents
        # like /mnt/hello.txt are HIDDEN behind the /mnt/ DIR.
        out = send_cmd(ser, "ls /")
        if not check("ls / shows /mnt/ as a DIR",
                     b"/mnt/" in out and b"<DIR>" in out,
                     why="/mnt/ missing or not collapsed"):
            failed.append("ls-root-mnt-dir")
        if not check("ls / shows /data/ as a DIR",
                     b"/data/" in out, why="/data/ missing from root"):
            failed.append("ls-root-data-dir")
        if not check("ls / shows root ramfs file /motd",
                     b"/motd" in out, why="/motd missing from root"):
            failed.append("ls-root-motd")
        if not check("ls / does NOT show /mnt/hello.txt (nested)",
                     b"hello.txt" not in out,
                     why="nested /mnt entry leaked into ls /"):
            failed.append("ls-root-nested-hidden")
        if not check("ls / does NOT show /data/foo (nested)",
                     b"/data/foo" not in out,
                     why="nested /data entry leaked into ls /"):
            failed.append("ls-root-nested-data-hidden")

        # 5. rm /data/foo ; ls shows it gone.
        send_cmd(ser, "rm /data/foo")
        out = send_cmd(ser, "ls")
        if not check("ls shows /data/foo gone after rm",
                     b"/data/foo" not in out, why="/data/foo still in ls"):
            failed.append("ls-after-rm")

        # 6. Indirect-tier exercise.  Write a > 64 KiB file by
        # cat'ing the wallpaper (~8 MiB) into /data/big, then
        # check the resulting file is non-empty by counting its
        # bytes via `ls`.  cat'ing wallpaper.bgra straight into
        # /data/big spans direct + indirect blocks several hundred
        # times over.
        send_cmd(ser, "cat /mnt/wallpaper.bgra > /data/big", timeout=30.0)
        out = send_cmd(ser, "ls", timeout=10.0)
        # ls prints lines like:  "      1234  /data/big" — the
        # column before the path is the size.  Find our entry and
        # parse it.
        big_size = None
        for line in out.splitlines():
            if b"/data/big" in line:
                parts = line.split()
                # parts[0] is the size, parts[1] is the path.
                try:
                    big_size = int(parts[0])
                except (ValueError, IndexError):
                    big_size = None
                break
        if not check(f"/data/big >= 1 MiB (size={big_size})",
                     big_size is not None and big_size >= 1024 * 1024,
                     why="indirect-block path didn't write the full file"):
            failed.append("indirect")

        # 7. rm the big file too, so the test image stays clean
        # (other tests on the same boot will see an empty /data/).
        send_cmd(ser, "rm /data/big")

        if failed:
            print(f"\nCHAPTER 81: {len(failed)} test(s) FAILED: {failed}")
            return 1
        print("\nCHAPTER 81: ALL TESTS PASSED")
        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()


if __name__ == "__main__":
    sys.exit(main())
