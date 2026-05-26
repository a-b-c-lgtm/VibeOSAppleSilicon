#!/usr/bin/env python3
# Standalone diagnostic: boot OS with QEMU output captured to file,
# then exercise the tar extract from /bin/doomobjs.tar -C /data
# and observe exactly where things break.
import os, sys, time, socket, subprocess, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SERIAL_SOCK = "/tmp/diag-doom-link.sock"
QEMU_LOG    = "/tmp/diag-doom-link-qemu.log"
DISK_IMG    = f"{ROOT}/build/disk.img"
DATA_IMG    = f"{ROOT}/build/data.img"

QEMU = [
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
    "-m", "8G", "-smp", "2", "-display", "none",
    "-serial", f"unix:{SERIAL_SOCK},server,nowait",
    "-global", "virtio-mmio.force-legacy=off",
    "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
    "-drive", f"if=none,file={DISK_IMG},format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"if=none,file={DATA_IMG},format=raw,id=hd1",
    "-device", "virtio-blk-device,drive=hd1",
    "-kernel", f"{ROOT}/build/kernel.elf",
]

def drain(sock, timeout=2.0, label=""):
    sock.settimeout(0.3)
    end = time.time() + timeout
    out = b""
    while time.time() < end:
        try:
            c = sock.recv(8192)
            if c:
                out += c
            else:
                break
        except socket.timeout:
            pass
    if label:
        print(f"=== {label} ({len(out)} bytes) ===", flush=True)
    sys.stdout.buffer.write(out)
    sys.stdout.flush()
    return out

def send(sock, cmd):
    print(f"\n>>> {cmd}", flush=True)
    sock.sendall((cmd + "\n").encode())

def main():
    # reformat
    subprocess.check_call([sys.executable,
                           f"{ROOT}/scripts/mkosfs2.py", DATA_IMG])

    try: os.unlink(SERIAL_SOCK)
    except FileNotFoundError: pass

    qlog = open(QEMU_LOG, "w")
    qemu = subprocess.Popen(QEMU, cwd=ROOT,
                            stdout=qlog, stderr=qlog,
                            preexec_fn=os.setsid)

    # wait for socket
    sock = None
    deadline = time.time() + 15.0
    while time.time() < deadline:
        if os.path.exists(SERIAL_SOCK):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(SERIAL_SOCK)
                break
            except OSError:
                sock = None
                time.sleep(0.1)
        else:
            time.sleep(0.1)
    if sock is None:
        try: os.killpg(qemu.pid, signal.SIGKILL)
        except Exception: pass
        raise RuntimeError("no socket")

    try:
        drain(sock, 30.0, "BOOT")

        send(sock, "/bin/ls /bin/doomobjs.tar")
        drain(sock, 3.0)

        send(sock, "/bin/wc -c /bin/doomobjs.tar")
        drain(sock, 3.0)

        send(sock, "/bin/ls /data")
        drain(sock, 3.0)

        # Now the dangerous step
        print("\n>>> /bin/tar xf /bin/doomobjs.tar -C /data  (with 180s drain)", flush=True)
        sock.sendall(b"/bin/tar xf /bin/doomobjs.tar -C /data\n")
        # Drain for up to 180 seconds, printing every chunk
        sock.settimeout(0.5)
        end = time.time() + 180.0
        last_byte = time.time()
        idle_limit = 30.0
        total = 0
        while time.time() < end:
            try:
                c = sock.recv(8192)
                if c:
                    total += len(c)
                    last_byte = time.time()
                    sys.stdout.buffer.write(c)
                    sys.stdout.flush()
                else:
                    print("\n!!! socket EOF (QEMU may have died)", flush=True)
                    break
            except socket.timeout:
                if time.time() - last_byte > idle_limit:
                    print(f"\n... {int(time.time()-last_byte)}s idle, bailing (total={total} bytes)", flush=True)
                    break
            except (BrokenPipeError, ConnectionResetError) as e:
                print(f"\n!!! socket error: {e}", flush=True)
                break

        # See if QEMU is still alive
        rc = qemu.poll()
        print(f"\n=== qemu poll rc={rc} ===", flush=True)

        if rc is None:
            # try ls
            try:
                send(sock, "/bin/ls /data")
                drain(sock, 5.0)
                send(sock, "/bin/ls /data/src 2>&1 | head -10")
                drain(sock, 5.0)
                send(sock, "/bin/ls /data/src 2>&1 | /bin/wc -l")
                drain(sock, 5.0)
            except Exception as e:
                print(f"\n!!! followup failed: {e}", flush=True)
    finally:
        try:
            os.killpg(qemu.pid, signal.SIGKILL)
        except Exception:
            pass
        qlog.close()

    print("\n=== tail of qemu log ===", flush=True)
    with open(QEMU_LOG) as f:
        lines = f.readlines()
    for line in lines[-40:]:
        sys.stdout.write(line)

if __name__ == "__main__":
    main()
