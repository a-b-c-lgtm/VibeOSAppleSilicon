#!/bin/bash
# scripts/_dbg_qemu_socket_diag.sh — chapter 130b sweep-crash diagnosis.
#
# Tests fail with "no serial socket: /tmp/osdev-...sock" but no qemu
# process exists. This script launches one qemu the same way
# scripts/test_echod.py does, redirecting stderr to a file, so the
# real failure reason is captured.
set +e
cd "$(dirname "$0")/.."

# Full clean shutdown
pkill -9 -f sweep.sh 2>/dev/null
pkill -9 -f 'python3 scripts/test_' 2>/dev/null
pkill -9 qemu-system-aarch64 2>/dev/null
sleep 3

echo "=== still running? ==="
pgrep -fl 'sweep|qemu-system|python3 scripts/test_' | head -10
echo "(end)"

echo
echo "=== stale sockets in /tmp ==="
ls /tmp/osdev*.sock /tmp/qmp-*.sock 2>/dev/null | head -30
rm -f /tmp/osdev*.sock /tmp/qmp-*.sock
echo "removed."

echo
echo "=== launch one qemu the way test_echod does ==="
SOCK=/tmp/osdev-serial-echod.sock
rm -f "$SOCK"
rm -f /tmp/qemu_diag_stdout.txt /tmp/qemu_diag_stderr.txt

qemu-system-aarch64 \
    -M virt,gic-version=3 -cpu host -accel hvf \
    -m 8G -smp 2 -display none \
    -serial unix:$SOCK,server,nowait \
    -global virtio-mmio.force-legacy=off \
    -device loader,file=$PWD/assets/virt.dtb,addr=0x44000000 \
    -drive if=none,file=$PWD/build/disk.img,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -drive if=none,file=$PWD/build/data.img,format=raw,id=hd1 \
    -device virtio-blk-device,drive=hd1 \
    -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
    -kernel $PWD/build/kernel.elf \
    > /tmp/qemu_diag_stdout.txt 2> /tmp/qemu_diag_stderr.txt &
QPID=$!
echo "qemu launched PID=$QPID"

sleep 6

echo
echo "=== qemu alive after 6s? ==="
if kill -0 $QPID 2>/dev/null; then
    echo "ALIVE (PID=$QPID)"
else
    wait $QPID 2>/dev/null
    echo "DEAD, exit=$?"
fi

echo
echo "=== socket created? ==="
ls -la "$SOCK" 2>&1
echo
echo "=== qemu stdout (first 60 lines) ==="
head -60 /tmp/qemu_diag_stdout.txt
echo "(stdout size=$(wc -c < /tmp/qemu_diag_stdout.txt 2>/dev/null) bytes)"
echo
echo "=== qemu stderr (first 60 lines) ==="
head -60 /tmp/qemu_diag_stderr.txt
echo "(stderr size=$(wc -c < /tmp/qemu_diag_stderr.txt 2>/dev/null) bytes)"

echo
echo "=== cleanup ==="
kill -9 $QPID 2>/dev/null
rm -f "$SOCK"
echo "DONE"
