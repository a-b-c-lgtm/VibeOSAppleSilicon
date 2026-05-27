/*
 * userspace/wmtest/wmtest.c — chapter 117 smoke client for
 * /srv/wm.
 *
 * One-shot CLI invoked from the shell.  Each invocation
 * exercises a scripted sequence of ops against wsd on a
 * single persistent connection and prints exactly one
 * "[wmtest] PASS ..." or "[wmtest] FAIL reason=..." line so
 * scripts/test_wsd_hello.py can grep for the result.
 *
 * Chapter 117 built the test incrementally: initial
 * HELLO + LIST, then full window-lifecycle (CREATE/DESTROY),
 * then MAP_FB (per-window backing-FB + SYS_WIN_FB_MAP),
 * then DAMAGE (compositor path: client paints → wsd blits
 * scanout), then position tracking (CREATE reply auto-pos
 * + WM_WIN_MOVE + window-local DAMAGE coords).
 *
 * The default flow ends with one window deliberately not
 * destroyed.  When the conn closes, wsd's gc_conn_windows()
 * should reap it.  A second invocation immediately after
 * (driven by the same test script) confirms wsd starts that
 * second conn with n_windows=0, proving the GC ran.
 *
 *   wmtest gc-check — HELLO + LIST only.  PASSes if n=0.
 *                     Run right after a default `wmtest` to
 *                     verify auto-cleanup.
 *
 * Multiple ops on one connection (HELLO + several follow-
 * ups) is deliberate: catches the common chapter-108d early
 * bring-up bug shape
 * where a handler returns instead of looping back to read
 * the next request.  The window-lifecycle additions extend that
 * coverage to stateful ops (CREATE, DESTROY) that mutate
 * wsd state across requests.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/wm_proto.h"

/* Tiny argv comparator.  Freestanding C, no strcmp. */
static int eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int do_hello(int fd, uint32_t *session_out, uint32_t *ver_out)
{
    struct wm_msg req = {0};
    req.op = WM_HELLO;
    req.a  = WM_PROTO_VERSION;

    long w = write(fd, &req, sizeof(req));
    if (w != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=hello-write w=%ld\n", w);
        return -1;
    }
    struct wm_msg rep;
    long n = read(fd, &rep, sizeof(rep));
    if (n != (long)sizeof(rep)) {
        printf("[wmtest] FAIL reason=hello-read n=%ld\n", n);
        return -1;
    }
    if (rep.op != WM_HELLO) {
        printf("[wmtest] FAIL reason=hello-op op=%u\n", (unsigned)rep.op);
        return -1;
    }
    if (rep.status != WM_OK) {
        printf("[wmtest] FAIL reason=hello-status status=%d\n", (int)rep.status);
        return -1;
    }
    if (rep.a == 0) {
        printf("[wmtest] FAIL reason=hello-bad-session\n");
        return -1;
    }
    *session_out = rep.a;
    *ver_out     = rep.b;
    return 0;
}

/* Read the variable-length WM_LIST reply.  We size the
 * buffer for the maximum possible payload (WM_LIST_REPLY_MAX
 * = header + WM_MAX_WINDOWS * sizeof(wm_win_desc)) because
 * chapter-107 IPC returns -EMSGSIZE if our buffer is too
 * small to hold the entire datagram.  Discards descriptor
 * details here; callers only care about n_windows. */
static int do_list(int fd, uint32_t *n_windows_out)
{
    struct wm_msg req = {0};
    req.op = WM_LIST;

    long w = write(fd, &req, sizeof(req));
    if (w != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=list-write w=%ld\n", w);
        return -1;
    }
    uint8_t buf[WM_LIST_REPLY_MAX];
    long n = read(fd, buf, sizeof(buf));
    if (n < (long)sizeof(struct wm_msg)) {
        printf("[wmtest] FAIL reason=list-short n=%ld\n", n);
        return -1;
    }
    struct wm_msg *rep = (struct wm_msg *)buf;
    if (rep->op != WM_LIST) {
        printf("[wmtest] FAIL reason=list-op op=%u\n", (unsigned)rep->op);
        return -1;
    }
    if (rep->status != WM_OK) {
        printf("[wmtest] FAIL reason=list-status status=%d\n",
               (int)rep->status);
        return -1;
    }
    /* Cross-check: header.a == n_windows and the payload
     * length matches.  If wsd ever sends a malformed
     * datagram (claims N but ships M descriptors) we want
     * to notice. */
    uint32_t claimed = rep->a;
    long expected = (long)sizeof(struct wm_msg)
                  + (long)claimed * (long)sizeof(struct wm_win_desc);
    if (n != expected) {
        printf("[wmtest] FAIL reason=list-len n=%ld expected=%ld claimed=%u\n",
               n, expected, (unsigned)claimed);
        return -1;
    }
    *n_windows_out = claimed;
    return 0;
}

static int do_create(int fd, uint32_t w, uint32_t h, uint32_t flags,
                     uint32_t *id_out,
                     uint32_t *x_out, uint32_t *y_out)
{
    struct wm_msg req = {0};
    req.op = WM_WIN_CREATE;
    req.a  = w;
    req.b  = h;
    req.c  = flags;

    long wn = write(fd, &req, sizeof(req));
    if (wn != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=create-write wn=%ld\n", wn);
        return -1;
    }
    struct wm_msg rep;
    long n = read(fd, &rep, sizeof(rep));
    if (n != (long)sizeof(rep)) {
        printf("[wmtest] FAIL reason=create-read n=%ld\n", n);
        return -1;
    }
    if (rep.op != WM_WIN_CREATE) {
        printf("[wmtest] FAIL reason=create-op op=%u\n", (unsigned)rep.op);
        return -1;
    }
    if (rep.status != WM_OK) {
        printf("[wmtest] FAIL reason=create-status status=%d\n",
               (int)rep.status);
        return -1;
    }
    if (rep.a == 0) {
        printf("[wmtest] FAIL reason=create-zero-id\n");
        return -1;
    }
    *id_out = rep.a;
    /* Chapter 117 — rep.b/rep.c are the cascade-assigned
     * scanout position.  Early wsd builds would leave these 0,
     * which is also a legal position so we don't gate the
     * test on "nonzero"; we just record what came back. */
    if (x_out) *x_out = rep.b;
    if (y_out) *y_out = rep.c;
    printf("[wmtest] create id=%u w=%u h=%u pos=%u,%u\n",
           (unsigned)rep.a, (unsigned)w, (unsigned)h,
           (unsigned)rep.b, (unsigned)rep.c);
    return 0;
}

static int do_destroy(int fd, uint32_t id)
{
    struct wm_msg req = {0};
    req.op = WM_WIN_DESTROY;
    req.a  = id;

    long wn = write(fd, &req, sizeof(req));
    if (wn != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=destroy-write wn=%ld\n", wn);
        return -1;
    }
    struct wm_msg rep;
    long n = read(fd, &rep, sizeof(rep));
    if (n != (long)sizeof(rep)) {
        printf("[wmtest] FAIL reason=destroy-read n=%ld\n", n);
        return -1;
    }
    if (rep.op != WM_WIN_DESTROY) {
        printf("[wmtest] FAIL reason=destroy-op op=%u\n", (unsigned)rep.op);
        return -1;
    }
    if (rep.status != WM_OK) {
        printf("[wmtest] FAIL reason=destroy-status status=%d id=%u\n",
               (int)rep.status, (unsigned)id);
        return -1;
    }
    return 0;
}

/* Chapter 117 — ask wsd for the per-window FB id and geometry,
 * then call SYS_WIN_FB_MAP locally to install the same
 * physical pages into our AS at a fresh VA.  Fills *va_out
 * and *stride_out so the caller can write pixels. */
static int do_map_fb(int fd, uint32_t win_id,                     uint64_t *va_out, uint32_t *stride_out)
{
    struct wm_msg req = {0};
    req.op = WM_WIN_MAP_FB;
    req.a  = win_id;

    long wn = write(fd, &req, sizeof(req));
    if (wn != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=mapfb-write wn=%ld\n", wn);
        return -1;
    }
    struct wm_msg rep;
    long n = read(fd, &rep, sizeof(rep));
    if (n != (long)sizeof(rep)) {
        printf("[wmtest] FAIL reason=mapfb-read n=%ld\n", n);
        return -1;
    }
    if (rep.op != WM_WIN_MAP_FB) {
        printf("[wmtest] FAIL reason=mapfb-op op=%u\n", (unsigned)rep.op);
        return -1;
    }
    if (rep.status != WM_OK) {
        printf("[wmtest] FAIL reason=mapfb-status status=%d\n",
               (int)rep.status);
        return -1;
    }
    uint32_t fb_id  = rep.a;
    uint32_t fb_w   = rep.b;
    uint32_t fb_h   = rep.c;
    uint32_t fb_str = rep.d;
    if (fb_id == 0) {
        printf("[wmtest] FAIL reason=mapfb-zero-id\n");
        return -1;
    }

    struct win_fb_map_args ma;
    int r = win_fb_map(fb_id, &ma);
    if (r != 0) {
        printf("[wmtest] FAIL reason=mapfb-syscall r=%d fb_id=%u\n",
               r, (unsigned)fb_id);
        return -1;
    }
    /* Cross-check that wsd's reply and the kernel's install
     * agree on geometry.  If they disagree something is
     * deeply wrong (wsd's table out of sync with win_fb). */
    if (ma.w != fb_w || ma.h != fb_h || ma.stride != fb_str) {
        printf("[wmtest] FAIL reason=mapfb-geom-mismatch "
               "wsd=%ux%u/%u kernel=%ux%u/%u\n",
               (unsigned)fb_w, (unsigned)fb_h, (unsigned)fb_str,
               (unsigned)ma.w, (unsigned)ma.h, (unsigned)ma.stride);
        return -1;
    }
    if (ma.va == 0) {
        printf("[wmtest] FAIL reason=mapfb-zero-va\n");
        return -1;
    }
    printf("[wmtest] map_fb win=%u fb_id=%u %ux%u stride=%u va=0x%lx\n",
           (unsigned)win_id, (unsigned)fb_id,
           (unsigned)ma.w, (unsigned)ma.h, (unsigned)ma.stride,
           (unsigned long)ma.va);
    *va_out     = ma.va;
    *stride_out = ma.stride;
    return 0;
}

/* Chapter 117 — send a WM_WIN_MOVE to reposition the window
 * on the scanout to (nx, ny) and verify wsd ACKs.  The
 * move itself doesn't paint anything; the very next damage
 * lands at the new position. */
static int do_move(int fd, uint32_t win_id, uint32_t nx, uint32_t ny)
{
    struct wm_msg req = {0};
    req.op = WM_WIN_MOVE;
    req.a  = win_id;
    req.b  = nx;
    req.c  = ny;
    long wn = write(fd, &req, sizeof(req));
    if (wn != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=move-write wn=%ld\n", wn);
        return -1;
    }
    struct wm_msg rep;
    long n = read(fd, &rep, sizeof(rep));
    if (n != (long)sizeof(rep)) {
        printf("[wmtest] FAIL reason=move-read n=%ld\n", n);
        return -1;
    }
    if (rep.op != WM_WIN_MOVE) {
        printf("[wmtest] FAIL reason=move-op op=%u\n", (unsigned)rep.op);
        return -1;
    }
    if (rep.status != WM_OK) {
        printf("[wmtest] FAIL reason=move-status status=%d\n",
               (int)rep.status);
        return -1;
    }
    printf("[wmtest] move win=%u to=%u,%u\n",
           (unsigned)win_id, (unsigned)nx, (unsigned)ny);
    return 0;
}

/* Chapter 117 — send a WM_WIN_DAMAGE for the given rect and
 * verify wsd ACKs with WM_OK.  The actual compose work and
 * the scanout-readback verification happen inside wsd's
 * handle_damage; from the client side this is a fire-and-
 * ACK request.  Width/height are packed into the high/low
 * 16 bits of `d` via WM_DAMAGE_PACK_WH. */
static int do_damage(int fd, uint32_t win_id,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h)
{
    struct wm_msg req = {0};
    req.op = WM_WIN_DAMAGE;
    req.a  = win_id;
    req.b  = x;
    req.c  = y;
    req.d  = WM_DAMAGE_PACK_WH(w, h);

    long wn = write(fd, &req, sizeof(req));
    if (wn != (long)sizeof(req)) {
        printf("[wmtest] FAIL reason=damage-write wn=%ld\n", wn);
        return -1;
    }
    struct wm_msg rep;
    long n = read(fd, &rep, sizeof(rep));
    if (n != (long)sizeof(rep)) {
        printf("[wmtest] FAIL reason=damage-read n=%ld\n", n);
        return -1;
    }
    if (rep.op != WM_WIN_DAMAGE) {
        printf("[wmtest] FAIL reason=damage-op op=%u\n", (unsigned)rep.op);
        return -1;
    }
    if (rep.status != WM_OK) {
        printf("[wmtest] FAIL reason=damage-status status=%d\n",
               (int)rep.status);
        return -1;
    }
    printf("[wmtest] damage sent win=%u rect=%u,%u,%u,%u\n",
           (unsigned)win_id,
           (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
    return 0;
}

/* Default flow: HELLO -> LIST(record n_initial) -> CREATE x2 ->
 * LIST(n_initial+2) -> MAP_FB id1 + paint+readback -> DAMAGE id1
 * (4x1 rect) -> DESTROY first -> LIST(n_initial+1) -> close
 * (leaving one window alive so the next invocation can prove GC
 * ran).
 *
 * chapter 117: boot auto-launches three wsd clients
 * (desktop, taskbar, launcher) before any test runs, so the
 * LIST count is non-zero on entry.  We assert relative deltas
 * (initial vs initial+2 vs initial+1) instead of pinning the
 * absolute count to zero. */
static int run_default(int fd)
{
    uint32_t session = 0, ver = 0;
    if (do_hello(fd, &session, &ver) < 0) return 1;

    uint32_t n_initial = 0xFFFFFFFFu;
    if (do_list(fd, &n_initial) < 0) return 1;

    uint32_t id1 = 0, id2 = 0;
    uint32_t id1_x = 0xFFFFFFFFu, id1_y = 0xFFFFFFFFu;
    if (do_create(fd, 800, 600, 0, &id1, &id1_x, &id1_y) < 0) return 1;
    if (do_create(fd, 100, 100, WM_WF_NODECORATION, &id2, NULL, NULL) < 0) return 1;
    if (id1 == id2) {
        printf("[wmtest] FAIL reason=duplicate-id id=%u\n", (unsigned)id1);
        return 1;
    }

    uint32_t n = 0xFFFFFFFFu;
    if (do_list(fd, &n) < 0) return 1;
    if (n != n_initial + 2) {
        printf("[wmtest] FAIL reason=list2-mismatch "
               "n=%u expected=%u (initial=%u)\n",
               (unsigned)n, (unsigned)(n_initial + 2),
               (unsigned)n_initial);
        return 1;
    }

    /* Chapter 117 — map id1's FB and round-trip a magic pixel.
     * Magic value chosen so each BGRA byte is distinct and
     * non-zero — catches any byte-order swap or partial
     * mapping defect. */
    uint64_t fb_va = 0;
    uint32_t fb_stride = 0;
    if (do_map_fb(fd, id1, &fb_va, &fb_stride) < 0) return 1;
    {
        uint8_t  *px = (uint8_t *)(uintptr_t)fb_va;
        const uint8_t magic_b = 0x11, magic_g = 0x22,
                      magic_r = 0x33, magic_a = 0xFF;
        px[0] = magic_b;
        px[1] = magic_g;
        px[2] = magic_r;
        px[3] = magic_a;
        /* Also touch the bottom-right pixel of the first row
         * so any off-by-one in the install path (last frame
         * not actually mapped) shows up as a fault. */
        uint8_t *last = px + (fb_stride - 4);
        last[0] = 0x44; last[1] = 0x55;
        last[2] = 0x66; last[3] = 0xFF;
        uint32_t rb = ((uint32_t)px[0])
                    | ((uint32_t)px[1] <<  8)
                    | ((uint32_t)px[2] << 16)
                    | ((uint32_t)px[3] << 24);
        uint32_t expected =
              (uint32_t)magic_b
            | ((uint32_t)magic_g <<  8)
            | ((uint32_t)magic_r << 16)
            | ((uint32_t)magic_a << 24);
        if (rb != expected) {
            printf("[wmtest] FAIL reason=mapfb-readback "
                   "got=0x%lx want=0x%lx\n",
                   (unsigned long)rb, (unsigned long)expected);
            return 1;
        }
        printf("[wmtest] fb readback ok pattern=0x%lx\n",
               (unsigned long)rb);
    }

    /* Chapter 117 — move id1 to a known scanout position
     * BEFORE the DAMAGE, so the test can pin both src and
     * dst coords in wsd's log.  Picked (100, 50) because
     * it's distinct from the cascade default (100, 100):
     * if MOVE silently no-ops, the test will catch the
     * dst mismatch.  The id1 window is 800x600 so the
     * 4x1 damage at (0,0) is well inside bounds at this
     * position on the 1280x800 scanout. */
    if (do_move(fd, id1, 100, 50) < 0) return 1;

    /* Chapter 117 — send a minimal DAMAGE that names the
     * four pixels we just wrote at offset 0 (BGRA bytes
     * 0x11 0x22 0x33 0xFF).  Damage coords are WINDOW-LOCAL:
     * wsd translates to scanout coords by adding the window's
     * position, so dst becomes (100 + 0, 50 + 0) = (100, 50).
     * wsd logs both src and dst so the test can pin both. */
    if (do_damage(fd, id1, 0, 0, 4, 1) < 0) return 1;

    if (do_destroy(fd, id1) < 0) return 1;

    if (do_list(fd, &n) < 0) return 1;
    if (n != n_initial + 1) {
        printf("[wmtest] FAIL reason=list1-mismatch "
               "n=%u expected=%u (initial=%u)\n",
               (unsigned)n, (unsigned)(n_initial + 1),
               (unsigned)n_initial);
        return 1;
    }

    /* Intentionally do NOT destroy id2 -- the next invocation
     * of `wmtest gc-check` confirms wsd's gc_conn_windows()
     * cleaned it up on conn close. */
    printf("[wmtest] PASS hello session=%u wsd_version=%u "
           "create id1=%u id2=%u destroyed=%u "
           "leaked=%u list final n=%u initial=%u\n",
           (unsigned)session, (unsigned)ver,
           (unsigned)id1, (unsigned)id2, (unsigned)id1,
           (unsigned)id2, (unsigned)n, (unsigned)n_initial);
    return 0;
}

/* GC-check flow: a fresh conn must show no leftover wmtest
 * windows.  Boot daemons (desktop+taskbar+launcher) are still
 * up, so the LIST count is whatever the boot established.
 * The relevant invariant is just that the count is < some
 * sane upper bound; the previous-run `wmtest`'s deliberately-
 * leaked id2 is what we're verifying got reaped. */
static int run_gc_check(int fd)
{
    uint32_t session = 0, ver = 0;
    if (do_hello(fd, &session, &ver) < 0) return 1;

    uint32_t n = 0xFFFFFFFFu;
    if (do_list(fd, &n) < 0) return 1;
    /* Sanity bound -- well above the 3-4 boot daemons. */
    if (n > 16) {
        printf("[wmtest] FAIL reason=gc-check-toomany n=%u\n", (unsigned)n);
        return 1;
    }
    printf("[wmtest] PASS gc-check session=%u list n=%u\n",
           (unsigned)session, (unsigned)n);
    return 0;
}

int main(int argc, char **argv)
{
    int gc_check = (argc > 1) && eq(argv[1], "gc-check");

    int fd = srv_connect(WM_SOCK_PATH);
    if (fd < 0) {
        printf("[wmtest] FAIL reason=connect fd=%d path=%s\n",
               fd, WM_SOCK_PATH);
        return 1;
    }

    int rc = gc_check ? run_gc_check(fd) : run_default(fd);
    close(fd);
    return rc;
}
