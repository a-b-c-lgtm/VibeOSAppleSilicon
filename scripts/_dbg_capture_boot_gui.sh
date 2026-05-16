#!/bin/bash
# scripts/_dbg_capture_boot_gui.sh — chapter 81 helper.
# Same as _dbg_capture_boot.sh but with virtio-gpu / kbd / tablet
# attached, so the desktop process actually has a screen and we
# can see its OOM / chunk-buffer messages.
set -u
DUR="${1:-6}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
pkill -9 -f qemu-system-aarch64 2>/dev/null
sleep 1
rm -f /tmp/raw.log
qemu-system-aarch64 \
    -M virt,gic-version=3 -cpu host -accel hvf -m 8G -display none \
    -serial file:/tmp/raw.log \
    -global virtio-mmio.force-legacy=off \
    -device loader,file="$ROOT/assets/virt.dtb",addr=0x44000000 \
    -device virtio-gpu-device,xres=1280,yres=800 \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -drive if=none,file="$ROOT/build/disk.img",format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive if=none,file="$ROOT/build/data.img",format=raw,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -kernel "$ROOT/build/kernel.elf" >/dev/null 2>&1 &
sleep "$DUR"
QPID=$(pgrep -n -f qemu-system-aarch64)
[[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null
wait 2>/dev/null
echo "---- desktop / chunk / sbrk ----"
grep -iE 'desktop|chunk|sbrk|wallpaper|FATAL' /tmp/raw.log | head -40
echo "---- end ----"
