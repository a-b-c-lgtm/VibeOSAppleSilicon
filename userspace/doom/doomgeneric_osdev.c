/*
 * userspace/doom/doomgeneric_osdev.c — chapter 172 platform shim.
 *
 * DoomGeneric's per-platform contract is six functions
 * (declared in doomgeneric.h):
 *
 *   void     DG_Init(void)
 *   void     DG_DrawFrame(void)
 *   void     DG_SleepMs(uint32_t ms)
 *   uint32_t DG_GetTicksMs(void)
 *   int      DG_GetKey(int *pressed, unsigned char *key)
 *   void     DG_SetWindowTitle(const char *title)
 *
 * Plus a `main(argc, argv)` that calls doomgeneric_Create()
 * once and then doomgeneric_Tick() in a loop.
 *
 * Our shim is a small adapter to the libgui wmclient (chapter
 * 108d) for pixels + input, the sleep_ms/uptime_ms syscalls
 * (chapter 21 / 29) for time, and printf.h for diagnostics.
 *
 * Pixel format
 * ------------
 * DoomGeneric writes 4-byte pixels into DG_ScreenBuffer.  The
 * `struct color` in i_video.h is a bitfield {b:8 g:8 r:8 a:8}
 * (low-to-high), so on little-endian aarch64 the in-memory
 * byte order is B,G,R,A — exactly what our framebuffer wants.
 * No swizzling needed; DG_DrawFrame is a per-row memcpy.
 *
 * Input
 * -----
 * wm_poll_event delivers ASCII bytes in GUI_EVENT_KEY events,
 * plus GUI_KEY_UP/DOWN/LEFT/RIGHT for arrow keys.  We map:
 *
 *   ASCII bytes           → uppercased ASCII (matches DOOM's
 *                            "Most key data are simple ascii
 *                            (uppercased)." rule in doomkeys.h)
 *   GUI_KEY_UP/DOWN/...   → KEY_{UP,DOWN,LEFT,RIGHT}ARROW
 *   ASCII ' '             → KEY_USE  (alt: spacebar)
 *   ASCII 'f' / 'F'       → KEY_FIRE (no Ctrl in our GUI yet)
 *   ASCII 0x1B            → KEY_ESCAPE (== 27)
 *   ASCII 0x0D / 0x0A     → KEY_ENTER (== 13)
 *
 * Releases ride GUI_EVENT_KEY_UP (chapter 197), so
 * gamekeydown[k] follows the real key state exactly — release
 * a movement key and the player stops the same tick.  The
 * shim's g_held[] table only exists to dedup auto-repeat
 * (virtio-keyboard emits KEY_VAL_REPEAT while a key is held,
 * each of which becomes another GUI_EVENT_KEY); we drop the
 * repeats so DoomGeneric sees one press, one release.
 *
 * Closing the window
 * ------------------
 * GUI_EVENT_CLOSE → I_Quit() via raise(SIGINT) which Doom's
 * own SIGINT handler (chapter 166) catches and unwinds via
 * longjmp.  Simpler in-shim: just call exit(0).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/stdlib.h"
#include "../libgui/wmclient.h"

#include "../../vendor/doomgeneric/src/doomgeneric.h"
#include "../../vendor/doomgeneric/src/doomkeys.h"

#include <stdint.h>

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 640
#endif
#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 400
#endif

/* Single window — Doom is single-buffer. */
static struct wm_window g_win;
static int              g_have_win = 0;

/* Key queue (press AND release per real input).  64 slots is
 * plenty; Doom drains it every tick. */
#define KQ_SZ 64
struct kq_ent { unsigned char pressed; unsigned char key; };
static struct kq_ent g_kq[KQ_SZ];
static unsigned      g_kq_r = 0, g_kq_w = 0;

/* Per-key held-state.  Used only to dedup virtio-keyboard's
 * KEY_VAL_REPEAT events (which arrive as additional
 * GUI_EVENT_KEY presses for an already-held key) so
 * DoomGeneric sees a clean one-press-one-release pair.  Doom
 * KEY_* values all fit in 0..255 (KEY_F12 = 0x80+0x46 = 0xC6
 * is the largest we'll see). */
static uint8_t       g_held[256];

static void kq_push(unsigned char pressed, unsigned char key)
{
    if (!key) return;                  /* don't queue NULs */
    unsigned next = (g_kq_w + 1) % KQ_SZ;
    if (next == g_kq_r) return;        /* queue full → drop */
    g_kq[g_kq_w].pressed = pressed;
    g_kq[g_kq_w].key     = key;
    g_kq_w = next;
}

/* Real GUI press arrived.  Push a press only on the leading
 * edge; subsequent KEY_VAL_REPEATs for the same key are
 * swallowed so gamekeydown[] stays true across host typematic
 * repeats without re-firing the press handler. */
static void on_doom_key_down(unsigned char k)
{
    if (!k) return;
    if (g_held[k]) return;
    kq_push(1, k);
    g_held[k] = 1;
}

/* Real GUI release arrived. */
static void on_doom_key_up(unsigned char k)
{
    if (!k) return;
    if (!g_held[k]) return;            /* drop spurious */
    kq_push(0, k);
    g_held[k] = 0;
}

/* Map our GUI key code (ASCII in low byte, or GUI_KEY_*) to
 * a DOOM KEY_*.  Returns 0 if unmapped (caller should drop). */
static unsigned char gui_to_doom(uint32_t arg0)
{
    /* Extended keys first (so 0x104 doesn't get masked to 0x04). */
    switch (arg0) {
        case GUI_KEY_UP:    return KEY_UPARROW;
        case GUI_KEY_DOWN:  return KEY_DOWNARROW;
        case GUI_KEY_LEFT:  return KEY_LEFTARROW;
        case GUI_KEY_RIGHT: return KEY_RIGHTARROW;
    }
    unsigned char c = (unsigned char)(arg0 & 0xFFu);
    switch (c) {
        case 0x1B: return KEY_ESCAPE;
        case 0x0A:                 /* LF: treat as ENTER */
        case 0x0D: return KEY_ENTER;
        case 0x7F:                 /* DEL: treat as BACKSPACE */
        case 0x08: return KEY_BACKSPACE;
        case 0x09: return KEY_TAB;
        case ' ':  return KEY_USE;
        case 'f':
        case 'F':  return KEY_FIRE;
        default:
            /* Letters are uppercased for Doom (per doomkeys.h). */
            if (c >= 'a' && c <= 'z') return (unsigned char)(c - 'a' + 'A');
            if (c >= 'A' && c <= 'Z') return c;
            if (c >= '0' && c <= '9') return c;
            return 0;
    }
}

static void puts1(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

void DG_Init(void)
{
    if (wm_create_window_input(DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                               0, "DOOM", &g_win) < 0)
    {
        puts1("[doom] wm_create_window_input failed\n");
        exit(1);
    }
    if (g_win.fb.w != DOOMGENERIC_RESX || g_win.fb.h != DOOMGENERIC_RESY) {
        printf("[doom] unexpected window dims %ux%u\n",
               g_win.fb.w, g_win.fb.h);
        exit(1);
    }
    g_have_win = 1;

    /* Paint a black background once so the window doesn't show
     * stale taskbar pixels while Doom is still loading the WAD. */
    uint32_t *p = (uint32_t *)g_win.fb.pixels;
    size_t n = (size_t)g_win.fb.stride / 4 * g_win.fb.h;
    for (size_t i = 0; i < n; i++) p[i] = 0xFF000000;    /* opaque black */
    wm_window_dirty(&g_win, 0, 0, g_win.fb.w, g_win.fb.h);

    puts1("[doom] window created\n");
}

/* Drain GUI events into the doom key queue.  Called from
 * DG_DrawFrame (per frame) and DG_GetKey (so a polling loop
 * still observes input). */
static void pump_events(void)
{
    struct gui_event ev;
    for (int i = 0; i < 32; i++) {
        int r = wm_poll_event(&ev);
        if (r <= 0) break;
        if (ev.type == GUI_EVENT_CLOSE) {
            puts1("[doom] close event\n");
            exit(0);
        }
        if (ev.type == GUI_EVENT_KEY) {
            unsigned char k = gui_to_doom(ev.arg0);
            if (k) on_doom_key_down(k);
        }
        if (ev.type == GUI_EVENT_KEY_UP) {
            unsigned char k = gui_to_doom(ev.arg0);
            if (k) on_doom_key_up(k);
        }
        /* Mouse events ignored. */
    }
}

void DG_DrawFrame(void)
{
    if (!g_have_win) return;

    /* DG_ScreenBuffer is RESX*RESY * 4 bytes, tightly packed. */
    uint8_t       *dst = g_win.fb.pixels;
    const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
    uint32_t       row_bytes = DOOMGENERIC_RESX * 4;
    for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
        uint8_t       *drow = dst + (size_t)y * g_win.fb.stride;
        const uint8_t *srow = src + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < row_bytes; x++) drow[x] = srow[x];
    }
    wm_window_dirty(&g_win, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY);

    pump_events();
}

void DG_SleepMs(uint32_t ms)
{
    sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)uptime_ms();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    pump_events();
    if (g_kq_r == g_kq_w) return 0;
    *pressed = g_kq[g_kq_r].pressed;
    *key     = g_kq[g_kq_r].key;
    g_kq_r = (g_kq_r + 1) % KQ_SZ;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    if (g_have_win && title) wm_set_title(&g_win, title);
}

int main(int argc, char **argv)
{
    /* If the caller didn't pass -iwad explicitly, default to
     * the staged shareware WAD path so `doom` from the shell
     * Just Works.  Doom's M_CheckParm walks myargv; we
     * synthesise the args by rebuilding argv.
     *
     * Chapter 173 — WAD lives at /data/doom1.wad (root of the
     * OSFS-2 mount).  mkosfs2.py is flat (no subdirs); the
     * earlier-planned `/data/wads/doom1.wad` would have needed
     * subdirectory support we don't have. */
    static const char *defaults[] = {
        "doom", "-iwad", "/data/doom1.wad", "-mb", "6"
    };
    static char *defargv[5];
    int has_iwad = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == 'i'
            && argv[i][2] == 'w' && argv[i][3] == 'a'
            && argv[i][4] == 'd' && argv[i][5] == '\0')
        {
            has_iwad = 1;
            break;
        }
    }
    if (!has_iwad) {
        for (int i = 0; i < 5; i++) defargv[i] = (char *)defaults[i];
        argc = 5;
        argv = defargv;
    }

    doomgeneric_Create(argc, argv);

    for (;;) {
        doomgeneric_Tick();
    }
    return 0;   /* unreachable */
}
