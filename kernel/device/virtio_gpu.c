/*
 * kernel/device/virtio_gpu.c — milestone-38 virtio-gpu (2D) driver.
 *
 * Mirrors the structure of virtio_blk.c: probe the same MMIO bus,
 * run the modern handshake, set up one virtqueue, then issue
 * synchronous polled commands.
 *
 * Memory layout for our single 4 KiB shared page:
 *
 *   offset 0x000 : descriptor table        (8 entries × 16 bytes)
 *   offset 0x080 : avail ring              (4 + 16 + 2 bytes)
 *   offset 0x0C0 : used ring               (4 + 64 + 2 bytes)
 *   offset 0x200 : staging request buffer  (256 bytes max)
 *   offset 0x300 : staging response buffer (384 bytes max)
 *
 * The largest request we ever issue is RESOURCE_ATTACH_BACKING with
 * one mem-entry: 24 (hdr) + 8 (resource_id, nr_entries) + 16
 * (mem_entry) = 48 bytes.  The largest response is the GET_DISPLAY_INFO
 * payload: 24 (hdr) + 16 × 24 (per-scanout pmode) = 408 bytes — does
 * not fit; we cap at 388 bytes (= 24 + 16 × 24 - 4 padding) which is
 * still bigger than what we actually need (only scanout 0).  Actually
 * VIRTIO_GPU_MAX_SCANOUTS is 16 per spec but QEMU only ever reports
 * up to its `max_outputs` parameter (default 1), so the rest of the
 * pmodes table is just zeroed; truncating it is safe in practice.
 * To stay strictly correct we reserve 384 bytes of response window
 * which fits 14 full pmodes (384 - 24) / 24 = 15 — close enough,
 * and we only ever read pmodes[0].
 */

#include "virtio_gpu.h"
#include "virtio_mmio.h"
#include "mmio.h"
#include "../core/serial.h"
#include "../core/pmem.h"

#include <stdint.h>
#include <stddef.h>

/* GCC's optimizer turns large struct initialisers (`struct foo x = { 0 }`
 * or `{ .field = ... }`) into memset calls.  In freestanding code there
 * is no libc to satisfy the symbol, so we provide a one-liner here.
 * See /memories/freestanding-c-memset-trap.md for the full story. */
void *memset(void *d, int c, size_t n);
void *memset(void *d, int c, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* ---- Layout of our shared page ---- */
#define QUEUE_SIZE         8u

#define DESC_TABLE_OFF     0x000u
#define AVAIL_RING_OFF     0x080u
#define USED_RING_OFF      0x0C0u
#define STAGE_REQ_OFF      0x200u
#define STAGE_RESP_OFF     0x300u
#define STAGE_REQ_CAPACITY  256u
#define STAGE_RESP_CAPACITY 384u

/* ---- virtio-gpu command + response codes (spec 5.7.6.7) ---- */
#define VGPU_CMD_GET_DISPLAY_INFO         0x0100u
#define VGPU_CMD_RESOURCE_CREATE_2D       0x0101u
#define VGPU_CMD_RESOURCE_UNREF           0x0102u
#define VGPU_CMD_SET_SCANOUT              0x0103u
#define VGPU_CMD_RESOURCE_FLUSH           0x0104u
#define VGPU_CMD_TRANSFER_TO_HOST_2D      0x0105u
#define VGPU_CMD_RESOURCE_ATTACH_BACKING  0x0106u

#define VGPU_RESP_OK_NODATA               0x1100u
#define VGPU_RESP_OK_DISPLAY_INFO         0x1101u

/* ---- On-the-wire structs (all little-endian, all packed) ---- */
struct vgpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

#define VGPU_MAX_SCANOUTS_WIRE 16
struct vgpu_resp_display_info {
    struct vgpu_ctrl_hdr hdr;
    struct {
        struct vgpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VGPU_MAX_SCANOUTS_WIRE];
} __attribute__((packed));

struct vgpu_req_resource_create_2d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct vgpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_req_resource_attach_backing {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct vgpu_mem_entry entry;   /* exactly one — we always pass one */
} __attribute__((packed));

struct vgpu_req_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct vgpu_req_transfer_to_host_2d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_req_resource_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ---- Driver state ---- */
static uintptr_t g_gpu_mmio_base = 0;        /* 0 = not initialised */
static uint8_t  *g_gpu_page      = NULL;     /* shared queue + staging page */
static uint16_t  g_avail_idx_seen = 0;
static uint16_t  g_used_idx_seen  = 0;
static uint32_t  g_gpu_width  = 0;
static uint32_t  g_gpu_height = 0;

/* ---- helpers ---- */
static inline uint32_t r32(uintptr_t off)
{
    return mmio_read32(g_gpu_mmio_base + off);
}
static inline void w32(uintptr_t off, uint32_t v)
{
    mmio_write32(g_gpu_mmio_base + off, v);
}
static struct vring_desc *desc_tbl(void)
{
    return (struct vring_desc *)(g_gpu_page + DESC_TABLE_OFF);
}
static struct vring_avail *avail_ring(void)
{
    return (struct vring_avail *)(g_gpu_page + AVAIL_RING_OFF);
}
static struct vring_used *used_ring(void)
{
    return (struct vring_used *)(g_gpu_page + USED_RING_OFF);
}

static void mem_copy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void mem_zero(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}

/* ---- probe + init ---- */
static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_GPU)
        return 0;
    return 1;
}

static int setup_queue(void)
{
    /* Select control queue (queue 0). */
    w32(VIRTIO_MMIO_QUEUE_SEL, 0);
    if (r32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-gpu] queue 0 already ready, refusing\n");
        return -1;
    }
    uint32_t qmax = r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < QUEUE_SIZE) {
        serial_puts("[virtio-gpu] queue 0 max=");
        serial_puthex(qmax);
        serial_puts(" < required ");
        serial_puthex(QUEUE_SIZE);
        serial_puts("\n");
        return -1;
    }
    w32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    uint64_t page_pa = (uint64_t)(uintptr_t)g_gpu_page;
    uint64_t desc_pa  = page_pa + DESC_TABLE_OFF;
    uint64_t avail_pa = page_pa + AVAIL_RING_OFF;
    uint64_t used_pa  = page_pa + USED_RING_OFF;

    w32(VIRTIO_MMIO_QUEUE_DESC_LO,   (uint32_t)(desc_pa & 0xffffffffu));
    w32(VIRTIO_MMIO_QUEUE_DESC_HI,   (uint32_t)(desc_pa >> 32));
    w32(VIRTIO_MMIO_QUEUE_DRIVER_LO, (uint32_t)(avail_pa & 0xffffffffu));
    w32(VIRTIO_MMIO_QUEUE_DRIVER_HI, (uint32_t)(avail_pa >> 32));
    w32(VIRTIO_MMIO_QUEUE_DEVICE_LO, (uint32_t)(used_pa & 0xffffffffu));
    w32(VIRTIO_MMIO_QUEUE_DEVICE_HI, (uint32_t)(used_pa >> 32));

    dsb();
    w32(VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int init_device(uintptr_t base)
{
    g_gpu_mmio_base = base;

    /* 1. Reset. */
    w32(VIRTIO_MMIO_STATUS, 0);
    /* 2. ACK. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    /* 3. DRIVER. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Read device features (we only care about VERSION_1). */
    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(hi & 1u)) {
        serial_puts("[virtio-gpu] device does not advertise VERSION_1\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 5. Accept VERSION_1 only. */
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 1);

    /* 6. FEATURES_OK. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK);
    if (!(r32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-gpu] device cleared FEATURES_OK\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 7. Allocate the shared page and set up the control queue. */
    g_gpu_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_gpu_page) {
        serial_puts("[virtio-gpu] out of memory for queue page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (setup_queue() < 0) {
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 8. DRIVER_OK. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK |
                            VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

/* Issue one virtio-gpu command synchronously.  Builds a 2-descriptor
 * chain (request, then a device-writable response slot) and polls the
 * used ring for completion.  Returns 0 on OK, negative on transport
 * failure or non-OK device response. */
static int submit(const void *req, uint32_t req_len,
                  void *resp, uint32_t resp_len)
{
    if (req_len > STAGE_REQ_CAPACITY || resp_len > STAGE_RESP_CAPACITY) {
        serial_puts("[virtio-gpu] submit: buffer too large\n");
        return -1;
    }
    uint8_t *req_buf  = g_gpu_page + STAGE_REQ_OFF;
    uint8_t *resp_buf = g_gpu_page + STAGE_RESP_OFF;
    mem_copy(req_buf,  req,  req_len);
    mem_zero(resp_buf, resp_len);

    struct vring_desc *d = desc_tbl();
    uint64_t page_pa = (uint64_t)(uintptr_t)g_gpu_page;

    d[0].addr  = page_pa + STAGE_REQ_OFF;
    d[0].len   = req_len;
    d[0].flags = VRING_DESC_F_NEXT;
    d[0].next  = 1;

    d[1].addr  = page_pa + STAGE_RESP_OFF;
    d[1].len   = resp_len;
    d[1].flags = VRING_DESC_F_WRITE;     /* device-writable */
    d[1].next  = 0;

    struct vring_avail *av = avail_ring();
    uint16_t slot = g_avail_idx_seen % QUEUE_SIZE;
    av->ring[slot] = 0;                  /* head index */
    dmb();
    g_avail_idx_seen++;
    av->idx = g_avail_idx_seen;
    dmb();

    w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    struct vring_used *u = used_ring();
    uint16_t target = g_used_idx_seen + 1;
    for (uint64_t spin = 0; spin < (1ULL << 28); spin++) {
        dmb();
        if (u->idx == target) goto got_it;
    }
    serial_puts("[virtio-gpu] timeout waiting for completion\n");
    return -1;

got_it:
    g_used_idx_seen = target;

    /* Copy the device's response back to the caller. */
    if (resp && resp_len) mem_copy(resp, resp_buf, resp_len);

    /* Inspect the response header. */
    struct vgpu_ctrl_hdr *hdr = (struct vgpu_ctrl_hdr *)resp_buf;
    if (hdr->type != VGPU_RESP_OK_NODATA &&
        hdr->type != VGPU_RESP_OK_DISPLAY_INFO) {
        serial_puts("[virtio-gpu] non-OK response type=");
        serial_puthex(hdr->type);
        serial_puts("\n");
        return -2;
    }
    return 0;
}

int virtio_gpu_init(void)
{
    /* Find a virtio-gpu on the bus. */
    for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (probe_slot(base)) {
            serial_puts("[virtio-gpu] found GPU at slot ");
            serial_puthex(s);
            serial_puts(" base=");
            serial_puthex(base);
            serial_puts("\n");
            if (init_device(base) < 0) return -1;
            goto have_device;
        }
    }
    serial_puts("[virtio-gpu] no virtio-gpu device found on the bus\n");
    return -1;

have_device:
    /* Ask the host for the geometry of scanout 0. */
    struct vgpu_ctrl_hdr req = { .type = VGPU_CMD_GET_DISPLAY_INFO };
    struct vgpu_resp_display_info info;
    /* Copy back only the bytes we have room for in the staging slot;
     * we read pmodes[0] only. */
    if (submit(&req, sizeof(req), &info, STAGE_RESP_CAPACITY) < 0) {
        serial_puts("[virtio-gpu] GET_DISPLAY_INFO failed\n");
        return -1;
    }

    g_gpu_width  = info.pmodes[0].r.width;
    g_gpu_height = info.pmodes[0].r.height;

    serial_puts("[virtio-gpu] scanout 0 = ");
    serial_puthex(g_gpu_width);
    serial_puts(" x ");
    serial_puthex(g_gpu_height);
    serial_puts(" enabled=");
    serial_puthex(info.pmodes[0].enabled);
    serial_puts("\n");

    if (g_gpu_width == 0 || g_gpu_height == 0) {
        serial_puts("[virtio-gpu] degenerate geometry, refusing\n");
        return -1;
    }
    return 0;
}

int      virtio_gpu_present(void) { return g_gpu_mmio_base != 0; }
uint32_t virtio_gpu_width(void)   { return g_gpu_width;          }
uint32_t virtio_gpu_height(void)  { return g_gpu_height;         }

int virtio_gpu_set_framebuffer(uint64_t phys, uint32_t length,
                               uint32_t width, uint32_t height)
{
    if (!virtio_gpu_present()) return -1;

    /* RESOURCE_CREATE_2D */
    {
        struct vgpu_req_resource_create_2d req = {
            .hdr         = { .type = VGPU_CMD_RESOURCE_CREATE_2D },
            .resource_id = VIRTIO_GPU_FB_RESOURCE_ID,
            .format      = VIRTIO_GPU_FMT_B8G8R8X8_UNORM,
            .width       = width,
            .height      = height,
        };
        struct vgpu_ctrl_hdr resp;
        if (submit(&req, sizeof(req), &resp, sizeof(resp)) < 0) {
            serial_puts("[virtio-gpu] RESOURCE_CREATE_2D failed\n");
            return -1;
        }
    }

    /* RESOURCE_ATTACH_BACKING (single mem-entry pointing at the phys
     * region the framebuffer module just allocated). */
    {
        struct vgpu_req_resource_attach_backing req = {
            .hdr         = { .type = VGPU_CMD_RESOURCE_ATTACH_BACKING },
            .resource_id = VIRTIO_GPU_FB_RESOURCE_ID,
            .nr_entries  = 1,
            .entry       = { .addr = phys, .length = length, .padding = 0 },
        };
        struct vgpu_ctrl_hdr resp;
        if (submit(&req, sizeof(req), &resp, sizeof(resp)) < 0) {
            serial_puts("[virtio-gpu] RESOURCE_ATTACH_BACKING failed\n");
            return -1;
        }
    }

    /* SET_SCANOUT (display the resource on output 0, full surface). */
    {
        struct vgpu_req_set_scanout req = {
            .hdr         = { .type = VGPU_CMD_SET_SCANOUT },
            .r           = { .x = 0, .y = 0, .width = width, .height = height },
            .scanout_id  = 0,
            .resource_id = VIRTIO_GPU_FB_RESOURCE_ID,
        };
        struct vgpu_ctrl_hdr resp;
        if (submit(&req, sizeof(req), &resp, sizeof(resp)) < 0) {
            serial_puts("[virtio-gpu] SET_SCANOUT failed\n");
            return -1;
        }
    }
    return 0;
}

int virtio_gpu_flush_rect(uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h,
                          uint64_t offset)
{
    if (!virtio_gpu_present()) return -1;

    /* TRANSFER_TO_HOST_2D — copy from guest backing into host
     * resource. */
    {
        struct vgpu_req_transfer_to_host_2d req = {
            .hdr         = { .type = VGPU_CMD_TRANSFER_TO_HOST_2D },
            .r           = { .x = x, .y = y, .width = w, .height = h },
            .offset      = offset,
            .resource_id = VIRTIO_GPU_FB_RESOURCE_ID,
        };
        struct vgpu_ctrl_hdr resp;
        if (submit(&req, sizeof(req), &resp, sizeof(resp)) < 0) {
            serial_puts("[virtio-gpu] TRANSFER_TO_HOST_2D failed\n");
            return -1;
        }
    }

    /* RESOURCE_FLUSH — push from host resource onto the display. */
    {
        struct vgpu_req_resource_flush req = {
            .hdr         = { .type = VGPU_CMD_RESOURCE_FLUSH },
            .r           = { .x = x, .y = y, .width = w, .height = h },
            .resource_id = VIRTIO_GPU_FB_RESOURCE_ID,
        };
        struct vgpu_ctrl_hdr resp;
        if (submit(&req, sizeof(req), &resp, sizeof(resp)) < 0) {
            serial_puts("[virtio-gpu] RESOURCE_FLUSH failed\n");
            return -1;
        }
    }
    return 0;
}
