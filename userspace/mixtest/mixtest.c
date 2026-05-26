/* userspace/mixtest/mixtest.c — chapter 108c mixed-paths assertion.
 *
 * Verifies the kernel's one-window-one-draw-path contract:
 *
 *   POSITIVE (a window that never installed a userspace mapping
 *   must still accept the legacy kernel-side draw syscalls):
 *
 *     gui_create_window → gui_fill_rect → gui_draw_text → gui_present
 *     all return 0.  This is the path that `notify` and the
 *     WM's own title bars use; we never want chapter 108c to
 *     regress it.
 *
 *   NEGATIVE (a window that DID install a userspace mapping via
 *   gui_window_fb must refuse kernel-side draw syscalls):
 *
 *     gui_create_window → gui_window_fb → gui_fill_rect → -EBUSY
 *                                       → gui_draw_text → -EBUSY
 *                                       → gui_present   → -EBUSY
 *
 *   Mixing the two paths on the same window would race: the
 *   kernel writes into w->pixels while the owner writes into
 *   the same memory through the mapping.  -EBUSY at the
 *   syscall edge makes the violation visible instead of
 *   manifesting as torn pixels.
 *
 * Both sub-tests run in one process.  Success line emitted on
 * the serial console:
 *
 *     [mixtest] all checks passed
 *
 * Any failure prints `[mixtest] FAIL: <reason>` and the
 * scripts/test_busy_on_mix.py harness grep's for either token.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"

/* errno code mirror (matches kernel/core/wm.c).  We don't have
 * a shared header for userspace yet — just hard-code the few
 * values we need to check. */
#define EBUSY_RC  16

static int g_failed = 0;

static void fail(const char *why, int rc)
{
    printf("[mixtest] FAIL: %s (rc=%d)\n", why, rc);
    g_failed = 1;
}

static void ok(const char *what)
{
    printf("[mixtest] OK: %s\n", what);
}

/* Phase 1: an unmapped window must accept kernel draws. */
static void test_unmapped_kernel_path(void)
{
    int w = gui_create_window(120, 80, "mixtest-unmapped");
    if (w < 0) { fail("create unmapped window", w); return; }

    int rc;
    rc = gui_fill_rect(w, 0, 0, 120, 80, 0xFF00FF00u);
    if (rc != 0) { fail("gui_fill_rect on unmapped should succeed", rc);
                   gui_destroy_window(w); return; }

    rc = gui_draw_text(w, 8, 16, "hi", 0xFFFFFFFFu, 0xFF000000u, 0);
    if (rc != 0) { fail("gui_draw_text on unmapped should succeed", rc);
                   gui_destroy_window(w); return; }

    /* gui_present: dummy 1-pixel source on the stack. */
    uint32_t one_px = 0xFF112233u;
    rc = gui_present(w, 0, 0, 1, 1, (const uint8_t *)&one_px);
    if (rc != 0) { fail("gui_present on unmapped should succeed", rc);
                   gui_destroy_window(w); return; }

    gui_destroy_window(w);
    ok("unmapped window accepts kernel draw syscalls");
}

/* Phase 2: a window that installed a mapping must refuse them. */
static void test_mapped_returns_ebusy(void)
{
    int w = gui_create_window(120, 80, "mixtest-mapped");
    if (w < 0) { fail("create mapped window", w); return; }

    struct gui_fb fb;
    int rc = gui_window_fb(w, &fb);
    if (rc < 0) { fail("gui_window_fb", rc);
                  gui_destroy_window(w); return; }

    /* Sanity-check the mapping looks sane before we test the
     * negative path — if the mapping is broken, EBUSY would
     * still come back but for the wrong reason. */
    if (fb.w != 120 || fb.h != 80 || fb.pixels == 0) {
        fail("gui_window_fb returned bogus fb", 0);
        gui_destroy_window(w); return;
    }
    /* And actually use the mapping: write one pixel through it.
     * This is what real apps do, and it proves the mapping is
     * live before we assert that kernel draws are now refused. */
    *(uint32_t *)fb.pixels = 0xFF445566u;

    rc = gui_fill_rect(w, 0, 0, 120, 80, 0xFF00FF00u);
    if (rc != -1 || errno != EBUSY_RC) {
        fail("gui_fill_rect on mapped should be EBUSY", rc);
        gui_destroy_window(w); return;
    }

    rc = gui_draw_text(w, 8, 16, "hi", 0xFFFFFFFFu, 0xFF000000u, 0);
    if (rc != -1 || errno != EBUSY_RC) {
        fail("gui_draw_text on mapped should be EBUSY", rc);
        gui_destroy_window(w); return;
    }

    uint32_t one_px = 0xFF112233u;
    rc = gui_present(w, 0, 0, 1, 1, (const uint8_t *)&one_px);
    if (rc != -1 || errno != EBUSY_RC) {
        fail("gui_present on mapped should be EBUSY", rc);
        gui_destroy_window(w); return;
    }

    gui_destroy_window(w);
    ok("mapped window refuses kernel draw syscalls with EBUSY");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    test_unmapped_kernel_path();
    test_mapped_returns_ebusy();

    if (g_failed) {
        printf("[mixtest] FAIL: one or more checks failed\n");
        return 1;
    }
    printf("[mixtest] all checks passed\n");
    return 0;
}
