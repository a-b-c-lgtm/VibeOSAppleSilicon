/*
 * kernel/device/virtio_snd.c — chapter 96 virtio-sound driver.
 *
 * Spec reference: virtio v1.2 §5.14 (sound device).  See
 * virtio_snd.h for the design summary.
 *
 * Layout of the one shared queue page (4 KiB), addresses set
 * once during init and never moved:
 *
 *   +0x000  CTRL desc table     (8 entries * 16 = 128)
 *   +0x080  CTRL avail ring     (4 + 8*2 + 2 = 22, padded to 64)
 *   +0x0C0  CTRL used ring      (4 + 8*8 + 2 = 70, padded to 128)
 *   +0x140  TX   desc table     (8 entries * 16 = 128)
 *   +0x1C0  TX   avail ring     (22, padded to 64)
 *   +0x200  TX   used ring      (70, padded to 128)
 *   +0x280  unused
 *
 * The CTRL request/response payload buffers are 256 bytes each
 * (more than enough for the largest control structure we send,
 * `virtio_snd_pcm_set_params` at 24 bytes, and the largest
 * response we read, `virtio_snd_pcm_info` at 32 bytes per
 * stream * NUM_STREAMS_MAX).
 *
 * The TX data slab is one 64 KiB buffer used as the device-
 * readable side of every PCM message; we re-fill it from the
 * synth output and re-use it.  TX status response is a 16-byte
 * area at the tail.
 *
 * Why a single 64 KiB TX buffer?  virtio descriptors must point
 * to physically contiguous memory; pmem_alloc_contig(16) gives
 * us 64 KiB which is ~370 ms of mono S16/44_100 Hz playback —
 * comfortably more than any chime we ship.
 */

#include "virtio_snd.h"
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

/* ---- virtio device id ---- */
#define VIRTIO_DEVICE_ID_SOUND   25u

/* ---- virtio-snd device-config layout (spec §5.14.4) ---- */
struct virtio_snd_config {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
} __attribute__((packed));

/* ---- common request header (§5.14.6.4) ---- */
struct virtio_snd_hdr {
    uint32_t code;
} __attribute__((packed));

/* ---- request / response codes (§5.14.6.4) ---- */
#define VIRTIO_SND_R_PCM_INFO          0x0100u
#define VIRTIO_SND_R_PCM_SET_PARAMS    0x0101u
#define VIRTIO_SND_R_PCM_PREPARE       0x0102u
#define VIRTIO_SND_R_PCM_RELEASE       0x0103u
#define VIRTIO_SND_R_PCM_START         0x0104u
#define VIRTIO_SND_R_PCM_STOP          0x0105u

#define VIRTIO_SND_S_OK                0x8000u
#define VIRTIO_SND_S_BAD_MSG           0x8001u
#define VIRTIO_SND_S_NOT_SUPP          0x8002u
#define VIRTIO_SND_S_IO_ERR            0x8003u

/* ---- PCM stream parameters ---- */
/* PCM formats (§5.14.6.6.1.1): */
#define VIRTIO_SND_PCM_FMT_S16         5u
/* PCM rates (§5.14.6.6.1.2): */
#define VIRTIO_SND_PCM_RATE_44100      9u

/* ---- PCM-channel header (§5.14.6.6.2) ---- */
struct virtio_snd_pcm_hdr {
    struct virtio_snd_hdr hdr;
    uint32_t stream_id;
} __attribute__((packed));

/* ---- PCM SET_PARAMS request (§5.14.6.6.3.1) ---- */
struct virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
} __attribute__((packed));

/* ---- PCM TX message header + status (§5.14.6.8) ---- */
struct virtio_snd_pcm_xfer {
    uint32_t stream_id;
} __attribute__((packed));

struct virtio_snd_pcm_status {
    uint32_t status;
    uint32_t latency_bytes;
} __attribute__((packed));

/* ---- queue layout ---- */
#define CTRL_QID         0u
#define EVENT_QID        1u   /* not used */
#define TX_QID           2u
#define RX_QID           3u   /* not used */

#define CTRL_QSIZE       8u
#define TX_QSIZE         8u

/* offsets inside the shared 4 KiB ring page */
#define CTRL_DESC_OFF    0x000u
#define CTRL_AVAIL_OFF   0x080u
#define CTRL_USED_OFF    0x0C0u
#define TX_DESC_OFF      0x140u
#define TX_AVAIL_OFF     0x1C0u
#define TX_USED_OFF      0x200u

/* CTRL request / response staging (256-byte slots inside a
 * one-page allocation; we use only the first ~128 bytes of
 * each in practice). */
#define CTRL_REQ_OFF     0x000u
#define CTRL_RESP_OFF    0x100u
#define CTRL_PAGE_BYTES  4096u

/* TX data slab — one 64 KiB physically contiguous region used
 * as the entire device-readable side of every PCM message.
 * The xfer header (4 bytes) sits at the front, samples follow.
 * The pcm_status response is at TX_STATUS_OFF inside the same
 * region for convenience (single allocation, single base PA). */
#define TX_SLAB_PAGES    16u
#define TX_SLAB_BYTES    (TX_SLAB_PAGES * 4096u)
#define TX_HDR_OFF       0u
#define TX_DATA_OFF      sizeof(struct virtio_snd_pcm_xfer)
#define TX_STATUS_OFF    (TX_SLAB_BYTES - 64u)
#define TX_DATA_MAX      (TX_STATUS_OFF - TX_DATA_OFF)

/* PCM stream we drive.  Stream 0 is the first output stream on
 * QEMU's virtio-snd-device by default. */
#define PCM_STREAM_ID    0u

/* Stream parameters: mono S16 @ 44_100 Hz.  Period = TX_DATA_MAX
 * so the device's internal "buffer" matches our submission size
 * one-to-one — every TX message corresponds to one period and
 * the used ring fires when the period has been consumed. */
#define PCM_RATE_HZ      44100u
#define PCM_BYTES_PER_FR 2u
#define PCM_PERIOD_BYTES TX_DATA_MAX

/* GCC-emitted memset/memcpy fallbacks.  Weak so they don't
 * collide with the copies in virtio_blk / virtio_net. */
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

/* ---- driver state ---- */
static uintptr_t g_snd_mmio_base = 0;     /* 0 == not initialised */
static uint8_t  *g_snd_ring_page = NULL;  /* 4 KiB ring page      */
static uint8_t  *g_snd_ctrl_page = NULL;  /* 4 KiB CTRL req/resp  */
static uint8_t  *g_snd_tx_slab   = NULL;  /* 64 KiB TX data slab  */
static uint64_t  g_snd_ctrl_pa   = 0;
static uint64_t  g_snd_tx_pa     = 0;

static uint16_t  g_ctrl_avail_idx = 0;
static uint16_t  g_ctrl_used_seen = 0;
static uint16_t  g_tx_avail_idx   = 0;
static uint16_t  g_tx_used_seen   = 0;

static uint32_t  g_pcm_streams = 0;       /* count from config space */

/* ---- helpers ---- */
static inline uint32_t r32(uintptr_t off)
{
    return mmio_read32(g_snd_mmio_base + off);
}
static inline void w32(uintptr_t off, uint32_t v)
{
    mmio_write32(g_snd_mmio_base + off, v);
}
static struct vring_desc  *ctrl_desc(void)
    { return (struct vring_desc  *)(g_snd_ring_page + CTRL_DESC_OFF); }
static struct vring_avail *ctrl_avail(void)
    { return (struct vring_avail *)(g_snd_ring_page + CTRL_AVAIL_OFF); }
static struct vring_used  *ctrl_used(void)
    { return (struct vring_used  *)(g_snd_ring_page + CTRL_USED_OFF); }
static struct vring_desc  *tx_desc(void)
    { return (struct vring_desc  *)(g_snd_ring_page + TX_DESC_OFF); }
static struct vring_avail *tx_avail(void)
    { return (struct vring_avail *)(g_snd_ring_page + TX_AVAIL_OFF); }
static struct vring_used  *tx_used(void)
    { return (struct vring_used  *)(g_snd_ring_page + TX_USED_OFF); }

/* ---- probe ---- */
static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_SOUND)
        return 0;
    return 1;
}

static int setup_one_queue(uint32_t qid, uint32_t qsize,
                           uintptr_t desc_off,
                           uintptr_t avail_off,
                           uintptr_t used_off)
{
    w32(VIRTIO_MMIO_QUEUE_SEL, qid);
    if (r32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-snd] queue already ready, refusing\n");
        return -1;
    }
    uint32_t qmax = r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < qsize) {
        serial_puts("[virtio-snd] queue too small: max=");
        serial_puthex(qmax);
        serial_puts(" want=");
        serial_puthex(qsize);
        serial_puts("\n");
        return -1;
    }
    w32(VIRTIO_MMIO_QUEUE_NUM, qsize);

    uint64_t page_pa  = (uint64_t)(uintptr_t)g_snd_ring_page;
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

/* ---- CTRL queue: send one request, wait for one response. ---- */

/* Submit a 2-descriptor chain: [request, response] where the
 * request is `req_len` device-readable bytes and the response
 * is `resp_len` device-writable bytes.  Both buffers live in
 * g_snd_ctrl_page at fixed offsets.
 *
 * Blocks via yield() until the device returns the chain via the
 * CTRL used ring.  Returns the number of response bytes the
 * device wrote (or 0 on timeout). */
static uint32_t ctrl_submit_and_wait(uint32_t req_len,
                                     uint32_t resp_len)
{
    /* Two-descriptor chain in slots 0..1.  We never submit more
     * than one request at a time, so re-using slots 0/1 forever
     * is safe. */
    struct vring_desc *d = ctrl_desc();
    d[0].addr  = g_snd_ctrl_pa + CTRL_REQ_OFF;
    d[0].len   = req_len;
    d[0].flags = VRING_DESC_F_NEXT;
    d[0].next  = 1;
    d[1].addr  = g_snd_ctrl_pa + CTRL_RESP_OFF;
    d[1].len   = resp_len;
    d[1].flags = VRING_DESC_F_WRITE;
    d[1].next  = 0;

    /* Publish slot 0 (head of the chain) on the avail ring. */
    struct vring_avail *av = ctrl_avail();
    uint16_t slot = g_ctrl_avail_idx % CTRL_QSIZE;
    av->ring[slot] = 0;
    dmb();
    g_ctrl_avail_idx++;
    av->idx = g_ctrl_avail_idx;
    dsb();
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, CTRL_QID);

    /* Poll the used ring with a yield() between each iteration
     * so other threads can run.  Generous 500 ms cap because
     * QEMU's virtio-snd handler takes a moment on the very
     * first call (audio backend warm-up). */
    uint64_t start_ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
    uint64_t deadline = start_ms + 500;
    struct vring_used *u = ctrl_used();
    while (g_ctrl_used_seen == u->idx) {
        uint64_t now_ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
        if (now_ms > deadline) {
            serial_puts("[virtio-snd] CTRL timeout\n");
            return 0;
        }
        yield();
        dmb();
    }
    uint16_t used_slot = g_ctrl_used_seen % CTRL_QSIZE;
    uint32_t written   = u->ring[used_slot].len;
    g_ctrl_used_seen++;
    return written;
}

/* Read the first 4 bytes of the CTRL response as the status word. */
static uint32_t ctrl_response_status(void)
{
    struct virtio_snd_hdr *h = (struct virtio_snd_hdr *)
                               (g_snd_ctrl_page + CTRL_RESP_OFF);
    return h->code;
}

/* ---- High-level CTRL helpers ---- */

static int snd_pcm_set_params(uint32_t stream_id,
                              uint32_t buffer_bytes,
                              uint32_t period_bytes,
                              uint8_t  channels,
                              uint8_t  format_code,
                              uint8_t  rate_code)
{
    struct virtio_snd_pcm_set_params *r =
        (struct virtio_snd_pcm_set_params *)
        (g_snd_ctrl_page + CTRL_REQ_OFF);
    r->hdr.hdr.code   = VIRTIO_SND_R_PCM_SET_PARAMS;
    r->hdr.stream_id  = stream_id;
    r->buffer_bytes   = buffer_bytes;
    r->period_bytes   = period_bytes;
    r->features       = 0;
    r->channels       = channels;
    r->format         = format_code;
    r->rate           = rate_code;
    r->padding        = 0;

    if (ctrl_submit_and_wait(sizeof(*r), sizeof(struct virtio_snd_hdr)) == 0)
        return -1;
    if (ctrl_response_status() != VIRTIO_SND_S_OK) {
        serial_puts("[virtio-snd] SET_PARAMS rejected status=");
        serial_puthex(ctrl_response_status());
        serial_puts("\n");
        return -1;
    }
    return 0;
}

/* PCM_PREPARE / START / STOP / RELEASE all share the same wire
 * shape: virtio_snd_pcm_hdr request, virtio_snd_hdr response. */
static int snd_pcm_simple_cmd(uint32_t code, uint32_t stream_id)
{
    struct virtio_snd_pcm_hdr *r = (struct virtio_snd_pcm_hdr *)
                                   (g_snd_ctrl_page + CTRL_REQ_OFF);
    r->hdr.code  = code;
    r->stream_id = stream_id;

    if (ctrl_submit_and_wait(sizeof(*r), sizeof(struct virtio_snd_hdr)) == 0)
        return -1;
    if (ctrl_response_status() != VIRTIO_SND_S_OK) {
        serial_puts("[virtio-snd] cmd ");
        serial_puthex(code);
        serial_puts(" rejected status=");
        serial_puthex(ctrl_response_status());
        serial_puts("\n");
        return -1;
    }
    return 0;
}

/* ---- TX queue: submit one PCM payload, wait for status. ---- */

/* The TX slab already contains:
 *   [TX_HDR_OFF .. TX_DATA_OFF)        — virtio_snd_pcm_xfer header
 *   [TX_DATA_OFF .. TX_DATA_OFF + bytes) — PCM samples
 *   [TX_STATUS_OFF ..)                 — pcm_status response slot
 *
 * We submit a 2-descriptor chain (request, status) and wait for
 * the device to return it via the TX used ring. */
static int tx_submit_and_wait(uint32_t sample_bytes)
{
    /* Set the xfer header (stream id) every time — cheap, and
     * means callers don't have to remember to. */
    struct virtio_snd_pcm_xfer *xh = (struct virtio_snd_pcm_xfer *)
                                     (g_snd_tx_slab + TX_HDR_OFF);
    xh->stream_id = PCM_STREAM_ID;

    /* 2-descriptor chain in slots 0..1 of the TX desc table. */
    struct vring_desc *d = tx_desc();
    d[0].addr  = g_snd_tx_pa + TX_HDR_OFF;
    d[0].len   = sizeof(struct virtio_snd_pcm_xfer) + sample_bytes;
    d[0].flags = VRING_DESC_F_NEXT;
    d[0].next  = 1;
    d[1].addr  = g_snd_tx_pa + TX_STATUS_OFF;
    d[1].len   = sizeof(struct virtio_snd_pcm_status);
    d[1].flags = VRING_DESC_F_WRITE;
    d[1].next  = 0;

    struct vring_avail *av = tx_avail();
    uint16_t slot = g_tx_avail_idx % TX_QSIZE;
    av->ring[slot] = 0;
    dmb();
    g_tx_avail_idx++;
    av->idx = g_tx_avail_idx;
    dsb();
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, TX_QID);

    /* Wait for the device to consume the period.  The deadline
     * has to cover the playback duration itself plus a slack
     * margin: a 64 KiB period at 44_100 Hz mono S16 plays for
     * 64 * 1024 / 88200 ≈ 742 ms.  Allow 2 s. */
    uint64_t start_ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
    uint64_t deadline = start_ms + 2000;
    struct vring_used *u = tx_used();
    while (g_tx_used_seen == u->idx) {
        uint64_t now_ms = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
        if (now_ms > deadline) {
            serial_puts("[virtio-snd] TX timeout\n");
            return -1;
        }
        yield();
        dmb();
    }
    g_tx_used_seen++;

    struct virtio_snd_pcm_status *st = (struct virtio_snd_pcm_status *)
                                       (g_snd_tx_slab + TX_STATUS_OFF);
    if (st->status != VIRTIO_SND_S_OK) {
        serial_puts("[virtio-snd] TX status=");
        serial_puthex(st->status);
        serial_puts("\n");
        return -1;
    }
    return 0;
}

/* ---- init ---- */

static int init_device(uintptr_t base)
{
    g_snd_mmio_base = base;

    /* 1. Reset + handshake. */
    w32(VIRTIO_MMIO_STATUS, 0);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 2. Negotiate VERSION_1.  virtio-snd doesn't define any
     *    other features we care about for chapter 96 floor. */
    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t feat_hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(feat_hi & 1u)) {
        serial_puts("[virtio-snd] device does not advertise VERSION_1\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 1u);   /* VERSION_1 only */

    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK);
    if (!(r32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-snd] device cleared FEATURES_OK\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 3. Read device-config: jacks, streams, chmaps. */
    {
        struct virtio_snd_config cfg;
        for (uint32_t i = 0; i < sizeof(cfg); i++)
            ((uint8_t *)&cfg)[i] = mmio_read8(base + VIRTIO_MMIO_CONFIG + i);
        g_pcm_streams = cfg.streams;
        serial_puts("[virtio-snd] config jacks=");
        serial_puthex(cfg.jacks);
        serial_puts(" streams=");
        serial_puthex(cfg.streams);
        serial_puts(" chmaps=");
        serial_puthex(cfg.chmaps);
        serial_puts("\n");
        if (cfg.streams == 0) {
            serial_puts("[virtio-snd] no PCM streams advertised\n");
            w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }
    }

    /* 4. Allocate the ring page (one 4 KiB), the CTRL req/resp
     *    page (one 4 KiB), and the TX data slab (16 contiguous
     *    pages = 64 KiB). */
    g_snd_ring_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_snd_ring_page) {
        serial_puts("[virtio-snd] out of memory for ring page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    for (uint32_t i = 0; i < 4096; i++) g_snd_ring_page[i] = 0;

    g_snd_ctrl_pa = pmem_alloc_page();
    if (!g_snd_ctrl_pa) {
        serial_puts("[virtio-snd] out of memory for CTRL page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    g_snd_ctrl_page = (uint8_t *)(uintptr_t)g_snd_ctrl_pa;
    for (uint32_t i = 0; i < CTRL_PAGE_BYTES; i++) g_snd_ctrl_page[i] = 0;

    g_snd_tx_pa = pmem_alloc_contig(TX_SLAB_PAGES);
    if (!g_snd_tx_pa) {
        serial_puts("[virtio-snd] out of memory for TX slab\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    g_snd_tx_slab = (uint8_t *)(uintptr_t)g_snd_tx_pa;
    for (uint32_t i = 0; i < TX_SLAB_BYTES; i++) g_snd_tx_slab[i] = 0;

    /* 5. Wire up CTRLQ and TXQ.  EVENTQ and RXQ stay
     *    unconfigured; the device handles that gracefully. */
    if (setup_one_queue(CTRL_QID, CTRL_QSIZE,
                        CTRL_DESC_OFF, CTRL_AVAIL_OFF, CTRL_USED_OFF) < 0)
        return -1;
    if (setup_one_queue(TX_QID, TX_QSIZE,
                        TX_DESC_OFF, TX_AVAIL_OFF, TX_USED_OFF) < 0)
        return -1;

    /* 6. DRIVER_OK — must come BEFORE we send any control
     *    messages.  The device rejects CTRL submissions otherwise. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK |
                            VIRTIO_STATUS_DRIVER_OK);

    /* 7. Configure the PCM stream once and leave it running. */
    if (snd_pcm_set_params(PCM_STREAM_ID,
                           PCM_PERIOD_BYTES,    /* total buffer */
                           PCM_PERIOD_BYTES,    /* one period   */
                           1,                   /* mono         */
                           VIRTIO_SND_PCM_FMT_S16,
                           VIRTIO_SND_PCM_RATE_44100) < 0) {
        return -1;
    }
    if (snd_pcm_simple_cmd(VIRTIO_SND_R_PCM_PREPARE, PCM_STREAM_ID) < 0)
        return -1;
    if (snd_pcm_simple_cmd(VIRTIO_SND_R_PCM_START,   PCM_STREAM_ID) < 0)
        return -1;

    serial_puts("[virtio-snd] PCM ");
    serial_puthex(PCM_STREAM_ID);
    serial_puts(" ready: 1ch S16/44_100 Hz, period=");
    serial_puthex(PCM_PERIOD_BYTES);
    serial_puts("\n");
    return 0;
}

int virtio_snd_init(void)
{
    if (g_snd_mmio_base) return 0;
    for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (probe_slot(base)) {
            serial_puts("[virtio-snd] found device at slot ");
            serial_puthex(s);
            serial_puts(" base=");
            serial_puthex(base);
            serial_puts("\n");
            if (init_device(base) < 0) return -1;
            return 0;
        }
    }
    return -1;
}

int virtio_snd_present(void) { return g_snd_mmio_base != 0; }

/* ---- Public play API ---- */

int virtio_snd_play_square(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!g_snd_mmio_base) return -1;

    /* Clip parameters into a sensible range. */
    if (freq_hz < 20)        freq_hz = 20;
    if (freq_hz > 22050)     freq_hz = 22050;
    if (duration_ms < 1)     duration_ms = 1;
    if (duration_ms > 5000)  duration_ms = 5000;

    /* Total samples to play.  At 44_100 Hz mono, 1000 ms = 88_200
     * samples = 176_400 bytes.  We cap at 5 s = 882_000 bytes,
     * which we'll stream in TX_DATA_MAX-byte (~370 ms) periods. */
    uint64_t samples_to_play =
        ((uint64_t)PCM_RATE_HZ * (uint64_t)duration_ms) / 1000ull;
    uint64_t bytes_total = samples_to_play * PCM_BYTES_PER_FR;

    /* Square-wave half-period in samples.  At 1000 Hz this is
     * 22 samples (44_100 / 1000 / 2 = 22.05). */
    uint32_t half_period = (PCM_RATE_HZ / freq_hz) / 2;
    if (half_period == 0) half_period = 1;

    /* Modest amplitude to avoid clipping the host mixer when
     * coreaudio is the sink.  S16 range is ±32_767; ±10_000
     * is comfortably loud without being painful. */
    const int16_t HI = +10000;
    const int16_t LO = -10000;

    /* Stream-state across periods. */
    uint32_t sq_phase         = 0;        /* 0 .. half_period*2-1 */

    while (bytes_total > 0) {
        uint32_t bytes_this = (bytes_total > TX_DATA_MAX)
                            ? TX_DATA_MAX : (uint32_t)bytes_total;
        uint32_t samples_this = bytes_this / PCM_BYTES_PER_FR;

        int16_t *dst = (int16_t *)(g_snd_tx_slab + TX_DATA_OFF);
        for (uint32_t i = 0; i < samples_this; i++) {
            dst[i] = (sq_phase < half_period) ? HI : LO;
            sq_phase++;
            if (sq_phase >= half_period * 2) sq_phase = 0;
        }

        if (tx_submit_and_wait(bytes_this) < 0) return -1;
        bytes_total -= bytes_this;
    }

    serial_puts("[virtio-snd] played freq=");
    serial_puthex(freq_hz);
    serial_puts(" Hz duration=");
    serial_puthex(duration_ms);
    serial_puts(" ms\n");
    return 0;
}
