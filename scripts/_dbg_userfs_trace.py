#!/usr/bin/env python3
"""SMP=2 verbose trace of the userfs deadlock case."""
import os, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = '/tmp/osdev-userfs-debug4.sock'
DATA_IMG = os.path.join(ROOT, 'build', 'data.img')

try: os.unlink(SERIAL_SOCK)
except FileNotFoundError: pass

subprocess.check_call(
    ['python3', os.path.join(ROOT, 'scripts', 'mkosfs2.py'), DATA_IMG],
    stdout=subprocess.DEVNULL)

q = subprocess.Popen([
    'qemu-system-aarch64',
    '-M', 'virt,gic-version=3', '-cpu', 'host', '-accel', 'hvf',
    '-m', '8G', '-smp', '2',
    '-display', 'none',
    '-serial', f'unix:{SERIAL_SOCK},server,nowait',
    '-global', 'virtio-mmio.force-legacy=off',
    '-device', f'loader,file={ROOT}/assets/virt.dtb,addr=0x44000000',
    '-drive', f'if=none,file={ROOT}/build/disk.img,format=raw,id=hd0',
    '-device', 'virtio-blk-device,drive=hd0',
    '-drive', f'if=none,file={DATA_IMG},format=raw,id=hd1',
    '-device', 'virtio-blk-device,drive=hd1',
    '-kernel', os.path.join(ROOT, 'build', 'kernel.elf'),
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(SERIAL_SOCK)

def drain(s, t):
    out = b''
    deadline = time.time() + t
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.3)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
        elif out:
            break
    return out

try:
    print('=== BOOT ===')
    print(drain(s, 15.0).decode('utf-8','replace'))
    print('=== sending: /bin/echofs & ===')
    s.sendall(b'/bin/echofs &\n')
    print(drain(s, 4.0).decode('utf-8','replace'))
    print('=== sending: /bin/echo HELLO > /echo/buf ===')
    s.sendall(b'/bin/echo HELLO > /echo/buf\n')
    print(drain(s, 8.0).decode('utf-8','replace'))
    print('=== sending: /bin/cat /echo/buf ===')
    s.sendall(b'/bin/cat /echo/buf\n')
    print(drain(s, 5.0).decode('utf-8','replace'))
finally:
    s.close()
    q.kill()
    try: os.unlink(SERIAL_SOCK)
    except: pass
