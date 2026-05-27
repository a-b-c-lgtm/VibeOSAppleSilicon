/*
 * kernel/device/virtio_net.c — virtio-net driver.
 *
 * See virtio_net.h for the public contract and the buffer-layout
 * diagram.  This file follows the same shape as virtio_input.c
 * (same handshake, same ring conventions); diff against that
 * file to see only the net-specific pieces.
 *
 * One shared ring page (4 KiB) holds:
 *   +0x000  RX descriptor table  (16 * 16  = 256)
 *   +0x100  RX avail ring        (4 + 32 + 2 = 38, padded to 64)
 *   +0x140  RX used ring         (4 + 16*8 + 2 = 134, padded to 256)
 *   +0x240  TX descriptor table  (16 * 16  = 256)
 *   +0x340  TX avail ring        (38, padded to 64)
 *   +0x380  TX used ring         (134, padded to 256)
 *   +0x480  unused
 *
 * One contiguous data slab (12 pages) holds:
 *   16 RX buffers * 1536 bytes  = 24576  ( 6 pages)
 *   16 TX buffers * 1536 bytes  = 24576  ( 6 pages)
 *
 * Every descriptor is permanently bound to its slot; we never
 * remap.  The driver only ever touches `flags`, `len`, and the
 * avail/used rings — `addr` is set once during init.
 */

#include "virtio_net.h"
#include "virtio_mmio.h"
#include "mmio.h"
#include "../core/serial.h"
#include "../core/pmem.h"

#include <stdint.h>
#include <stddef.h>

static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* GCC may emit memset for `= { 0 }`; freestanding code has no libc.
 * Weak so we don't collide with the copy in virtio_gpu / virtio_input. */
__attribute__((weak))
void *memset(void *d, int c, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

__attribute__((weak))
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ---- virtio-net device-config layout (spec 5.1.4) ---- */
struct virtio_net_config {
    uint8_t  mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} __attribute__((packed));

/* ---- virtio_net_hdr (spec 5.1.6).  Always 12 bytes under
 * VIRTIO_F_VERSION_1, even without MRG_RXBUF. ---- */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

#define VNET_HDR_BYTES   12u
#define VNET_BUF_BYTES   1536u   /* 12 hdr + 1518 frame + 6 slack */

/* ---- Feature bits ---- */
#define VIRTIO_NET_F_MAC      (1ULL << 5)
#define VIRTIO_NET_F_STATUS   (1ULL << 16)
#define VIRTIO_NET_F_MRG_RXBUF (1ULL << 15)

/* ---- Layout of our shared queue page ---- */
#define QUEUE_SIZE       16u

#define RX_DESC_OFF      0x000u
#define RX_AVAIL_OFF     0x100u
#define RX_USED_OFF      0x140u
#define TX_DESC_OFF      0x240u
#define TX_AVAIL_OFF     0x340u
#define TX_USED_OFF      0x380u

#define RX_QID           0u
#define TX_QID           1u

/* ---- Driver state ---- */
static uintptr_t g_net_mmio_base = 0;     /* 0 == not initialised */
static uint8_t  *g_net_ring_page = NULL;  /* the 4 KiB queue page */
static uint8_t  *g_net_data_base = NULL;  /* the 12-page slab     */
static uint64_t  g_net_data_pa   = 0;
static uint8_t   g_net_mac[VIRTIO_NET_MAC_LEN];

static uint16_t  g_rx_avail_idx = 0;
static uint16_t  g_rx_used_seen = 0;
static uint16_t  g_tx_avail_idx = 0;
static uint16_t  g_tx_used_seen = 0;

static virtio_net_rx_cb g_rx_cb = NULL;

/* ---- helpers ---- */
static inline uint32_t r32(uintptr_t off)
{
    return mmio_read32(g_net_mmio_base + off);
}
static inline void w32(uintptr_t off, uint32_t v)
{
    mmio_write32(g_net_mmio_base + off, v);
}
static struct vring_desc  *rx_desc(void)  { return (struct vring_desc  *)(g_net_ring_page + RX_DESC_OFF); }
static struct vring_avail *rx_avail(void) { return (struct vring_avail *)(g_net_ring_page + RX_AVAIL_OFF); }
static struct vring_used  *rx_used(void)  { return (struct vring_used  *)(g_net_ring_page + RX_USED_OFF); }
static struct vring_desc  *tx_desc(void)  { return (struct vring_desc  *)(g_net_ring_page + TX_DESC_OFF); }
static struct vring_avail *tx_avail(void) { return (struct vring_avail *)(g_net_ring_page + TX_AVAIL_OFF); }
static struct vring_used  *tx_used(void)  { return (struct vring_used  *)(g_net_ring_page + TX_USED_OFF); }

/* Per-buffer kernel pointer / physical address. */
static inline uint8_t *rx_buf_va(uint16_t i)
{
    return g_net_data_base + (uint64_t)i * VNET_BUF_BYTES;
}
static inline uint64_t rx_buf_pa(uint16_t i)
{
    return g_net_data_pa + (uint64_t)i * VNET_BUF_BYTES;
}
static inline uint8_t *tx_buf_va(uint16_t i)
{
    return g_net_data_base + (uint64_t)(QUEUE_SIZE + i) * VNET_BUF_BYTES;
}
static inline uint64_t tx_buf_pa(uint16_t i)
{
    return g_net_data_pa + (uint64_t)(QUEUE_SIZE + i) * VNET_BUF_BYTES;
}

/* Publish RX descriptor `i` as a fresh empty buffer the device may
 * fill with the next incoming frame. */
static void rx_publish(uint16_t i)
{
    struct vring_avail *av = rx_avail();
    uint16_t slot = g_rx_avail_idx % QUEUE_SIZE;
    av->ring[slot] = i;
    dmb();
    g_rx_avail_idx++;
    av->idx = g_rx_avail_idx;
    dmb();
}

/* ---- probe + init ---- */
static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_NET)
        return 0;
    return 1;
}

static int setup_one_queue(uint32_t qid, uintptr_t desc_off,
                           uintptr_t avail_off, uintptr_t used_off)
{
    w32(VIRTIO_MMIO_QUEUE_SEL, qid);
    if (r32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-net] queue already ready, refusing\n");
        return -1;
    }
    uint32_t qmax = r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < QUEUE_SIZE) {
        serial_puts("[virtio-net] queue too small: max=");
        serial_puthex(qmax);
        serial_puts("\n");
        return -1;
    }
    w32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    uint64_t page_pa  = (uint64_t)(uintptr_t)g_net_ring_page;
    uint64_t desc_pa  = page_pa + desc_off;
    uint64_t avail_pa = page_pa + avail_off;
    uint64_t used_pa  = page_pa + used_off;
    w32(VIRTIO_MMIO_QUEUE_DESC_LO,   (uint32_t)(desc_pa & 0xffffffffu));
    w32(VIRTIO_MMIO_QUEUE_DESC_HI,   (uint32_t)(desc_pa >> 32));
    w32(VIRTIO_MMIO_QUEUE_DRIVER_LO, (uint32_t)(avail_pa & 0xffffffffu));
    w32(VIRTIO_MMIO_QUEUE_DRIVER_HI, (uint32_t)(avail_pa >> 32));
    w32(VIRTIO_MMIO_QUEUE_DEVICE_LO, (uint32_t)(used_pa  & 0xffffffffu));
    w32(VIRTIO_MMIO_QUEUE_DEVICE_HI, (uint32_t)(used_pa  >> 32));
    dsb();
    w32(VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int init_device(uintptr_t base)
{
    g_net_mmio_base = base;

    /* 1. Reset. */
    w32(VIRTIO_MMIO_STATUS, 0);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 2. Read device features (lo + hi 32-bit halves). */
    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 0);
    uint32_t feat_lo = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t feat_hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(feat_hi & 1u)) {
        serial_puts("[virtio-net] device does not advertise VERSION_1\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 3. Decide which features WE want.
     *    - VERSION_1 is mandatory (bit 32, in the hi half).
     *    - F_MAC if the device offers it: lets us use the
     *      device-config MAC instead of generating one.
     *    - F_STATUS for completeness (lets us peek link state).
     *    - We DECLINE F_MRG_RXBUF: keeping each RX frame in
     *      exactly one descriptor lets us use a single buffer
     *      slot per descriptor, no reassembly logic required. */
    uint32_t want_lo = 0;
    uint32_t want_hi = 1u;     /* VERSION_1 */

    if (feat_lo & (uint32_t)VIRTIO_NET_F_MAC)    want_lo |= (uint32_t)VIRTIO_NET_F_MAC;
    if (feat_lo & (uint32_t)VIRTIO_NET_F_STATUS) want_lo |= (uint32_t)VIRTIO_NET_F_STATUS;
    /* NOTE: we deliberately do NOT set F_MRG_RXBUF. */

    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, want_lo);
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, want_hi);

    /* 4. FEATURES_OK. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK);
    if (!(r32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-net] device cleared FEATURES_OK\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 5. Read the MAC out of device-config (assumes F_MAC was
     *    accepted; if not, fall back to a locally-administered
     *    52:54:00:xx:xx:xx). */
    if (want_lo & (uint32_t)VIRTIO_NET_F_MAC) {
        for (int i = 0; i < VIRTIO_NET_MAC_LEN; i++)
            g_net_mac[i] = mmio_read8(base + VIRTIO_MMIO_CONFIG + i);
    } else {
        g_net_mac[0] = 0x52; g_net_mac[1] = 0x54; g_net_mac[2] = 0x00;
        g_net_mac[3] = 0xAA; g_net_mac[4] = 0xBB; g_net_mac[5] = 0xCC;
    }

    /* 6. Allocate the ring page (one 4 KiB page) and the data slab
     *    (12 contiguous pages = 48 KiB).  We deliberately use
     *    pmem_alloc_contig so descriptor addr fields can be a
     *    simple base+offset rather than per-page lookups. */
    g_net_ring_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_net_ring_page) {
        serial_puts("[virtio-net] out of memory for ring page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    /* Zero the queue rings: leftover bits in idx/flags would lie
     * to the device about how much work is queued. */
    for (uint32_t i = 0; i < 4096; i++) g_net_ring_page[i] = 0;

    g_net_data_pa = pmem_alloc_contig(12);
    if (!g_net_data_pa) {
        serial_puts("[virtio-net] out of memory for data slab\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    g_net_data_base = (uint8_t *)(uintptr_t)g_net_data_pa;

    /* 7. Wire up both queues. */
    if (setup_one_queue(RX_QID, RX_DESC_OFF, RX_AVAIL_OFF, RX_USED_OFF) < 0)
        return -1;
    if (setup_one_queue(TX_QID, TX_DESC_OFF, TX_AVAIL_OFF, TX_USED_OFF) < 0)
        return -1;

    /* 8. Pre-bind every RX descriptor to its permanent slot and
     *    publish them all so the device has 16 free buffers ready
     *    to fill the moment we set DRIVER_OK. */
    {
        struct vring_desc *d = rx_desc();
        for (uint16_t i = 0; i < QUEUE_SIZE; i++) {
            d[i].addr  = rx_buf_pa(i);
            d[i].len   = VNET_BUF_BYTES;
            d[i].flags = VRING_DESC_F_WRITE;
            d[i].next  = 0;
            rx_publish(i);
        }
    }
    /* TX descriptors are bound but NOT published — we publish them
     * one at a time inside virtio_net_tx() as frames are queued. */
    {
        struct vring_desc *d = tx_desc();
        for (uint16_t i = 0; i < QUEUE_SIZE; i++) {
            d[i].addr  = tx_buf_pa(i);
            d[i].len   = 0;
            d[i].flags = 0;     /* device-readable; no WRITE flag */
            d[i].next  = 0;
        }
    }

    /* 9. DRIVER_OK. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK |
                            VIRTIO_STATUS_DRIVER_OK);

    /* 10. Kick the RX queue once so the device knows there are
     *     buffers waiting. */
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, RX_QID);
    return 0;
}

int virtio_net_init(void)
{
    if (g_net_mmio_base) return 0;     /* already up */
    for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (probe_slot(base)) {
            serial_puts("[virtio-net] found NIC at slot ");
            serial_puthex(s);
            serial_puts(" base=");
            serial_puthex(base);
            serial_puts("\n");
            if (init_device(base) < 0) return -1;
            serial_puts("[virtio-net] MAC=");
            for (int i = 0; i < VIRTIO_NET_MAC_LEN; i++) {
                serial_puthex(g_net_mac[i]);
                if (i != VIRTIO_NET_MAC_LEN - 1) serial_puts(":");
            }
            serial_puts(" QUEUE_SIZE=");
            serial_puthex(QUEUE_SIZE);
            serial_puts("\n");
            return 0;
        }
    }
    return -1;
}

int virtio_net_present(void) { return g_net_mmio_base != 0; }

void virtio_net_get_mac(uint8_t out[VIRTIO_NET_MAC_LEN])
{
    if (!g_net_mmio_base) {
        for (int i = 0; i < VIRTIO_NET_MAC_LEN; i++) out[i] = 0;
        return;
    }
    for (int i = 0; i < VIRTIO_NET_MAC_LEN; i++) out[i] = g_net_mac[i];
}

void virtio_net_set_rx_callback(virtio_net_rx_cb cb)
{
    g_rx_cb = cb;
}

int virtio_net_tx(const uint8_t *frame, uint32_t len)
{
    if (!g_net_mmio_base) return -1;
    if (len == 0 || len > VIRTIO_NET_FRAME_MAX) return -1;
    /* Reap any TX buffers the device has handed back so we don't
     * stall waiting for our own descriptors.  This is not strictly
     * necessary on the very first call, but it's cheap. */
    {
        struct vring_used *u = tx_used();
        dmb();
        while (g_tx_used_seen != u->idx) {
            /* The TX used ring tells us which descriptor the device
             * consumed; in our scheme each descriptor slot is also
             * its own "in flight" indicator (set when published,
             * implicitly free when reaped).  No bookkeeping array
             * needed because we publish slots round-robin and never
             * have more than QUEUE_SIZE in flight. */
            g_tx_used_seen++;
        }
    }

    /* Outstanding TX = avail.idx_we_published - used.idx_acked.
     * If equal to QUEUE_SIZE the queue is full and we drop the
     * frame.  Real drivers would block; we don't have a good way
     * to block in driver context without a per-queue waitqueue. */
    uint16_t inflight = (uint16_t)(g_tx_avail_idx - g_tx_used_seen);
    if (inflight >= QUEUE_SIZE) {
        serial_puts("[virtio-net] TX queue full, dropping frame\n");
        return -1;
    }

    uint16_t i = g_tx_avail_idx % QUEUE_SIZE;
    uint8_t *buf = tx_buf_va(i);
    /* 12-byte header: all-zero is a valid "no checksum offload, no
     * GSO, single buffer" header under VERSION_1. */
    for (uint32_t k = 0; k < VNET_HDR_BYTES; k++) buf[k] = 0;
    memcpy(buf + VNET_HDR_BYTES, frame, len);

    struct vring_desc *d = &tx_desc()[i];
    d->len   = VNET_HDR_BYTES + len;
    d->flags = 0;     /* device-readable */
    d->next  = 0;

    /* Publish onto the TX avail ring. */
    struct vring_avail *av = tx_avail();
    av->ring[i] = i;
    dmb();
    g_tx_avail_idx++;
    av->idx = g_tx_avail_idx;
    dsb();

    /* Kick. */
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, TX_QID);
    return 0;
}

int virtio_net_drain_rx(void)
{
    if (!g_net_mmio_base) return 0;
    int processed = 0;
    struct vring_used *u = rx_used();
    dmb();
    while (g_rx_used_seen != u->idx) {
        uint16_t slot = g_rx_used_seen % QUEUE_SIZE;
        uint32_t total = u->ring[slot].len;
        uint32_t did   = u->ring[slot].id;
        if (did >= QUEUE_SIZE) {
            serial_puts("[virtio-net] RX used.id out of range\n");
            break;
        }
        /* total includes the 12-byte header; strip it. */
        uint32_t frame_len = (total >= VNET_HDR_BYTES)
                           ? total - VNET_HDR_BYTES : 0;
        const uint8_t *frame = rx_buf_va((uint16_t)did) + VNET_HDR_BYTES;
        if (g_rx_cb && frame_len > 0) g_rx_cb(frame, frame_len);

        /* Recycle the descriptor.  addr is permanent; only flags
         * need to be re-asserted because the device may have
         * cleared them. */
        struct vring_desc *d = &rx_desc()[did];
        d->len   = VNET_BUF_BYTES;
        d->flags = VRING_DESC_F_WRITE;
        d->next  = 0;
        rx_publish((uint16_t)did);

        g_rx_used_seen++;
        processed++;
    }
    if (processed > 0) {
        /* Re-kick so the device knows we've replenished buffers. */
        w32(VIRTIO_MMIO_QUEUE_NOTIFY, RX_QID);
    }
    return processed;
}
