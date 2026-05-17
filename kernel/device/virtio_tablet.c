/*
 * kernel/device/virtio_tablet.c — milestone-41 virtio-input tablet.
 *
 * This driver is an EV_ABS-savvy sibling of virtio_input.c.  Same
 * handshake, same single-page queue layout, same lazy polling from
 * the syscall path.  Differences:
 *
 *   1. probe_slot() requires the device's EV_BITS to advertise
 *      EV_ABS support — virtio_input.c rejects exactly the same
 *      devices, so the two drivers partition the bus cleanly.
 *
 *   2. We read the device's ABS_X / ABS_Y range from the
 *      VIRTIO_INPUT_CFG_ABS_INFO config selector at init so we can
 *      scale events to the current framebuffer resolution.
 *
 *   3. handle_event() decodes EV_ABS X/Y / EV_KEY BTN_LEFT / etc.
 *      and forwards them to wm_pointer_move / wm_pointer_button
 *      instead of an ASCII ring buffer.
 */

#include "virtio_tablet.h"
#include "virtio_mmio.h"
#include "mmio.h"
#include "fb.h"
#include "../core/serial.h"
#include "../core/pmem.h"
#include "../core/wm.h"

#include <stdint.h>
#include <stddef.h>

static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

__attribute__((weak))
void *memset(void *d, int c, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

/* ---- evdev codes we care about ---- */
#define EV_SYN         0x00u
#define EV_KEY         0x01u
#define EV_ABS         0x03u

#define ABS_X          0x00u
#define ABS_Y          0x01u

#define BTN_LEFT       0x110u
#define BTN_RIGHT      0x111u
#define BTN_MIDDLE     0x112u

#define KEY_VAL_RELEASE 0u
#define KEY_VAL_PRESS   1u

/* Mirror the WM's button bitmap for clarity. */
#define WM_BTN_LEFT    0x1u
#define WM_BTN_RIGHT   0x2u
#define WM_BTN_MIDDLE  0x4u

/* ---- shared queue page layout (same as virtio_input.c) ---- */
#define QUEUE_SIZE       32u
#define DESC_TABLE_OFF   0x000u
#define AVAIL_RING_OFF   0x200u
#define USED_RING_OFF    0x300u
#define EVENT_SLOTS_OFF  0x600u
#define EVENT_SLOT_BYTES 8u

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

/* virtio-input config-space layout (5.8.4). */
#define VIRTIO_INPUT_CFG_ID_NAME   0x01u
#define VIRTIO_INPUT_CFG_EV_BITS   0x11u
#define VIRTIO_INPUT_CFG_ABS_INFO  0x12u
#define CFG_OFF_SELECT  0x00u
#define CFG_OFF_SUBSEL  0x01u
#define CFG_OFF_SIZE    0x02u
#define CFG_OFF_UNION   0x08u

/* ABS_INFO union content. */
struct virtio_input_absinfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
} __attribute__((packed));

/* ---- driver state ---- */
static uintptr_t g_mmio_base   = 0;          /* 0 = not initialised */
static uint8_t  *g_page        = NULL;       /* shared queue + slots */
static uint16_t  g_avail_idx   = 0;
static uint16_t  g_used_idx    = 0;
static uint32_t  g_abs_x_max   = 0x7FFFu;    /* QEMU default */
static uint32_t  g_abs_y_max   = 0x7FFFu;
static uint32_t  g_buttons     = 0;          /* WM_BTN_* bitmap */
static int32_t   g_last_sx     = -1;
static int32_t   g_last_sy     = -1;

/* Pending raw absolute coords; we wait for the EV_SYN report
 * before pushing into the WM, so motion + button events in the
 * same evdev "frame" are coalesced.  We always remember the most
 * recent absolute coords so a buttons-only frame can re-emit
 * motion at the cached location. */
static uint32_t  g_last_ax     = 0;
static uint32_t  g_last_ay     = 0;
static int       g_have_pending = 0;
static int       g_pending_x_set = 0;
static int       g_pending_y_set = 0;

/* ---- helpers ---- */
static inline uint32_t r32(uintptr_t off)
{
    return mmio_read32(g_mmio_base + off);
}
static inline void w32(uintptr_t off, uint32_t v)
{
    mmio_write32(g_mmio_base + off, v);
}
static struct vring_desc *desc_tbl(void)
{
    return (struct vring_desc *)(g_page + DESC_TABLE_OFF);
}
static struct vring_avail *avail_ring(void)
{
    return (struct vring_avail *)(g_page + AVAIL_RING_OFF);
}
static struct vring_used *used_ring(void)
{
    return (struct vring_used *)(g_page + USED_RING_OFF);
}
static struct virtio_input_event *event_slot(uint16_t idx)
{
    return (struct virtio_input_event *)
           (g_page + EVENT_SLOTS_OFF + (uint64_t)idx * EVENT_SLOT_BYTES);
}

/* ---- config-space helpers ---- */
static uint8_t cfg_query_size(uintptr_t base, uint8_t select, uint8_t subsel)
{
    mmio_write8(base + VIRTIO_MMIO_CONFIG + CFG_OFF_SELECT, select);
    mmio_write8(base + VIRTIO_MMIO_CONFIG + CFG_OFF_SUBSEL, subsel);
    dsb();
    return mmio_read8(base + VIRTIO_MMIO_CONFIG + CFG_OFF_SIZE);
}

static uint32_t cfg_read_abs_max(uintptr_t base, uint8_t axis)
{
    /* Select ABS_INFO, subsel = axis (0=ABS_X, 1=ABS_Y).  The
     * absinfo struct lives at config + 0x08 and starts with .min,
     * .max as little-endian 32-bit fields. */
    mmio_write8(base + VIRTIO_MMIO_CONFIG + CFG_OFF_SELECT,
                VIRTIO_INPUT_CFG_ABS_INFO);
    mmio_write8(base + VIRTIO_MMIO_CONFIG + CFG_OFF_SUBSEL, axis);
    dsb();
    uint8_t size = mmio_read8(base + VIRTIO_MMIO_CONFIG + CFG_OFF_SIZE);
    if (size < sizeof(struct virtio_input_absinfo)) return 0;
    /* min at +0x08, max at +0x0C — read max directly. */
    return mmio_read32(base + VIRTIO_MMIO_CONFIG + CFG_OFF_UNION + 4);
}

static void publish_descriptor(uint16_t desc_idx)
{
    struct vring_avail *av = avail_ring();
    av->ring[g_avail_idx % QUEUE_SIZE] = desc_idx;
    dmb();
    g_avail_idx++;
    av->idx = g_avail_idx;
    dmb();
}

/* ---- probe + init ---- */
static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_INPUT)
        return 0;
    /* Require EV_ABS support — keyboards report 0 here. */
    if (cfg_query_size(base, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS) == 0)
        return 0;
    return 1;
}

static int setup_queue(void)
{
    w32(VIRTIO_MMIO_QUEUE_SEL, 0);
    if (r32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-tablet] queue 0 already ready\n");
        return -1;
    }
    uint32_t qmax = r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < QUEUE_SIZE) {
        serial_puts("[virtio-tablet] queue too small\n");
        return -1;
    }
    w32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    uint64_t page_pa  = (uint64_t)(uintptr_t)g_page;
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

    struct vring_desc *d = desc_tbl();
    for (uint16_t i = 0; i < QUEUE_SIZE; i++) {
        d[i].addr  = page_pa + EVENT_SLOTS_OFF + (uint64_t)i * EVENT_SLOT_BYTES;
        d[i].len   = EVENT_SLOT_BYTES;
        d[i].flags = VRING_DESC_F_WRITE;
        d[i].next  = 0;
        publish_descriptor(i);
    }
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    return 0;
}

static int init_device(uintptr_t base)
{
    g_mmio_base = base;

    w32(VIRTIO_MMIO_STATUS, 0);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(hi & 1u)) {
        serial_puts("[virtio-tablet] device does not advertise VERSION_1\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    w32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w32(VIRTIO_MMIO_DRIVER_FEATURES, 1);

    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK);
    if (!(r32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-tablet] device cleared FEATURES_OK\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Read the device's absolute axis ranges before bringing the
     * queue up — done once at init. */
    uint32_t xmax = cfg_read_abs_max(base, ABS_X);
    uint32_t ymax = cfg_read_abs_max(base, ABS_Y);
    if (xmax) g_abs_x_max = xmax;
    if (ymax) g_abs_y_max = ymax;

    g_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_page) {
        serial_puts("[virtio-tablet] out of memory for queue page\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (setup_queue() < 0) {
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                            VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK |
                            VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

int virtio_tablet_init(void)
{
    for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (probe_slot(base)) {
            serial_puts("[virtio-tablet] found tablet at slot ");
            serial_puthex(s);
            serial_puts(" base=");
            serial_puthex(base);
            serial_puts("\n");
            if (init_device(base) < 0) return -1;
            serial_puts("[virtio-tablet] ready (abs=");
            serial_puthex(g_abs_x_max);
            serial_puts("x");
            serial_puthex(g_abs_y_max);
            serial_puts(")\n");
            return 0;
        }
    }
    return -1;
}

int virtio_tablet_present(void) { return g_mmio_base != 0; }

/* Scale device-absolute coords to framebuffer pixels.  Saturate so
 * we never feed the WM coordinates outside the screen. */
static void absolute_to_screen(uint32_t ax, uint32_t ay,
                               int32_t *sx, int32_t *sy)
{
    const struct fb_info *fb = fb_get_info();
    uint32_t fw = fb ? fb->width  : 1280;
    uint32_t fh = fb ? fb->height : 800;
    uint64_t x = (uint64_t)ax * (uint64_t)(fw ? fw - 1 : 0);
    uint64_t y = (uint64_t)ay * (uint64_t)(fh ? fh - 1 : 0);
    if (g_abs_x_max == 0) { *sx = 0; }
    else                  { *sx = (int32_t)(x / g_abs_x_max); }
    if (g_abs_y_max == 0) { *sy = 0; }
    else                  { *sy = (int32_t)(y / g_abs_y_max); }
    if (*sx < 0)               *sx = 0;
    if (*sx >= (int32_t)fw)    *sx = (int32_t)fw - 1;
    if (*sy < 0)               *sy = 0;
    if (*sy >= (int32_t)fh)    *sy = (int32_t)fh - 1;
}

static void flush_pending_motion(void)
{
    if (!g_have_pending) return;
    g_have_pending  = 0;
    g_pending_x_set = g_pending_y_set = 0;
    int32_t sx, sy;
    absolute_to_screen(g_last_ax, g_last_ay, &sx, &sy);
    /* Just remember the latest screen coords here.  We deliberately
     * do NOT call wm_pointer_move per-EV_SYN: under a fast drag the
     * device emits dozens of EV_ABS+EV_SYN pairs per polling cycle,
     * and every wm_pointer_move triggers a full-screen compose_all
     * (cursor sprite update).  Doing that 50x per drain is the
     * dominant cause of paint-app lag.  emit_pending_motion() below
     * pushes ONE wm_pointer_move per drain at the latest position;
     * handle_button() flushes early before any click. */
    g_last_sx = sx;
    g_last_sy = sy;
}

static void emit_pending_motion(void)
{
    flush_pending_motion();
    if (g_last_sx < 0 || g_last_sy < 0) return;
    /* wm_pointer_move() de-duplicates internally if the position
     * has not changed since its last call, so an unconditional
     * call here is safe and free in the common idle case. */
    wm_pointer_move(g_last_sx, g_last_sy);
}

static void handle_button(uint16_t code, uint32_t value)
{
    /* Make sure the WM has the latest position before the click is
     * dispatched (drag/focus depend on the cursor location).  We
     * emit here (not just resolve) because clicks are observed at
     * the cursor's current location — the WM must already have
     * moved the cursor to that spot. */
    emit_pending_motion();

    uint32_t bit;
    switch (code) {
    case BTN_LEFT:   bit = WM_BTN_LEFT;   break;
    case BTN_RIGHT:  bit = WM_BTN_RIGHT;  break;
    case BTN_MIDDLE: bit = WM_BTN_MIDDLE; break;
    default: return;
    }
    int down = (value == KEY_VAL_PRESS);
    if (down)  g_buttons |=  bit;
    else       g_buttons &= ~bit;
    wm_pointer_button(bit, down);
}

static void handle_event(const struct virtio_input_event *ev)
{
    switch (ev->type) {
    case EV_SYN:
        flush_pending_motion();
        return;
    case EV_ABS:
        if (ev->code == ABS_X) {
            g_last_ax       = ev->value;
            g_pending_x_set = 1;
            g_have_pending  = 1;
        } else if (ev->code == ABS_Y) {
            g_last_ay       = ev->value;
            g_pending_y_set = 1;
            g_have_pending  = 1;
        }
        return;
    case EV_KEY:
        handle_button(ev->code, ev->value);
        return;
    default:
        return;
    }
}

void virtio_tablet_poll(void)
{
    if (!g_mmio_base) return;

    struct vring_used *u = used_ring();
    /* Chapter 106b fast path: pump_input_into_wm() lands here on
     * every cooperative sys_yield.  Skip the MMIO traffic
     * (INTERRUPT_STATUS read + QUEUE_NOTIFY write both trap to
     * HVF) when the device hasn't produced new events.  u->idx
     * lives in shared RAM and is free to read.  See
     * virtio_input_poll for the same reasoning. */
    if ((uint16_t)(u->idx - g_used_idx) == 0) return;

    uint32_t istat = r32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (istat) w32(VIRTIO_MMIO_INTERRUPT_ACK, istat);

    dmb();
    while ((uint16_t)(u->idx - g_used_idx) > 0) {
        struct vring_used_elem *e = &u->ring[g_used_idx % QUEUE_SIZE];
        uint16_t desc_idx = (uint16_t)e->id;
        uint32_t got_len  = e->len;
        if (desc_idx < QUEUE_SIZE && got_len >= sizeof(struct virtio_input_event)) {
            const struct virtio_input_event *ev = event_slot(desc_idx);
            handle_event(ev);
        }
        if (desc_idx < QUEUE_SIZE) publish_descriptor(desc_idx);
        g_used_idx++;
    }
    /* In case the device sent an EV_ABS without a final EV_SYN
     * (shouldn't happen, but be defensive). */
    emit_pending_motion();
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}
