#!/usr/bin/env python3
"""scripts/_dbg_dump_hello_o.py — boot, assemble hello.s, copy
the resulting /tmp/hello.o into /data/hello.o, shut down, then
parse data.img on the host to extract the bytes for analysis.

Used during chapter-131f Bug 4 diagnosis (GNU ld rejected /tmp/hello.o
with "file format not recognized" even though test_bin_as said the .o
contained ELF magic).  Keeps in tree per debug-scripts-policy.
"""
from __future__ import annotations
import os, signal, sys, time, socket, struct, subprocess, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SERIAL_SOCK = "/tmp/osdev-dump-hello.sock"
DATA_IMG = f"{ROOT}/build/data.img"
PROMPT = b"/$ "

SOURCE = (
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    mov x0, #42\n"
    "    mov x8, #2\n"
    "    svc #0\n"
)


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
                s.connect(SERIAL_SOCK); s.settimeout(2.0); return s
            except OSError:
                time.sleep(0.1)
        else:
            time.sleep(0.1)
    raise SystemExit("could not connect to serial socket")


def drain(s, deadline):
    out = b""
    while time.time() < deadline:
        try:
            c = s.recv(8192)
            if not c: break
            out += c
        except socket.timeout:
            if out: break
    return out


def wait_for(s, needle, timeout=10.0):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def send_cmd(s, cmd, timeout=15.0):
    s.sendall(cmd.encode() + b"\n")
    return wait_for(s, PROMPT, timeout)


def hard_kill(q):
    try: q.terminate(); q.wait(timeout=3.0)
    except Exception:
        try: q.kill()
        except Exception: pass


# ---- OSFS-2 extractor ----
BLOCK_SIZE = 4096

def osfs2_extract(img_path, name):
    """Read /data/<name> from an OSFS-2 image.  Returns bytes or raises."""
    with open(img_path, "rb") as f:
        data = f.read()

    # Superblock (block 0): we don't need it explicitly for this dump.
    # Inode table starts at block 3.  Inode size = 128 bytes.
    # Root dir is inode 1, with one preallocated data block.
    INODE_TABLE_OFF = 3 * BLOCK_SIZE
    INODE_SIZE = 128

    def read_inode(ino):
        off = INODE_TABLE_OFF + ino * INODE_SIZE
        b = data[off:off + INODE_SIZE]
        # Layout: u32 type, u32 size, u32 nlink, u32 mode,
        #         u32 ctime_ms, u32 mtime_ms,
        #         u32 direct[16], u32 indirect, u8 reserved[36]
        typ, size, _nlink, _mode = struct.unpack_from("<IIII", b, 0)
        # Skip 8 more bytes (ctime, mtime).
        direct = struct.unpack_from("<16I", b, 24)
        (indirect,) = struct.unpack_from("<I", b, 24 + 64)
        return typ, size, list(direct), indirect

    def read_block(blk):
        return data[blk * BLOCK_SIZE:(blk + 1) * BLOCK_SIZE]

    def read_file_bytes(ino):
        typ, size, direct, indirect = read_inode(ino)
        out = bytearray()
        # Direct blocks first.
        for blk in direct:
            if blk == 0: break
            out += read_block(blk)
            if len(out) >= size: break
        # Indirect.
        if len(out) < size and indirect:
            blk_data = read_block(indirect)
            ptrs = struct.unpack("<1024I", blk_data)
            for blk in ptrs:
                if blk == 0: break
                out += read_block(blk)
                if len(out) >= size: break
        return bytes(out[:size]), size

    # Root dir = inode 1.
    typ, size, direct, indirect = read_inode(1)
    print(f"root inode: type={typ} size={size} direct[0]={direct[0]}")
    # Read dirents from root.
    root_data = b""
    for blk in direct:
        if blk == 0: break
        root_data += read_block(blk)
        if len(root_data) >= size: break
    root_data = root_data[:size]

    # Each dirent is 64 bytes: u32 ino, char name[60].
    target_ino = 0
    for off in range(0, len(root_data), 64):
        ino, = struct.unpack_from("<I", root_data, off)
        if ino == 0: continue
        name_bytes = root_data[off + 4:off + 64]
        nul = name_bytes.find(b"\x00")
        nm = name_bytes[:nul if nul >= 0 else 60].decode("latin-1", errors="replace")
        print(f"  dirent ino={ino} name={nm!r}")
        if nm == name:
            target_ino = ino
    if not target_ino:
        raise SystemExit(f"no /data/{name} found")

    bytes_, sz = read_file_bytes(target_ino)
    print(f"target inode={target_ino} size={sz}")
    return bytes_


def main():
    reformat_data()
    q = boot()
    s = conn()
    try:
        wait_for(s, PROMPT, timeout=20.0)
        # Write source.
        send_cmd(s, "rm /tmp/hello.s 2>/dev/null; true")
        for line in SOURCE.strip("\n").split("\n"):
            send_cmd(s, f"echo '{line}' >> /tmp/hello.s")
        # Assemble.
        out = send_cmd(s, "/bin/as /tmp/hello.s -o /tmp/hello.o", timeout=20.0)
        print("[as out]", out.decode(errors="replace"))
        # Verify size in-OS.
        out = send_cmd(s, "ls /tmp/hello.o")
        print("[ls .o]", out.decode(errors="replace"))
        # Copy to /data.
        out = send_cmd(s, "cat /tmp/hello.o > /data/hello.o", timeout=30.0)
        print("[copy]", out.decode(errors="replace"))
        # Sync (try sync if available)
        send_cmd(s, "sync 2>/dev/null; true")
        # Verify the /data copy size.
        out = send_cmd(s, "ls /data/hello.o")
        print("[ls /data]", out.decode(errors="replace"))
    finally:
        try: s.close()
        except Exception: pass
        hard_kill(q)
        # qemu shutdown drops fs cache — we wrote via the kernel which
        # writes back through the cache; the data.img file should now
        # have valid OSFS-2 contents.
        time.sleep(0.5)

    img = str(ROOT / "build/data.img")
    raw = osfs2_extract(img, "hello.o")
    print(f"extracted {len(raw)} bytes")
    print("Full hex dump:")
    for i in range(0, len(raw), 16):
        chunk = raw[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        ascii_ = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  {i:04x}: {hexs:<48}  {ascii_}")
    elf_magic = b"\x7fELF"
    print("\nstartswith ELF magic:", raw[:4] == elf_magic)
    idx = raw.find(elf_magic)
    print(f"ELF magic byte offset in file: {idx}")

if __name__ == "__main__":
    main()
