/*
 * kernel/device/virtio_rng.c — chapter 123 entropy source driver.
 *
 * Modelled after virtio_snd.c (same MMIO transport, same v2
 * handshake), but the entropy device is the simplest virtio
 * device in the spec: ONE virtqueue, descriptors point at
 * device-writable buffers, host fills them with random bytes,
 * device returns them via the used ring.  No config space, no
 * feature negotiation beyond VERSION_1.
 *
 * Layout of the shared ring page (one 4 KiB physical page,
 * physically contiguous because virtio descriptor addresses are
 * physical):
 *
 *   +0x000  REQ desc table   (QSIZE entries * 16 = 128)
 *   +0x080  REQ avail ring   (4 + QSIZE*2 + 2 = 22, padded to 64)
 *   +0x0C0  REQ used ring    (4 + QSIZE*8 + 2 = 70, padded to 128)
 *
 * The bounce buffer (one separate page) holds the bytes the
 * device writes; we copy out of it into the caller's buffer.
 * A separate page avoids accidentally letting the device write
 * over our ring metadata if something goes wrong.
 *
 * We submit one descriptor at a time and wait for it via the
 * used ring, so QSIZE=8 is more than enough.
 */

#include "virtio_rng.h"
#include "virtio_mmio.h"
#include "mmio.h"
#include "../core/serial.h"
#include "../core/pmem.h"
#include "../core/thread.h"
#include "../core/timer.h"

#include <stdint.h>
#include <stddef.h>

static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

#define VIRTIO_DEVICE_ID_ENTROPY   4u

#define REQ_QID          0u
#define REQ_QSIZE        8u

#define REQ_DESC_OFF     0x000u
#define REQ_AVAIL_OFF    0x080u
#define REQ_USED_OFF     0x0C0u

#define BOUNCE_BYTES     4096u

static uintptr_t g_rng_mmio_base = 0;
static uint8_t  *g_rng_ring_page = NULL;
static uint8_t  *g_rng_bounce    = NULL;
static uint64_t  g_rng_bounce_pa = 0;

static uint16_t  g_avail_idx = 0;
static uint16_t  g_used_seen = 0;

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

static inline uint32_t r32(uintptr_t off)
{ return mmio_read32(g_rng_mmio_base + off); }
static inline void w32(uintptr_t off, uint32_t v)
{ mmio_write32(g_rng_mmio_base + off, v); }

static struct vring_desc  *req_desc(void)
    { return (struct vring_desc  *)(g_rng_ring_page + REQ_DESC_OFF); }
static struct vring_avail *req_avail(void)
    { return (struct vring_avail *)(g_rng_ring_page + REQ_AVAIL_OFF); }
static struct vring_used  *req_used(void)
    { return (struct vring_used  *)(g_rng_ring_page + REQ_USED_OFF); }

static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_ENTROPY)
        return 0;
    return 1;
}

static int setup_queue(void)
{
    w32(VIRTIO_MMIO_QUEUE_SEL, REQ_QID);
    if (r32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-rng] queue already ready, refusing\n");
        return -1;
    }
    uint32_t qmax = r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < REQ_QSIZE) {
        serial_puts("[virtio-rng] queue too small: max=");
        serial_puthex(qmax);
        serial_puts("\n");
        return -1;
    }
    w32(VIRTIO_MMIO_QUEUE_NUM, REQ_QSIZE);

    uint64_t page_pa  = (uint64_t)(uintptr_t)g_rng_ring_page;
    uint64_t desc_pa  = page_pa + REQ_DESC_OFF;
    uint64_t avail_pa = page_pa + REQ_AVAIL_OFF;
    uint64_t used_pa  = page_pa + REQ_USED_OFF;
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

/* Submit one device-writable descriptor of `chunk` bytes pointing
 * at the bounce buffer and wait for the device to fill it.
 * Returns the number of bytes the device wrote, or -1 on error.
 *
 * The deadline is generous (1 s) because virtio-rng latency is
 * dominated by host scheduling, not by the device itself; in
 * practice we see < 1 ms per request under HVF. */
static long submit_one(uint32_t chunk)
{
    if (chunk == 0 || chunk > BOUNCE_BYTES) return -1;

    struct vring_desc *d = req_desc();
    d[0].addr  = g_rng_bounce_pa;
    d[0].len   = chunk;
    d[0].flags = VRING_DESC_F_WRITE;
    d[0].next  = 0;

    struct vring_avail *av = req_avail();
    uint16_t slot = g_avail_idx % REQ_QSIZE;
    av->ring[slot] = 0;
    dmb();
    g_avail_idx++;
    av->idx = g_avail_idx;
    dsb();
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, REQ_QID);

    uint64_t start_ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
    uint64_t deadline = start_ms + 1000;
    struct vring_used *u = req_used();
    while (g_used_seen == u->idx) {
        uint64_t now_ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
        if (now_ms > deadline) {
            serial_puts("[virtio-rng] request timeout\n");
            return -1;
        }
        yield();
        dmb();
    }
    /* The used-ring `len` field is the number of bytes the
     * device wrote into our descriptor.  For virtio-rng this
     * may be < chunk if the host's pool is shallow, so we
     * always trust the device's count rather than `chunk`. */
    uint32_t written = u->ring[g_used_seen % REQ_QSIZE].len;
    if (written > chunk) written = chunk;
    g_used_seen++;
    return (long)written;
}

static int init_device(uintptr_t base)
{
    g_rng_mmio_base = base;

    w32(VIRTIO_MMIO_STATUS, 0);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* virtio-rng exposes no driver-visible features other than
     * VERSION_1; clear the low feature word, set bit 32 (the
     * top half of feature select 1) to accept VERSION_1. */
    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t feat_hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(feat_hi & 1u)) {
        serial_puts("[virtio-rng] device does not advertise VERSION_1\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 1u);

    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK);
    if (!(r32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-rng] device cleared FEATURES_OK\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    g_rng_ring_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_rng_ring_page) {
        serial_puts("[virtio-rng] out of memory for ring page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    for (uint32_t i = 0; i < 4096; i++) g_rng_ring_page[i] = 0;

    g_rng_bounce_pa = pmem_alloc_page();
    if (!g_rng_bounce_pa) {
        serial_puts("[virtio-rng] out of memory for bounce page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    g_rng_bounce = (uint8_t *)(uintptr_t)g_rng_bounce_pa;

    if (setup_queue() < 0) {
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK |
                            VIRTIO_STATUS_DRIVER_OK);

    serial_puts("[virtio-rng] online, bounce=");
    serial_puthex(BOUNCE_BYTES);
    serial_puts(" bytes\n");
    return 0;
}

int virtio_rng_init(void)
{
    if (g_rng_mmio_base) return 0;
    for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (probe_slot(base)) {
            serial_puts("[virtio-rng] found device at slot ");
            serial_puthex(s);
            serial_puts(" base=");
            serial_puthex(base);
            serial_puts("\n");
            return init_device(base);
        }
    }
    return -1;
}

int virtio_rng_present(void) { return g_rng_mmio_base != 0; }

long virtio_rng_get(void *out, size_t len)
{
    if (!g_rng_mmio_base) return -1;
    if (!out && len) return -1;

    uint8_t *dst = (uint8_t *)out;
    size_t   got = 0;
    while (got < len) {
        size_t want = len - got;
        if (want > BOUNCE_BYTES) want = BOUNCE_BYTES;
        long n = submit_one((uint32_t)want);
        if (n <= 0) return (long)got > 0 ? (long)got : -1;
        for (long i = 0; i < n; i++) dst[got + (size_t)i] = g_rng_bounce[i];
        got += (size_t)n;
    }
    return (long)got;
}
