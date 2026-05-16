#!/usr/bin/env python3
"""scripts/test_fsync.py — chapter 82 durability test.

Validates that the OSFS-2 write-back cache (chapter 82) has both
required durability properties:

  Test A.  fsync forces a write to survive a hard reboot.
           Boot QEMU, write a file, run /bin/sync (which calls
           fsync), KILL QEMU (no graceful shutdown).  Boot
           again with the SAME data.img.  The file must still
           be present and intact.

  Test B.  The 5-second background flusher catches lazy writers.
           Boot QEMU, write a DIFFERENT file, do NOT call sync,
           wait > 5 s for the flusher, KILL QEMU.  Boot again.
           The file must still be present.

  Test C.  (Negative control) Without sync AND without waiting
           for the flusher, the file is lost.  Boot QEMU, write
           a third file, KILL QEMU within ~1 s.  Boot again \u2014
           the file is gone.  This proves Test A's success isn't
           a fluke from "all writes are always durable anyway."

Each "boot" reuses build/data.img across runs; we DO mkfs once
at the very start so the first boot starts from a clean slate.

Why a separate script?  test_osfs2.py only does single-boot
verification; durability is fundamentally a multi-boot property.

This test deliberately uses subprocess.kill (SIGKILL) instead of
terminate (SIGTERM) so QEMU has zero chance to flush anything on
shutdown \u2014 it's the closest we can get to "yank the power cord."
"""
import os, select, signal, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERIAL_SOCK = "/tmp/osdev-serial-fsync.sock"
DATA_IMG = f"{ROOT}/build/data.img"

PROMPT = b"$ "

def cleanup_sock():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass

def reformat_data():
    """Reset build/data.img to a freshly-mkfs'd OSFS-2 image."""
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
    """SIGKILL the VM \u2014 no flush opportunity, simulates power loss."""
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

def check(label, ok, why=""):
    if ok:
        print(f"PASS: {label}")
        return True
    print(f"FAIL: {label}{(': ' + why) if why else ''}")
    return False

# --------------------------------------------------------------------
# High-level boot helpers.
# --------------------------------------------------------------------

def boot_to_shell():
    q = boot()
    ser = conn()
    out = wait_for(ser, PROMPT, 20.0)
    if PROMPT not in out:
        hard_kill(q)
        raise RuntimeError("shell prompt never appeared")
    return q, ser

# --------------------------------------------------------------------
# The three sub-tests.
# --------------------------------------------------------------------

def test_a_fsync_persistence():
    """Write + sync + hard-kill -> reboot -> file present."""
    payload = b"FSYNC-A-PERSISTENT"
    # Boot 1: write the file with explicit sync.
    q, ser = boot_to_shell()
    try:
        send_cmd(ser, "echo " + payload.decode() + " > /data/persist_a")
        out = send_cmd(ser, "/bin/sync", timeout=10.0)
        # sync should print nothing on success; check the prompt came
        # back without a "sync:" error line.
        if b"sync:" in out:
            return False, "sync command failed: " + out.decode(errors="replace")
    finally:
        hard_kill(q)

    # Boot 2: read it back.  Cache is gone (kernel restarted) so this
    # must come from disk.
    q, ser = boot_to_shell()
    try:
        out = send_cmd(ser, "cat /data/persist_a")
        ok = payload in out
        return ok, ("payload missing after reboot" if not ok else "")
    finally:
        send_cmd(ser, "rm /data/persist_a", timeout=5.0)
        # After cleanup we want it durable too \u2014 sync so next test
        # starts from a known state.
        send_cmd(ser, "/bin/sync", timeout=5.0)
        hard_kill(q)

def test_b_background_flusher():
    """Write WITHOUT sync, wait >5 s, hard-kill -> reboot -> file present."""
    payload = b"FSYNC-B-FLUSHER"
    q, ser = boot_to_shell()
    try:
        send_cmd(ser, "echo " + payload.decode() + " > /data/persist_b")
        # Do NOT call sync.  Wait for the 5-second background flusher
        # to run.  Use 8 s to give plenty of margin (the flusher's
        # phase is unknown relative to our write).
        time.sleep(8.0)
    finally:
        hard_kill(q)

    q, ser = boot_to_shell()
    try:
        out = send_cmd(ser, "cat /data/persist_b")
        ok = payload in out
        return ok, ("payload missing after flusher window" if not ok else "")
    finally:
        send_cmd(ser, "rm /data/persist_b", timeout=5.0)
        send_cmd(ser, "/bin/sync", timeout=5.0)
        hard_kill(q)

def test_c_lost_without_sync():
    """Write WITHOUT sync, kill IMMEDIATELY -> reboot -> file gone.

    Negative control: proves the cache really is write-back.  If this
    test's file SURVIVES, then either we accidentally implemented a
    write-through cache (defeating the point) or fsync on every write
    snuck in somewhere.
    """
    payload = b"FSYNC-C-LOST"
    q, ser = boot_to_shell()
    try:
        send_cmd(ser, "echo " + payload.decode() + " > /data/lost_c")
        # IMMEDIATELY kill \u2014 no sleep.  The flusher fires every 5 s
        # and this whole sequence runs in well under that.
    finally:
        hard_kill(q)

    q, ser = boot_to_shell()
    try:
        out = send_cmd(ser, "cat /data/lost_c")
        # cat should fail with ENOENT-like behaviour and definitely
        # NOT print our payload.
        ok = payload not in out
        return ok, ("payload survived without sync (cache too eager)"
                    if not ok else "")
    finally:
        send_cmd(ser, "/bin/sync", timeout=5.0)
        hard_kill(q)

# --------------------------------------------------------------------

def main():
    print("[chapter 82] reformatting build/data.img ...")
    reformat_data()

    failed = []

    print("\n[chapter 82] Test A: fsync makes writes durable across hard reboot")
    ok, why = test_a_fsync_persistence()
    if not check("file written + fsync'd survives hard kill", ok, why):
        failed.append("A")

    print("\n[chapter 82] Test B: background flusher catches un-sync'd writes")
    ok, why = test_b_background_flusher()
    if not check("file un-sync'd but >5 s old survives hard kill", ok, why):
        failed.append("B")

    print("\n[chapter 82] Test C: writes without sync OR flush window are lost")
    ok, why = test_c_lost_without_sync()
    if not check("file un-sync'd and immediately killed is GONE", ok, why):
        failed.append("C")

    if failed:
        print(f"\nCHAPTER 82: {len(failed)} test(s) FAILED: {failed}")
        return 1
    print("\nCHAPTER 82: ALL TESTS PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
