#!/usr/bin/env python3
"""scripts/_dbg_data_mount.py — show what's in /data after seeding."""
import socket, subprocess, os, time, select, signal, sys
ROOT = "/Users/seusher/Desktop/osdev"
SOCK = "/tmp/osdev-data-dbg.sock"
try: os.unlink(SOCK)
except FileNotFoundError: pass

# Reseed data.img with a known file.
subprocess.check_call([
    "python3", f"{ROOT}/scripts/mkosfs2.py", f"{ROOT}/build/data.img",
    "cross_hello=/etc/hosts",
])

q = subprocess.Popen([
    "qemu-system-aarch64","-M","virt,gic-version=3","-cpu","host","-accel","hvf",
    "-m","8G","-smp","2","-display","none",
    "-serial",f"unix:{SOCK},server,nowait",
    "-global","virtio-mmio.force-legacy=off",
    "-device",f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive",f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
    "-device","virtio-blk-device,drive=hd0",
    "-drive",f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
    "-device","virtio-blk-device,drive=hd1",
    "-kernel",f"{ROOT}/build/kernel.elf",
], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
deadline = time.time() + 5
while time.time() < deadline and not os.path.exists(SOCK):
    time.sleep(0.05)
sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); sk.connect(SOCK)

def w(needle, to=10):
    d = time.time()+to; b = b""
    while time.time() < d:
        r,_,_ = select.select([sk],[],[],0.1)
        if r:
            c = sk.recv(65536)
            if not c: break
            b += c
            if needle in b: return b
    return b

def s(cmd, to=10):
    sk.sendall(cmd.encode()+b"\n")
    return w(b"/$ ", to)

try:
    w(b"/$ ", 25)
    print("=== ls /data ===")
    print(s("ls /data").decode("utf-8","replace"))
    print("=== ls / ===")
    print(s("ls /").decode("utf-8","replace"))
    print("=== first 80 bytes of /data/cross_hello ===")
    out = s("cat /data/cross_hello")
    print(out[:300].decode("utf-8","replace"))
finally:
    try: sk.close()
    except: pass
    try: q.send_signal(signal.SIGKILL); q.wait(timeout=3)
    except: pass
    try: os.unlink(SOCK)
    except: pass
    # Restore empty data.img
    subprocess.check_call([
        "python3", f"{ROOT}/scripts/mkosfs2.py", f"{ROOT}/build/data.img",
    ])
