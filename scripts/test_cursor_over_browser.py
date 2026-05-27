#!/usr/bin/env python3
"""Regression: cursor save/restore correctness over BROWSER text.

The launcher version of this test (test_cursor_over_window.py)
sweeps the cursor over uniform-color button rects, which would
miss any save/restore bug that only manifests on detailed
content.  This version drives the cursor over the .note div in
test_layout.html — yellow box with dark text and a 2px gold
border — exactly the content the user reported distortion on
in chapter 118.

Strategy:
  1. Boot, wait for launcher.
  2. Click launcher's browser button (spawns /bin/browser --gui
     /mnt/test_layout.html).
  3. Wait for first render.
  4. Take baseline screenshot with cursor parked at top-left.
  5. Drive the cursor across multiple Y bands of the browser's
     body region — multiple passes through the .note text.
  6. Park cursor at top-left again.
  7. Take final screenshot.
  8. Diff every BROWSER BODY pixel (skipping the toolbar at top
     and decoration title bar) against baseline.  Should be 0
     differences.
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMP  = '/tmp/osdev-cb-qmp.sock'
SER  = '/tmp/osdev-cb-ser.sock'

FB_W = 1280
FB_H = 800
ABS_MAX = 0x7FFF

# Launcher is now a Start-menu-style panel (NODECORATION +
# ALWAYS_ON_TOP), hidden by default and anchored above the
# taskbar at (0, FB_H - BAR_H - 232) = (0, 540).  It is
# summoned by clicking the taskbar's Start button.  Because
# the launcher window uses wm_create_window_at, it does NOT
# advance the cascade counter -- the first cascade-positioned
# client (the browser, here) lands at slot 0 = (100, 100).
BAR_H      = 28
LAUNCHER_X = 0
LAUNCHER_Y = FB_H - BAR_H - 232           # 540
BTN_TOP    = 16          # offset inside launcher body
BTN_GAP    = 8
BTN_H      = 36
TITLE_H    = 0           # launcher: NODECORATION, no wsd title bar

# Taskbar Start button (userspace/taskbar/taskbar.c).
START_BTN_X     = 8
START_BTN_Y_OFF = 4
START_BTN_W     = 60
START_BTN_H     = BAR_H - 8
START_CX = START_BTN_X + START_BTN_W // 2                        # 38
START_CY = (FB_H - BAR_H) + START_BTN_Y_OFF + START_BTN_H // 2   # 786

# Launcher buttons (gui_term, paint, notepad, browser).
PAINT_X    = LAUNCHER_X + 16 + 208 // 2                          # 120
BROWSER_BTN_Y = (LAUNCHER_Y + TITLE_H + BTN_TOP
                 + 3 * (BTN_H + BTN_GAP) + BTN_H // 2)           # 706


def cleanup():
    for p in (QMP, SER):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


def boot():
    cleanup()
    return subprocess.Popen([
        'qemu-system-aarch64',
        '-M', 'virt,gic-version=3',
        '-cpu', 'host',
        '-accel', 'hvf',
        '-m', '8G',
        '-smp', '2',
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
                s = socket.socket(socket.AF_UNIX)
                s.connect(path)
                return s
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f'no socket {path}')


def qrl(qmp):
    buf = b''
    while not buf.endswith(b'\n'):
        c = qmp.recv(4096)
        if not c:
            raise RuntimeError('qmp closed')
        buf += c
    return json.loads(buf)


def qsend(qmp, obj):
    qmp.sendall((json.dumps(obj) + '\n').encode())
    while True:
        m = qrl(qmp)
        if 'return' in m or 'error' in m:
            return m


def move(qmp, x, y):
    ax = int(x * ABS_MAX / FB_W)
    ay = int(y * ABS_MAX / FB_H)
    qsend(qmp, {
        'execute': 'input-send-event',
        'arguments': {
            'events': [
                {'type': 'abs', 'data': {'axis': 'x', 'value': ax}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': ay}},
            ]
        },
    })


def button(qmp, down):
    qsend(qmp, {
        'execute': 'input-send-event',
        'arguments': {
            'events': [
                {'type': 'btn',
                 'data': {'down': bool(down), 'button': 'left'}},
            ]
        },
    })


def left_click(qmp, x, y):
    move(qmp, x, y)
    time.sleep(0.05)
    button(qmp, True)
    time.sleep(0.05)
    button(qmp, False)


def drain(s, deadline):
    out = b''
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            c = s.recv(8192)
            if not c:
                break
            out += c
    return out


def wait_for(s, needle, timeout):
    if isinstance(needle, str):
        needle = needle.encode()
    deadline = time.time() + timeout
    buf = b''
    while time.time() < deadline:
        buf += drain(s, time.time() + 0.4)
        if needle in buf:
            return buf
    return buf


def dump(qmp, path):
    if os.path.exists(path):
        os.unlink(path)
    qsend(qmp, {'execute': 'screendump', 'arguments': {'filename': path}})
    end = time.time() + 5
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 100:
            time.sleep(0.2)
            return
        time.sleep(0.1)
    raise RuntimeError(f'no screendump {path}')


def read_ppm(path):
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        if magic != b'P6':
            raise RuntimeError(f'unexpected PPM magic {magic}')
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        w, h = (int(x) for x in line.split())
        f.readline()
        return w, h, f.read()


def px(ppm, x, y):
    w, h, d = ppm
    o = (y * w + x) * 3
    return (d[o], d[o + 1], d[o + 2])


def main():
    q = boot()
    try:
        ser = conn(SER)
        qmp = conn(QMP)
        qrl(qmp)
        qsend(qmp, {'execute': 'qmp_capabilities'})

        # Wait for desktop to settle.
        if b'$ ' not in wait_for(ser, b'$ ', 30.0):
            print('FAIL: shell never appeared')
            return 1
        time.sleep(1.0)

        # Park cursor away first so the launcher hover state
        # doesn't bias the click.
        move(qmp, 5, 5)
        time.sleep(0.3)

        # Launcher is hidden at boot -- summon it by clicking
        # the taskbar's Start button.  The Start-click ->
        # wsd-restore -> launcher-repaint hop is racy under
        # sweep load, so click and wait for the serial line
        # the taskbar prints when it dispatches the toggle.
        print(f'  clicking Start button at '
              f'({START_CX}, {START_CY}) to summon launcher')
        left_click(qmp, START_CX, START_CY)
        wait_for(ser, b'start -> show launcher', 3.0)
        time.sleep(0.5)

        # Click browser button.
        print(f'  clicking launcher browser button at '
              f'({PAINT_X}, {BROWSER_BTN_Y})')
        left_click(qmp, PAINT_X, BROWSER_BTN_Y)

        # Wait for browser's first render.  Browser logs
        # "[timing] first frame" in --timing mode, else logs
        # other progress.  We look for the BIND message wsd
        # emits when browser binds its window.
        log = wait_for(ser, b'[wsd] bind win=', 20.0)
        if b'[wsd] bind win=' not in log:
            print('FAIL: browser did not bind a window in 20s')
            return 1
        # Give the parser thread time to finish the first
        # render of the page.
        time.sleep(3.0)
        # Park cursor away.
        move(qmp, 5, 5)
        time.sleep(0.8)
        dump(qmp, '/tmp/cb_baseline.ppm')
        baseline = read_ppm('/tmp/cb_baseline.ppm')

        # Browser window: created by wsd with default size
        # 1024x720 (per browser.c WIN_W/WIN_H).  Because the
        # launcher uses wm_create_window_at (does NOT consume
        # a cascade slot), the browser is the first
        # cascade-positioned client and lands at slot 0 =
        # (100, 100).  Body starts at y = 100 + 24 = 124.
        # Toolbar occupies first BR_TB_H ~= 32 px of body,
        # so actual content is from y ~= 156 down.
        BR_X = 100
        BR_Y = 100
        BR_TITLE_H = 24                           # decorated window
        BR_BODY_X0 = BR_X
        BR_BODY_Y0 = BR_Y + BR_TITLE_H + 32   # skip toolbar
        BR_BODY_X1 = min(BR_X + 1024, FB_W)
        BR_BODY_Y1 = min(BR_Y + 720,  FB_H)

        print(f'  browser body region '
              f'({BR_BODY_X0},{BR_BODY_Y0}) .. '
              f'({BR_BODY_X1},{BR_BODY_Y1})')

        # Sweep cursor across multiple Y bands through the body.
        # 18-px sprite height, so step Y by ~6 to ensure
        # overlap between sweeps (catches save buffer offset
        # bugs).  Sweep X coarsely.  Insert a small sleep
        # between moves so wsd's poller actually paints the
        # sprite at each intermediate position (simulating
        # real user motion -- without the sleep, the poller
        # might see only the FINAL position and skip all
        # intermediate save/restore cycles).
        for y in range(BR_BODY_Y0 + 10, BR_BODY_Y1 - 30, 6):
            for x in range(BR_BODY_X0 + 10, BR_BODY_X1 - 30, 20):
                move(qmp, x, y)
                time.sleep(0.01)

        # Now park cursor far away and let things settle.
        move(qmp, 5, 5)
        time.sleep(1.5)
        dump(qmp, '/tmp/cb_after.ppm')
        after = read_ppm('/tmp/cb_after.ppm')

        # Diff every body pixel.
        diffs = []
        for x in range(BR_BODY_X0, BR_BODY_X1, 2):
            for y in range(BR_BODY_Y0, BR_BODY_Y1, 2):
                pb = px(baseline, x, y)
                pa = px(after, x, y)
                if pb != pa:
                    diffs.append((x, y, pb, pa))

        print()
        print(f'baseline: /tmp/cb_baseline.ppm')
        print(f'after:    /tmp/cb_after.ppm')
        print(f'body diffs (2-px sample over '
              f'{BR_BODY_X1 - BR_BODY_X0}x{BR_BODY_Y1 - BR_BODY_Y0}): '
              f'{len(diffs)}')
        for d in diffs[:20]:
            print(f'  ({d[0]},{d[1]}): base={d[2]}  after={d[3]}')

        if diffs:
            print()
            print('FAIL: cursor sweep distorted browser body content')
            return 1

        print()
        print('PASS: browser body pixels identical to baseline '
              'after cursor sweep')
        return 0
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()
        cleanup()


if __name__ == '__main__':
    sys.exit(main())
