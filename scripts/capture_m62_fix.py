import json, os, select, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QMP_SOCK    = "/tmp/osdev-qmp-m62.sock"
SERIAL_SOCK = "/tmp/osdev-serial-m62.sock"
DUMP_PATH   = "/tmp/m62-after-fix.ppm"

FB_W = 1280
FB_H = 800

def cleanup():
    for p in (QMP_SOCK, SERIAL_SOCK):
        try: os.unlink(p)
        except OSError: pass

def boot():
    cleanup()
    return subprocess.Popen([
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "host", "-accel", "hvf",
        "-m", "8G", "-smp", "2", "-display", "none",
        "-serial", f"unix:{SERIAL_SOCK},server,nowait",
        "-qmp",    f"unix:{QMP_SOCK},server,nowait",
        "-global", "virtio-mmio.force-legacy=off",
        "-device", f"loader,file={ROOT}/assets/virt.dtb,addr=0x44000000",
        "-device", f"virtio-gpu-device,xres={FB_W},yres={FB_H}",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
                                            d/                                            d/       io-                                            d/                   ild/d                                    "-device", "virtio-blk-device,drive=hd1",
                                          -de          rt      -devic       v=n0",
                     "none,id=audio0",
                     "noio                     "noudio0"          "-         f"{RO                     "noio  ,                      "noio stde                     "noio                     "noudio0"           + 15.0
    while time.time() < deadline:
        if os.        if os.        if os.        if os.        if  =         if os.        if os.        if os.                   if os.        if os.        if os.         except OSError: pass
        time.sleep(0.1)
    raise RuntimeError(f"no socket: {path}")

def qread(qmp):
    buf = b""
    while not buf.endswith(b"\n"):
        c = qmp.recv(4096)
        if not c: break
        buf += c
                                                                                                                                                                                                                                                           .t                                                                                 ct                                     :
                                                                                                                                                                                                                       qm                                (qmp); qsend(qmp, {"execute": "qmp_capabilities"})
        print("Waiting for prompt...")
        if not wait_for_prompt(ser):
            print("FAIL: Prompt not reached")
            return 1
                         browser...")
        ser.sendall(b"browser --gui        ser.seou        ser.\n")
        time.sleep(10)
        print("Taking screenshot...")
        qsend(qmp, {"e        qsenreendump", "arguments": {"filename": DUMP_PATH}})
        time.sleep(1)
        if os.path.exists(DUMP_PATH):
            print(f"PASS: Saved to {DUMP_PATH}")
            return 0
        else:
            print("FAIL: screenshot failed")
            return 1
    finally:
        q.terminate()

if __name__ == "__main__":
    main()
