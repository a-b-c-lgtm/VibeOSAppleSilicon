/*
 * userspace/fontd/ttf.c — chapter 115 TTF rasteriser, ported
 * straight from kernel/device/ttf.c (chapter 104).
 *
 * Differences from the kernel version:
 *
 *   1. heap allocator is malloc/free (libc malloc.h) instead of
 *      kmalloc/kfree.
 *   2. blob pointer is passed in via ttf_init_face() instead of
 *      being a fixed _binary_DejaVuSans_ttf_* extern (so fontd
 *      can choose what to embed; chapter 116 may add more
 *      faces).
 *   3. The face owns a per-size linked list of caches instead
 *      of a single flat array.  Lets one daemon serve glyphs
 *      at several pixel sizes — the chapter-stub multi-size
 *      promise.
 *   4. No `struct bitmap_font` wrapper — the kernel needed one
 *      so its framebuffer text path could speak to both bitmap
 *      and TTF faces; fontd is TTF-only.
 *
 * Everything else — the cmap-fmt4 walker, the simple-glyph
 * parser, the quadratic Bezier flattener, the scanline
 * rasteriser, the AA supersampling, the rotate-to-on-curve
 * trick — is byte-for-byte the same as kernel/device/ttf.c
 * shipped in chapter 104.  That's the entire point of the
 * chapter: this is the SAME code, just at EL0.
 */

#include "ttf.h"
#include "../libc/malloc.h"

#include <stddef.h>
#include <stdint.h>

/* GCC's optimiser turns struct-assignment-by-value into a call
 * to libc memcpy when the struct is large enough (here, the
 * outline struct holds ~4 KiB of point/contour state).  fontd
 * is a freestanding userspace binary with no libc memcpy in
 * scope; provide a minimal one ourselves so the relocation
 * resolves.  Same trap and same fix that userspace/libc/layout.h
 * uses; see /memories/freestanding-c-memset-trap.md. */
static __attribute__((used)) void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static __attribute__((used)) void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

/* The rasteriser uses several large working buffers (outline,
 * scratch, segment list).  Promoted to file scope so we don't
 * blow the daemon's thread stack — same trade-off the kernel
 * version made for the kernel's 16 KiB stack. */

/* Supersampling factor for grayscale AA: 4x4 = 16 samples/px. */
#define SS              4
#define SS2             (SS * SS)

/* Cache shape.  Flat array per (face, size) keyed on cp for
 * cp < 0x100 (ASCII + Latin-1).  Hash for higher codepoints.
 * Same size-and-shape choice the kernel version made. */
#define CACHE_FLAT_N    0x100u

/* Max points/contours we will rasterise.  Big enough for any
 * BMP glyph in DejaVu Sans (chapter 104 audit). */
#define MAX_POINTS    512
#define MAX_CONTOURS  32
#define MAX_SEGS      2048

/* ------------------------------------------------------------------ */
/* Big-endian readers                                                 */
/* ------------------------------------------------------------------ */

static uint16_t rb_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static int16_t rb_s16(const uint8_t *p)
{
    return (int16_t)rb_u16(p);
}
static uint32_t rb_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ------------------------------------------------------------------ */
/* Cache per (face, size)                                             */
/* ------------------------------------------------------------------ */

struct ttf_size_cache {
    struct ttf_size_cache *next;
    uint16_t                size_px;
    int32_t                 scale_q16;     /* size_px * 65536 / units_per_em */
    int                     cell_height_px;
    int                     cell_ascent_px;
    int                     cell_width_px;
    struct font_glyph       notdef;
    struct font_glyph      *cache[CACHE_FLAT_N];  /* per-cp, NULL if not cached */
};

struct ttf_face {
    const uint8_t *blob;
    uint32_t       blob_size;

    /* Table directory pointers. */
    const uint8_t *t_head;
    const uint8_t *t_maxp;
    const uint8_t *t_cmap;
    const uint8_t *t_hhea;
    const uint8_t *t_hmtx;
    const uint8_t *t_loca;
    const uint8_t *t_glyf;

    /* Selected from head/maxp/hhea. */
    uint16_t units_per_em;
    int16_t  loca_long;
    uint16_t num_glyphs;
    uint16_t num_long_hmtx;
    int16_t  ascender;
    int16_t  descender;
    int16_t  line_gap;

    /* cmap format-4 subtable. */
    const uint8_t *cmap_fmt4;

    /* Per-size cache list. */
    struct ttf_size_cache *sizes;
};

/* ------------------------------------------------------------------ */
/* Table directory                                                    */
/* ------------------------------------------------------------------ */

static uint32_t tag(const char s[4])
{
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16)
         | ((uint32_t)(uint8_t)s[2] << 8)  |  (uint32_t)(uint8_t)s[3];
}

static const uint8_t *find_table(const uint8_t *blob, uint32_t blob_size,
                                 const char tag4[4])
{
    if (blob_size < 12) return NULL;
    uint16_t num_tables = rb_u16(blob + 4);
    if ((uint32_t)12 + (uint32_t)num_tables * 16 > blob_size) return NULL;
    uint32_t want = tag(tag4);
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = blob + 12 + i * 16;
        if (rb_u32(rec) == want) {
            uint32_t off = rb_u32(rec + 8);
            uint32_t len = rb_u32(rec + 12);
            if (off > blob_size || off + len > blob_size) return NULL;
            return blob + off;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* cmap format 4                                                      */
/* ------------------------------------------------------------------ */

static const uint8_t *pick_cmap_fmt4(const uint8_t *cmap)
{
    if (!cmap) return NULL;
    uint16_t num_sub = rb_u16(cmap + 2);
    for (uint16_t i = 0; i < num_sub; i++) {
        const uint8_t *rec = cmap + 4 + i * 8;
        uint16_t plat = rb_u16(rec);
        uint16_t enc  = rb_u16(rec + 2);
        uint32_t off  = rb_u32(rec + 4);
        const uint8_t *sub = cmap + off;
        uint16_t fmt = rb_u16(sub);
        if (fmt != 4) continue;
        if (plat == 0) return sub;
        if (plat == 3 && enc == 1) return sub;
    }
    return NULL;
}

static uint16_t cmap_lookup(const uint8_t *sub, uint32_t cp)
{
    if (!sub) return 0;
    uint16_t seg_count_x2 = rb_u16(sub + 6);
    uint16_t seg_count    = (uint16_t)(seg_count_x2 / 2);
    const uint8_t *end_codes   = sub + 14;
    const uint8_t *start_codes = end_codes + seg_count_x2 + 2;
    const uint8_t *id_delta    = start_codes + seg_count_x2;
    const uint8_t *id_range_off= id_delta + seg_count_x2;

    for (uint16_t i = 0; i < seg_count; i++) {
        uint16_t ec = rb_u16(end_codes + i * 2);
        if (ec < cp) continue;
        uint16_t sc = rb_u16(start_codes + i * 2);
        if (sc > cp) return 0;
        uint16_t iro = rb_u16(id_range_off + i * 2);
        if (iro == 0) {
            return (uint16_t)((int32_t)cp + (int16_t)rb_u16(id_delta + i * 2));
        }
        const uint8_t *p = id_range_off + i * 2 + iro
                         + (uint32_t)(cp - sc) * 2;
        uint16_t g = rb_u16(p);
        if (g == 0) return 0;
        return (uint16_t)(g + (int16_t)rb_u16(id_delta + i * 2));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* hmtx + loca                                                        */
/* ------------------------------------------------------------------ */

static uint16_t glyph_advance_funits(const struct ttf_face *f, uint16_t gid)
{
    if (!f->t_hmtx) return f->units_per_em / 2;
    if (gid < f->num_long_hmtx) {
        return rb_u16(f->t_hmtx + gid * 4);
    }
    return rb_u16(f->t_hmtx + (f->num_long_hmtx - 1) * 4);
}

static uint32_t glyph_offset(const struct ttf_face *f, uint16_t gid)
{
    if (!f->t_loca || !f->t_glyf) return 0;
    if (f->loca_long) {
        return rb_u32(f->t_loca + gid * 4);
    }
    return (uint32_t)rb_u16(f->t_loca + gid * 2) * 2;
}

static uint32_t glyph_size(const struct ttf_face *f, uint16_t gid)
{
    return glyph_offset(f, gid + 1) - glyph_offset(f, gid);
}

/* ------------------------------------------------------------------ */
/* Simple-glyph parsing                                               */
/* ------------------------------------------------------------------ */

#define TTF_FLAG_ON_CURVE   0x01
#define TTF_FLAG_X_SHORT    0x02
#define TTF_FLAG_Y_SHORT    0x04
#define TTF_FLAG_REPEAT     0x08
#define TTF_FLAG_X_SAME     0x10
#define TTF_FLAG_Y_SAME     0x20

struct outline_pt {
    int32_t x, y;
    uint8_t on_curve;
};

struct outline {
    struct outline_pt pts[MAX_POINTS];
    uint16_t          end_pt[MAX_CONTOURS];
    int               n_pts;
    int               n_contours;
    int32_t           x_min, y_min, x_max, y_max;
};

static int32_t funit_to_subpx(int32_t scale_q16, int32_t v)
{
    int64_t prod = (int64_t)v * (int64_t)scale_q16;
    return (int32_t)(prod >> 10);
}

static int parse_simple_glyph(const struct ttf_face *f, int32_t scale_q16,
                              uint32_t goff, uint32_t glen,
                              struct outline *out)
{
    if (glen < 10) return -1;
    const uint8_t *g = f->t_glyf + goff;
    int16_t n_contours = rb_s16(g);
    if (n_contours <= 0) return -1;
    if (n_contours > MAX_CONTOURS) return -1;

    const uint8_t *p = g + 10;
    if ((uint32_t)(p - g) > glen) return -1;

    int n_pts = 0;
    for (int i = 0; i < n_contours; i++) {
        uint16_t end = rb_u16(p);
        p += 2;
        out->end_pt[i] = end;
        if (end + 1 > n_pts) n_pts = end + 1;
    }
    if (n_pts > MAX_POINTS) return -1;
    out->n_contours = n_contours;
    out->n_pts      = n_pts;

    uint16_t ins_len = rb_u16(p);
    p += 2 + ins_len;
    if ((uint32_t)(p - g) > glen) return -1;

    uint8_t flags[MAX_POINTS];
    int fi = 0;
    while (fi < n_pts) {
        uint8_t flag = *p++;
        flags[fi++] = flag;
        if (flag & TTF_FLAG_REPEAT) {
            uint8_t rep = *p++;
            while (rep-- && fi < n_pts) flags[fi++] = flag;
        }
    }
    if ((uint32_t)(p - g) > glen) return -1;

    int32_t cur = 0;
    for (int i = 0; i < n_pts; i++) {
        uint8_t fl = flags[i];
        int32_t dx;
        if (fl & TTF_FLAG_X_SHORT) {
            dx = *p++;
            if (!(fl & TTF_FLAG_X_SAME)) dx = -dx;
        } else {
            if (fl & TTF_FLAG_X_SAME) dx = 0;
            else { dx = rb_s16(p); p += 2; }
        }
        cur += dx;
        out->pts[i].x = funit_to_subpx(scale_q16, cur);
    }
    cur = 0;
    for (int i = 0; i < n_pts; i++) {
        uint8_t fl = flags[i];
        int32_t dy;
        if (fl & TTF_FLAG_Y_SHORT) {
            dy = *p++;
            if (!(fl & TTF_FLAG_Y_SAME)) dy = -dy;
        } else {
            if (fl & TTF_FLAG_Y_SAME) dy = 0;
            else { dy = rb_s16(p); p += 2; }
        }
        cur += dy;
        out->pts[i].y      = funit_to_subpx(scale_q16, cur);
        out->pts[i].on_curve = (uint8_t)(flags[i] & TTF_FLAG_ON_CURVE);
    }

    out->x_min = out->y_min =  0x7fffffff;
    out->x_max = out->y_max = -0x7fffffff;
    for (int i = 0; i < n_pts; i++) {
        if (out->pts[i].x < out->x_min) out->x_min = out->pts[i].x;
        if (out->pts[i].x > out->x_max) out->x_max = out->pts[i].x;
        if (out->pts[i].y < out->y_min) out->y_min = out->pts[i].y;
        if (out->pts[i].y > out->y_max) out->y_max = out->pts[i].y;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bezier flattening                                                  */
/* ------------------------------------------------------------------ */

struct seg {
    int32_t x0, y0, x1, y1;
};

struct seg_list {
    struct seg s[MAX_SEGS];
    int        n;
};

static struct outline    g_outline;
static struct outline    g_outline_flipped;
static struct outline_pt g_flat_buf[MAX_POINTS * 2];
static struct outline_pt g_flat_rot[MAX_POINTS * 2];
static struct seg_list   g_segs;

static void seg_add(struct seg_list *L, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1)
{
    if (y0 == y1) return;
    if (L->n >= MAX_SEGS) return;
    L->s[L->n].x0 = x0; L->s[L->n].y0 = y0;
    L->s[L->n].x1 = x1; L->s[L->n].y1 = y1;
    L->n++;
}

static void flatten_quad(struct seg_list *L,
                         int32_t x0, int32_t y0,
                         int32_t cx, int32_t cy,
                         int32_t x2, int32_t y2)
{
    const int steps = 16;
    int32_t px = x0, py = y0;
    for (int i = 1; i <= steps; i++) {
        int32_t t  = (i * 1024) / steps;
        int32_t it = 1024 - t;
        int64_t a = (int64_t)it * it;
        int64_t b = (int64_t)2 * it * t;
        int64_t c = (int64_t)t * t;
        int64_t qx = (a * x0 + b * cx + c * x2) / ((int64_t)1024 * 1024);
        int64_t qy = (a * y0 + b * cy + c * y2) / ((int64_t)1024 * 1024);
        seg_add(L, px, py, (int32_t)qx, (int32_t)qy);
        px = (int32_t)qx;
        py = (int32_t)qy;
    }
}

static void flatten_outline(struct seg_list *L, const struct outline *o)
{
    int start = 0;
    for (int c = 0; c < o->n_contours; c++) {
        int end = o->end_pt[c];
        int n   = end - start + 1;
        if (n < 2) { start = end + 1; continue; }

        struct outline_pt *buf = g_flat_buf;
        int bn = 0;
        for (int i = 0; i < n; i++) {
            int idx = start + i;
            int nxt = start + (i + 1) % n;
            buf[bn++] = o->pts[idx];
            if (!o->pts[idx].on_curve && !o->pts[nxt].on_curve) {
                struct outline_pt m;
                m.x = (o->pts[idx].x + o->pts[nxt].x) / 2;
                m.y = (o->pts[idx].y + o->pts[nxt].y) / 2;
                m.on_curve = 1;
                buf[bn++] = m;
            }
            if (bn >= MAX_POINTS * 2) break;
        }
        if (!buf[0].on_curve) {
            int first_on = -1;
            for (int i = 0; i < bn; i++) if (buf[i].on_curve) { first_on = i; break; }
            if (first_on < 0) {
                struct outline_pt m;
                m.x = (buf[bn-1].x + buf[0].x) / 2;
                m.y = (buf[bn-1].y + buf[0].y) / 2;
                m.on_curve = 1;
                buf[bn++] = m;
                first_on = bn - 1;
            }
            struct outline_pt *rot = g_flat_rot;
            for (int i = 0; i < bn; i++) rot[i] = buf[(first_on + i) % bn];
            for (int i = 0; i < bn; i++) buf[i] = rot[i];
        }

        int i = 0;
        while (i < bn) {
            int j = (i + 1) % bn;
            if (buf[j].on_curve) {
                seg_add(L, buf[i].x, buf[i].y, buf[j].x, buf[j].y);
                i++;
            } else {
                int k = (i + 2) % bn;
                flatten_quad(L,
                             buf[i].x, buf[i].y,
                             buf[j].x, buf[j].y,
                             buf[k].x, buf[k].y);
                i += 2;
            }
            if (i >= bn) break;
        }
        start = end + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Scanline rasterisation                                             */
/* ------------------------------------------------------------------ */

static void sort_int_asc(int32_t *xs, int n)
{
    for (int i = 1; i < n; i++) {
        int32_t v = xs[i];
        int     j = i - 1;
        while (j >= 0 && xs[j] > v) { xs[j+1] = xs[j]; j--; }
        xs[j+1] = v;
    }
}

static void rasterise(const struct seg_list *L,
                      int32_t org_x, int32_t org_y,
                      int bmp_w, int bmp_h,
                      uint8_t *alpha)
{
    (void)org_x; /* not used after the per-segment local_a offset */
    uint8_t row_count[1024];
    if (bmp_w > 1024) bmp_w = 1024;

    for (int py = 0; py < bmp_h; py++) {
        for (int i = 0; i < bmp_w; i++) row_count[i] = 0;

        for (int sy = 0; sy < SS; sy++) {
            int32_t y = org_y + (int32_t)py * 64
                              + (int32_t)sy * (64 / SS) + (64 / SS / 2);

            int32_t crossings[64];
            int     nc = 0;
            for (int s = 0; s < L->n; s++) {
                int32_t y0 = L->s[s].y0;
                int32_t y1 = L->s[s].y1;
                int hit;
                if (y0 < y1) hit = (y >= y0 && y < y1);
                else         hit = (y >= y1 && y < y0);
                if (!hit) continue;
                int32_t dy = y1 - y0;
                int32_t dx = L->s[s].x1 - L->s[s].x0;
                int64_t num = (int64_t)(y - y0) * dx;
                int32_t x   = L->s[s].x0 + (int32_t)(num / dy);
                if (nc < 64) crossings[nc++] = x;
            }
            if (nc < 2) continue;
            sort_int_asc(crossings, nc);

            for (int p = 0; p + 1 < nc; p += 2) {
                int32_t xa = crossings[p];
                int32_t xb = crossings[p + 1];
                int32_t step = 64 / SS;
                int32_t half = step / 2;
                int32_t local_a = xa;
                int32_t local_b = xb;
                int sx_first = (int)((local_a + half - 1) / step);
                int sx_last  = (int)((local_b - half)     / step);
                if (sx_first < 0) sx_first = 0;
                int sx_max = bmp_w * SS - 1;
                if (sx_last > sx_max) sx_last = sx_max;
                for (int sx = sx_first; sx <= sx_last; sx++) {
                    int px = sx / SS;
                    if (px >= 0 && px < bmp_w) row_count[px]++;
                }
            }
        }

        for (int x = 0; x < bmp_w; x++) {
            uint8_t c = row_count[x];
            if (c > SS2) c = SS2;
            alpha[py * bmp_w + x] = (uint8_t)(((uint16_t)c * 255u) / SS2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Glyph-level driver                                                 */
/* ------------------------------------------------------------------ */

static int rasterise_glyph(const struct ttf_face *f,
                           const struct ttf_size_cache *sz,
                           uint16_t gid, struct font_glyph *out)
{
    uint32_t goff = glyph_offset(f, gid);
    uint32_t glen = glyph_size(f, gid);
    uint16_t adv_funits = glyph_advance_funits(f, gid);

    int32_t adv_subpx = funit_to_subpx(sz->scale_q16, adv_funits);
    int     adv_px    = (adv_subpx + 32) / 64;

    if (glen == 0) {
        out->pixels       = NULL;
        out->bitmap_w     = 0;
        out->bitmap_h     = 0;
        out->left_bearing = 0;
        out->top_bearing  = 0;
        out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
        return 0;
    }

    struct outline *o = &g_outline;
    if (parse_simple_glyph(f, sz->scale_q16, goff, glen, o) != 0) {
        out->pixels       = NULL;
        out->bitmap_w     = 0;
        out->bitmap_h     = 0;
        out->left_bearing = 0;
        out->top_bearing  = 0;
        out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
        return 0;
    }

    int32_t pad = 64;
    int32_t xmin = (o->x_min - pad) >> 6;
    int32_t xmax = (o->x_max + pad + 63) >> 6;
    int32_t ymin = (o->y_min - pad) >> 6;
    int32_t ymax = (o->y_max + pad + 63) >> 6;
    int bmp_w = (int)(xmax - xmin);
    int bmp_h = (int)(ymax - ymin);
    if (bmp_w <= 0 || bmp_h <= 0 || bmp_w > 256 || bmp_h > 256) {
        out->pixels       = NULL;
        out->bitmap_w     = 0;
        out->bitmap_h     = 0;
        out->left_bearing = 0;
        out->top_bearing  = 0;
        out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
        return 0;
    }

    int32_t org_x_sub = xmin * 64;
    int32_t y_max_sub = ymax * 64;
    g_segs.n = 0;
    g_outline_flipped = *o;
    for (int i = 0; i < g_outline_flipped.n_pts; i++) {
        g_outline_flipped.pts[i].x = g_outline_flipped.pts[i].x - org_x_sub;
        g_outline_flipped.pts[i].y = y_max_sub - g_outline_flipped.pts[i].y;
    }

    flatten_outline(&g_segs, &g_outline_flipped);

    uint8_t *alpha = (uint8_t *)malloc((size_t)bmp_w * bmp_h);
    if (!alpha) return -1;
    for (int i = 0; i < bmp_w * bmp_h; i++) alpha[i] = 0;
    rasterise(&g_segs, 0, 0, bmp_w, bmp_h, alpha);

    out->pixels       = alpha;
    out->bitmap_w     = (uint16_t)bmp_w;
    out->bitmap_h     = (uint16_t)bmp_h;
    out->left_bearing = (int16_t)xmin;
    out->top_bearing  = (int16_t)ymax;
    out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-size cache management                                          */
/* ------------------------------------------------------------------ */

static struct ttf_size_cache *find_size(struct ttf_face *f, uint16_t size_px)
{
    for (struct ttf_size_cache *s = f->sizes; s; s = s->next) {
        if (s->size_px == size_px) return s;
    }
    return NULL;
}

static struct ttf_size_cache *make_size(struct ttf_face *f, uint16_t size_px)
{
    if (size_px == 0 || size_px > 128) return NULL;
    struct ttf_size_cache *sz = (struct ttf_size_cache *)malloc(sizeof(*sz));
    if (!sz) return NULL;
    /* Explicit zero — avoid GCC's hidden memset call on `= {0}`
     * for sizeof(*sz) > ~64 bytes.  See memory note
     * `freestanding-c-memset-trap.md` (userspace inherits the
     * same trap when -ffreestanding is on). */
    for (size_t i = 0; i < sizeof(*sz); i++) ((uint8_t *)sz)[i] = 0;
    sz->size_px = size_px;

    sz->scale_q16 = ((int32_t)size_px << 16) / (int32_t)f->units_per_em;

    int32_t asc_sub = funit_to_subpx(sz->scale_q16,  f->ascender);
    int32_t dsc_sub = funit_to_subpx(sz->scale_q16, -f->descender);
    int32_t gap_sub = funit_to_subpx(sz->scale_q16,  f->line_gap);
    sz->cell_ascent_px = (asc_sub + 63) / 64;
    sz->cell_height_px = ((asc_sub + dsc_sub + gap_sub) + 63) / 64;
    if (sz->cell_height_px < (int)size_px) sz->cell_height_px = (int)size_px;

    uint16_t gid_m = cmap_lookup(f->cmap_fmt4, 'M');
    if (gid_m) {
        int32_t adv = funit_to_subpx(sz->scale_q16, glyph_advance_funits(f, gid_m));
        sz->cell_width_px = (adv + 32) / 64;
    } else {
        sz->cell_width_px = (int)size_px / 2;
    }
    if (sz->cell_width_px < 1) sz->cell_width_px = 1;

    if (rasterise_glyph(f, sz, 0, &sz->notdef) != 0) {
        sz->notdef.pixels = NULL;
        sz->notdef.bitmap_w = sz->notdef.bitmap_h = 0;
        sz->notdef.advance = (uint16_t)sz->cell_width_px;
    }

    sz->next = f->sizes;
    f->sizes = sz;
    return sz;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

struct ttf_face *ttf_init_face(const uint8_t *blob, uint32_t blob_size)
{
    if (!blob || blob_size < 12) return NULL;
    uint32_t st = rb_u32(blob);
    if (st != 0x00010000 && st != 0x74727565 /* 'true' */) return NULL;

    struct ttf_face *f = (struct ttf_face *)malloc(sizeof(*f));
    if (!f) return NULL;
    for (size_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    f->blob = blob; f->blob_size = blob_size;

    f->t_head = find_table(blob, blob_size, "head");
    f->t_maxp = find_table(blob, blob_size, "maxp");
    f->t_cmap = find_table(blob, blob_size, "cmap");
    f->t_hhea = find_table(blob, blob_size, "hhea");
    f->t_hmtx = find_table(blob, blob_size, "hmtx");
    f->t_loca = find_table(blob, blob_size, "loca");
    f->t_glyf = find_table(blob, blob_size, "glyf");
    if (!f->t_head || !f->t_maxp || !f->t_cmap || !f->t_hhea
        || !f->t_hmtx || !f->t_loca || !f->t_glyf) {
        free(f);
        return NULL;
    }

    f->units_per_em  = rb_u16(f->t_head + 18);
    f->loca_long     = rb_s16(f->t_head + 50);
    f->num_glyphs    = rb_u16(f->t_maxp + 4);
    f->num_long_hmtx = rb_u16(f->t_hhea + 34);
    f->ascender      = rb_s16(f->t_hhea + 4);
    f->descender     = rb_s16(f->t_hhea + 6);
    f->line_gap      = rb_s16(f->t_hhea + 8);

    f->cmap_fmt4 = pick_cmap_fmt4(f->t_cmap);
    if (!f->cmap_fmt4) { free(f); return NULL; }

    return f;
}

int ttf_get_glyph(struct ttf_face *face, uint32_t cp, uint16_t size_px,
                  struct font_glyph *out)
{
    if (!face || !out) return -1;
    if (size_px == 0) size_px = 16;

    struct ttf_size_cache *sz = find_size(face, size_px);
    if (!sz) {
        sz = make_size(face, size_px);
        if (!sz) return -1;
    }

    if (cp < CACHE_FLAT_N && sz->cache[cp]) {
        *out = *sz->cache[cp];
        return 0;
    }

    uint16_t gid = cmap_lookup(face->cmap_fmt4, cp);
    if (gid == 0) {
        *out = sz->notdef;
        return 0;
    }

    struct font_glyph gi;
    if (rasterise_glyph(face, sz, gid, &gi) != 0) {
        *out = sz->notdef;
        return 0;
    }

    if (cp < CACHE_FLAT_N) {
        struct font_glyph *slot = (struct font_glyph *)malloc(sizeof(*slot));
        if (slot) {
            *slot = gi;
            sz->cache[cp] = slot;
        }
    }
    *out = gi;
    return 0;
}

int ttf_get_metrics(struct ttf_face *face, uint32_t cp, uint16_t size_px,
                    struct font_glyph *out)
{
    int rc = ttf_get_glyph(face, cp, size_px, out);
    if (rc == 0) {
        /* The caller asked for metrics only — but our cache always
         * has the full bitmap, so we just blank out `pixels` to
         * make the contract obvious.  Saves no work; future
         * optimisation: short-circuit before raster if the
         * cache misses. */
        out->pixels = NULL;
    }
    return rc;
}

void ttf_warm_ascii(struct ttf_face *face, uint16_t size_px)
{
    if (!face) return;
    struct font_glyph tmp;
    for (uint32_t cp = 0x20; cp <= 0x7E; cp++) {
        (void)ttf_get_glyph(face, cp, size_px, &tmp);
    }
}
