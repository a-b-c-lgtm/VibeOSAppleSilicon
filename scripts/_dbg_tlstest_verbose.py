"""Diagnostic wrapper for tlstest — print the FULL serial output."""
import importlib.util, os, sys, time, select

spec = importlib.util.spec_from_file_location("t", "scripts/test_tlstest.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

q = m.boot()
try:
    ser = m.conn()
    log = m.read_until(ser, [b"$ "], 90.0)
    sys.stdout.write("=== BOOT LOG (until shell prompt) ===\n")
    sys.stdout.write(log.decode("ascii", "replace"))
    sys.stdout.write("\n=== END BOOT LOG ===\n")

    if b"$ " not in log:
        print("never saw shell prompt"); sys.exit(2)

    ser.sendall(b"tlstest\n")
    buf = bytearray()
    deadline = time.time() + 20.0
    while time.time() < deadline:
        r,_,_ = select.select([ser],[],[],0.5)
        if r:
            c = ser.recv(8192)
            if not c: break
            buf.extend(c)
    sys.stdout.write("=== TLSTEST OUTPUT (full) ===\n")
    sys.stdout.write(bytes(buf).decode("ascii", "replace"))
    sys.stdout.write("\n=== END TLSTEST OUTPUT ===\n")
finally:
    q.kill(); q.wait()
    try: os.unlink(m.SOCK)
    except FileNotFoundError: pass
