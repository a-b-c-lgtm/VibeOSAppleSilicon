#!/usr/bin/env python3
"""scripts/_dbg_boot_log.py — chapter 82 ad-hoc boot log capture.

Used during the chapter-82 reaper-deadlock debug session to grab
the full serial output of a single boot.  When test_osfs2.py
started failing with "shell prompt never appeared" after we
added the flusher thread, this script was the diagnostic that
revealed boot was hanging at "[busy-B] done" — the line right
before preemption_demo()'s thread_wait(NULL) reaper loop would
block on the never-exiting flusher.

Keep for reference (see /memories/kernel-thread-lifetime-reaper-trap.md
and book chapter 82 "The reaper deadlock").
"""
import socket, subprocess, time, os, signal, sys
sock = '/tmp/osdev-diag.sock'
try: os.unlink(sock)
except FileNotFoundError: pass
ROOT = os.path.abspath(os.path.dirname(__file__) + "/..")
q = subprocess.Popen(['qemu-system-aarch64',
  '-M','virt,gic-version=3','-cpu','host','-accel','hvf','-m','8G','-display','none',
  '-serial', f'unix:{sock},server,nowait',
  '-global','virtio-mmio.force-legacy=off',
  '-device', f'loader,file={ROOT}/assets/virt.dtb,addr=0x44000000',
  '-drive', f'if=none,file={ROOT}/build/disk.img,format=raw,id=hd0','-device','virtio-blk-device,drive=hd0',
  '-drive', f'if=none,file={ROOT}/build/data.img,format=raw,id=hd1','-device','virtio-blk-device,drive=hd1',
  '-kernel', f'{ROOT}/build/kernel.elf'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
deadline = time.time() + 5
while time.time() < deadline:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock); break
    except OSError:
        time.sleep(0.1)
s.settimeout(0.5)
buf = b''
start = time.time()
runtime = float(sys.argv[1]) if len(sys.argv) > 1 else 18.0
while time.time() - start < runtime:
    try:
        c = s.recv(4096)
        if not c: break
        buf += c
    except OSError: pass
q.send_signal(signal.SIGKILL); q.wait()
try: os.unlink(sock)
except FileNotFoundError: pass
print('=== TOTAL BYTES ===', len(buf))
print('=== LAST 6 KiB ===')
print(buf[-6144:].decode(errors='replace'))
