# Chapter 20 — virtio-blk and persistent storage

This chapter takes the virtio-mmio framework from chapter 19 and
uses it to issue exactly two operations on a real block device: a
read and a write. By the end, the kernel can:

- Negotiate features with the QEMU virtio-blk device.
- Read the device's capacity (in 512-byte sectors) from its
  configuration space.
- Build a 3-descriptor request chain, kick the device, and poll
  the used ring for completion.
- Round-trip a sector to disk and back, verifying the data
  matches.

A real filesystem on top of this is the next milestone. Today we
just prove the wire works.

## The disk image

QEMU's virtio-blk device needs a backing store. The Makefile
creates a 1 MiB raw image at `build/disk.img` (see the [Makefile](../../../Makefile))
on demand, with sector 0 stamped with a recognizable magic
string:

```make
$(DISK):
	@printf 'OSDEV virtio-blk sector 0 — milestone 11\n' > $(DISK).sector0
	@dd if=/dev/zero of=$(DISK) bs=512 count=2048 status=none
	@dd if=$(DISK).sector0 of=$(DISK) bs=512 count=1 conv=notrunc status=none
	@rm -f $(DISK).sector0
```

The QEMU command-line wires it in via the standard two-piece
syntax — backend (`-drive`) and frontend (`-device`):

```
-drive  if=none,file=build/disk.img,format=raw,id=hd0
-device virtio-blk-device,drive=hd0
```

`if=none` tells QEMU not to attach the drive to a default
controller. The `-device` line then attaches it explicitly to a
new virtio-blk-device on the next free virtio-mmio slot.

In our run that turns out to be **slot 31** — QEMU populates
virtio-mmio slots top-down. The base address is therefore
`0x0a000000 + 31 * 0x200 = 0x0a003e00`.

## The block-device configuration

Once we set the `Status` bits up to `FEATURES_OK` (chapter 19),
the device's per-type configuration space at offset `0x100` from
the MMIO base becomes meaningful. For virtio-blk, the layout
starts with:

| Offset | Size | Field |
|--------|------|-------|
| 0x00   | 8    | capacity (in 512-byte sectors, little-endian) |
| 0x08   | 4    | size_max (only valid if `SIZE_MAX` feature negotiated) |
| 0x0C   | 4    | seg_max |
| ...    |      | (more, all gated on optional features) |

We only read capacity:

```c
uint32_t lo  = mmio_read32(base + VIRTIO_MMIO_CONFIG + 0);
uint32_t hi2 = mmio_read32(base + VIRTIO_MMIO_CONFIG + 4);
g_blk_capacity = ((uint64_t)hi2 << 32) | lo;
```

Two 32-bit reads instead of one 64-bit read because the
`mmio_read64` helper isn't strictly necessary here and using it
would risk picking up an unaligned access; the device-config
range is `Device-nGnRnE` like the rest of the bus, and 64-bit
loads against Device memory are technically permitted but worth
avoiding when you don't need them.

For our 1 MiB image the kernel prints:

```
[virtio-blk] ready, capacity = 0x800 sectors (0x100000 bytes)
```

That's 2048 × 512 = 1 048 576 bytes. ✓

## The request format

Every virtio-blk request starts with the same 16-byte header:

```c
struct virtio_blk_req_hdr {
    uint32_t type;        // 0=in (read), 1=out (write), 4=flush, ...
    uint32_t reserved;
    uint64_t sector;      // LBA of first sector (always 512-byte units)
};
```

Followed by `n × 512` bytes of data (driver-owned for writes,
device-owned for reads). Followed by a 1-byte status returned by
the device:

```c
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2
```

These three buffers are always *separate descriptors* in the
chain. They are not a single struct laid out contiguously. (The
header and status could be — but the data buffer typically lives
in some user-supplied page far away, and the status byte is
device-writable while the header is not.)

## The 3-descriptor chain

Here is the descriptor table after we've prepared a read request
for sector 5:

```
desc[0]: addr = page_pa + 0x200,    // header
         len  = 16,
         flags = NEXT,                  next = 1

desc[1]: addr = page_pa + 0x400,    // 512-byte data buffer
         len  = 512,
         flags = NEXT | WRITE,          next = 2     // device writes for read

desc[2]: addr = page_pa + 0x210,    // 1-byte status
         len  = 1,
         flags = WRITE,                 next = 0     // device writes status
```

For a write, `desc[1].flags` drops the `WRITE` bit (since the
*driver* writes the data buffer, not the device). The header is
always driver-owned; the status is always device-owned.

This convention takes a moment to settle in: `WRITE` here means
"the *device* writes this descriptor's buffer." Driver-writable
descriptors don't have any flag — that's the default.

## Publishing the request

After populating descriptors 0..2 we publish the chain head into
the available ring at our running index, then bump `idx` so the
device sees it:

```c
struct vring_avail *av = avail_ring();
uint16_t slot = g_avail_idx_seen % QUEUE_SIZE;
av->ring[slot] = 0;            // head descriptor index
dmb();                         // make ring slot visible before idx bump
g_avail_idx_seen++;
av->idx = g_avail_idx_seen;
dmb();
w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);   // kick queue 0
```

`g_avail_idx_seen` is our private cursor. It and `av->idx` always
agree; the only reason for the extra variable is so we can wrap
correctly (`% QUEUE_SIZE`) without having to load the device-
visible `av->idx` from cacheable memory.

The two `dmb` barriers matter (see chapter 19 for the
discussion). Without them the device may read stale entries.

## Polling for completion

A real driver would set up an interrupt handler. We poll. The
spin loop is simple and bounded so a misbehaving device can't
wedge the kernel forever:

```c
struct vring_used *u = used_ring();
uint16_t target = g_used_idx_seen + 1;
for (uint64_t spin = 0; spin < (1ULL << 28); spin++) {
    dmb();
    if (u->idx == target) goto got_it;
}
return -1;
```

When `u->idx` reaches the value we're waiting for, the device has
finished. The `dmb` inside the loop forces a fresh read each
iteration; without it the compiler could legitimately hoist the
load and we'd spin forever waiting on stale cache.

`u->ring[(target-1) % QUEUE_SIZE]` would tell us the head id and
the byte count, but we don't bother to inspect it — there is
exactly one outstanding request, and we only ever push descriptor
chain id = 0. Once we have multiple outstanding requests we'll
read the used-ring entry to find out which request finished.

After the device acks completion, we check the status byte and
either copy the data buffer into the caller's `buf` (read) or
just return success (write).

## Smoke test, end to end

[`kernel/core/main.c`](../../../kernel/core/main.c) calls
`virtio_blk_init()` after `vfs_init()` and, on success, runs two
checks:

1. Read sector 0 and dump the first 32 bytes as both ASCII and
   hex.
2. Write a stamped 512-byte pattern (`i ^ 0x5a`) to sector 1,
   read it back into a different buffer, and compare bit-for-bit.

Trimmed output:

```
probing virtio-mmio bus for a block device ... [virtio-blk] found block device at slot 0x1f base=0x0a003e00
[virtio-blk] ready, capacity = 0x800 sectors (0x100000 bytes)
ok
[blk] sector 0 first 32 bytes:
  OSDEV virtio-blk sector 0 ... mi
  hex: 4f 53 44 45 56 20 76 69 72 74 69 6f 2d 62 6c 6b 20 73 65 63 74 6f 72 20 30 20 e2 80 94 20 6d 69
[blk] sector 1 write+read round-trip OK
```

The last 6 hex bytes (`e2 80 94 20 6d 69`) are the UTF-8 encoding
of `"— mi"` — the em-dash in our seed string is `e2 80 94`. Kernel
serial prints non-ASCII as `.`, hence the `...` in the ASCII line.

## What this driver doesn't do

Each is a small, self-contained future improvement.

- **Interrupt-driven completion.** The whole point of a kernel
  with threads is that other threads make progress while one is
  blocked on I/O. Today the polling thread holds the CPU. Adding
  a sleep queue + the virtio-blk IRQ (SPI 16+slot, so SPI 47 for
  slot 31) is straightforward; the spin loop becomes
  `thread_block_on(&blk_wait)` and the IRQ handler walks the used
  ring and wakes the right thread.
- **Multiple outstanding requests.** Today we encode one chain
  starting at descriptor 0 and have one outstanding request. A
  real driver maintains a free list of descriptor indices and a
  per-request structure indexed by the head id from the used ring.
- **Multi-sector transfers.** A long read into a `> 512`-byte
  buffer needs either a multi-segment data descriptor (if the
  buffer is virtually-contiguous-but-physically-scattered) or
  the indirect-descriptor feature.
- **Flush.** virtio-blk's `FLUSH` request is how the driver tells
  the device "make sure prior writes are durable." We don't
  negotiate that feature today, so write durability is whatever
  QEMU defaults to (typically write-back to host page cache; data
  is durable to host RAM but not to disk until host fsync).
- **Read-only.** We don't honour `VIRTIO_BLK_F_RO`. If the host
  attaches a read-only image, our writes will fail with `IOERR`
  status and we'll just return -1.
- **DMA on a real machine with an IOMMU.** Real ARM SBCs route
  device DMA through a SMMU. Setting up SMMU mappings is a whole
  subsystem on its own; on QEMU virt without an IOMMU we don't
  need it.

## What this unlocks

Now that we can read and write arbitrary sectors of a real disk,
the next several milestones get to be filesystem-flavoured:

- A simple block cache (LRU, dirty bit, write-back).
- A read-only superblock and inode layout (FAT-12 or our own minimal
  format).
- Mounting the disk under VFS so `cat /mnt/foo` actually pulls
  bytes through `virtio_blk_read`.
- Eventually loading user binaries from disk instead of from the
  embedded ramfs, so we can stop rebuilding the kernel every time
  we change `/bin/sh`.

Chapter 21 will introduce virtio-console next — the same transport,
a different request format, and a serial path that doesn't depend
on the boot UART.
