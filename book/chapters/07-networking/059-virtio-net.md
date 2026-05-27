# Chapter 59 — virtio-net: getting bytes on and off the wire

This chapter opens Part VII of the book. From here on, our kernel
is no longer alone: it can talk to other machines. The whole part
walks the same vertical slice the rest of the book has used —
*one device, one syscall, one user program at a time* — until we
have a working text-mode browser in chapter 64.

The first stop is the same one every networking stack has to make:
a driver that can hand a frame to a wire and get a frame back.

## What we are building

A polling-only virtio-mmio network driver, mirroring the structure
of `virtio_blk` and `virtio_input`. By the end of this chapter:

- `kernel/device/virtio_net.c` walks the mmio bus, finds a NIC,
  negotiates features, and sets up two virtqueues (RX = 0,
  TX = 1).
- A new probe in `kernel/core/main.c` reports the discovered MAC
  and runs an in-kernel **ARP self-test**: it builds a 42-byte
  broadcast ARP request from `10.0.2.15`, hands it to the
  driver, then polls the RX queue waiting for the SLIRP gateway
  at `10.0.2.2` to answer.

Crucially, we have **no Ethernet, no ARP, no IPv4 stack yet**.
That's all for later chapters. What we are validating in this chapter
is exactly the driver: `virtio_net_tx()` reaches the wire and
`virtio_net_drain_rx()` produces frames the wire put on us.

The self-test exists because shipping a driver "without bugs you
have noticed" is not the same as shipping one that works.

## Why MMIO and not PCI

QEMU's virt machine exposes virtio devices on **two** transports:

1. virtio-mmio — a simple memory-mapped register window per slot,
   discovered by walking 32 fixed slots starting at `0x0a000000`
   with stride `0x200`.
2. virtio-pci — the same protocol, but discovered through PCI
   configuration space.

Most third-party hobby kernels (and the VibeOS reference in this
repo's `VibeOS/` tree) use PCI for the NIC. We use MMIO for one
reason: **we already have a working virtio-mmio probe** for
`virtio_blk`, `virtio_gpu`, `virtio_input`, and `virtio_tablet`.
Adding PCI here would be at least three new subsystems
(configuration space access, PCI bridge enumeration, and
MSI/MSI-X) for no functional gain.

## Bus and device layout

The mmio probe is the same one every device uses:

```c
for (uint32_t slot = 0; slot < VIRTIO_MMIO_SLOTS; ++slot) {
    uintptr_t base = VIRTIO_MMIO_BASE + slot * VIRTIO_MMIO_STRIDE;
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC) continue;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2)               continue;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_NET)
        continue;
    /* found one */
}
```

The only new value is `VIRTIO_DEVICE_ID_NET = 1`, added to
`kernel/device/virtio_mmio.h`.

## Feature negotiation

The virtio specification distinguishes several "feature bits" the
device offers and the driver must accept-or-decline. We accept the
bare minimum that lets us treat the device as a *modern*
virtio 1.x net device:

| Bit | Name           | Why we want it                                    |
|-----|----------------|---------------------------------------------------|
| 5   | `F_MAC`        | The device tells us our MAC instead of inventing one. |
| 16  | `F_STATUS`     | Lets us read the link-up bit if we want it later. |
| 32  | `F_VERSION_1`  | Modern transport. Requires the 12-byte header.    |

Three are *deliberately declined*:

- `MRG_RXBUF` (bit 15) — would let the device chain RX descriptors
  to merge a large frame across multiple buffers. Our buffers are
  1536 bytes, comfortably above the 1518-byte Ethernet MTU. Not
  declining it would force us to walk descriptor chains on RX,
  which is a lot of code for no benefit.
- `CSUM` / `GUEST_CSUM` — checksum offload. We don't have IP yet;
  there is nothing to offload.
- `MQ`, `RSS`, `CTRL_VQ` — multi-queue and the control virtqueue.
  We have one queue pair; we don't need a third queue to manage
  filters we don't have.

The negotiation is the standard 10-step modern handshake (Reset →
ACK → DRIVER → read host feature words → write driver feature
words → FEATURES_OK → re-read STATUS to confirm → set up queues
→ DRIVER_OK → kick).

## The 12-byte header that nobody talks about

Every modern virtio-net frame — both directions — is preceded by
a 12-byte `struct virtio_net_hdr`:

```c
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;   /* "1" if we declined MRG_RXBUF */
} __attribute__((packed));
```

If you forget this header on TX, the device drops your frame
silently. If you forget to skip it on RX, your "Ethernet" frame
starts at byte 12 of what you handed to the upper layer and
nothing parses.

Under VERSION_1 the header is **always 12 bytes**, regardless of
whether MRG_RXBUF was negotiated. This is one of the few places
the spec made the simple choice.

In our driver:

- TX: zero the 12 bytes, then copy the caller's Ethernet frame
  starting at byte 12.
- RX: when a used buffer comes back, hand `frame_buf + 12` to
  the callback with `len = used.len - 12`.

## Two queues, one ring page

We allocate one 4 KiB page for both rings, and one contiguous
12-page slab (48 KiB) for the data buffers:

```
ring page (one 4 KiB page)
  RX_DESC   @ 0x000  (16 * 16 B = 256 B)
  RX_AVAIL  @ 0x100  (header + 16 entries)
  RX_USED   @ 0x140  (header + 16 entries, padded)
  TX_DESC   @ 0x240  (16 * 16 B = 256 B)
  TX_AVAIL  @ 0x340  (header + 16 entries)
  TX_USED   @ 0x380  (header + 16 entries, padded)

data slab (12 contiguous pages)
  rx_buf[0..15]  @ slab[i      * 1536]
  tx_buf[0..15]  @ slab[(i+16) * 1536]
```

Sixteen descriptors per ring is small but plenty: the self-test
needs exactly one of each, the eventual ARP/IP stack rarely has
more than a couple in flight, and we can grow it later without
changing any of the public driver API.

A word on memory layout: we rely on the fact that everything
allocated by `pmem_alloc_page()` and `pmem_alloc_contig()` in our
kernel sits below 4 GiB and is identity-mapped, so the physical
address we write into a virtio descriptor is the same number
we'd dereference as a C pointer. If we ever turn on a higher-half
kernel mapping, this is one of the spots to fix.

## TX: the easiest path

```c
int virtio_net_tx(const uint8_t *frame, uint32_t len)
{
    if (len > VIRTIO_NET_FRAME_MAX) return -1;
    reap_tx_used();                       /* free completed descriptors */
    if (g_tx_inflight >= QUEUE_SIZE) return -1;

    uint32_t slot = g_tx_next++ & (QUEUE_SIZE - 1);
    uint8_t *buf  = (uint8_t *)tx_buf_va(slot);

    memset(buf, 0, VNET_HDR_BYTES);       /* the famous 12 zero bytes */
    memcpy(buf + VNET_HDR_BYTES, frame, len);

    g_tx_desc[slot].addr  = tx_buf_pa(slot);
    g_tx_desc[slot].len   = VNET_HDR_BYTES + len;
    g_tx_desc[slot].flags = 0;
    g_tx_desc[slot].next  = 0;

    publish_avail(g_tx_avail, slot);      /* idx++; full barrier */
    mmio_write32(g_base + VIRTIO_MMIO_QUEUE_NOTIFY, TX_QID);
    g_tx_inflight++;
    return 0;
}
```

The only subtlety is `reap_tx_used()`, which walks the device's
TX used ring and decrements `g_tx_inflight` for each frame the
device confirms it has consumed. Without it, after sixteen
transmits we'd be permanently full.

## RX: prepost everything, drain on demand

There is no IRQ. The device puts frames into descriptors that we
must have already published in the RX avail ring. So during
`init_device()` we publish all 16 descriptors at once.

`virtio_net_drain_rx()` is then a polling routine the rest of the
kernel calls whenever it wants to look for new frames:

```c
uint32_t virtio_net_drain_rx(void)
{
    uint32_t processed = 0;
    while (g_rx_used->idx != g_rx_used_last) {
        struct used_elem *u = &g_rx_used->ring[g_rx_used_last & (QUEUE_SIZE-1)];
        uint32_t slot = u->id;
        uint32_t len  = u->len;
        if (len > VNET_HDR_BYTES && g_rx_cb)
            g_rx_cb((const uint8_t *)rx_buf_va(slot) + VNET_HDR_BYTES,
                    len - VNET_HDR_BYTES);

        /* Recycle the descriptor: re-publish to avail ring. */
        publish_avail(g_rx_avail, slot);
        g_rx_used_last++;
        processed++;
    }
    if (processed) mmio_write32(g_base + VIRTIO_MMIO_QUEUE_NOTIFY, RX_QID);
    return processed;
}
```

Notice: every consumed buffer goes straight back into the avail
ring. The pool stays full. The kernel never has to "allocate an
RX buffer" — that work was done once at init.

## The self-test: ARP without an ARP stack

How do we know the driver works before later chapters have built any
network stack at all?

We exploit the fact that QEMU's SLIRP backend is a complete
user-space TCP/IP implementation that *will* answer ARP requests
on the virtual network. So during boot, after `virtio_net_init()`
returns OK, `main.c` calls `net_self_test()`, which:

1. Builds a 42-byte raw Ethernet frame:
   - dst MAC = `ff:ff:ff:ff:ff:ff` (broadcast)
   - src MAC = our MAC (read out of the device by `F_MAC`)
   - ethertype = `0x0806` (ARP)
   - hardware = Ethernet, protocol = IPv4, op = 1 (request)
   - sender IP = `10.0.2.15`, target IP = `10.0.2.2`
2. Hands the frame to `virtio_net_tx()`.
3. Sets an RX callback that flips a flag if it sees an ARP reply
   (`op = 2`) whose sender IP is the SLIRP gateway.
4. Spins up to 50 million iterations, calling
   `virtio_net_drain_rx()` every 4096 iterations.
5. Prints PASS or FAIL.

When everything is wired correctly, the SLIRP backend answers the
ARP within microseconds. On a freshly-booted kernel:

```
probing virtio-mmio bus for a NIC ... [virtio-net] found NIC at slot 0x000000000000001e base=0x000000000a003c00
[virtio-net] MAC=...:0x52:0x54:0x00:0x12:0x34:0x56 QUEUE_SIZE=0x10
[virtio-net] self-test: TX broadcast ARP, await reply
[virtio-net] self-test: ARP reply received from gateway (driver OK)
ok (network online)
```

The MAC you see is QEMU's classic `52:54:00:12:34:56` — *the*
SLIRP MAC. If you ever forget what the SLIRP gateway is, just
look at the boot log of any virt machine in any project ever.

## What the self-test actually proves

In ascending order of subtlety, the self-test verifies:

1. The mmio probe found the NIC.
2. Feature negotiation succeeded (we got past the `FEATURES_OK`
   ack — if we hadn't, the device would refuse all subsequent
   commands).
3. The 12-byte header layout is correct on both TX and RX.
4. `virtio_net_tx()` hands a frame to the wire (otherwise SLIRP
   sees nothing and never replies).
5. The RX descriptor pool was preposted (otherwise the reply
   would arrive at the device but bounce, since there's nowhere
   to put it).
6. `virtio_net_drain_rx()` correctly indexes the used ring,
   strips the header, and recycles the buffer.

That's the whole driver, end to end, in one boot. Anything we add
later (ARP stack, IPv4 stack, sockets) builds on this foundation
without revisiting it.

## Why polling and not IRQ

`virtio_blk` and `virtio_input` both poll. That's a deliberate
choice for this kernel — it keeps the GIC programming we need to
do per device down to "none." The cost is a few hundred wasted
cycles per `virtio_net_drain_rx()` call.

For a NIC that cost matters more than for a block device (which
is only polled around the time a syscall asked for a sector). We
mitigate it by:

- Calling `virtio_net_drain_rx()` from a single dedicated kernel
  context (later: a per-device kthread), not from every preemption
  point.
- Sizing the RX ring to 16 descriptors so a small burst doesn't
  drop frames during a missed poll.

If the latency ever bites us we'll wire up an IRQ in milestone
55, after the rest of the stack is built and we know what we're
optimizing for.

## Files added or changed

| File                               | What                                                       |
|------------------------------------|------------------------------------------------------------|
| `kernel/device/virtio_mmio.h`      | new `VIRTIO_DEVICE_ID_NET = 1`                             |
| `kernel/device/virtio_net.h`       | new — public driver API (init/tx/get_mac/drain/callback)   |
| `kernel/device/virtio_net.c`       | new — full driver, ~340 LOC                                 |
| `kernel/core/main.c`               | NIC probe + `net_self_test()`                              |
| `Makefile`                         | adds `virtio_net.o` + `-netdev user,id=n0 -device virtio-net-device,netdev=n0` |
| `scripts/test_virtio_net.py`       | new — boots headless, asserts the four PASS lines          |

## Where this leads

Everything in Part VII layers on this driver. The next chapter
defines the on-the-wire byte layouts (Ethernet header, ARP
packet, IPv4 header) and replaces the in-kernel ARP self-test
with a real ARP cache. Then ICMP echo, then UDP, then DHCP, then
TCP, then sockets — at which point we can start writing chapter 64
and pull our first HTTP page over our own stack.
