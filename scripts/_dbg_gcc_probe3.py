#!/usr/bin/env python3
"""scripts/_dbg_gcc_probe3.py -- diagnose why xgcc -S and -c
emit "linker input file unused" with empty progname/filename.

Verbose mode (-v) reveals which cc1/as/ld subprocess xgcc spawns
(or skips).  Also lists /bin, /lib, and shows argv printout via a
small echo-style probe.
"""
import os, select, signal, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = "/tmp/osdev-gcc-probe3.sock"
DATA = f"{ROOT}/build/data.img"
PROMPT = b"$ "


def cleanup_sock():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass


def boot():
    cleanup_sock()
    subprocess.check_call(["python3", f"{ROOT}/scripts/mkosfs2.py", DATA],
                          stdout=subprocess.DEVNULL)
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M","virt,gic-version=3","-cpu","host","-accel","hvf",
        "-m","8G","-smp","2","-display","none",
        "-serial",f"unix:{SOCK},server,nowait",
        "-global","virtio-mmio.force-legacy=off",
        "-device",f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-drive",f"if=none,file={ROOT}/build/disk.img,format=raw,id=hd0",
        "-device","virtio-blk-device,drive=hd0",
        "-drive",f"if=none,file={DATA},format=raw,id=hd1",
        "-device","virtio-blk-device,drive=hd1",
        "-kernel",f"{ROOT}/build/kernel.elf",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn():
    deadline = time.time() + 5
    while time.time() < deadline:
        if os.path.exists(SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no serial sock")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r,_,_ = select.select([s],[],[],0.1)
        if r:
            c = s.recv(65536)
            if not c: break
            out += c
        elif out: break
    return out


def wait_for(s, needle, timeout=10.0):
    if isinstance(needle, str): needle = needle.encode()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def send_cmd(s, cmd, timeout=60.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0: out = out[idx + len(cmd):]
    return out


def main():
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=30.0)

        # Stage a hello.c
        send_cmd(s, "rm /tmp/hello.c", timeout=10.0)
        send_cmd(s, "echo 'void _start(void) {' >> /tmp/hello.c")
        send_cmd(s, "echo '    register long n asm(\"x8\") = 2;' >> /tmp/hello.c")
        send_cmd(s, "echo '    register long c asm(\"x0\") = 42;' >> /tmp/hello.c")
        send_cmd(s, "echo '    asm volatile(\"svc #0\" :: \"r\"(n), \"r\"(c));' >> /tmp/hello.c")
        send_cmd(s, "echo '}' >> /tmp/hello.c")

        for cmd in [
            "ls /bin",
            "ls /lib",
            "cat /tmp/hello.c",
            "/bin/xgcc -v -S -o /tmp/hello.s /tmp/hello.c",
            "ls /tmp",
            "cat /tmp/hello.s",
            "/bin/xgcc -v -c -o /tmp/hello.o /tmp/hello.c",
            "ls /tmp",
            "/bin/xgcc -v /tmp/hello.c",   # default behaviour, no -c/-S
        ]:
            print(f"\n========= {cmd}")
            out = send_cmd(s, cmd, timeout=120.0)
            print(out.decode("utf-8","replace"))
            print(f"=========")

    finally:
        try: q.send_signal(signal.SIGKILL); q.wait(timeout=3)
        except Exception: pass
        cleanup_sock()


if __name__ == "__main__":
    main()
