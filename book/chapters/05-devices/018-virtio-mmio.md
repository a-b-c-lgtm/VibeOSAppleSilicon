# Chapter 18 — virtio-mmio: bus, queues, and the modern transport

## Why virtio is the right place to start Part V

Up to this point our kernel has talked to exactly two real devices:
the PL011 UART (chapter 3) and the GIC (chapter 8). Both are
character-stream interfaces — push a byte, ack an interrupt. They're
fine for boot, terminals, and timer ticks, but they will not get us
to a filesystem, a network, or a framebuffer. For that we need a
bus, a transport, and devices that move *blocks* of memory back and
forth.

There are roughly three serious choices on QEMU's `virt` machine:

1. **PCIe.** Real hardware, real complexity. Configuration space,
   BARs, message-signalled interrupts, capability lists, and a
   nontrivial host bridge. We will get here eventually because PCIe
   is what you find on real ARM SBCs. We won't get here today.
2. **virtio-pci.** virtio devices riding on PCIe. Same complexity
   as PCIe plus the virtio protocol on top.
3. **virtio-mmio.** virtio devices on a flat bank of MMIO pages.
   No bus enumeration, no configuration space, no MSI. Each device
   gets its own 0x200-byte register block at a known physical
   address. The same protocol (queues, descriptors, status
   handshake) as virtio-pci.

We pick **virtio-mmio**. It is the lowest-friction transport that
gives us access to *all* the QEMU virtual devices we want for the
rest of the book: blk, console, input, gpu, net. Two pages of code
for the transport, then the only thing that varies per-device is
the configuration register layout and the request format.

## Where the bus lives

QEMU's `virt` machine puts the virtio-mmio bus at physical
`0x0a000000`. It's already mapped device memory in our static L1
table — see [`kernel/arch/page_tables.c`](../../../kernel/arch/page_tables.c),
which covers all of `[0, 1 GiB)` as a single Device-nGnRnE block.
Slot N is at `0x0a000000 + N * 0x200`. There are 32 slots; their
IRQs are GIC SPI 16+N.

Empty slots are *not* removed from the address map. If you read
`MagicValue` from any slot you get `0x74726976` ("virt"). What
distinguishes "device present" from "slot empty" is the
`DeviceID` register: 0 for empty, 1 = network, 2 = block, 3 =
console, 18 = input, 16 = gpu, etc.

That's important — our probe loop walks slots 0..31 and looks at
`DeviceID`, not `MagicValue`.

## Modern (v2) vs legacy (v1) transport

`virtio-mmio` has had two incompatible register layouts.

- **Legacy (v1).** Pre-1.0 spec. Driver-side guest physical address
  is published as a single 32-bit `QueuePFN` value (queue page-frame
  number, multiplied by guest page size). The descriptor table,
  available ring, and used ring are laid out *contiguously* in one
  region whose size depends on the queue depth. Endianness is
  whatever the host chose. Lots of "interesting" feature bits.
- **Modern (v2).** virtio-1.0 and later. Three independent
  64-bit register pairs publish the descriptor table, available
  ring, and used ring at any aligned addresses. Endianness is
  little-endian. `VIRTIO_F_VERSION_1` is mandatory. Cleaner,
  simpler, more flexible.

We implement only v2. The price is one QEMU command-line flag:

```
-global virtio-mmio.force-legacy=off
```

Without that flag, QEMU presents the bus in legacy mode, our
probe sees `Version = 1`, and we refuse the device. With it, we
see `Version = 2` and the modern register layout works as
specified.

The Makefile bakes the flag into all `make run` / `make run-tcg` /
`make debug` targets so you can't forget.

## The probe loop

[`kernel/device/virtio_blk.c`](../../../kernel/device/virtio_blk.c)
opens with:

```c
for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
    uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
    if (probe_slot(base)) {
        ...
    }
}
```

where `probe_slot` is:

```c
if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u) return 0;
if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u) return 0;
if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_BLOCK) return 0;
return 1;
```

Running this probe on QEMU 11 with default flags, the
probe finds a slot with `Version = 1, DeviceID = 2`. That is
the legacy transport. Adding `-global virtio-mmio.force-legacy=off`
flips `Version` to `2`. The book code only ships the v2 path,
so the flag is mandatory.

In our run the device shows up in **slot 31** (`base = 0x0a003e00`).
QEMU populates virtio-mmio slots from the *top* down — slot 31
first, then 30, etc. — which is mildly counterintuitive but not
something we depend on. The probe loop walks all 32 slots either
way.

## The status handshake

Modern virtio brings every device up through the same six-step
status handshake. The bits live in the `Status` register; once
set, they are sticky until the next `Status = 0` reset.

```
1. Status = 0                                       // RESET
2. Status |= ACKNOWLEDGE                            // we see the device
3. Status |= DRIVER                                 // we know how to drive it
4. Negotiate features:
     read Device features (high & low halves)
     write Driver features (high & low halves)
5. Status |= FEATURES_OK                            // commit feature set
   if (Status & FEATURES_OK == 0) FAIL              // device rejected our set
6. Per-device init: virtqueues, config reads, etc.
7. Status |= DRIVER_OK                              // device may now be used
```

If anything goes wrong at any step (we don't get a feature we need,
the device clears `FEATURES_OK`, etc.) we set `FAILED` and walk
away. The kernel function that does all of this is `init_device()`
in [`kernel/device/virtio_blk.c`](../../../kernel/device/virtio_blk.c).

For this bring-up we negotiate exactly one feature: `VIRTIO_F_VERSION_1`.
This is required for modern. It lives at bit 32, so we have to
write the high half of `DriverFeatures`:

```c
w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
uint32_t hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
if (!(hi & 1u)) FAIL;       /* no VERSION_1, refuse */

w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);   /* low half: nothing */
w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
w32(VIRTIO_MMIO_DRIVER_FEATURES, 1);   /* high half: VERSION_1 */
```

We deliberately don't accept any optional feature. That keeps the
request format predictable: 16-byte header + 512-byte data + 1-byte
status, three descriptors per request. If we ever need
`VIRTIO_BLK_F_FLUSH` (a real flush command) or
`VIRTIO_BLK_F_RO` (read-only enforcement), this is the place to
opt in.

## Virtqueues — what they actually are

A virtqueue is a tiny single-producer, single-consumer ring system
that lives in a chunk of guest physical memory both sides agree on.
There are three rings per queue:

- **Descriptor table** — array of `QueueNum` entries, each 16
  bytes. Each entry is `{addr, len, flags, next}`. A request is
  encoded as a chain of one or more descriptors linked through
  `next` and the `NEXT` flag bit.

- **Available ring** — driver-to-device. Entry N tells the device
  "descriptor index `ring[N]` is the head of a request you should
  process." `idx` is bumped by the driver after publishing.

- **Used ring** — device-to-driver. Entry N tells the driver
  "I finished the request whose head was descriptor index `id`,
  and wrote `len` bytes back." `idx` is bumped by the device after
  publishing.

The driver and device both maintain a private `last_idx` cursor.
When `device.idx > driver.last_seen_idx`, there's new work for the
driver to drain. Likewise the device drains the avail ring.

Modern virtio doesn't constrain how the three rings are physically
laid out. Three separate registers publish the addresses:

| Register pair | Points to |
|--|--|
| `QueueDescLo/Hi`   | descriptor table |
| `QueueDriverLo/Hi` | available ring (driver writes) |
| `QueueDeviceLo/Hi` | used ring (device writes) |

We allocate one 4 KiB page from `pmem_alloc_page()` and carve it:

```
   offset 0x000  descriptor table  (8 entries × 16 = 128 bytes)
   offset 0x080  avail ring        (4 + 16 + 2  =  22 bytes)
   offset 0x0C0  used ring         (4 + 64 + 2  =  70 bytes)
   offset 0x200  request header    (16 bytes)
   offset 0x210  status byte
   offset 0x400  512-byte data buffer
```

A real driver would allocate the rings separately (there's no
*requirement* they share a page) and would have one such
{header, status, data} buffer set per outstanding request. We have
exactly one outstanding request at a time so one set is enough.

## Device memory? Cacheable memory? It matters.

The MMIO registers live in Device-nGnRnE memory (programmed by our
L1 device block). The shared queue page lives in **Normal,
Inner-Shareable, write-back, write-allocate** memory — that's the
attribute we map kernel RAM with.

This is correct, but it means we *must* place explicit barriers
between writes the device should see in order:

```c
av->ring[slot] = 0;            /* publish head */
dmb();                         /* writes drain before idx bump */
av->idx = g_avail_idx_seen;
dmb();                         /* idx visible before kick */
w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
```

The `dmb sy` here ensures the device, when it eventually reads
our ring, sees a consistent snapshot. Without the first barrier
the CPU could speculate the `av->idx` store ahead of the
`av->ring[slot]` store, and the device would see "queue has new
work at this slot" but read garbage.

The same applies on the receive side: we `dmb()` before checking
the device's used ring, so we don't read a stale cached value:

```c
for (...) {
    dmb();
    if (u->idx == target) goto got_it;
}
```

This is the polled version. With interrupts the IRQ handler does
the read; the architectural fence inside the eret path covers most
of the visibility, but a `dmb` is still good hygiene.

## Why one big shared page is fine

Every byte at `g_blk_page` is a guest physical address that the
device reads or writes via DMA-style accesses. On a real machine
those accesses go through an IOMMU and need pinning. On QEMU virt
without an IOMMU there is nothing to pin: the host hypervisor sees
the same pages.

We use `pmem_alloc_page()` because that page is identity-mapped at
its physical address (chapter 6) and we never relocate it.
Different mmap-style allocator? We'd need to be careful that the
PA we hand the device is the actual PA, not a VA.

## Recap

- virtio-mmio is a flat bank of 0x200-byte MMIO blocks at
  `0x0a000000`, one per device. Probe by walking the bank and
  looking at `DeviceID`.
- We implement the modern (v2) transport. QEMU defaults to
  legacy (v1); pass `-global virtio-mmio.force-legacy=off`.
- Bring-up is a six-step `Status` handshake, with a single mandatory
  feature for modern: `VIRTIO_F_VERSION_1`.
- Each queue has three rings — descriptors, avail, used — at
  arbitrary aligned addresses. We share one 4 KiB page among
  them and the per-request buffers.
- The shared page is cacheable Normal memory. Use `dmb sy`
  between publishing data and bumping the producer index, and
  again before checking the consumer index.

Chapter 19 takes everything in this chapter and uses it to do a
single read and a single write on a real disk.
