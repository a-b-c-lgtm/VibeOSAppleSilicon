#!/usr/bin/env python3
import os, select, socket, subprocess, time
SOCK="/tmp/osdev-debug.sock"
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
try: os.unlink(SOCK)
except FileNotFoundError: pass
q=subprocess.Popen([
    "qemu-system-aarch64",
    "-M","virt,gic-version=3","-cpu","host","-accel","hvf",
    "-m","8G", "-smp", "2","-display","none",
    "-serial",f"unix:{SOCK},server,nowait",
    "-global","virtio-mmio.force-legacy=off",
    "-device",f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive",f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive",  f"if=none,file={ROOT}/build/data.img,format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev","user,id=n0",
    "-device","virtio-net-device,netdev=n0",
    "-kernel",f"{ROOT}/build/kernel.elf",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
deadline=time.time()+5
s=None
while time.time()<deadline:
    if os.path.exists(SOCK):
        s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        s.connect(SOCK); break
    time.sleep(0.05)
buf=b""; end=time.time()+60
while time.time()<end:
    r,_,_=select.select([s],[],[],0.5)
    if r:
        c=s.recv(8192)
        if not c: break
        buf+=c
print("LEN:",len(buf))
print(buf[-3500:].decode('ascii','replace'))
q.terminate()
try: q.wait(timeout=3)
except Exception: q.kill()
