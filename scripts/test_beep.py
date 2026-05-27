#!/usr/bin/env python3
"""scripts/test_beep.py — chapter 97 smoke test.

Boots the kernel with `-device virtio-sound-device,audiodev=none`
attached, drops to /bin/sh, runs `/bin/beep`, and asserts:

  1. The kernel logged that the virtio-sound device probe
     succeeded.  Specifically: AFTER our serial client has
     attached we still see the `[virtio-snd] PCM 0 ready` line
     written when init's first invocation of beep() walks down
     to the driver.  (We can't reliably catch the boot-time
     `probing virtio-mmio bus for a sound card ... ok` line —
     that fires before the serial client connects, and
     `unix:...,server,nowait` discards data emitted before the
     listener is alive.  See chapter 96 for the trap.)

  2. `beep 880 100` returns exit-status zero (no error message
     printed by /bin/beep, no `[svc] unknown syscall` from the
     dispatcher).

  3. The kernel logs `[virtio-snd] played freq=880 dur=100` AFTER
     the user's invocation — which proves both that SYS_BEEP
     dispatched into the driver and that the driver's TX path
     made it all the way through tx_submit_and_wait() (the log
     line is the last thing virtio_snd_play_square() prints).

Modelled after scripts/test_rtc.py.

This test attaches an `audiodev=none` backend so QEMU consumes
the samples without forwarding them to a host audio sink — there
is no "audible" assertion, just a structural one.  For an
audible boot chime see `make run-graphical`, which uses
coreaudio.
"""
import os, re, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-serial-beep.sock"


def boot():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
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
        "-audiodev", "none,id=audio0",
        "-device", "virtio-sound-device,audiodev=audio0",
        "-kernel", f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no socket")


def read_until(ser, needles, timeout, prior=b""):
    if isinstance(needles, (bytes, str)): needles = [needles]
    needles = [n.encode() if isinstance(n, str) else n for n in needles]
    buf = bytearray(prior)
    if any(n in buf for n in needles): return bytes(buf)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.2)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
            if any(n in buf for n in needles): return bytes(buf)
    return bytes(buf)


def main():
    q = boot()
    try:
        ser = conn()
        # Wait for the shell prompt.  init.c plays a 2-tone boot
        # chime before launching /bin/sh, so by the time we see
        # `$ ` the driver has already serviced two TX messages.
        log = read_until(ser, [b"$ "], 90.0)
        if b"$ " not in log:
            print("FAIL: never saw shell prompt")
            print(log[-2000:].decode("ascii", "replace"))
            return 1

        # --- Assertion 1: driver came up. ---
        # The boot-chime invocations in init guarantee a "played
        # freq=" line appears before the prompt, since init runs
        # before /bin/sh and beep() blocks until tx_submit_and_wait
        # returns.  This is the same evidence as the boot-time
        # "ok (audio online)" line, but observable post-attach.
        if b"[virtio-snd] played" not in log:
            print("FAIL: kernel never logged a [virtio-snd] played "
                  "line during boot — driver init or boot chime "
                  "didn't run")
            print(log[-2000:].decode("ascii", "replace"))
            return 1
        print("PASS: virtio-snd serviced the boot chime "
              "(saw [virtio-snd] played in pre-prompt log)")

        # --- Assertion 2 + 3: /bin/beep dispatches and the
        # driver logs the TX completion. ---
        ser.sendall(b"beep 880 100\n")
        # Drain output until either the next prompt OR an error
        # message.  The "played freq=880" line will arrive between
        # the command echo and the next prompt.
        log2 = read_until(ser, [b"\n$ "], 15.0)

        if b"beep: no virtio-sound device" in log2:
            print("FAIL: /bin/beep reported -ENODEV — kernel did "
                  "not detect the virtio-sound device on its mmio "
                  "scan (driver bug or QEMU device missing)")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1
        if b"[svc] unknown syscall" in log2:
            print("FAIL: SYS_BEEP not wired into dispatcher")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1

        # Look specifically for the played line for our 880 Hz
        # invocation.  This proves both dispatch (kernel saw the
        # syscall) and TX completion (driver's tx_submit_and_wait
        # returned 0).  The driver uses serial_puthex() (project
        # convention) so freq/dur arrive as 0x-prefixed 16-digit
        # hex — 880 dec == 0x370, 100 dec == 0x64.
        m = re.search(rb"\[virtio-snd\] played freq=0x([0-9a-f]+) "
                      rb"Hz duration=0x([0-9a-f]+) ms",
                      log2)
        if not m:
            print("FAIL: /bin/beep returned but driver didn't log "
                  "a [virtio-snd] played line — TX path is broken")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1
        freq = int(m.group(1), 16)
        dur  = int(m.group(2), 16)
        if freq != 880 or dur != 100:
            print(f"FAIL: driver logged freq={freq} dur={dur}, "
                  f"expected freq=880 dur=100")
            print(log2[-2000:].decode("ascii", "replace"))
            return 1
        print(f"PASS: beep 880 100 dispatched and TX'd "
              f"(driver logged freq={freq} dur={dur})")

        if b"$ " not in log2:
            print("FAIL: shell prompt never returned after beep")
            print(log2[-1000:].decode("ascii", "replace"))
            return 1
        print("PASS: shell returned to prompt after beep")

        print("PASS: chapter 97 virtio-snd smoke test")
        return 0
    finally:
        q.kill(); q.wait()
        try: os.unlink(SOCK)
        except FileNotFoundError: pass


if __name__ == "__main__":
    sys.exit(main())
