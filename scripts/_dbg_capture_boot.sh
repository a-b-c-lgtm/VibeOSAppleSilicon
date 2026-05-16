#!/bin/bash
# scripts/_dbg_capture_boot.sh — chapter 81 helper.
# Boots the kernel headless with both disks attached, captures
# serial output to /tmp/raw.log, kills QEMU after $1 seconds
# (default 4), and prints a filtered grep of the relevant boot
# lines.  Used to diagnose virtio-blk probe-order issues without
# fighting bash history-expansion of `$!`.
set -u
DUR="${1:-4}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
pkill -9 -f qemu-system-aarch64 2>/dev/null
sleep 1
rm -f /tmp/raw.log
qemu-system-aarch64 \
    -M virt,gic-version=3 -cpu host -accel hvf -m 8G -nographic \
    -serial file:/tmp/raw.log \
    -global virtio-mmio.force-legacy=off \
    -device loader,file="$ROOT/assets/virt.dtb",addr=0x44000000 \
    -drive if=none,file="$ROOT/build/disk.img",format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive if=none,file="$ROOT/build/data.img",format=raw,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -kernel "$ROOT/build/kernel.elf" >/dev/null 2>&1 &
QPID=$?
QPID=$(pgrep -n -f qemu-system-aarch64)
sleep "$DUR"
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null
echo "---- filtered serial output ----"
grep -iE 'virtio-blk|osfs|FATAL|mounting|init started|/bin/init' /tmp/raw.log | head -60
echo "---- end ----"
