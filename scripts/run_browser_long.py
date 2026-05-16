#!/usr/bin/env python3
"""Like run_browser_cmd.py but tuned for slow paint dumps:
   - boots the same headless QEMU
   - waits up to 240 s wall / 90 s of idle for the command to finish
   - ALSO captures stdout while the browser is producing paint lines
     (those don't trigger our idle timeout because they keep arriving)
   - prints everything written to serial after the prompt.

Usage:  python3 scripts/run_browser_long.py 'browser --paint http://...'
"""
import os, select, socket, subprocess, sys, time

if len(sys.argv) < 2:
    print("usage: run_browser_long.py <command-line>"); sys.exit(1)
CMD = " ".join(sys.argv[1:])

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOCK = "/tmp/osdev-runlong.sock"
try: os.unlink(SOCK)
except FileNotFoundError: pass

q = subprocess.Popen([
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-display", "none",
    "-serial", f"unix:{SOCK},server,nowait",
    "-global", "virtio-mmio.force-legacy=off",
    "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive",  f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev", "user,id=n0",
    "-device", "virtio-net-device,netdev=n0",
    "-kernel", f"{ROOT}/build/kernel.elf",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

deadline = time.time() + 15
s = None
while time.time() < deadline:
    if os.path.exists(SOCK):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK); break
        except OSError: s = None
    time.sleep(0.1)

if s is None:
    print("ERR: could not connect to qemu serial socket"); q.terminate(); sys.exit(2)

buf = b""
def drain_until(needle, secs):
    global buf
    t = time.time() + secs
    while time.time() < t:
        r,_,_ = select.select([s],[],[],0.25)
        if not r: continue
        try: c = s.recv(16384)
        except OSError: break
        if not c: break
        buf += c
        if needle in buf: return True
    return False

drain_until(b"/$ ", 30)
prompt_at = len(buf)
s.sendall(CMD.encode() + b"\n")

# Wait until either:
#  - we've been idle for IDLE_S seconds (no new bytes), OR
#  - we hit WALL_S seconds total wall time, OR
#  - we see a fresh "/$ " prompt (shell returned)
WALL_S = 240.0
IDLE_S = 90.0
end  = time.time() + WALL_S
last = time.time()
while time.time() < end:
    r,_,_ = select.select([s],[],[],0.5)
    if r:
        try: c = s.recv(16384)
        except OSError: break
        if not c: break
        buf += c
        last = time.time()
        # Did the shell prompt come back AFTER we sent the command?
        if buf.find(b"/$ ", prompt_at + 1) != -1:
            # Give it a beat in case more data follows.
            time.sleep(0.5)
            r2,_,_ = select.select([s],[],[],0.1)
            if r2:
                try: buf += s.recv(16384)
                except OSError: pass
            break
    elif time.time() - last > IDLE_S:
        break

q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()
try: os.unlink(SOCK)
except Exception: pass

sys.stdout.buffer.write(buf)
