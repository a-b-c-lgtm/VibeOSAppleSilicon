#!/usr/bin/env python3
"""scripts/_dbg_gcc_probe.py -- chapter 186 gcc abort triage.

After /bin/gcc --version returned exit 134 (SIGABRT-from-abort)
with no output, this probe runs a battery of progressively-
simpler /bin/xgcc invocations to figure out where the call
chain dies:

  - /bin/xgcc -dumpversion       (does *anything* print?)
  - /bin/xgcc -dumpmachine       (does spec lookup work?)
  - /bin/xgcc                    (no args -> "no input files")
  - /bin/strace /bin/xgcc -dumpversion (last 200 syscalls)
  - /bin/gcc --version           (with wrapper)

Idle-loops the shell after each one so transient buffering
doesn't eat output.  Discardable: kept as a debug script per
debug-scripts-policy.
"""
import os, select, signal, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = "/tmp/osdev-gcc-probe.sock"
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

        for cmd in [
            "ls /bin",
            "/bin/xgcc -dumpversion; echo END $?",
            "/bin/xgcc -dumpmachine; echo END $?",
            "/bin/xgcc -print-search-dirs; echo END $?",
            "/bin/xgcc; echo END $?",
            "/bin/xgcc --help; echo END $?",
            "/bin/gcc -dumpversion; echo END $?",
            "/bin/strace /bin/xgcc -dumpversion; echo END $?",
        ]:
            print(f"\n========= {cmd}")
            out = send_cmd(s, cmd, timeout=90.0)
            print(out.decode("utf-8","replace"))
            print(f"=========")

    finally:
        try: q.send_signal(signal.SIGKILL); q.wait(timeout=3)
        except Exception: pass
        cleanup_sock()


if __name__ == "__main__":
    main()
