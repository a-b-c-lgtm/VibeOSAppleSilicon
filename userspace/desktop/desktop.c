/*
 * userspace/desktop/desktop.c — milestone-50 desktop environment.
 *
 * Owns the wallpaper.  This program is the userspace authority on
 * what the bottom layer of the desktop looks like; the kernel
 * provides only a generic windowing primitive (PIN_TO_BOTTOM) and
 * the framebuffer.  Architecturally this is the same pattern as
 * X11's root window or Wayland's background surface: a process,
 * not a kernel feature.
 *
 * Boot sequence:
 *   1. Query the actual scanout dimensions via
 *      gui_get_screen_size().  The framebuffer is whatever
 *      QEMU was launched with — 1280x800, 1920x1080, etc.
 *   2. open("/mnt/wallpaper.bgra")  — raw BGRA blob baked
 *      onto OSFS at build time by scripts/img_to_bgra.py.
 *      The file starts with an 8-byte header: u32 W, u32 H.
 *   3. Create a screen-sized PIN_TO_BOTTOM window at (0, 0).
 *   4. Stream pixels from disk into the window via gui_present
 *      in ROW_CHUNK-sized batches.  We never hold the full
 *      multi-MB image in memory; the heap stays small.
 *   5. gui_flush.
 *   6. Sleep loop.  Eats GUI_EVENT_CLOSE if it ever arrives.
 *
 * If anything fails (no disk, no file, kernel rejects flags), the
 * process exits cleanly.  The kernel's gradient fallback then
 * remains visible.  No crashes, no half-painted windows.
 */
#include "../libc/syscall.h"
#include "../libc/malloc.h"
#include "../libc/printf.h"

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

    int win = gui_create_window_ex(screen_w, screen_h, "desktop",
                                   GUI_WIN_FLAG_NO_DECORATION |
                                   GUI_WIN_FLAG_PIN_TO_BOTTOM,
                                   0, 0);
    if (win < 0) {
        printf("desktop: cannot create wallpaper window (rc=%d)\n", win);
        close(fd);
        return 1;
    }

    /* The window is screen-sized.  Blit the image into it
     * starting at (off_x, off_y), centred if smaller, clipped
     * to the screen if larger.  Anything outside the image
     * stays whatever the WM initialised the window to (which is
     * black on a fresh window).  In the typical case the build-
     * time wallpaper resolution exactly matches the runtime
     * scanout, so off_x == off_y == 0 and there is no border. */
    int32_t off_x = ((int32_t)screen_w - (int32_t)img_w) / 2;
    int32_t off_y = ((int32_t)screen_h - (int32_t)img_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    uint32_t blit_w = img_w < screen_w ? img_w : screen_w;
    /* If the image is wider than the screen, we'd need to skip
     * the left margin in each row of the source; that requires
     * a full image load or a per-row seek+read.  For now we
     * just clip on the right.  Most setups will have an exact
     * match anyway. */

    size_t chunk_bytes = (size_t)ROW_CHUNK * (size_t)img_w * 4u;
    uint8_t *chunk = (uint8_t *)malloc(chunk_bytes);
    if (!chunk) {
        printf("desktop: out of memory for chunk buffer (%lu bytes)\n",
               (unsigned long)chunk_bytes);
        gui_destroy_window(win);
        close(fd);
        return 1;
    }

    /* Stream the image row-chunks-at-a-time straight from disk
     * into the window.  Every gui_present call copies the chunk
     * into the kernel-owned window buffer; we then reuse the
     * same chunk buffer for the next read. */
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

        gui_present(win, off_x, off_y + (int32_t)y,
                    blit_w, actual_rows, chunk);
        y += actual_rows;
        if (got < want) break;
    }

    gui_flush(win);
    close(fd);
    /* Don't free the chunk buffer — we never need to malloc more,
     * and freeing it just exercises the user heap unnecessarily. */

    /* Idle forever, eating any incoming events.  We MUST stay
     * alive: when the desktop process exits, the kernel calls
     * wm_destroy_owner, which would tear down our wallpaper
     * window and reveal the kernel's gradient fallback.
     *
     * We yield() in a tight loop instead of sleep_ms(N) for one
     * specific reason: every sys_yield runs the kernel-side
     * pump_input_into_wm, which drains the virtio-tablet used
     * ring and repaints the cursor.  When NO app window is open,
     * desktop is the only thing keeping the cursor alive — a
     * 500 ms sleep here means the cursor only updates twice per
     * second when the user closes their last window.  Yield-
     * polling makes the cursor track the host pointer at the
     * scheduler's full rate (~thousands of Hz under HVF) at the
     * cost of `desktop` always being on the runqueue.  On a
     * single-CPU cooperative scheduler that's free; the next
     * runnable thread always gets picked anyway. */
    for (;;) {
        struct gui_event ev;
        while (gui_poll_event(&ev)) {
            if (ev.type == GUI_EVENT_CLOSE) {
                gui_destroy_window(win);
                return 0;
            }
            /* Ignore everything else (KEY/MOUSE_DOWN — clicks
             * actually fall through PIN_TO_BOTTOM windows in the
             * kernel hit-test, but if a future hit-test ever
             * delivers an event to us we just drop it). */
        }
        yield();
    }
}
