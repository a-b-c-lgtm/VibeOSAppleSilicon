#!/usr/bin/env bash
# Regenerate assets/virt.dtb.
#
# QEMU's `dumpdtb` only includes the /memory node when QEMU runs
# its Linux boot-protocol loader, which it does when `-kernel`
# points at something that isn't a valid ELF.  We exploit that
# by passing `-kernel /dev/null`: the load fails harmlessly, but
# only after dumpdtb has captured a DTB that has /memory@40000000
# already populated to match `-m 2G`.
#
# Re-run this whenever the QEMU virt machine layout changes
# (new device, different gic-version, different RAM size, etc.).
set -euo pipefail
cd "$(dirname "$0")/.."

# RAM size baked into the dumped DTB.  Must match (or exceed) the
# `-m` value passed to QEMU when running the kernel; the kernel
# trusts the DTB as the authoritative memory map and only adds
# pages it explicitly sees there.  Override with QEMU_MEM=16G ...
QEMU_MEM=${QEMU_MEM:-8G}

# CPU count baked into the dumped DTB's /cpus node.  Chapter 87
# (PSCI secondary boot) reads this to decide how many cores to
# wake.  Must match (or be >= ) the `-smp` value passed to QEMU
# when running the kernel; extra DTB cpus that aren't actually
# present cause psci_cpu_on to return -7 (NOT_PRESENT) which is
# logged but non-fatal.  Override with QEMU_SMP=4 for stress.
QEMU_SMP=${QEMU_SMP:-2}

mkdir -p assets

qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=assets/virt.dtb \
    -cpu cortex-a72 -accel tcg -m "$QEMU_MEM" -smp "$QEMU_SMP" -nographic \
    -kernel /dev/null >/dev/null 2>&1 || true

# Sanity-check: confirm /memory is present.
if ! dtc -I dtb -O dts assets/virt.dtb 2>/dev/null | grep -q "memory@"; then
    echo "build_dtb.sh: /memory node missing from generated DTB" >&2
    exit 1
fi

echo "regenerated assets/virt.dtb"
