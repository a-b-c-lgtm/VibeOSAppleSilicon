#!/usr/bin/env python3
"""Aggressive diagnostic: cursor over .note gold border.

User reports persistent distortion of the .note element's
2-px gold border in test_layout.html after cursor sweeps
across it.  Existing test_cursor_over_browser.py sweeps the
body in 20-px X / 6-px Y steps and diffs at 2-px sample
stride and reports 0 diffs -- but those steps could miss a
thin horizontal line bug if the line is between sample rows
or the X step is large enough to hop OVER the line cleanly.

This script:

  1. Boots, opens browser via launcher, waits for parser to
     finish all in-flight work.
  2. Takes baseline full-FB PPM.
  3. Locates the gold border line by scanning the body for
     a horizontal run of gold-ish pixels.
  4. Sweeps the cursor in 1-px X steps directly along the
     border line (left to right and back).
  5. Parks cursor far away, takes after PPM.
  6. Diffs EVERY pixel of the body region (1-px sample).
  7. Reports any pixel that differs, with coords + colors.

Also dumps a mid-sweep screenshot so we can see if there's
distortion DURING the sweep (which might or might not get
cleaned up afterwards).
"""
import os
import select
import socket
import subprocess
import sys
import time
import json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMP  = '/tmp/osdev-gb-qmp.sock'
SER  = '/tmp/osdev-gb-ser.sock'

FB_W = 1920
FB_H = 1080
ABS_MAX = 0x7FFF
TITLE_H = 24

# Launcher pos (slot 0) and browser button geometry.
LAUNCHER_X = 100
LAUNCHER_Y = 100
BTN_TOP = 16
BTN_GAP = 8
BTN_H = 36
PAINT_X = LAUNCHER_X + 16 + 208 // 2
BROWSER_BTN_Y = (LAUNCHER_Y + TITLE_H + BTN_TOP
                 + 3 * (BTN_H + BTN_GAP) + BTN_H // 2)

# Browser cascade: slot 1 -> (140, 140).
BR_X = 140
BR_Y = 140
BR_W = 1024
BR_H = 720
BR_BODY_X0 = BR_X
BR_BODY_Y0 = BR_Y + TITLE_H + 32  # skip browser toolbar
BR_BODY_X1 = min(BR_X + BR_W, FB_W)
BR_BODY_Y1 = min(BR_Y + BR_H, FB_H)


def cleanup():
    for p in (QMP, SER):
        try: os.unlink(p)
        except FileNotFoundError: pass


def boot():
    cleanup()
    return subprocess.Popen([
        'qemu-system-aarch64',
        '-M', 'virt,gic-version=3',
        '-cpu', 'host', '-accel', 'hvf',
        '-m', '8G', '-smp', '2',
        '-display', 'none',
        '-serial', f'unix:{SER},server,nowait',
        '-qmp',    f'unix:{QMP},server,nowait',
        '-global', 'virtio-mmio.force-legacy=off',
        '-device', f'loader,file={ROOT}/assets/virt.dtb,addr=0x44000000',
        '-device', f'virtio-gpu-device,xres={FB_W},yres={FB_H}',
        '-device', 'virtio-keyboard-device',
        '-device', 'virtio-tablet-device',
        '-drive',  f'if=none,file={ROOT}/build/disk.img,format=raw,id=hd0',
        '-device', 'virtio-blk-device,drive=hd0',
        '-drive',  f'if=none,file={ROOT}/build/data.img,format=raw,id=hd1',
        '-device', 'virtio-blk-device,drive=hd1',
        '-kernel', f'{ROOT}/build/kernel.elf',
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def conn(p, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(p):
            try:
                s = socket.socket(socket.AF_UNIX); s.connect(p); return s
            except OSError: pass
        time.sleep(0.05)
    raise RuntimeError(f'no socket {p}')


def qrl(s):
    s.settimeout(2.0)
    try:
        while True:
            d = s.recv(8192)
            if not d: break
    except Exception: pass
    s.settimeout(None)


def qsend(s, obj):
    s.sendall((json.dumps(obj) + '\n').encode())
    end = time.time() + 5.0
    buf = b''
    s.settimeout(0.5)
    while time.time() < end:
        try:
            d = s.recv(8192)
            if not d: break
            buf += d
            if b'"return"' in buf or b'"error"' in buf: break
        except socket.timeout: break
    s.settimeout(None)
    return buf


def move(qmp, x, y):
    ax = int(x * ABS_MAX / (FB_W - 1))
    ay = int(y * ABS_MAX / (FB_H - 1))
    qsend(qmp, {'execute': 'input-send-event',
                'arguments': {'events': [
                    {'type': 'abs', 'data': {'axis': 'x', 'value': ax}},
                    {'type': 'abs', 'data': {'axis': 'y', 'value': ay}},
                ]}})


def click(qmp, x, y):
    move(qmp, x, y)
    time.sleep(0.05)
    qsend(qmp, {'execute': 'input-send-event',
                'arguments': {'events': [
                    {'type': 'btn', 'data': {'down': True,  'button': 'left'}},
                ]}})
    time.sleep(0.05)
    qsend(qmp, {'execute': 'input-send-event',
                'arguments': {'events': [
                    {'type': 'btn', 'data': {'down': False, 'button': 'left'}},
                ]}})


def dump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {'execute': 'screendump',
                'arguments': {'filename': path, 'format': 'ppm'}})
    # wait for file to appear and be non-empty.
    end = time.time() + 5.0
    last_size = 0
    while time.time() < end:
        if os.path.exists(path):
            sz = os.path.getsize(path)
            if sz > 0 and sz == last_size:
                return
            last_size = sz
        time.sleep(0.05)


def read_ppm(path):
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        assert magic == b'P6', f'bad magic {magic!r}'
        # skip comments
        line = f.readline().strip()
        while line.startswith(b'#'):
            line = f.readline().strip()
        w, h = (int(x) for x in line.split())
        f.readline()  # maxval
        return w, h, f.read()


def px(ppm, x, y):
    w, h, d = ppm
    o = (y * w + x) * 3
    return (d[o], d[o+1], d[o+2])


def wait_for(s, marker, timeout=20.0):
    end = time.time() + timeout
    buf = b''
    s.setblocking(False)
    while time.time() < end:
        rd, _, _ = select.select([s], [], [], 0.1)
        if not rd: continue
        try: d = s.recv(8192)
        except BlockingIOError: continue
        if not d: continue
        buf += d
        if marker in buf: return buf
    return buf


def find_gold_border(ppm):
    """Find a horizontal run of gold-ish pixels in the body
    region.  Gold border in test_layout.html: ~0xDDC050."""
    w, h, _ = ppm
    # CSS border-color is a gold; rendered BGRA approx
    # (0x50, 0xc0, 0xdd) in (R,G,B).  Use a tolerance.
    def is_gold(r, g, b):
        return (0x90 <= r <= 0xff
                and 0x80 <= g <= 0xd0
                and 0x30 <= b <= 0x80)
    candidates = []
    for y in range(BR_BODY_Y0, BR_BODY_Y1):
        run = 0
        run_start = -1
        for x in range(BR_BODY_X0, BR_BODY_X1):
            r, g, b = px(ppm, x, y)
            if is_gold(r, g, b):
                if run == 0: run_start = x
                run += 1
            else:
                if run >= 50:  # need a long horizontal run
                    candidates.append((y, run_start, run_start + run - 1, run))
                run = 0
        if run >= 50:
            candidates.append((y, run_start, run_start + run - 1, run))
    return candidates


def main():
    q = boot()
    try:
        ser = conn(SER)
        qmp = conn(QMP)
        qrl(qmp)
        qsend(qmp, {'execute': 'qmp_capabilities'})

        log = wait_for(ser, b'$ ', 30.0)
        if b'$ ' not in log:
            print('FAIL: shell never appeared')
            return 1
        time.sleep(1.0)

        # Park far from launcher.
        move(qmp, 5, 5)
        time.sleep(0.3)

        # Click browser button.
        print(f'  clicking launcher browser button at ({PAINT_X}, {BROWSER_BTN_Y})')
        click(qmp, PAINT_X, BROWSER_BTN_Y)

        log = wait_for(ser, b'[wsd] bind win=', 20.0)
        if b'[wsd] bind win=' not in log:
            print('FAIL: browser never bound a window')
            return 1
        # Let parser finish.
        time.sleep(4.0)

        # Park cursor far away.
        move(qmp, 5, 5)
        time.sleep(1.5)
        dump(qmp, '/tmp/gb_baseline.ppm')
        baseline = read_ppm('/tmp/gb_baseline.ppm')

        # Find gold border line(s) in body.
        gold_lines = find_gold_border(baseline)
        if not gold_lines:
            print('FAIL: no gold border line found in baseline')
            print('      (browser may not have rendered .note element)')
            return 1
        print(f'  found {len(gold_lines)} gold horizontal runs:')
        for gl in gold_lines[:10]:
            print(f'    y={gl[0]}  x={gl[1]}..{gl[2]}  len={gl[3]}')

        # Pick the longest run.
        gold_lines.sort(key=lambda g: -g[3])
        gy, gx0, gx1, glen = gold_lines[0]
        print(f'  sweeping cursor along y={gy} from x={gx0} to x={gx1}')

        # Sweep cursor along the gold border line at 1-px X
        # steps, left-to-right, then right-to-left.  This is
        # the MOST AGGRESSIVE possible sweep over the
        # reportedly-distorted line.
        for x in range(gx0, gx1 + 1):
            move(qmp, x, gy)
            time.sleep(0.005)
        # Mid-sweep snapshot (cursor still at gx1, gy).
        dump(qmp, '/tmp/gb_midsweep.ppm')

        for x in range(gx1, gx0 - 1, -1):
            move(qmp, x, gy)
            time.sleep(0.005)
        # Also sweep ABOVE and BELOW the line so the sprite
        # crosses the border line at many y offsets.
        for dy in (-9, -6, -3, 3, 6, 9):
            for x in range(gx0, gx1 + 1, 1):
                move(qmp, x, gy + dy)
                time.sleep(0.003)

        # Park and let everything settle.
        move(qmp, 5, 5)
        time.sleep(2.0)
        dump(qmp, '/tmp/gb_after.ppm')
        after = read_ppm('/tmp/gb_after.ppm')

        # Diff every body pixel at 1-px stride.
        diffs = []
        for y in range(BR_BODY_Y0, BR_BODY_Y1):
            for x in range(BR_BODY_X0, BR_BODY_X1):
                pb = px(baseline, x, y)
                pa = px(after, x, y)
                if pb != pa:
                    diffs.append((x, y, pb, pa))
        # Diffs SPECIFICALLY on the gold border line.
        border_diffs = [d for d in diffs if d[1] == gy]
        print()
        print(f'  baseline: /tmp/gb_baseline.ppm')
        print(f'  midsweep: /tmp/gb_midsweep.ppm')
        print(f'  after:    /tmp/gb_after.ppm')
        print(f'  total body diffs:       {len(diffs)}')
        print(f'  on gold border (y={gy}): {len(border_diffs)}')
        for d in border_diffs[:30]:
            print(f'    ({d[0]},{d[1]}): base={d[2]}  after={d[3]}')

        # Also report diff locations grouped by row.
        from collections import Counter
        row_counts = Counter(d[1] for d in diffs)
        print()
        print(f'  diffs by row (top 20):')
        for y, c in sorted(row_counts.items(),
                            key=lambda kv: -kv[1])[:20]:
            print(f'    y={y}: {c} diff pixels')

        if diffs:
            print()
            print(f'FAIL: cursor sweep over gold border left '
                  f'{len(diffs)} distorted pixels in body')
            return 1
        print()
        print('PASS: gold border + body identical to baseline '
              'after aggressive cursor sweep')
        return 0
    finally:
        try:
            q.terminate(); q.wait(timeout=3)
        except Exception:
            q.kill()
        cleanup()


if __name__ == '__main__':
    sys.exit(main())
