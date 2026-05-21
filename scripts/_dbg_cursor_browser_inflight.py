#!/usr/bin/env python3
"""Boot the OS, open the browser, then do SLOW cursor motions
across the .note div (the yellow box).  Take screenshots
PERIODICALLY during the motion and save them so we can examine
TRANSIENT distortion mid-motion.

This is for diagnosing the chapter 108e "lines distorted as
cursor moves over them" report.  The pixel-perfect regression
test (test_cursor_over_browser.py) takes baseline + after,
which only catches PERSISTENT distortion (state that survives
after the cursor moves away).  This test captures the in-
flight state for visual inspection.

Outputs:
  /tmp/cdbg_baseline.ppm     -- before any motion
  /tmp/cdbg_mid_NN.ppm       -- during motion, NN = 0..9
  /tmp/cdbg_final.ppm        -- after cursor parked away
  /tmp/cdbg_diff_summary.txt -- pixel diff counts per step
"""
import json, os, select, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMP  = '/tmp/osdev-cdbg-qmp.sock'
SER  = '/tmp/osdev-cdbg-ser.sock'

FB_W, FB_H = 1280, 800
ABS_MAX = 0x7FFF
PAINT_X = 220
BROWSER_BTN_Y = 290


def cleanup():
    for p in (QMP, SER):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot():
    cleanup()
    return subprocess.Popen([
        'qemu-system-aarch64', '-M', 'virt,gic-version=3',
        '-cpu', 'host', '-accel', 'hvf', '-m', '8G', '-smp', '2',
        '-display', 'none',
        '-serial', f'unix:{SER},server,nowait',
        '-qmp', f'unix:{QMP},server,nowait',
        '-global', 'virtio-mmio.force-legacy=off',
        '-device', f'loader,file={ROOT}/assets/virt.dtb,addr=0x44000000',
        '-device', f'virtio-gpu-device,xres={FB_W},yres={FB_H}',
        '-device', 'virtio-keyboard-device',
        '-device', 'virtio-tablet-device',
        '-drive', f'if=none,file={ROOT}/build/disk.img,format=raw,id=hd0',
        '-device', 'virtio-blk-device,drive=hd0',
        '-drive', f'if=none,file={ROOT}/build/data.img,format=raw,id=hd1',
        '-device', 'virtio-blk-device,drive=hd1',
        '-kernel', f'{ROOT}/build/kernel.elf',
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn(path, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX); s.connect(path); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(f'no socket {path}')


def qrl(qmp):
    buf = b''
    while not buf.endswith(b'\n'):
        c = qmp.recv(4096)
        if not c: raise RuntimeError('qmp closed')
        buf += c
    return json.loads(buf)


def qsend(qmp, obj):
    qmp.sendall((json.dumps(obj) + '\n').encode())
    while True:
        m = qrl(qmp)
        if 'return' in m or 'error' in m: return m


def move(qmp, x, y):
    ax = int(x * ABS_MAX / FB_W)
    ay = int(y * ABS_MAX / FB_H)
    qsend(qmp, {'execute': 'input-send-event', 'arguments': {'events': [
        {'type': 'abs', 'data': {'axis': 'x', 'value': ax}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': ay}},
    ]}})


def button(qmp, down):
    qsend(qmp, {'execute': 'input-send-event', 'arguments': {'events': [
        {'type': 'btn', 'data': {'down': bool(down), 'button': 'left'}}]}})


def click(qmp, x, y):
    move(qmp, x, y); time.sleep(0.05)
    button(qmp, True); time.sleep(0.05)
    button(qmp, False)


def drain(s, deadline):
    out = b''
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c: break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str): needle = needle.encode()
    end = time.time() + timeout
    buf = b''
    while time.time() < end:
        buf += drain(s, time.time() + 0.4)
        if needle in buf: return buf
    return buf


def dump(qmp, path):
    if os.path.exists(path): os.unlink(path)
    qsend(qmp, {'execute': 'screendump', 'arguments': {'filename': path}})
    end = time.time() + 5
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 100:
            time.sleep(0.15); return
        time.sleep(0.1)
    raise RuntimeError(f'no screendump {path}')


def read_ppm(p):
    with open(p, 'rb') as f:
        m = f.readline().strip()
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = (int(x) for x in line.split())
        f.readline()
        return w, h, f.read()


def px(ppm, x, y):
    w, h, d = ppm
    o = (y*w + x)*3
    return (d[o], d[o+1], d[o+2])


def main():
    q = boot()
    try:
        ser = conn(SER); qmp = conn(QMP)
        qrl(qmp); qsend(qmp, {'execute': 'qmp_capabilities'})
        wait_for(ser, b'$ ', 30.0); time.sleep(1.0)

        # Park away, open browser.
        move(qmp, 5, 5); time.sleep(0.3)
        click(qmp, PAINT_X, BROWSER_BTN_Y)
        wait_for(ser, b'[wsd] bind win=', 20.0); time.sleep(3.0)

        # Baseline with cursor parked away.
        move(qmp, 5, 5); time.sleep(0.7)
        dump(qmp, '/tmp/cdbg_baseline.ppm')
        baseline = read_ppm('/tmp/cdbg_baseline.ppm')

        # Browser cascade slot 1: (140, 140).  Toolbar ~32 px.
        # The .note div content is somewhere around y=500..560
        # (depends on viewport).  Sweep across y=550 — middle
        # of the page body.
        BR_X = 140
        SWEEP_Y = 550
        START_X = 200
        END_X   = 1100

        # Take 10 mid-sweep screenshots, one for each tenth of
        # the path.  Between each, do many small cursor moves
        # to simulate slow user motion.
        path_x = list(range(START_X, END_X, 4))
        n = len(path_x)
        chunk = n // 10
        diffs_summary = []
        for step in range(10):
            for j in range(step*chunk, (step+1)*chunk):
                move(qmp, path_x[j], SWEEP_Y)
                time.sleep(0.005)
            # Stop the cursor for a moment to capture in-flight.
            time.sleep(0.3)
            mid = f'/tmp/cdbg_mid_{step:02d}.ppm'
            dump(qmp, mid)
            mid_ppm = read_ppm(mid)
            # Diff body region but EXCLUDE current cursor rect
            # (cursor IS expected to differ during in-flight).
            cur_x = path_x[(step+1)*chunk - 1] if (step+1)*chunk - 1 < n else END_X
            cur_y = SWEEP_Y
            cursor_excl_x0 = cur_x - 2
            cursor_excl_x1 = cur_x + 15
            cursor_excl_y0 = cur_y - 2
            cursor_excl_y1 = cur_y + 22
            d = 0
            for y in range(BR_X, FB_H, 4):
                for x in range(BR_X, FB_W, 4):
                    if (cursor_excl_x0 <= x <= cursor_excl_x1
                        and cursor_excl_y0 <= y <= cursor_excl_y1):
                        continue
                    pb = px(baseline, x, y)
                    pm = px(mid_ppm, x, y)
                    if pb != pm:
                        d += 1
                        if d <= 3:
                            diffs_summary.append(
                                f'step {step}: ({x},{y}) base={pb} mid={pm}')
            diffs_summary.append(f'step {step}: cursor@({cur_x},{cur_y})  '
                                 f'NON-CURSOR diffs={d}')

        # Park and take final.
        move(qmp, 5, 5); time.sleep(1.5)
        dump(qmp, '/tmp/cdbg_final.ppm')
        final = read_ppm('/tmp/cdbg_final.ppm')

        d_final = 0
        for y in range(BR_X, FB_H, 4):
            for x in range(BR_X, FB_W, 4):
                pb = px(baseline, x, y)
                pf = px(final, x, y)
                if pb != pf:
                    d_final += 1
                    if d_final <= 5:
                        diffs_summary.append(
                            f'FINAL: ({x},{y}) base={pb} final={pf}')
        diffs_summary.append(f'FINAL: diffs={d_final}')

        with open('/tmp/cdbg_diff_summary.txt', 'w') as f:
            for line in diffs_summary:
                f.write(line + '\n')

        print()
        for line in diffs_summary:
            print(line)

        return 0
    finally:
        try: q.terminate(); q.wait(timeout=3)
        except Exception: q.kill()
        cleanup()


if __name__ == '__main__':
    sys.exit(main())
