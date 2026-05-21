#!/usr/bin/env python3
"""Test cursor sprite vs gold border under RAPID motion.

Goal: catch any cursor-related distortion that only manifests
when the cursor moves faster than the wsd poller can process
each step.  At ~100Hz poller, sleeping 0ms between QMP moves
batches many positions into the same poller_tick, exercising
the union-rect computation for big jumps (up to hundreds of
pixels in one cursor_move_only call).

If the compose-based model is correct, the union rect covers
ALL pixels touched by the sprite between old and new positions,
and compose_rect re-blits them all from canonical sources.
This script verifies that.
"""
import os
import select
import socket
import subprocess
import sys
import time
import json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMP  = '/tmp/osdev-rapid-qmp.sock'
SER  = '/tmp/osdev-rapid-ser.sock'

FB_W = 1920
FB_H = 1080
ABS_MAX = 0x7FFF
TITLE_H = 24

LAUNCHER_X = 100
LAUNCHER_Y = 100
BTN_TOP = 16
BTN_GAP = 8
BTN_H = 36
PAINT_X = LAUNCHER_X + 16 + 208 // 2
BROWSER_BTN_Y = (LAUNCHER_Y + TITLE_H + BTN_TOP
                 + 3 * (BTN_H + BTN_GAP) + BTN_H // 2)

BR_X = 140; BR_Y = 140; BR_W = 1024; BR_H = 720
BR_BODY_X0 = BR_X
BR_BODY_Y0 = BR_Y + TITLE_H + 32
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
        '-M', 'virt,gic-version=3', '-cpu', 'host', '-accel', 'hvf',
        '-m', '8G', '-smp', '2', '-display', 'none',
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
    end = time.time() + 3.0
    buf = b''; s.settimeout(0.3)
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
    qsend(qmp, {'execute': 'input-send-event', 'arguments': {'events': [
        {'type': 'abs', 'data': {'axis': 'x', 'value': ax}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': ay}},
    ]}})


def click(qmp, x, y):
    move(qmp, x, y); time.sleep(0.05)
    qsend(qmp, {'execute': 'input-send-event', 'arguments': {'events': [
        {'type': 'btn', 'data': {'down': True, 'button': 'left'}},
    ]}})
    time.sleep(0.05)
    qsend(qmp, {'execute': 'input-send-event', 'arguments': {'events': [
        {'type': 'btn', 'data': {'down': False, 'button': 'left'}},
    ]}})


def dump(qmp, path):
    try: os.unlink(path)
    except FileNotFoundError: pass
    qsend(qmp, {'execute': 'screendump', 'arguments': {'filename': path, 'format': 'ppm'}})
    end = time.time() + 5.0; last_size = 0
    while time.time() < end:
        if os.path.exists(path):
            sz = os.path.getsize(path)
            if sz > 0 and sz == last_size: return
            last_size = sz
        time.sleep(0.05)


def read_ppm(path):
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        line = f.readline().strip()
        while line.startswith(b'#'):
            line = f.readline().strip()
        w, h = (int(x) for x in line.split())
        f.readline()
        return w, h, f.read()


def px(ppm, x, y):
    w, h, d = ppm
    o = (y * w + x) * 3
    return (d[o], d[o+1], d[o+2])


def wait_for(s, marker, timeout=20.0):
    end = time.time() + timeout; buf = b''
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


def main():
    q = boot()
    try:
        ser = conn(SER); qmp = conn(QMP)
        qrl(qmp); qsend(qmp, {'execute': 'qmp_capabilities'})

        log = wait_for(ser, b'$ ', 30.0)
        if b'$ ' not in log:
            print('FAIL: shell never appeared'); return 1
        time.sleep(1.0)

        move(qmp, 5, 5); time.sleep(0.3)
        click(qmp, PAINT_X, BROWSER_BTN_Y)

        log = wait_for(ser, b'[wsd] bind win=', 20.0)
        if b'[wsd] bind win=' not in log:
            print('FAIL: browser never bound a window'); return 1
        time.sleep(4.0)

        move(qmp, 5, 5); time.sleep(1.5)
        dump(qmp, '/tmp/rapid_baseline.ppm')
        baseline = read_ppm('/tmp/rapid_baseline.ppm')

        # RAPID motion: jump cursor wildly across the body
        # with NO sleeps.  Many positions will be batched into
        # the same poller_tick, exercising big union rects.
        import random
        random.seed(42)
        print('  rapid sweep: 2000 random positions over body')
        for _ in range(2000):
            x = random.randint(BR_BODY_X0 + 50, BR_BODY_X1 - 50)
            y = random.randint(BR_BODY_Y0 + 50, BR_BODY_Y1 - 50)
            move(qmp, x, y)  # NO sleep between moves

        # Also: ZIGZAG across the gold border line (y=696-697)
        # at high speed.
        print('  zigzag across y=696 gold border, 500 swings')
        for i in range(500):
            x = BR_BODY_X0 + 50 + (i % 60) * 12
            y = 696 + (i % 2) * 2  # alternate y=696 and y=698
            move(qmp, x, y)

        # Park and let everything settle.
        move(qmp, 5, 5)
        time.sleep(2.0)
        dump(qmp, '/tmp/rapid_after.ppm')
        after = read_ppm('/tmp/rapid_after.ppm')

        # Diff every body pixel at 1-px stride.
        diffs = []
        for y in range(BR_BODY_Y0, BR_BODY_Y1):
            for x in range(BR_BODY_X0, BR_BODY_X1):
                pb = px(baseline, x, y)
                pa = px(after, x, y)
                if pb != pa:
                    diffs.append((x, y, pb, pa))

        print()
        print(f'  body diffs after rapid motion: {len(diffs)}')
        for d in diffs[:30]:
            print(f'    ({d[0]},{d[1]}): base={d[2]}  after={d[3]}')
        if diffs:
            # Group by y to spot row patterns.
            from collections import Counter
            row_counts = Counter(d[1] for d in diffs)
            print('  top 10 rows with diffs:')
            for y, c in sorted(row_counts.items(), key=lambda kv: -kv[1])[:10]:
                print(f'    y={y}: {c}')
            print()
            print('FAIL: rapid cursor motion left distortion in body')
            return 1
        print()
        print('PASS: rapid cursor motion left body pixel-perfect identical to baseline')
        return 0
    finally:
        try:
            q.terminate(); q.wait(timeout=3)
        except Exception:
            q.kill()
        cleanup()


if __name__ == '__main__':
    sys.exit(main())
