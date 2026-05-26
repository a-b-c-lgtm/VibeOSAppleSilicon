#!/usr/bin/env python3
"""scripts/_dbg_ld_input.py -- chapter 131f debug.

Reproduces the test_bin_ld_ar.py failure where /bin/ld
(GNU binutils-2.44 cross-built) reports
  /tmp/hello.o: file not recognized: file format not recognized
even though /bin/as produced a valid ELF64 AArch64 .o.

This script:
  1. Boots osdev to a shell.
  2. Assembles the same tiny hello.s as test_bin_ld_ar.
  3. Dumps the first 128 bytes of /tmp/hello.o as hex
     (so we can verify the ELF header on the host).
  4. Copies /tmp/hello.o to /data/hello.o so we can pull it
     off the data.img and run aarch64-elf-readelf on it.
  5. Runs /bin/ld again and prints its full stderr.

Kept per debug-scripts-policy.
"""

import os, signal, socket, subprocess, sys, time, select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_SOCK = "/tmp/osdev-ld-input.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"$ "

SOURCE = r""".text
.global _start
_start:
    mov x0, #42
    mov x8, #2
    svc #0
"""


def cleanup_sock():
    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass


def reformat_data():
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
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def hard_kill(q):
    try:
        q.send_signal(signal.SIGKILL); q.wait(timeout=3)
    except Exception: pass
    cleanup_sock()


def conn():
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SERIAL_SOCK); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError("no serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
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


def send_cmd(s, cmd, timeout=15.0):
    if isinstance(cmd, str): cmd = cmd.encode()
    s.sendall(cmd + b"\n")
    out = wait_for(s, PROMPT, timeout)
    idx = out.find(cmd)
    if idx >= 0: out = out[idx + len(cmd):]
    return out


def main():
    print("[chapter 131f debug] /bin/ld input bfd diagnostic")
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)

        send_cmd(s, "rm /tmp/hello.s 2>/dev/null; true")
        for line in SOURCE.strip("\n").split("\n"):
            send_cmd(s, f"echo '{line}' >> /tmp/hello.s")

        out = send_cmd(s, "/bin/as /tmp/hello.s -o /tmp/hello.o",
                        timeout=20.0)
        print("[as stderr]")
        print(out.decode(errors='replace'))

        # Copy to /data so we can extract from data.img later.
        out = send_cmd(s, "cp /tmp/hello.o /data/hello.o", timeout=20.0)
        print("[cp]", out.decode(errors='replace'))

        # Size + xxd of first 256 bytes via cat (binary).
        out = send_cmd(s, "wc -c /tmp/hello.o", timeout=20.0)
        print("[wc]", out.decode(errors='replace'))

        # Now try the link and capture full output.
        out = send_cmd(s,
            "/bin/ld -T /bin/osdev.ld -e _start "
            "-o /tmp/hello /tmp/hello.o 2>&1",
            timeout=30.0)
        print("[ld]")
        print(out.decode(errors='replace'))

        # Try with verbose.
        out = send_cmd(s,
            "/bin/ld --verbose -T /bin/osdev.ld -e _start "
            "-o /tmp/hello /tmp/hello.o 2>&1 | head -40",
            timeout=30.0)
        print("[ld --verbose]")
        print(out.decode(errors='replace'))

    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)

    # Now extract /data/hello.o from data.img and dump it.
    print("\n[host extract] dumping /data/hello.o from data.img")
    rc = subprocess.call([
        "python3", f"{ROOT}/scripts/mkosfs2.py",
        "--extract", DATA_IMG, "hello.o", "/tmp/_dbg_ld_input_hello.o",
    ])
    if rc == 0 and os.path.exists("/tmp/_dbg_ld_input_hello.o"):
        sz = os.path.getsize("/tmp/_dbg_ld_input_hello.o")
        print(f"  size = {sz} bytes")
        with open("/tmp/_dbg_ld_input_hello.o", "rb") as f:
            head = f.read(64)
        print(f"  ehdr = {head.hex(' ', 1)}")
        # Dump with readelf.
        rc2 = subprocess.call(
            ["aarch64-elf-readelf", "-h",
             "/tmp/_dbg_ld_input_hello.o"])
        print(f"  readelf rc = {rc2}")
    else:
        print(f"  extract failed (rc={rc})")

if __name__ == "__main__":
    main()
