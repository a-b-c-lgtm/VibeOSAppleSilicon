#!/usr/bin/env python3
"""Regression: chapter 118 follow-up #5 -- title-bar damage repair.

Boots the OS, waits for the launcher to appear, takes a baseline
screendump with the cursor parked OFF the launcher, then drives
the virtio-tablet to drag the cursor ACROSS the launcher's
TITLE BAR (the row containing the word "launcher"), then back
OFF, and re-dumps.

The launcher's title bar pixels along the cursor's path MUST
match the baseline byte-for-byte after the cursor moves away.

This catches two bugs that were both live before this chapter:

  A. compose_rect skipped paint_decoration_clipped entirely
     for any window with fb_va == 0.  Decoration is wsd-owned
     (bar bg + text + buttons drawn from constants) and does
     NOT need the per-window FB; gating it on fb_va meant a
     transient zero VA (e.g. between resize-Phase-5 reinstall
     and client remap) would wipe the bar and never restore it.

  B. paint_decoration_clipped's title text was DROPPED when the
     dirty clip rect's left edge was past tx (= bar_x + 8).
     The cursor's union rect during cursor_move_only is exactly
     this case for any cursor-over-title sweep that doesn't
     include the bar's leftmost 8 px -- the cfill_rect repainted
     the bar bg over the swept strip, the text was skipped, and
     the launcher's periodic damage (which DOES include the
     bar's left edge) only arrived if the launcher had a reason
     to re-render.  In practice the launcher renders only on
     hover-state change, so the wiped text strip persisted
     indefinitely once the cursor parked outside any button.

Logged as test_cursor_over_title per user policy
"Always keep your debug scripts."
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMP  = '/tmp/osdev-ct-qmp.sock'
SER  = '/tmp/osdev-ct-ser.sock'

for p in (QMP, SER):
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass

q = subprocess.Popen([
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
    '-device', 'virtio-gpu-device,xres=1280,yres=800',
    '-device', 'virtio-keyboard-device',
    '-device', 'virtio-tablet-device',
    '-drive', f'if=none,file={ROOT}/build/disk.img,format=raw,id=hd0',
    '-device', 'virtio-blk-device,drive=hd0',
    '-drive', f'if=none,file={ROOT}/build/data.img,format=raw,id=hd1',
    '-device', 'virtio-blk-device,drive=hd1',
    '-kernel', f'{ROOT}/build/kernel.elf',
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def waitsock(path, timeout=15):
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


def qmp_send(sock, cmd):
    sock.sendall((json.dumps(cmd) + '\n').encode())
    time.sleep(0.05)
    try:
        while True:
            r, _, _ = select.select([sock], [], [], 0.05)
            if not r:
                break
            sock.recv(8192)
    except Exception:
        pass


def move(sock, x, y, w=1280, h=800):
    ax = int(x * 32767 / w)
    ay = int(y * 32767 / h)
    qmp_send(sock, {
        'execute': 'input-send-event',
        'arguments': {
            'events': [
                {'type': 'abs', 'data': {'axis': 'x', 'value': ax}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': ay}},
            ]
        },
    })


def dump(sock, path):
    if os.path.exists(path):
        os.unlink(path)
    qmp_send(sock, {
        'execute': 'screendump',
        'arguments': {'filename': path},
    })
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
        f.readline()  # maxval
        return w, h, f.read()


def px(ppm, x, y):
    w, h, d = ppm
    o = (y * w + x) * 3
    return (d[o], d[o + 1], d[o + 2])


def main():
    try:
        qmp = waitsock(QMP)
        ser = waitsock(SER)
        qmp.recv(8192)
        qmp_send(qmp, {'execute': 'qmp_capabilities'})

        # Wait for desktop to settle.
        ser.setblocking(False)
        log = b''
        end = time.time() + 30
        while time.time() < end:
            r, _, _ = select.select([ser], [], [], 0.2)
            if r:
                try:
                    d = ser.recv(8192)
                    if d:
                        log += d
                        sys.stdout.write(d.decode(errors='replace'))
                        sys.stdout.flush()
                except Exception:
                    pass
            if log.count(b'[wsd] compose_all') >= 2:
                break
        time.sleep(1.5)

        # Park cursor far from the launcher.
        move(qmp, 5, 5)
        time.sleep(0.7)
        dump(qmp, '/tmp/ct_baseline.ppm')
        baseline = read_ppm('/tmp/ct_baseline.ppm')

        # Launcher is at (100, 100) (first cascade window).
        # Title bar covers (100, 100)..(340, 124).  The word
        # "launcher" starts at x=108 (bar_x + 8 px padding).
        # Drag cursor across the title text, staying inside
        # the bar's vertical band.
        path = [(x, 110) for x in range(120, 335, 10)]
        path += [(x, 110) for x in range(330, 115, -10)]
        for (x, y) in path:
            move(qmp, x, y)
            time.sleep(0.06)

        # Park cursor far away again so the title bar pixels
        # are free of sprite overlay in the after-shot.
        move(qmp, 5, 5)
        time.sleep(1.2)
        dump(qmp, '/tmp/ct_after.ppm')
        after = read_ppm('/tmp/ct_after.ppm')

        # Compare every pixel inside the launcher's title bar
        # (excluding the cursor's parked footprint at (5,5),
        # which is well outside the bar anyway).
        bar_diffs = []
        for x in range(100, 340):
            for y in range(100, 124):
                pb = px(baseline, x, y)
                pa = px(after, x, y)
                if pb != pa:
                    bar_diffs.append((x, y, pb, pa))

        print()
        print(f'baseline: /tmp/ct_baseline.ppm')
        print(f'after:    /tmp/ct_after.ppm')
        print(f'title-bar diffs: {len(bar_diffs)}')
        for d in bar_diffs[:12]:
            print(f'  diff @ ({d[0]},{d[1]}): '
                  f'base={d[2]} after={d[3]}')

        if bar_diffs:
            print()
            print('FAIL: title bar pixels changed after cursor '
                  'sweep across title -- damage was not repaired')
            sys.exit(1)
        print()
        print('PASS: title bar identical to baseline after '
              'cursor sweep across title')
    finally:
        try:
            q.terminate()
            q.wait(timeout=3)
        except Exception:
            q.kill()
        for p in (QMP, SER):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass


if __name__ == '__main__':
    main()
