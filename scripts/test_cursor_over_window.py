#!/usr/bin/env python3
"""Regression: chapter 118 cursor save/restore correctness.

Boots the OS, waits for the launcher to appear, takes a baseline
screendump with the cursor parked OFF the launcher, then drives
the virtio-tablet to drag the cursor ACROSS the launcher body
through several positions, then back OFF, and re-dumps.

The launcher pixels along the cursor's path MUST match the
baseline byte-for-byte after the cursor moves away.  If the
cursor sprite leaves trails or the save buffer captures stale
sprite pixels, those pixels stay distorted and this test fails.

This is the test that would have caught the "ghost sprite
trails on the browser body" bug the user reported in chapter
109b after the first sole-router rewrite.

Logged as test_cursor_over_window per user policy
"Always keep your debug scripts."  Not included in the bare
sweep until promoted (still test_*, not _dbg_*, because it
verifies a regression we explicitly want to lock in).
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMP  = '/tmp/osdev-cw-qmp.sock'
SER  = '/tmp/osdev-cw-ser.sock'

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
    """Move virtio-tablet to absolute scanout coords (x, y)."""
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
        # QMP greeting + caps
        qmp.recv(8192)
        qmp_send(qmp, {'execute': 'qmp_capabilities'})

        # Wait for launcher to bind to wsd.
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
            if b'[wsd] compose_all' in log:
                # Wait for at least 2 compose_all messages so we
                # know the desktop is settled.
                if log.count(b'[wsd] compose_all') >= 2:
                    break

        # Give the desktop a moment to settle.
        time.sleep(1.5)

        # Park cursor at top-left corner — well away from
        # launcher (which sits roughly at (100, 100)..(340, 360)
        # per cascade base 100,100 step 40,40 + title-bar shift).
        move(qmp, 5, 5)
        time.sleep(0.7)
        dump(qmp, '/tmp/cw_baseline.ppm')

        baseline = read_ppm('/tmp/cw_baseline.ppm')

        # Probe path: move cursor through 8 points on the
        # launcher body (avoiding the title bar at y=100..124).
        # Launcher body covers roughly x=100..340, y=124..360.
        # Sample row y=200 (middle of body) for x=120..320.
        path = [(x, 200) for x in range(120, 325, 25)]
        path += [(x, 250) for x in range(320, 115, -25)]
        for (x, y) in path:
            move(qmp, x, y)
            time.sleep(0.08)

        # Move cursor back OFF the launcher.
        move(qmp, 5, 5)
        time.sleep(1.0)
        dump(qmp, '/tmp/cw_after.ppm')

        after = read_ppm('/tmp/cw_after.ppm')

        # Compare every pixel along the cursor path region.
        # The cursor is 11x18, so any saved-pixel pair within
        # 18 px of the path counts as "touched".
        touched_xs = set()
        touched_ys = set()
        for (x, y) in path:
            for dx in range(-2, 13):
                touched_xs.add(x + dx)
            for dy in range(-2, 20):
                touched_ys.add(y + dy)

        diffs = []
        for x in sorted(touched_xs):
            for y in sorted(touched_ys):
                if x < 0 or y < 0 or x >= 1280 or y >= 800:
                    continue
                pb = px(baseline, x, y)
                pa = px(after, x, y)
                if pb != pa:
                    diffs.append((x, y, pb, pa))

        # Also do a coarse sweep over the whole launcher body
        # to catch any stray distortion outside the path.
        launcher_body_diffs = []
        for x in range(100, 340, 4):
            for y in range(124, 360, 4):
                pb = px(baseline, x, y)
                pa = px(after, x, y)
                if pb != pa:
                    launcher_body_diffs.append((x, y, pb, pa))

        print()
        print(f'baseline saved: /tmp/cw_baseline.ppm')
        print(f'after saved:    /tmp/cw_after.ppm')
        print(f'touched-region diffs: {len(diffs)}')
        for d in diffs[:10]:
            print(f'  diff @ ({d[0]},{d[1]}): base={d[2]} after={d[3]}')
        print(f'launcher-body diffs (4-px sample): '
              f'{len(launcher_body_diffs)}')
        for d in launcher_body_diffs[:10]:
            print(f'  diff @ ({d[0]},{d[1]}): base={d[2]} after={d[3]}')

        if diffs or launcher_body_diffs:
            print()
            print('FAIL: cursor left ghost trails on the launcher body')
            sys.exit(1)
        print()
        print('PASS: launcher body pixels identical to baseline '
              'after cursor sweep')
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
