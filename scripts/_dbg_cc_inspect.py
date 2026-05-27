#!/usr/bin/env python3
"""scripts/_dbg_cc_inspect.py — capture cc emit + linked binary for chapter 157 diagnosis."""
import os, socket, subprocess, time, select, signal, sys

ROOT = "/Users/seusher/Desktop/osdev"
SOCK = "/tmp/osdev-cc-inspect.sock"
try: os.unlink(SOCK)
except FileNotFoundError: pass
subprocess.check_call(
    ["python3", f"{ROOT}/scripts/mkosfs2.py", f"{ROOT}/build/data.img"],
    stdout=subprocess.DEVNULL,
)
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
time.sleep(2)
deadline = time.time() + 5
while time.time() < deadline and not os.path.exists(SOCK):
    time.sleep(0.05)
sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); sk.connect(SOCK)

def drain(deadline):
    out=b""
    while time.time()<deadline:
        r,_,_=select.select([sk],[],[],0.1)
        if r:
            c=sk.recv(8192)
            if not c: break
            out+=c
        elif out: break
    return out

def wait_for(needle, t=15):
    d=time.time()+t; buf=b""
    while time.time()<d:
        buf+=drain(time.time()+0.4)
        if needle in buf: return buf
    return buf

def send(cmd, t=15):
    sk.sendall(cmd.encode()+b"\n")
    return wait_for(b"/$ ", t)

try:
    wait_for(b"/$ ", 25)
    send("rm /tmp/hello.c 2>/dev/null; true")
    for line in [
        'int main(void) {',
        '    printf("hi\\n");',
        '    return 0;',
        '}',
    ]:
        send(f"echo '{line}' >> /tmp/hello.c")
    print("=== cat /tmp/hello.c ===")
    print(send("cat /tmp/hello.c").decode("utf-8","replace"))
    print("=== /bin/cc -S /tmp/hello.c -o /tmp/hello.s ===")
    print(send("/bin/cc -S /tmp/hello.c -o /tmp/hello.s", t=30).decode("utf-8","replace"))
    print("=== cat /tmp/hello.s ===")
    print(send("cat /tmp/hello.s").decode("utf-8","replace"))
    print("=== /bin/cc /tmp/hello.c -o /tmp/hello ===")
    print(send("/bin/cc /tmp/hello.c -o /tmp/hello", t=30).decode("utf-8","replace"))
    print("=== od -tx1 -N64 /tmp/hello.cc.o ===")
    print(send("od -An -tx1 -N64 /tmp/hello.cc.o").decode("utf-8","replace"))
    print("=== od -tx1 -N96 /tmp/hello (header) ===")
    print(send("od -An -tx1 -N96 /tmp/hello").decode("utf-8","replace"))
    print("=== od -tx1 -j4096 -N96 /tmp/hello (text @0x1000) ===")
    print(send("od -An -tx1 -j4096 -N96 /tmp/hello").decode("utf-8","replace"))
    print("=== /tmp/hello (run) ===")
    print(send("/tmp/hello", t=15).decode("utf-8","replace"))
finally:
    try: sk.close()
    except: pass
    try: q.send_signal(signal.SIGKILL); q.wait(timeout=3)
    except: pass
    try: os.unlink(SOCK)
    except: pass
