/*
 * kernel/device/virtio_input.c — milestone-39 virtio-input keyboard.
 *
 * Mirrors the structure of virtio_blk.c / virtio_gpu.c: probe, run
 * the modern handshake, set up one virtqueue, then service the used
 * ring lazily from `virtio_input_poll`.
 *
 * Memory layout for our single 4 KiB shared page:
 *
 *   offset 0x000 : descriptor table        (32 entries × 16 bytes)
 *   offset 0x200 : avail ring              (4 + 64 + 2 bytes)
 *   offset 0x300 : used ring               (4 + 256 + 2 bytes)
 *   offset 0x600 : 32 × 8-byte event slots (one per descriptor)
 *
 * Each descriptor in the table is permanently bound to its event
 * slot — slot index 0..31 lives at offset 0x600 + 8*i.  When the
 * device fills a slot, the used ring publishes its descriptor index;
 * after we read the event we put the descriptor back on the avail
 * ring so the device can fill it again.
 *
 * NOTE: The virtio-input spec (5.8.6) actually places the eventq at
 * id 0 and the statusq at id 1.  We never set up the statusq — we
 * do not need LED control or rumble — and QEMU does not require it.
 */

#include "virtio_input.h"
#include "virtio_mmio.h"
#include "mmio.h"
#include "../core/serial.h"
#include "../core/pmem.h"
#include "../core/wm.h"          /* GUI_KEY_* extended-key codes */

#include <stdint.h>
#include <stddef.h>

static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* GCC's optimizer can synthesize memset for `struct foo x = { 0 }`.
 * In freestanding code we provide a one-liner; weak so it does not
 * collide with the copy in virtio_gpu.c if both TUs are linked. */
__attribute__((weak))
void *memset(void *d, int c, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

/* ---- Layout of our shared page ---- */
#define QUEUE_SIZE       32u

#define DESC_TABLE_OFF   0x000u   /* 32 * 16 = 512 bytes  -> [0x000..0x200) */
#define AVAIL_RING_OFF   0x200u   /* 4 + 64 + 2 = 70 bytes -> [0x200..0x246) */
#define USED_RING_OFF    0x300u   /* 4 + 32*8 + 2 = 262   -> [0x300..0x406) */
#define EVENT_SLOTS_OFF  0x600u   /* 32 * 8 = 256 bytes   -> [0x600..0x700) */
#define EVENT_SLOT_BYTES 8u

/* virtio-input event (spec 5.8.6) — Linux evdev compatible. */
struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

/* evdev event types we care about. */
#define EV_SYN              0x00u
#define EV_KEY              0x01u
#define EV_REL              0x02u
#define EV_ABS              0x03u

/* evdev key value semantics. */
#define KEY_VAL_RELEASE     0u
#define KEY_VAL_PRESS       1u
#define KEY_VAL_REPEAT      2u

/* Subset of Linux input-event-codes.h that maps to ASCII / control.
 * Source: include/uapi/linux/input-event-codes.h */
enum {
    KC_RESERVED   = 0,
    KC_ESC        = 1,
    KC_1          = 2,
    KC_2, KC_3, KC_4, KC_5, KC_6, KC_7, KC_8, KC_9,
    KC_0          = 11,
    KC_MINUS      = 12,
    KC_EQUAL      = 13,
    KC_BACKSPACE  = 14,
    KC_TAB        = 15,
    KC_Q          = 16,
    KC_W, KC_E, KC_R, KC_T, KC_Y, KC_U, KC_I, KC_O,
    KC_P          = 25,
    KC_LBRACE     = 26,
    KC_RBRACE     = 27,
    KC_ENTER      = 28,
    KC_LCTRL      = 29,
    KC_A          = 30,
    KC_S, KC_D, KC_F, KC_G, KC_H, KC_J, KC_K, KC_L,
    KC_SEMICOLON  = 39,
    KC_APOSTROPHE = 40,
    KC_GRAVE      = 41,
    KC_LSHIFT     = 42,
    KC_BACKSLASH  = 43,
    KC_Z          = 44,
    KC_X, KC_C, KC_V, KC_B, KC_N,
    KC_M          = 50,
    KC_COMMA      = 51,
    KC_DOT        = 52,
    KC_SLASH      = 53,
    KC_RSHIFT     = 54,
    KC_KPASTERISK = 55,
    KC_LALT       = 56,
    KC_SPACE      = 57,
    KC_CAPSLOCK   = 58,
    KC_F1         = 59,
    /* … skip F1..F10 … */
    KC_HOME       = 102,
    KC_UP         = 103,
    KC_PAGEUP     = 104,
    KC_LEFT       = 105,
    KC_RIGHT      = 106,
    KC_END        = 107,
    KC_DOWN       = 108,
    KC_PAGEDOWN   = 109,
    KC_DELETE     = 111,
    KC_RCTRL      = 97,
    KC_RALT       = 100,
};

/* Unshifted ASCII for the printable keycodes. 0 means "not a single
 * printable byte" (handled by the cursor-key escape path or just
 * dropped). */
static const char kc_to_ascii[128] = {
    [KC_1] = '1', [KC_2] = '2', [KC_3] = '3', [KC_4] = '4',
    [KC_5] = '5', [KC_6] = '6', [KC_7] = '7', [KC_8] = '8',
    [KC_9] = '9', [KC_0] = '0',
    [KC_MINUS] = '-', [KC_EQUAL] = '=',
    [KC_Q] = 'q', [KC_W] = 'w', [KC_E] = 'e', [KC_R] = 'r',
    [KC_T] = 't', [KC_Y] = 'y', [KC_U] = 'u', [KC_I] = 'i',
    [KC_O] = 'o', [KC_P] = 'p',
    [KC_LBRACE] = '[', [KC_RBRACE] = ']',
    [KC_A] = 'a', [KC_S] = 's', [KC_D] = 'd', [KC_F] = 'f',
    [KC_G] = 'g', [KC_H] = 'h', [KC_J] = 'j', [KC_K] = 'k',
    [KC_L] = 'l',
    [KC_SEMICOLON] = ';', [KC_APOSTROPHE] = '\'',
    [KC_GRAVE] = '`',
    [KC_BACKSLASH] = '\\',
    [KC_Z] = 'z', [KC_X] = 'x', [KC_C] = 'c', [KC_V] = 'v',
    [KC_B] = 'b', [KC_N] = 'n', [KC_M] = 'm',
    [KC_COMMA] = ',', [KC_DOT] = '.', [KC_SLASH] = '/',
    [KC_SPACE] = ' ',
};

/* Shifted-row replacement for non-letter keys.  Letters are
 * uppercased by toupper() at lookup time so we don't have to
 * duplicate the alpha row here. */
static const char kc_to_ascii_shift[128] = {
    [KC_1] = '!', [KC_2] = '@', [KC_3] = '#', [KC_4] = '$',
    [KC_5] = '%', [KC_6] = '^', [KC_7] = '&', [KC_8] = '*',
    [KC_9] = '(', [KC_0] = ')',
    [KC_MINUS] = '_', [KC_EQUAL] = '+',
    [KC_LBRACE] = '{', [KC_RBRACE] = '}',
    [KC_SEMICOLON] = ':', [KC_APOSTROPHE] = '"',
    [KC_GRAVE] = '~',
    [KC_BACKSLASH] = '|',
    [KC_COMMA] = '<', [KC_DOT] = '>', [KC_SLASH] = '?',
    [KC_SPACE] = ' ',
};

/* ---- Driver state ---- */
static uintptr_t g_in_mmio_base   = 0;        /* 0 = not initialised */
static uint8_t  *g_in_page        = NULL;     /* shared queue + slots */
static uint16_t  g_avail_idx_seen = 0;        /* our last published avail.idx */
static uint16_t  g_used_idx_seen  = 0;        /* last used.idx we've observed */

/* Modifier state, derived from press/release of shift/ctrl. */
static uint8_t   g_shift_down     = 0;
static uint8_t   g_ctrl_down      = 0;

/* Tiny ring buffer for translated ASCII.  Power-of-2 size so we can
 * mask instead of mod.  Keys held down + autorepeat in QEMU rarely
 * exceed ~30 bytes/sec; 128 is more than enough latency cushion. */
#define KBD_RING_SIZE 128
static char     g_kbd_ring[KBD_RING_SIZE];
static uint32_t g_kbd_head = 0;
static uint32_t g_kbd_tail = 0;

static int ring_empty(void) { return g_kbd_head == g_kbd_tail; }
static int ring_full(void)
{
    return ((g_kbd_head + 1) & (KBD_RING_SIZE - 1)) == g_kbd_tail;
}
static void ring_push(char c)
{
    if (ring_full()) return;       /* drop oldest-most-recent: no-op */
    g_kbd_ring[g_kbd_head] = c;
    g_kbd_head = (g_kbd_head + 1) & (KBD_RING_SIZE - 1);
}
static int ring_pop(char *out)
{
    if (ring_empty()) return 0;
    *out = g_kbd_ring[g_kbd_tail];
    g_kbd_tail = (g_kbd_tail + 1) & (KBD_RING_SIZE - 1);
    return 1;
}

/* ---- Key-release ring ----------------------------------------
 *
 * The byte ring above is the byte-stream view of presses (the
 * shell's read(0) sees the same bytes a real terminal would).
 * Releases don't have a natural byte representation — there is no
 * ASCII for "the W key just came up" — so they ride a parallel
 * ring of uint32_t GUI key codes (ASCII 0..255 or one of the
 * GUI_KEY_* extended codes 0x101..0x108).  The consumer in
 * syscall.c's pump_input_into_wm drains this ring and calls
 * wm_keyboard_release for each entry, which delivers a
 * GUI_EVENT_KEY_UP to the focused window.  Apps that don't care
 * (the shell, notepad, the launcher) ignore the event type.
 *
 * QEMU's virtio-keyboard never emits more than one release per
 * EV_SYN batch and the consumer drains on every yield, so 32
 * slots is a wide margin. */
#define REL_RING_SIZE 32
static uint32_t g_rel_ring[REL_RING_SIZE];
static uint32_t g_rel_head = 0;
static uint32_t g_rel_tail = 0;

static int rel_ring_empty(void) { return g_rel_head == g_rel_tail; }
static void rel_ring_push(uint32_t key)
{
    uint32_t next = (g_rel_head + 1) & (REL_RING_SIZE - 1);
    if (next == g_rel_tail) return;   /* full → drop newest */
    g_rel_ring[g_rel_head] = key;
    g_rel_head = next;
}
static int rel_ring_pop(uint32_t *out)
{
    if (rel_ring_empty()) return 0;
    *out = g_rel_ring[g_rel_tail];
    g_rel_tail = (g_rel_tail + 1) & (REL_RING_SIZE - 1);
    return 1;
}

/* Map an evdev keycode to the GUI key code that the matching
 * press path would have produced.  Used only on release, so the
 * focused window sees press('W') / release('W') as a symmetric
 * pair (case differences from a shift held during press vs
 * release don't matter for the game-input use case — the codes
 * here are the unshifted identity of the physical key).
 *
 * Returns 0 for codes that have no GUI side (modifiers, function
 * keys we don't translate, unknowns); the caller drops those. */
static uint32_t code_to_gui_release(uint16_t code)
{
    switch (code) {
    case KC_UP:        return GUI_KEY_UP;
    case KC_DOWN:      return GUI_KEY_DOWN;
    case KC_LEFT:      return GUI_KEY_LEFT;
    case KC_RIGHT:     return GUI_KEY_RIGHT;
    case KC_HOME:      return GUI_KEY_HOME;
    case KC_END:       return GUI_KEY_END;
    case KC_PAGEUP:    return GUI_KEY_PGUP;
    case KC_PAGEDOWN:  return GUI_KEY_PGDN;
    case KC_BACKSPACE: return 0x7Fu;
    case KC_DELETE:    return 0x7Fu;
    case KC_TAB:       return 0x09u;
    case KC_ENTER:     return 0x0Du;
    case KC_ESC:       return 0x1Bu;
    default: break;
    }
    if (code < 128) {
        char base = kc_to_ascii[code];
        if (base) return (uint32_t)(uint8_t)base;
    }
    return 0;
}

/* ---- helpers ---- */
static inline uint32_t r32(uintptr_t off)
{
    return mmio_read32(g_in_mmio_base + off);
}
static inline void w32(uintptr_t off, uint32_t v)
{
    mmio_write32(g_in_mmio_base + off, v);
}
static struct vring_desc *desc_tbl(void)
{
    return (struct vring_desc *)(g_in_page + DESC_TABLE_OFF);
}
static struct vring_avail *avail_ring(void)
{
    return (struct vring_avail *)(g_in_page + AVAIL_RING_OFF);
}
static struct vring_used *used_ring(void)
{
    return (struct vring_used *)(g_in_page + USED_RING_OFF);
}
static struct virtio_input_event *event_slot(uint16_t idx)
{
    return (struct virtio_input_event *)
           (g_in_page + EVENT_SLOTS_OFF + (uint64_t)idx * EVENT_SLOT_BYTES);
}

/* Publish descriptor `desc_idx` as a fresh empty buffer the device
 * can fill with the next event. */
static void publish_descriptor(uint16_t desc_idx)
{
    struct vring_avail *av = avail_ring();
    uint16_t slot = g_avail_idx_seen % QUEUE_SIZE;
    av->ring[slot] = desc_idx;
    dmb();
    g_avail_idx_seen++;
    av->idx = g_avail_idx_seen;
    dmb();
}

/* ---- probe + init ---- */

/* virtio-input device-specific config layout (spec 5.8.4):
 *   +0x00  u8 select         (VIRTIO_INPUT_CFG_*)
 *   +0x01  u8 subsel         (selector-dependent)
 *   +0x02  u8 size           (size of returned data in u)
 *   +0x08  union u           (string[128] / bitmap[128] / absinfo / devids)
 *
 * We use the EV_BITS query to tell keyboards apart from tablets:
 * write select=0x11 (EV_BITS), subsel=EV_ABS, then read `size`.  A
 * tablet/mouse reports a non-zero EV_ABS bitmap; a keyboard reports
 * size=0 because it has no absolute axes. */
#define VIRTIO_INPUT_CFG_EV_BITS  0x11u

static uint8_t cfg_query_size(uintptr_t base, uint8_t select, uint8_t subsel)
{
    /* Single-byte writes / reads — the config register region behaves
     * like normal memory, but we use the dedicated 8-bit accessors
     * to keep each access aligned to its declared width. */
    mmio_write8(base + VIRTIO_MMIO_CONFIG + 0x00, select);
    mmio_write8(base + VIRTIO_MMIO_CONFIG + 0x01, subsel);
    /* The device updates `size` synchronously when select/subsel are
     * written; no fence needed in QEMU but DSB doesn't hurt. */
    dsb();
    return mmio_read8(base + VIRTIO_MMIO_CONFIG + 0x02);
}

static int probe_slot(uintptr_t base)
{
    if (mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_VERSION) != 2u)
        return 0;
    if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_INPUT)
        return 0;
    /* Reject tablet / mouse devices — they belong to virtio_tablet. */
    if (cfg_query_size(base, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS) != 0)
        return 0;
    return 1;
}

static int setup_queue(void)
{
    /* Select event queue (queue 0). */
    w32(VIRTIO_MMIO_QUEUE_SEL, 0);
    if (r32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        serial_puts("[virtio-input] queue 0 already ready, refusing\n");
        return -1;
    }
    uint32_t qmax = r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < QUEUE_SIZE) {
        serial_puts("[virtio-input] queue 0 max=");
        serial_puthex(qmax);
        serial_puts(" < required ");
        serial_puthex(QUEUE_SIZE);
        serial_puts("\n");
        return -1;
    }
    w32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    uint64_t page_pa  = (uint64_t)(uintptr_t)g_in_page;
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

    /* Pre-bind every descriptor to its permanent slot and publish
     * them all so the device has 32 buffers ready to fill. */
    struct vring_desc *d = desc_tbl();
    for (uint16_t i = 0; i < QUEUE_SIZE; i++) {
        d[i].addr  = page_pa + EVENT_SLOTS_OFF + (uint64_t)i * EVENT_SLOT_BYTES;
        d[i].len   = EVENT_SLOT_BYTES;
        d[i].flags = VRING_DESC_F_WRITE;     /* device writes the event */
        d[i].next  = 0;
        publish_descriptor(i);
    }
    /* One kick after all of them are queued. */
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    return 0;
}

static int init_device(uintptr_t base)
{
    g_in_mmio_base = base;

    /* 1. Reset. */
    w32(VIRTIO_MMIO_STATUS, 0);
    /* 2. ACK. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    /* 3. DRIVER. */
    w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Read device features (we only care about VERSION_1 in the
     *    upper 32 bits). */
    w32(VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t hi = r32(VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(hi & 1u)) {
        serial_puts("[virtio-input] device does not advertise VERSION_1\n");
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
        serial_puts("[virtio-input] device cleared FEATURES_OK\n");
        w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 7. Allocate the shared page. */
    g_in_page = (uint8_t *)(uintptr_t)pmem_alloc_page();
    if (!g_in_page) {
        serial_puts("[virtio-input] out of memory for queue page\n");
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

int virtio_input_init(void)
{
    /* QEMU may attach more than one virtio-input device (e.g. one
     * keyboard + one tablet).  We accept the FIRST one we find and
     * call it the keyboard; in QEMU's standard graphical configs
     * with `-device virtio-keyboard-device` that's correct.
     *
     * A future milestone can read the device-specific config space
     * (VIRTIO_INPUT_CFG_ID_NAME) to disambiguate devices. */
    for (uint32_t s = 0; s < VIRTIO_MMIO_SLOTS; s++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)s * VIRTIO_MMIO_STRIDE;
        if (probe_slot(base)) {
            serial_puts("[virtio-input] found input device at slot ");
            serial_puthex(s);
            serial_puts(" base=");
            serial_puthex(base);
            serial_puts("\n");
            if (init_device(base) < 0) return -1;
            serial_puts("[virtio-input] keyboard ready (QUEUE_SIZE=");
            serial_puthex(QUEUE_SIZE);
            serial_puts(")\n");
            return 0;
        }
    }
    return -1;
}

int virtio_input_present(void) { return g_in_mmio_base != 0; }

/* Translate an evdev keycode + current modifier state to one or more
 * ASCII bytes pushed into the ring.  Cursor keys produce ANSI escape
 * sequences (`\e[A` etc) so the existing readline editor works. */
static void translate_key(uint16_t code)
{
    /* Cursor keys → ANSI CSI sequences. */
    switch (code) {
    case KC_UP:    ring_push(0x1B); ring_push('['); ring_push('A'); return;
    case KC_DOWN:  ring_push(0x1B); ring_push('['); ring_push('B'); return;
    case KC_RIGHT: ring_push(0x1B); ring_push('['); ring_push('C'); return;
    case KC_LEFT:  ring_push(0x1B); ring_push('['); ring_push('D'); return;
    case KC_HOME:  ring_push(0x1B); ring_push('['); ring_push('H'); return;
    case KC_END:   ring_push(0x1B); ring_push('['); ring_push('F'); return;
    /* PageUp / PageDown use the parametric CSI form `ESC [ 5 ~`
     * and `ESC [ 6 ~` (the standard xterm sequences).  The WM
     * parser in wm.c was extended in lockstep to recognise the
     * `[N~` shape and re-emit it as GUI_KEY_PGUP / GUI_KEY_PGDN. */
    case KC_PAGEUP:   ring_push(0x1B); ring_push('['); ring_push('5'); ring_push('~'); return;
    case KC_PAGEDOWN: ring_push(0x1B); ring_push('['); ring_push('6'); ring_push('~'); return;
    case KC_DELETE: ring_push(0x7F); return;       /* DEL */
    case KC_BACKSPACE: ring_push(0x7F); return;
    case KC_TAB:   ring_push('\t'); return;
    case KC_ENTER: ring_push('\r'); return;        /* mirrored serial path */
    case KC_ESC:   ring_push(0x1B); return;
    default: break;
    }

    /* Pure modifier keys: state already updated, no byte. */
    if (code == KC_LSHIFT || code == KC_RSHIFT ||
        code == KC_LCTRL  || code == KC_RCTRL  ||
        code == KC_LALT   || code == KC_RALT   ||
        code == KC_CAPSLOCK)
        return;

    /* Printable ASCII path. */
    if (code >= 128) return;

    char base = kc_to_ascii[code];
    if (!base) return;

    char emit;
    if (g_shift_down) {
        char shifted = kc_to_ascii_shift[code];
        if (shifted) emit = shifted;
        else if (base >= 'a' && base <= 'z') emit = (char)(base - 'a' + 'A');
        else emit = base;
    } else {
        emit = base;
    }

    /* Ctrl+letter → control byte (Ctrl-A == 0x01, ..., Ctrl-_ = 0x1F).
     * Match the existing serial path's behaviour: Ctrl-C delivers
     * SIGINT via vfs.c reading 0x03 from the byte stream, Ctrl-K
     * etc. drive the readline kill ring. */
    if (g_ctrl_down) {
        if (emit >= 'a' && emit <= 'z') {
            ring_push((char)(emit - 'a' + 1));
            return;
        }
        if (emit >= 'A' && emit <= 'Z') {
            ring_push((char)(emit - 'A' + 1));
            return;
        }
    }

    ring_push(emit);
}

/* Process one evdev event from a slot. */
static void handle_event(const struct virtio_input_event *ev)
{
    if (ev->type == EV_SYN) return;
    if (ev->type != EV_KEY) return;

    uint16_t code  = ev->code;
    uint32_t value = ev->value;

    /* Update modifier state on press / release first, regardless of
     * whether this is a repeat. */
    int down = (value == KEY_VAL_PRESS || value == KEY_VAL_REPEAT);
    if (code == KC_LSHIFT || code == KC_RSHIFT) g_shift_down = (uint8_t)down;
    if (code == KC_LCTRL  || code == KC_RCTRL)  g_ctrl_down  = (uint8_t)down;

    /* RELEASE for non-modifier keys: produce a key-up event for the
     * focused window via the parallel release ring.  Modifiers
     * never produced bytes either; their tracker above already
     * handled the state flip. */
    if (value == KEY_VAL_RELEASE) {
        if (code == KC_LSHIFT || code == KC_RSHIFT ||
            code == KC_LCTRL  || code == KC_RCTRL  ||
            code == KC_LALT   || code == KC_RALT   ||
            code == KC_CAPSLOCK)
            return;
        uint32_t k = code_to_gui_release(code);
        if (k) rel_ring_push(k);
        return;
    }

    translate_key(code);
}

void virtio_input_poll(void)
{
    if (!g_in_mmio_base) return;

    struct vring_used *u = used_ring();
    /* Chapter 106b fast path: every cooperative sys_yield calls
     * pump_input_into_wm() which lands here.  On a busy desktop
     * (browser fetching, gui_term/desktop/launcher idling) that's
     * tens of thousands of calls during a single page load.  The
     * device only touches the used ring when there's a new key, so
     * if u->idx hasn't moved we can skip the MMIO traffic
     * (INTERRUPT_STATUS read + QUEUE_NOTIFY write both trap to
     * HVF).  u->idx itself lives in shared RAM and is free to
     * read.  Without this guard, chapter-106b measured ~4s of CPU
     * burnt per HN fetch in idle MMIO traps. */
    if ((uint16_t)(u->idx - g_used_idx_seen) == 0) return;

    /* Acknowledge any device interrupt level, even though we are not
     * currently consuming the IRQ — this prevents the device from
     * staying flagged forever if we ever wire one up later. */
    uint32_t istat = r32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (istat) w32(VIRTIO_MMIO_INTERRUPT_ACK, istat);

    dmb();
    while ((uint16_t)(u->idx - g_used_idx_seen) > 0) {
        struct vring_used_elem *e = &u->ring[g_used_idx_seen % QUEUE_SIZE];
        uint16_t desc_idx = (uint16_t)e->id;
        uint32_t got_len  = e->len;
        if (desc_idx < QUEUE_SIZE && got_len >= sizeof(struct virtio_input_event)) {
            const struct virtio_input_event *ev = event_slot(desc_idx);
            handle_event(ev);
        }
        /* Re-publish the descriptor so the device can fill it again. */
        if (desc_idx < QUEUE_SIZE) publish_descriptor(desc_idx);
        g_used_idx_seen++;
    }
    /* Tell the device we've added fresh buffers (no-op if it already
     * has plenty queued, but cheap). */
    w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

int virtio_input_try_getc(char *out)
{
    virtio_input_poll();
    return ring_pop(out);
}

int virtio_input_try_get_release(uint32_t *out)
{
    virtio_input_poll();
    return rel_ring_pop(out);
}
