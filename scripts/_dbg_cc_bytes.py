#!/usr/bin/env python3
"""scripts/_dbg_cc_bytes.py — dump /tmp/hello.cc.o + /tmp/hello via serial cat, decode .text entry."""
import os, socket, subprocess, time, select, signal, sys, struct

ROOT = "/Users/seusher/Desktop/osdev"
SOCK = "/tmp/osdev-cc-bytes.sock"
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
deadline = time.time() + 5
while time.time() < deadline and not os.path.exists(SOCK):
    time.sleep(0.05)
sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); sk.connect(SOCK)
PROMPT = b"/$ "

def drain_to(needle, timeout):
    out = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r,_,_ = select.select([sk],[],[],0.1)
        if r:
            c = sk.recv(65536)
            if not c: break
            out += c
            if needle in out: return out
    return out

def send(cmd, timeout=20):
    sk.sendall(cmd.encode("latin1")+b"\n")
    return drain_to(PROMPT, timeout)

try:
    drain_to(PROMPT, 25)

    # write source
    send("rm /tmp/hello.c 2>/dev/null; true")
    for line in [
        'int main(void) {',
        '    printf("hi\\n");',
        '    return 0;',
        '}',
    ]:
        send(f"echo '{line}' >> /tmp/hello.c")

    # build
    out = send("/bin/cc /tmp/hello.c -o /tmp/hello", timeout=30)
    print("=== cc build ===")
    print(out.decode("utf-8","replace"))

    # cat the linked binary, capture raw bytes between echo and prompt
    cmd = b"cat /tmp/hello\n"
    sk.sendall(cmd)
    blob = drain_to(PROMPT, 30)
    # blob contains: "cat /tmp/hello\r\n" + raw bytes + "\r\n/$ "
    # Strip the echoed command and prompt suffix.
    # Find the echo end (first newline after the command echo).
    i = blob.find(b"cat /tmp/hello")
    if i >= 0:
        j = blob.find(b"\n", i)
        if j >= 0:
            payload = blob[j+1:]
        else:
            payload = blob
    else:
        payload = blob
    # strip trailing prompt
    if payload.endswith(PROMPT):
        payload = payload[:-len(PROMPT)]
    # strip trailing \r\n
    while payload.endswith(b"\r") or payload.endswith(b"\n"):
        payload = payload[:-1]

    print(f"=== /tmp/hello raw payload len={len(payload)} ===")
    # Try to find ELF header
    elf = payload.find(b"\x7fELF")
    print(f"ELF magic at offset {elf}")
    if elf >= 0:
        b = payload[elf:elf+96]
        print("first 96 bytes (hex):")
        print(" ".join(f"{x:02x}" for x in b))
        # e_entry is at offset 24, 8 bytes LE
        if len(b) >= 32:
            e_entry = struct.unpack_from("<Q", b, 24)[0]
            print(f"e_entry = 0x{e_entry:016x}")
        # phdr at offset 64 (e_phoff=64)
        # phdr layout: p_type(4) p_flags(4) p_offset(8) p_vaddr(8) p_paddr(8) p_filesz(8) p_memsz(8) p_align(8) = 56
        if len(payload) >= elf+64+56*2:
            ph1 = payload[elf+64 : elf+64+56]
            ph2 = payload[elf+64+56 : elf+64+112]
            p1 = struct.unpack("<IIQQQQQQ", ph1)
            p2 = struct.unpack("<IIQQQQQQ", ph2)
            print(f"PH1: type=0x{p1[0]:x} flags=0x{p1[1]:x} off=0x{p1[2]:x} vaddr=0x{p1[3]:x} paddr=0x{p1[4]:x} filesz=0x{p1[5]:x} memsz=0x{p1[6]:x} align=0x{p1[7]:x}")
            print(f"PH2: type=0x{p2[0]:x} flags=0x{p2[1]:x} off=0x{p2[2]:x} vaddr=0x{p2[3]:x} paddr=0x{p2[4]:x} filesz=0x{p2[5]:x} memsz=0x{p2[6]:x} align=0x{p2[7]:x}")
        # dump bytes at .text (file offset 0x1000)
        if len(payload) >= elf+0x1000+64:
            tx = payload[elf+0x1000:elf+0x1000+64]
            print("bytes at file offset 0x1000 (.text):")
            print(" ".join(f"{x:02x}" for x in tx))
            # decode first 4-byte word
            w = struct.unpack_from("<I", tx, 0)[0]
            print(f"first insn word = 0x{w:08x}")
            # is this a BL?  0x94000000 mask 0xfc000000
            if (w & 0xfc000000) == 0x94000000:
                imm = w & 0x03ffffff
                if imm & 0x02000000: imm |= 0xfc000000  # sign extend
                imm = struct.unpack("<i", struct.pack("<I", imm))[0]
                print(f"  BL with imm26={imm} (byte offset {imm*4})")
            elif (w & 0xfc000000) == 0x14000000:
                imm = w & 0x03ffffff
                if imm & 0x02000000: imm |= 0xfc000000
                imm = struct.unpack("<i", struct.pack("<I", imm))[0]
                print(f"  B with imm26={imm} (byte offset {imm*4})")
            elif w == 0:
                print("  ZERO -> undefined instr trap")
finally:
    try: sk.close()
    except: pass
    try: q.send_signal(signal.SIGKILL); q.wait(timeout=3)
    except: pass
    try: os.unlink(SOCK)
    except: pass
