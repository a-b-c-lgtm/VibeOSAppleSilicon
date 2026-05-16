/*
 * kernel/device/virtio_blk.c — milestone-11 virtio-blk driver.
 *
 * What this driver does
 * ---------------------
 *  - Probe slots 0..31 of the virtio-mmio bus at 0x0a000000 looking
 *    for the magic value 0x74726976, version 2, device id 2 (block).
 *  - Bring the first match through the modern feature-negotiation
 *    handshake: RESET → ACK → DRIVER → DriverFeatures = VERSION_1 →
 *    FEATURES_OK → (verify still set) → setup virtqueue 0 →
 *    DRIVER_OK.
 *  - Allocate a single 4 KiB page from pmem and carve it into:
 *
 *        offset 0x000 : descriptor table   (8 × 16 = 128 bytes)
 *        offset 0x080 : avail ring          (4 + 16 + 2 = 22 bytes)
 *        offset 0x0C0 : used ring           (4 + 64 + 2 = 70 bytes)
 *        offset 0x200 : per-request buffers (header, status)
 *        offset 0x400 : per-request 512-byte data buffer
 *
 *    Aligned with plenty of room.  A second sector buffer would
 *    fit in the same page if we wanted to overlap requests.
 *
 *  - read/write builds a 3-descriptor chain (header out, data, status
 *    in), bumps avail.idx, kicks via QueueNotify, then busy-polls
 *    used.idx with a memory barrier between checks.
 *
 * What this driver does NOT do
 * ----------------------------
 *  - No interrupt driven completion (we'd want that for any thread
 *    other than the one issuing the I/O to make progress).
 *  - No request queueing — one outstanding request at a time.
 *  - No support for VIRTIO_BLK_F_RO (read-only devices); we'd need
 *    to surface that via a `read_only` flag.
 *  - No multi-sector transfers.  blk_read_n() would build a
 *    multi-segment data descriptor or use VIRTIO_BLK_F_SEG_MAX.
 *
 * The point of milestone 11 is to prove that the kernel can talk
 * to a real device on a real device tree using a real DMA-style
 * shared-memory transport.  Once that works, layering a real
 * filesystem on top is "just" code.
 */

#include "virtio_blk.h"
#include "virtio_mmio.h"
#include "mmio.h"
#include "../core/serial.h"
#include "../core/pmem.h"

#include <stdint.h>
#include <stddef.h>

static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

#define QUEUE_SIZE              8u
#define DESC_TABLE_OFF          0x000u
#define AVAIL_RING_OFF          0x080u
#define USED_RING_OFF           0x0C0u
#define HEADER_OFF              0x200u   /* 16 bytes */
#define STATUS_OFF              0x210u   /* 1 byte   */
#define DATA_BUF_OFF            0x400u   /* 512 bytes */

/* virtio_blk request header (5.2.6) */
#define VIRTIO_BLK_T_IN         0u  /* read  */
#define VIRTIO_BLK_T_OUT        1u  /* write */

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

#define VIRTIO_BLK_S_OK         0u
#define VIRTIO_BLK_S_IOERR      1u
#define VIRTIO_BLK_S_UNSUPP     2u

/* ---- driver state ---- */
struct blk_dev {
    uintptr_t mmio_base;     /* 0 = slot empty */
    uint64_t  capacity;
    uint8_t  *page;          /* the carved-up shared page */
    uint16_t  avail_idx_seen;
    uint16_t  used_idx_seen;
};

static struct blk_dev g_devs[VIRTIO_BLK_MAX_DEVS];
static int            g_dev_count = 0;

/* Back-compat shims for the single-device API.  All point to
 * device 0; chapters before 81 only knew about one disk and the
 * rest of the codebase still calls these names. */
#define g_blk_mmio_base   (g_devs[0].mmio_base)
#define g_blk_capacity    (g_devs[0].capacity)
#define g_blk_page        (g_devs[0].page)
#define g_avail_idx_seen  (g_devs[0].avail_idx_seen)
#define g_used_idx_seen   (g_devs[0].used_idx_seen)

/* ---- helpers ---- */
static inline uint32_t r32_dev(int d, uintptr_t off) {
    return mmio_read32(g_devs[d].mmio_base + off);
}
static inline void w32_dev(int d, uintptr_t off, uint32_t v) {
    mmio_write32(g_devs[d].mmio_base + off, v);
}
static inline uint32_t r32(uintptr_t off) { return r32_dev(0, off); }
static inline void     w32(uintptr_t off, uint32_t v) { w32_dev(0, off, v); }

static struct vring_desc *desc_tbl_dev(int d) {
    return (struct vring_desc *)(g_devs[d].page + DESC_TABLE_OFF);
}
static struct vring_avail *avail_ring_dev(int d) {
    return (struct vring_avail *)(g_devs[d].page + AVAIL_RING_OFF);
}
static struct vring_used *used_ring_dev(int d) {
    return (struct vring_used *)(g_devs[d].page + USED_RING_OFF);
}
static struct virtio_blk_req_hdr *req_hdr_dev(int d) {
    return (struct virtio_blk_req_hdr *)(g_devs[d].page + HEADER_OFF);
}
static volatile uint8_t *req_status_dev(int d) {
    return (volatile uint8_t *)(g_devs[d].page + STATUS_OFF);
}
static uint8_t *data_buf_dev(int d) {
    return g_devs[d].page + DATA_BUF_OFF;
}

/* ---- probe + init ---- */
static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_BLOCK)
        return 0;
    return 1;
}

static int setup_queue(int d)
{
    /* Select queue 0. */
    w32_dev(d, VIRTIO_MMIO_QUEUE_SEL, 0);
    if (r32_dev(d, VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-blk] queue 0 already ready, refusing\n");
        return -1;
    }
    uint32_t qmax = r32_dev(d, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < QUEUE_SIZE) {
        serial_puts("[virtio-blk] queue 0 max=");
        serial_puthex(qmax);
        serial_puts(" < required ");
        serial_puthex(QUEUE_SIZE);
        serial_puts("\n");
        return -1;
    }
    w32_dev(d, VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    uint64_t page_pa = (uint64_t)(uintptr_t)g_devs[d].page;
    uint64_t desc_pa  = page_pa + DESC_TABLE_OFF;
    uint64_t avail_pa = page_pa + AVAIL_RING_OFF;
    uint64_t used_pa  = page_pa + USED_RING_OFF;

    w32_dev(d, VIRTIO_MMIO_QUEUE_DESC_LO,   (uint32_t)(desc_pa & 0xffffffffu));
    w32_dev(d, VIRTIO_MMIO_QUEUE_DESC_HI,   (uint32_t)(desc_pa >> 32));
    w32_dev(d, VIRTIO_MMIO_QUEUE_DRIVER_LO, (uint32_t)(avail_pa & 0xffffffffu));
    w32_dev(d, VIRTIO_MMIO_QUEUE_DRIVER_HI, (uint32_t)(avail_pa >> 32));
    w32_dev(d, VIRTIO_MMIO_QUEUE_DEVICE_LO, (uint32_t)(used_pa & 0xffffffffu));
    w32_dev(d, VIRTIO_MMIO_QUEUE_DEVICE_HI, (uint32_t)(used_pa >> 32));

    dsb();
    w32_dev(d, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int init_device(int d, uintptr_t base)
{
    g_devs[d].mmio_base = base;

    /* 1. Reset. */
    w32_dev(d, VIRTIO_MMIO_STATUS, 0);
    /* 2. ACK. */
    w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    /* 3. DRIVER. */
    w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Read device features (we only care about VERSION_1 in
     *    the high 32 bits). */
    w32_dev(d, VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t hi = r32_dev(d, VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(hi & 1u)) {
        serial_puts("[virtio-blk] device does not advertise VERSION_1\n");
        w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 5. Tell the device we accept VERSION_1, and nothing else.
     *    (No RO / SEG_MAX / FLUSH negotiation yet.) */
    w32_dev(d, VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w32_dev(d, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    w32_dev(d, VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w32_dev(d, VIRTIO_MMIO_DRIVER_FEATURES, 1);

    /* 6. FEATURES_OK. */
    w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                                   VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK);
    if (!(r32_dev(d, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-blk] device cleared FEATURES_OK\n");
        w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 7. Allocate the shared page and set up queue 0. */
    g_devs[d].page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_devs[d].page) {
        serial_puts("[virtio-blk] out of memory for queue page\n");
        w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (setup_queue(d) < 0) {
        w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 8. Read capacity from device-specific config. */
    uint32_t lo = mmio_read32(base + VIRTIO_MMIO_CONFIG + 0);
    uint32_t hi2 = mmio_read32(base + VIRTIO_MMIO_CONFIG + 4);
    g_devs[d].capacity = ((uint64_t)hi2 << 32) | lo;

    /* 9. DRIVER_OK. */
    w32_dev(d, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                                   VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK |
                                   VIRTIO_STATUS_DRIVER_OK);

    serial_puts("[virtio-blk] dev");
    serial_puthex((uint64_t)d);
    serial_puts(" ready, capacity = ");
    serial_puthex(g_devs[d].capacity);
    serial_puts(" sectors (");
    serial_puthex(g_devs[d].capacity * VIRTIO_BLK_SECTOR_SIZE);
    serial_puts(" bytes)\n");
    return 0;
}

int virtio_blk_init(void)
{
    g_dev_count = 0;
    for (int i = 0; i < VIRTIO_BLK_MAX_DEVS; i++) {
        g_devs[i].mmio_base      = 0;
        g_devs[i].capacity       = 0;
        g_devs[i].page           = NULL;
        g_devs[i].avail_idx_seen = 0;
        g_devs[i].used_idx_seen  = 0;
    }
    /* Walk slots HIGH to LOW.  QEMU's `virt` machine assigns the
     * first -drive (hd0) to the highest-numbered virtio-mmio
     * slot it can find, the second (hd1) to the next one down,
     * and so on.  A low-to-high walk would therefore enumerate
     * the data disk before the kernel disk and break OSFS-1's
     * "I'm always on dev 0" assumption.  See chapter 81 for the
     * post-mortem on this trap. */
    for (int s = (int)VIRTIO_MMIO_SLOTS - 1; s >= 0; s--) {
        if (g_dev_count >= VIRTIO_BLK_MAX_DEVS) break;
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (!probe_slot(base)) continue;
        serial_puts("[virtio-blk] found block device at slot ");
        serial_puthex((uint64_t)(uint32_t)s);
        serial_puts(" base=");
        serial_puthex(base);
        serial_puts(" -> dev");
        serial_puthex((uint64_t)g_dev_count);
        serial_puts("\n");
        if (init_device(g_dev_count, base) == 0)
            g_dev_count++;
    }
    if (g_dev_count == 0) {
        serial_puts("[virtio-blk] no virtio-blk device found on the bus\n");
        return -1;
    }
    return 0;
}

int  virtio_blk_count(void)              { return g_dev_count; }
int  virtio_blk_present(void)            { return g_devs[0].mmio_base != 0; }
uint64_t virtio_blk_capacity(void)       { return g_devs[0].capacity; }
int  virtio_blk_dev_present(int dev)
{
    if (dev < 0 || dev >= VIRTIO_BLK_MAX_DEVS) return 0;
    return g_devs[dev].mmio_base != 0;
}
uint64_t virtio_blk_dev_capacity(int dev)
{
    if (dev < 0 || dev >= VIRTIO_BLK_MAX_DEVS) return 0;
    return g_devs[dev].capacity;
}

/* ---- I/O ---- */
static int do_request(int dev, uint32_t type, uint64_t sector,
                      void *buf, int is_read)
{
    if (!virtio_blk_dev_present(dev)) return -1;

    /* Fill header, copy out-data into the shared page if writing. */
    struct virtio_blk_req_hdr *h = req_hdr_dev(dev);
    h->type = type;
    h->reserved = 0;
    h->sector = sector;

    if (!is_read && buf) {
        const uint8_t *src = (const uint8_t *)buf;
        uint8_t *dst = data_buf_dev(dev);
        for (size_t i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) dst[i] = src[i];
    }

    /* Build descriptor chain at indices 0,1,2:
     *   0: header   (out)
     *   1: data     (in for read, out for write)
     *   2: status   (in, device-writable, 1 byte) */
    struct vring_desc *d = desc_tbl_dev(dev);
    uint64_t page_pa = (uint64_t)(uintptr_t)g_devs[dev].page;

    d[0].addr  = page_pa + HEADER_OFF;
    d[0].len   = sizeof(struct virtio_blk_req_hdr);
    d[0].flags = VRING_DESC_F_NEXT;
    d[0].next  = 1;

    d[1].addr  = page_pa + DATA_BUF_OFF;
    d[1].len   = VIRTIO_BLK_SECTOR_SIZE;
    d[1].flags = VRING_DESC_F_NEXT | (is_read ? VRING_DESC_F_WRITE : 0u);
    d[1].next  = 2;

    d[2].addr  = page_pa + STATUS_OFF;
    d[2].len   = 1;
    d[2].flags = VRING_DESC_F_WRITE;     /* device writes status byte */
    d[2].next  = 0;

    *req_status_dev(dev) = 0xff;         /* sentinel: device must overwrite */

    /* Publish the head (descriptor 0) into the avail ring at the
     * next slot, then bump avail.idx with a release-style barrier. */
    struct vring_avail *av = avail_ring_dev(dev);
    uint16_t slot = g_devs[dev].avail_idx_seen % QUEUE_SIZE;
    av->ring[slot] = 0;                  /* head index */
    dmb();                               /* writes complete before idx bump */
    g_devs[dev].avail_idx_seen++;
    av->idx = g_devs[dev].avail_idx_seen;
    dmb();

    /* Kick. */
    w32_dev(dev, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll the used ring.  We wait for used.idx to reach our
     * expected value (g_used_idx_seen + 1). */
    struct vring_used *u = used_ring_dev(dev);
    uint16_t target = g_devs[dev].used_idx_seen + 1;
    /* Crude bounded spin to avoid wedging on a buggy host. */
    for (uint64_t spin = 0; spin < (1ULL << 28); spin++) {
        dmb();
        if (u->idx == target) goto got_it;
    }
    serial_puts("[virtio-blk] timeout waiting for completion\n");
    return -1;

got_it:
    g_devs[dev].used_idx_seen = target;

    if (*req_status_dev(dev) != VIRTIO_BLK_S_OK) {
        serial_puts("[virtio-blk] request failed, status=");
        serial_puthex(*req_status_dev(dev));
        serial_puts("\n");
        return -1;
    }

    if (is_read && buf) {
        const uint8_t *src = data_buf_dev(dev);
        uint8_t *dst = (uint8_t *)buf;
        for (size_t i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) dst[i] = src[i];
    }
    return 0;
}

int virtio_blk_read(uint64_t sector, void *buf)
{
    return do_request(0, VIRTIO_BLK_T_IN, sector, buf, 1);
}

int virtio_blk_write(uint64_t sector, const void *buf)
{
    return do_request(0, VIRTIO_BLK_T_OUT, sector, (void *)buf, 0);
}

int virtio_blk_dev_read(int dev, uint64_t sector, void *buf)
{
    return do_request(dev, VIRTIO_BLK_T_IN, sector, buf, 1);
}

int virtio_blk_dev_write(int dev, uint64_t sector, const void *buf)
{
    return do_request(dev, VIRTIO_BLK_T_OUT, sector, (void *)buf, 0);
}
