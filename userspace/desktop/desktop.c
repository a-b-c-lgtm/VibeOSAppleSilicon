/*
 * userspace/desktop/desktop.c — desktop environment,
 * ported to /srv/wm in chapter 117.
 *
 * Owns the wallpaper.  This program is the userspace authority
 * on what the bottom layer of the desktop looks like; the
 * kernel provides only the framebuffer, and wsd provides the
 * compositor.  Architecturally this is the same pattern as
 * X11's root window or Wayland's background surface: a
 * process, not a kernel feature.
 *
 * Boot sequence:
 *   1. Query the actual scanout dimensions via
 *      gui_get_screen_size().  The framebuffer is whatever
 *      QEMU was launched with — 1280x800, 1920x1080, etc.
 *   2. open("/mnt/wallpaper.bgra")  — raw BGRA blob baked
 *      onto OSFS at build time by scripts/img_to_bgra.py.
 *      The file starts with an 8-byte header: u32 W, u32 H.
 *   3. wm_create_window_at(screen_w, screen_h, NO_DECORATION,
 *      0, 0, NULL, &win) — claim the (0,0) slot WITHOUT
 *      perturbing the wsd cascade counter; subsequent
 *      cascade-positioned clients keep their layout.
 *   4. Stream pixels from disk into the window's mapped FB
 *      via draw_blit_bgra in ROW_CHUNK-sized batches.  We
 *      never hold the full multi-MB image in memory; the
 *      heap stays small.
 *   5. wm_window_dirty over the whole wallpaper.
 *   6. Sleep loop.
 *
 * If anything fails (no disk, no file, wsd unreachable), the
 * process exits cleanly.  wsd's flat-colour wallpaper then
 * remains visible.  No crashes, no half-painted windows.
 */
#include "../libc/syscall.h"
#include "../libc/malloc.h"
#include "../libc/printf.h"
#include "../libgui/draw.h"
#include "../libgui/wmclient.h"

/* How many rows we read+blit at a time.  At 1920 wide, 16 rows
 * = 16*1920*4 = 120 KB — fits comfortably in the user heap and
 * amortises per-syscall cost. */
#define ROW_CHUNK       16

static const char *WALLPAPER_PATH = "/mnt/wallpaper.bgra";

/* Read exactly `want` bytes from `fd` into `buf`, looping over
 * short reads (virtio-blk transfer boundaries).  Returns total
 * bytes actually read; less than `want` on EOF or error. */
static size_t read_full(int fd, void *buf, size_t want)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < want) {
        long n = read(fd, p + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uint32_t screen_w = 0, screen_h = 0;
    if (gui_get_screen_size(&screen_w, &screen_h) != 0 ||
        screen_w == 0 || screen_h == 0) {
        printf("desktop: no GUI screen, exiting\n");
        return 1;
    }

    int fd = open(WALLPAPER_PATH, 0);
    if (fd < 0) {
        printf("desktop: cannot open %s (rc=%d), keeping fallback wallpaper\n",
               WALLPAPER_PATH, fd);
        return 1;
    }

    /* Read 8-byte header: u32 W, u32 H (little-endian).  This
     * tells us how the BGRA blob is laid out without us having
     * to assume it matches the screen. */
    uint32_t img_w = 0, img_h = 0;
    if (read_full(fd, &img_w, 4) != 4 ||
        read_full(fd, &img_h, 4) != 4 ||
        img_w == 0 || img_h == 0) {
        printf("desktop: bad wallpaper header\n");
        close(fd);
        return 1;
    }

    /* Open a screen-sized wsd window at the (0,0) slot.  No
     * input shadow (the wallpaper doesn't take input -- click-
     * through is the contract; if a child desktop ever wants
     * a right-click menu it'll open a separate input window).
     *
     * PIN_TO_BOTTOM tells wsd to keep this window at the very
     * bottom of the z-order, even if the user clicks on the
     * background.  Without it, a click on a wallpaper pixel
     * not occluded by a foreground window would z_raise the
     * wallpaper to the top and lock the user out of every
     * other app (chapter 118 follow-up fix). */
    struct wm_window win;
    if (wm_create_window_at(screen_w, screen_h,
                            GUI_WIN_FLAG_NO_DECORATION
                            | GUI_WIN_FLAG_PIN_TO_BOTTOM,
                            0, 0, NULL, &win) < 0) {
        printf("desktop: wm_create_window_at failed\n");
        close(fd);
        return 1;
    }

    /* The window is screen-sized.  Blit the image into it
     * starting at (off_x, off_y), centred if smaller, clipped
     * to the screen if larger.  Anything outside the image
     * stays whatever the wsd buffer was zeroed to.  In the
     * typical case the build-time wallpaper resolution exactly
     * matches the runtime scanout, so off_x == off_y == 0 and
     * there is no border. */
    int32_t off_x = ((int32_t)screen_w - (int32_t)img_w) / 2;
    int32_t off_y = ((int32_t)screen_h - (int32_t)img_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    uint32_t blit_w = img_w < screen_w ? img_w : screen_w;

    size_t chunk_bytes = (size_t)ROW_CHUNK * (size_t)img_w * 4u;
    uint8_t *chunk = (uint8_t *)malloc(chunk_bytes);
    if (!chunk) {
        printf("desktop: out of memory for chunk buffer (%lu bytes)\n",
               (unsigned long)chunk_bytes);
        wm_destroy_window(&win);
        close(fd);
        return 1;
    }

    /* Stream the image row-chunks-at-a-time straight from disk
     * into the window's mapped framebuffer.  draw_blit_bgra is
     * just a clipped memcpy; wsd sees one WM_WIN_DAMAGE at the
     * end, not one round-trip per chunk. */
    uint32_t y = 0;
    while (y < img_h) {
        uint32_t rows = img_h - y;
        if (rows > ROW_CHUNK) rows = ROW_CHUNK;
        size_t want = (size_t)rows * (size_t)img_w * 4u;
        size_t got  = read_full(fd, chunk, want);
        if (got == 0) break;

        uint32_t actual_rows = (uint32_t)(got / ((size_t)img_w * 4u));
        if (actual_rows == 0) break;

        /* Don't blit rows that fall off the bottom of the
         * screen if the image is taller than the scanout. */
        if (off_y + (int32_t)y + (int32_t)actual_rows > (int32_t)screen_h) {
            int32_t over = (off_y + (int32_t)y + (int32_t)actual_rows)
                         - (int32_t)screen_h;
            if (over >= (int32_t)actual_rows) break;
            actual_rows -= (uint32_t)over;
        }

        draw_blit_bgra(&win.fb, off_x, off_y + (int32_t)y,
                       blit_w, actual_rows,
                       (const uint32_t *)chunk, img_w);
        y += actual_rows;
        if (got < want) break;
    }

    /* One damage call covering the whole wallpaper so wsd
     * composites it in a single pass. */
    wm_window_dirty(&win, 0, 0, screen_w, screen_h);
    close(fd);
    /* Don't free the chunk buffer — we never need to malloc more,
     * and freeing it just exercises the user heap unnecessarily. */

    /* Idle forever.  We MUST stay alive: when desktop exits,
     * wsd GCs our wallpaper window and the flat wsd
     * wallpaper bleeds back through.  yield() in a tight
    * loop because we have no events to handle (a future
    * chapter can add an explicit wsd shutdown signal). */
    for (;;) {
        yield();
    }
}
