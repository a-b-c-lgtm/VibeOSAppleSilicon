/*
 * TrueType rasteriser -- Chapter 102.
 *
 * The end-to-end pipeline for one glyph:
 *
 *   codepoint
 *      |  cmap (format 4) lookup
 *      v
 *   glyph index
 *      |  loca[] -- byte offset into glyf table
 *      v
 *   glyf entry (numberOfContours, xMin/yMin/xMax/yMax, then
 *               either a simple-glyph layout or a compound one)
 *      |  parse to a list of (x, y, on_curve) points organised
 *      |  by contour
 *      v
 *   contours of quadratic Bezier curves
 *      |  flatten each Bezier to short line segments at the
 *      |  target pixel size (uniform subdivision, depth 4 ~= 16
 *      |  segments per curve -- plenty for 16 px text)
 *      v
 *   list of line segments in 26.6 fixed point pixel coords
 *      |  scanline rasterise into a temporary 4x supersampled
 *      |  alpha buffer (one bit per subpixel: inside / outside,
 *      |  via the even-odd fill rule on the winding count)
 *      v
 *   alpha bitmap (one byte per output pixel; alpha = count_of_
 *                 covered_subpixels * 255 / 16)
 *      |  kmalloc'd, cached forever, returned as glyph_info
 *      v
 *   text.c / wm.c alpha-blend into framebuffer / window buffer
 *
 * Conventions:
 *  - All TTF integers are big-endian. We read them via the
 *    rb_u16 / rb_s16 / rb_u32 helpers.
 *  - The glyf table uses "font units" (FUnits). One em is
 *    `head.unitsPerEm` FUnits (typically 1000 or 2048). We
 *    convert FUnits -> pixels by multiplying by the scale
 *    (`pixel_size / unitsPerEm`) at parse time.
 *  - Y in TTF is up-positive (baseline = 0, ascender = positive).
 *    Our framebuffer is y-down. We flip Y at the very end when
 *    we know the glyph's pixel bbox.
 *  - We treat the cache as flat: an array indexed by codepoint
 *    for cp < 0x100 (covers ASCII + Latin-1 which is everything
 *    the system currently prints); anything else goes through a
 *    tiny linear hash. For chapter 102's "one font, one size"
 *    scope the flat array is plenty.
 */

#include "ttf.h"
#include "../core/heap.h"

#include <stddef.h>
#include <stdint.h>

/* The TTF rasteriser uses several large working buffers (an outline
 * with up to MAX_POINTS points, a segment list with up to MAX_SEGS
 * segments, a doubled scratch outline for the implicit-midpoint walk,
 * and rotation scratch). Together they total ~64 KB -- far too much
 * for the kernel stack (16 KB). All rasterisation happens from a
 * single drawing path (no nested calls, no interrupts that rasterise),
 * so we promote the buffers to file-scope statics. This is the same
 * tradeoff stb_truetype.h makes for its 'rasterizer state'. */

/* The font blob, exposed by Makefile's objcopy -I binary rule.
 * Symbols are emitted with underscores by objcopy. */
extern const uint8_t _binary_DejaVuSans_ttf_start[];
extern const uint8_t _binary_DejaVuSans_ttf_end[];

/* Target rendering size in pixels. Chapter 102 ships one size.  */
#define TTF_PIXEL_SIZE  16

/* Supersampling factor for grayscale AA. 4x4 means 16 subpixel
 * samples per output pixel, mapping to alpha in {0, 17, 34, ..., 255}. */
#define SS              4
#define SS2             (SS * SS)

/* Cache geometry. Flat array for the first 0x100 codepoints
 * (ASCII + Latin-1 supplement); that's what userspace prints today. */
#define CACHE_FLAT_N    0x100

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
/* Parsed face                                                        */
/* ------------------------------------------------------------------ */

struct ttf_face {
    const uint8_t *blob;
    uint32_t       blob_size;

    /* Table directory pointers (NULL if missing). */
    const uint8_t *t_head;
    const uint8_t *t_maxp;
    const uint8_t *t_cmap;
    const uint8_t *t_hhea;
    const uint8_t *t_hmtx;
    const uint8_t *t_loca;
    const uint8_t *t_glyf;

    /* Selected from head/maxp/hhea. */
    uint16_t units_per_em;
    int16_t  loca_long;          /* 0 = uint16 offsets, 1 = uint32 */
    uint16_t num_glyphs;
    uint16_t num_long_hmtx;      /* hhea.numberOfHMetrics */

    int16_t  ascender;           /* FUnits, from hhea */
    int16_t  descender;          /* FUnits (typically negative) */
    int16_t  line_gap;           /* FUnits */

    /* Picked from cmap; one of (platform=0/unicode) or
     * (platform=3/microsoft, encoding=1/unicode-bmp), format 4. */
    const uint8_t *cmap_fmt4;    /* points at the format-4 subtable */

    /* Pixel-size conversion. The glyf bbox is in FUnits; we scale
     * to subpixel units (1/64 px) by multiplying by `scale_q16`
     * and right-shifting 16. Storing scale as a Q16.16 lets us
     * use integer math throughout. */
    int32_t  scale_q16;          /* (TTF_PIXEL_SIZE * 65536) / units_per_em */

    /* Cell metrics in pixels. cell_height = ascent + |descent| + linegap,
     * rounded; cell_width = some sensible fallback advance for
     * missing-glyph cases (we use the advance of 'M' or, failing
     * that, half the em). */
    int      cell_height_px;
    int      cell_ascent_px;     /* pixels above baseline */
    int      cell_width_px;

    /* Cache: flat for cp < CACHE_FLAT_N (entry pointer or NULL).
     * Entries are kmalloc'd; never freed. */
    struct glyph_info *cache[CACHE_FLAT_N];

    /* The .notdef placeholder, rasterised once at init. */
    struct glyph_info notdef;
};

/* ------------------------------------------------------------------ */
/* Table directory: find the seven tables we need                     */
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
/* cmap format 4 lookup (codepoint -> glyph index)                    */
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
        /* Accept (0, any) Unicode or (3, 1) Microsoft Unicode BMP. */
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

    /* Linear search is fine: seg_count is small (~200 for DejaVu). */
    for (uint16_t i = 0; i < seg_count; i++) {
        uint16_t ec = rb_u16(end_codes + i * 2);
        if (ec < cp) continue;
        uint16_t sc = rb_u16(start_codes + i * 2);
        if (sc > cp) return 0;
        uint16_t iro = rb_u16(id_range_off + i * 2);
        if (iro == 0) {
            return (uint16_t)((int32_t)cp + (int16_t)rb_u16(id_delta + i * 2));
        }
        /* The complicated index expression from the TTF spec. */
        const uint8_t *p = id_range_off + i * 2 + iro
                         + (uint32_t)(cp - sc) * 2;
        uint16_t g = rb_u16(p);
        if (g == 0) return 0;
        return (uint16_t)(g + (int16_t)rb_u16(id_delta + i * 2));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* hmtx: per-glyph advance width                                      */
/* ------------------------------------------------------------------ */

static uint16_t glyph_advance_funits(const struct ttf_face *f, uint16_t gid)
{
    if (!f->t_hmtx) return f->units_per_em / 2;
    if (gid < f->num_long_hmtx) {
        return rb_u16(f->t_hmtx + gid * 4);
    }
    /* Past the long-metric array, advance is the last entry's. */
    return rb_u16(f->t_hmtx + (f->num_long_hmtx - 1) * 4);
}

/* loca: glyph-index -> byte offset into glyf. */
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

/* Per-point flags. */
#define TTF_FLAG_ON_CURVE   0x01
#define TTF_FLAG_X_SHORT    0x02
#define TTF_FLAG_Y_SHORT    0x04
#define TTF_FLAG_REPEAT     0x08
#define TTF_FLAG_X_SAME     0x10    /* x is positive (short) or same (long) */
#define TTF_FLAG_Y_SAME     0x20    /* y is positive (short) or same (long) */

/* Max points and contours we will rasterise. Big enough for any
 * glyph in DejaVu Sans BMP coverage. */
#define MAX_POINTS    512
#define MAX_CONTOURS  32

struct outline_pt {
    int32_t x, y;        /* subpixel: 1/64 of a pixel */
    uint8_t on_curve;
};

struct outline {
    struct outline_pt pts[MAX_POINTS];
    uint16_t          end_pt[MAX_CONTOURS];  /* index of last point per contour */
    int               n_pts;
    int               n_contours;
    /* Pixel-space bbox of the points (subpixel units). */
    int32_t           x_min, y_min, x_max, y_max;
};

/* Multiply FUnit value by the face's scale and convert to 26.6
 * subpixel fixed-point. */
static int32_t funit_to_subpx(const struct ttf_face *f, int32_t v)
{
    /* v * scale_q16 / 65536 gives pixels (Q0); we want 1/64 px so
     * shift left 6 after scaling. To avoid losing precision the
     * combined formula is (v * scale_q16) >> 10 (since 16 - 6 = 10). */
    int64_t prod = (int64_t)v * (int64_t)f->scale_q16;
    return (int32_t)(prod >> 10);
}

/* Parse a simple glyph at offset `goff` of size `glen` into `out`.
 * Returns 0 on success; -1 on any parse error or compound glyph
 * (caller treats as missing). */
static int parse_simple_glyph(const struct ttf_face *f,
                              uint32_t goff, uint32_t glen,
                              struct outline *out)
{
    if (glen < 10) return -1;
    const uint8_t *g = f->t_glyf + goff;
    int16_t n_contours = rb_s16(g);
    if (n_contours <= 0) return -1;                   /* compound or empty */
    if (n_contours > MAX_CONTOURS) return -1;

    /* Header: numContours(2) + xMin/yMin/xMax/yMax (8). */
    const uint8_t *p = g + 10;
    if ((uint32_t)(p - g) > glen) return -1;

    /* End points of each contour (uint16 * n_contours). */
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

    /* Instructions: skip. */
    uint16_t ins_len = rb_u16(p);
    p += 2 + ins_len;
    if ((uint32_t)(p - g) > glen) return -1;

    /* Flags array: one byte per point, with REPEAT-encoded runs. */
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

    /* X coords: delta-encoded per flag combination. */
    int32_t cur = 0;
    for (int i = 0; i < n_pts; i++) {
        uint8_t fl = flags[i];
        int32_t dx;
        if (fl & TTF_FLAG_X_SHORT) {
            dx = *p++;
            if (!(fl & TTF_FLAG_X_SAME)) dx = -dx;
        } else {
            if (fl & TTF_FLAG_X_SAME) {
                dx = 0;
            } else {
                dx = rb_s16(p); p += 2;
            }
        }
        cur += dx;
        out->pts[i].x = funit_to_subpx(f, cur);
    }
    /* Y coords: same shape. */
    cur = 0;
    for (int i = 0; i < n_pts; i++) {
        uint8_t fl = flags[i];
        int32_t dy;
        if (fl & TTF_FLAG_Y_SHORT) {
            dy = *p++;
            if (!(fl & TTF_FLAG_Y_SAME)) dy = -dy;
        } else {
            if (fl & TTF_FLAG_Y_SAME) {
                dy = 0;
            } else {
                dy = rb_s16(p); p += 2;
            }
        }
        cur += dy;
        out->pts[i].y      = funit_to_subpx(f, cur);
        out->pts[i].on_curve = (uint8_t)(flags[i] & TTF_FLAG_ON_CURVE);
    }

    /* Compute bbox in subpixel units. */
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

/* Append a line segment to the segment list. */
struct seg {
    int32_t x0, y0, x1, y1;     /* subpixel units (1/64 px) */
};

#define MAX_SEGS 2048

struct seg_list {
    struct seg s[MAX_SEGS];
    int        n;
};

/* Singletons -- see comment at top of file. */
static struct outline   g_outline;          /* parsed glyph outline */
static struct outline   g_outline_flipped;  /* same, Y-flipped + origin-shifted */
static struct outline_pt g_flat_buf[MAX_POINTS * 2];  /* implicit-midpoint walk */
static struct outline_pt g_flat_rot[MAX_POINTS * 2];  /* rotation scratch */
static struct seg_list  g_segs;             /* flattened line segments */

static void seg_add(struct seg_list *L, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1)
{
    if (y0 == y1) return;       /* horizontal segments don't affect the fill */
    if (L->n >= MAX_SEGS) return;
    L->s[L->n].x0 = x0; L->s[L->n].y0 = y0;
    L->s[L->n].x1 = x1; L->s[L->n].y1 = y1;
    L->n++;
}

/* Flatten one quadratic Bezier curve (P0, P1, P2) to line segments
 * via uniform subdivision at fixed depth (16 segments per curve).
 * That's overkill for big glyphs and exact-enough for small ones --
 * at 16 px the worst case is sub-pixel deviation. */
static void flatten_quad(struct seg_list *L,
                         int32_t x0, int32_t y0,
                         int32_t cx, int32_t cy,
                         int32_t x2, int32_t y2)
{
    const int steps = 16;
    int32_t px = x0, py = y0;
    for (int i = 1; i <= steps; i++) {
        int32_t t  = (i * 1024) / steps;        /* 0..1024 */
        int32_t it = 1024 - t;
        /* B(t) = (1-t)^2 P0 + 2(1-t)t C + t^2 P2
         * Computed as (it*it*P0 + 2*it*t*C + t*t*P2) / 1024^2.
         * 64-bit intermediates to avoid overflow on big glyphs. */
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

/* Walk a parsed outline contour-by-contour, flattening every quad
 * Bezier and emitting line segments. Implicit on-curve midpoints
 * between two off-curve points are handled per the TTF spec.
 *
 * For each contour, iterate from the first to the last point with
 * wrap. At each point look at (prev, cur, next):
 *   - if cur is on-curve   -> nothing; line moves emit from segments
 *                              between consecutive on-curve points
 *   - if cur is off-curve  -> cur is a control point; we emit a
 *                              quadratic Bezier from the previous
 *                              on-curve (or implicit midpoint) to
 *                              the next on-curve (or implicit
 *                              midpoint), with cur as the control.
 *
 * To keep the implementation small we use the textbook "implicit
 * midpoint" walk: build a synthetic point list where every
 * off-off pair has an implicit on-curve midpoint inserted, then
 * traverse triples (on, off, on) and (on, on, _) -- the latter
 * being a straight line. */
static void flatten_outline(struct seg_list *L, const struct outline *o)
{
    int start = 0;
    for (int c = 0; c < o->n_contours; c++) {
        int end = o->end_pt[c];
        int n   = end - start + 1;
        if (n < 2) { start = end + 1; continue; }

        /* Build the synthetic point array (max 2x original).
         * Uses the file-scope g_flat_buf to avoid blowing the
         * kernel stack -- see comment at top of file. */
        struct outline_pt *buf = g_flat_buf;
        int bn = 0;
        for (int i = 0; i < n; i++) {
            int idx = start + i;
            int nxt = start + (i + 1) % n;
            buf[bn++] = o->pts[idx];
            if (!o->pts[idx].on_curve && !o->pts[nxt].on_curve) {
                /* Insert implicit on-curve midpoint. */
                struct outline_pt m;
                m.x = (o->pts[idx].x + o->pts[nxt].x) / 2;
                m.y = (o->pts[idx].y + o->pts[nxt].y) / 2;
                m.on_curve = 1;
                buf[bn++] = m;
            }
            if (bn >= MAX_POINTS * 2) break;
        }
        /* Make sure the first point is on-curve. If not, rotate. */
        if (!buf[0].on_curve) {
            /* Find first on-curve, rotate. If none exist, insert
             * implicit midpoint between buf[bn-1] and buf[0]. */
            int first_on = -1;
            for (int i = 0; i < bn; i++) if (buf[i].on_curve) { first_on = i; break; }
            if (first_on < 0) {
                struct outline_pt m;
                m.x = (buf[bn-1].x + buf[0].x) / 2;
                m.y = (buf[bn-1].y + buf[0].y) / 2;
                m.on_curve = 1;
                /* Rotation isn't needed -- just append the midpoint
                 * at end and start traversal from it. We handle that
                 * by extending buf and adjusting bn. */
                buf[bn++] = m;
                first_on = bn - 1;
            }
            /* Rotate buf so first_on is at index 0. Uses the
             * file-scope g_flat_rot scratch (same reason as above). */
            struct outline_pt *rot = g_flat_rot;
            for (int i = 0; i < bn; i++) rot[i] = buf[(first_on + i) % bn];
            for (int i = 0; i < bn; i++) buf[i] = rot[i];
        }

        /* Traverse: at every on-curve point look at next-1 and
         * next-2 to decide line or curve. */
        int i = 0;
        while (i < bn) {
            int j = (i + 1) % bn;
            if (buf[j].on_curve) {
                seg_add(L, buf[i].x, buf[i].y, buf[j].x, buf[j].y);
                i++;
            } else {
                int k = (i + 2) % bn;
                /* k must be on-curve by construction. */
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

/* Given a flattened segment list in subpixel units, paint an alpha
 * bitmap of size (bmp_w x bmp_h) into `alpha`. The bitmap origin
 * (alpha[0]) corresponds to subpixel coordinate (org_x, org_y),
 * which the caller has chosen to align the glyph's pixel bbox to
 * integer pixels.
 *
 * The algorithm:
 *  - For each output row (one pixel tall = SS subpixel rows), and
 *    for each of the SS subpixel rows, intersect every segment with
 *    the horizontal line at the subpixel row's y.
 *  - Collect the x crossings, sort, walk pairs of crossings as
 *    "inside" spans. Each subpixel inside SS x SS is one sample;
 *    output alpha = sum_of_samples_inside * 255 / 16.
 *
 * Simplifications: we use the even-odd fill rule (which matches
 * non-overlapping TrueType contours for the BMP glyphs we render);
 * we sort crossings with a tiny insertion sort (segments per row
 * is typically < 16 for 16px text). */

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
    /* Per-pixel sample counter (max SS*SS per pixel). */
    /* We accumulate one full pixel row at a time (SS subrows). */
    uint8_t row_count[1024];      /* enough for any one glyph row */
    if (bmp_w > 1024) bmp_w = 1024;

    for (int py = 0; py < bmp_h; py++) {
        /* Zero per-pixel sample counts for this output row. */
        for (int i = 0; i < bmp_w; i++) row_count[i] = 0;

        for (int sy = 0; sy < SS; sy++) {
            /* y coord of this subpixel scanline (subpixel units).
             * Pixels are 64 subpx tall; subrows are 64/SS = 16. */
            int32_t y = org_y + (int32_t)py * 64
                              + (int32_t)sy * (64 / SS) + (64 / SS / 2);

            /* Find all segment crossings at this y. */
            int32_t crossings[64];
            int     nc = 0;
            for (int s = 0; s < L->n; s++) {
                int32_t y0 = L->s[s].y0;
                int32_t y1 = L->s[s].y1;
                if ((y0 <= y && y >= y1) || (y0 >= y && y <= y1)) {
                    /* segment crosses y -- check both ends. */
                }
                int hit;
                if (y0 < y1) hit = (y >= y0 && y < y1);
                else         hit = (y >= y1 && y < y0);
                if (!hit) continue;
                int32_t dy = y1 - y0;
                int32_t dx = L->s[s].x1 - L->s[s].x0;
                /* x = x0 + (y - y0) * dx / dy. Use int64 for product. */
                int64_t num = (int64_t)(y - y0) * dx;
                int32_t x   = L->s[s].x0 + (int32_t)(num / dy);
                if (nc < 64) crossings[nc++] = x;
            }
            if (nc < 2) continue;
            sort_int_asc(crossings, nc);

            /* Walk pairs of crossings; each pair brackets an "inside"
             * span. For each subpixel column inside the span,
             * increment the column's per-row sample count. Even-odd
             * fill. */
            for (int p = 0; p + 1 < nc; p += 2) {
                int32_t xa = crossings[p];
                int32_t xb = crossings[p + 1];
                /* Step through subpixel columns inside [xa, xb). */
                int32_t step = 64 / SS;
                int32_t half = step / 2;
                /* First subpixel column whose centre is >= xa - org_x. */
                int32_t local_a = xa - org_x;
                int32_t local_b = xb - org_x;
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

        /* Convert per-pixel sample counts to alpha values for this row.
         * Each pixel has up to SS*SS = 16 samples; alpha = count*255/16. */
        for (int x = 0; x < bmp_w; x++) {
            uint8_t c = row_count[x];
            if (c > SS2) c = SS2;
            alpha[py * bmp_w + x] = (uint8_t)(((uint16_t)c * 255u) / SS2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Top-level: rasterise one glyph and fill a glyph_info               */
/* ------------------------------------------------------------------ */

static int rasterise_glyph(const struct ttf_face *f, uint16_t gid,
                           struct glyph_info *out)
{
    uint32_t goff = glyph_offset(f, gid);
    uint32_t glen = glyph_size(f, gid);
    uint16_t adv_funits = glyph_advance_funits(f, gid);

    /* Compute advance in pixels (round-to-nearest). */
    int32_t adv_subpx = funit_to_subpx(f, adv_funits);
    int     adv_px    = (adv_subpx + 32) / 64;

    /* Empty glyph (whitespace such as U+0020). */
    if (glen == 0) {
        out->pixels       = NULL;
        out->bitmap_w     = 0;
        out->bitmap_h     = 0;
        out->left_bearing = 0;
        out->top_bearing  = 0;
        out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
        return 0;
    }

    /* Parse into the file-scope outline (stack-safe). */
    struct outline *o = &g_outline;
    if (parse_simple_glyph(f, goff, glen, o) != 0) {
        /* Compound or parse error -- return empty cell with the
         * face's advance so layout still works. */
        out->pixels       = NULL;
        out->bitmap_w     = 0;
        out->bitmap_h     = 0;
        out->left_bearing = 0;
        out->top_bearing  = 0;
        out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
        return 0;
    }

    /* Pick the integer pixel bbox that contains all points. We
     * flip Y here because TTF is y-up but our framebuffer is y-down. */
    int32_t pad = 64;       /* one pixel of padding to avoid clipping AA */
    int32_t xmin = (o->x_min - pad) >> 6;            /* floor */
    int32_t xmax = (o->x_max + pad + 63) >> 6;       /* ceil */
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

    /* Translate every point so the bbox origin is at (0, 0). Also
     * flip Y: y_new = (ymax_subpx - y). We do this in-place into
     * the file-scope g_outline_flipped to keep all the heavy
     * buffers off the kernel stack. */
    int32_t org_x_sub = xmin * 64;
    int32_t y_max_sub = ymax * 64;
    g_segs.n = 0;
    g_outline_flipped = *o;
    for (int i = 0; i < g_outline_flipped.n_pts; i++) {
        g_outline_flipped.pts[i].x = g_outline_flipped.pts[i].x - org_x_sub;
        g_outline_flipped.pts[i].y = y_max_sub - g_outline_flipped.pts[i].y;
    }

    flatten_outline(&g_segs, &g_outline_flipped);

    /* Allocate the alpha buffer and rasterise. */
    uint8_t *alpha = (uint8_t *)kmalloc((size_t)bmp_w * bmp_h);
    if (!alpha) return -1;
    for (int i = 0; i < bmp_w * bmp_h; i++) alpha[i] = 0;
    rasterise(&g_segs, 0, 0, bmp_w, bmp_h, alpha);

    /* Bearings:
     *   left_bearing = xmin (pixels)        -- how far right of the
     *                                          pen origin the glyph's
     *                                          left edge sits
     *   top_bearing  = ymax (pixels)        -- distance from baseline
     *                                          up to bitmap top (since
     *                                          we flipped Y, the bitmap
     *                                          starts at TTF y = ymax). */
    out->pixels       = alpha;
    out->bitmap_w     = (uint16_t)bmp_w;
    out->bitmap_h     = (uint16_t)bmp_h;
    out->left_bearing = (int16_t)xmin;
    out->top_bearing  = (int16_t)ymax;
    out->advance      = (uint16_t)(adv_px > 0 ? adv_px : 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

int ttf_init_default(struct bitmap_font *out_font)
{
    const uint8_t *blob = _binary_DejaVuSans_ttf_start;
    uint32_t       blob_size = (uint32_t)(_binary_DejaVuSans_ttf_end - blob);
    if (blob_size < 12) return -1;
    /* Sanity-check the SFNT scaler type. */
    uint32_t st = rb_u32(blob);
    if (st != 0x00010000 && st != 0x74727565 /* 'true' */) {
        return -1;
    }

    struct ttf_face *f = (struct ttf_face *)kmalloc(sizeof(*f));
    if (!f) return -1;
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
        kfree(f);
        return -1;
    }

    f->units_per_em  = rb_u16(f->t_head + 18);
    f->loca_long     = rb_s16(f->t_head + 50);
    f->num_glyphs    = rb_u16(f->t_maxp + 4);
    f->num_long_hmtx = rb_u16(f->t_hhea + 34);
    f->ascender      = rb_s16(f->t_hhea + 4);
    f->descender     = rb_s16(f->t_hhea + 6);
    f->line_gap      = rb_s16(f->t_hhea + 8);

    f->cmap_fmt4 = pick_cmap_fmt4(f->t_cmap);
    if (!f->cmap_fmt4) { kfree(f); return -1; }

    /* scale_q16 = pixel_size * 65536 / units_per_em. */
    f->scale_q16 = ((int32_t)TTF_PIXEL_SIZE << 16) / (int32_t)f->units_per_em;

    /* Pixel-space cell metrics. */
    int32_t asc_sub = funit_to_subpx(f, f->ascender);
    int32_t dsc_sub = funit_to_subpx(f, -f->descender);  /* positive depth below baseline */
    int32_t gap_sub = funit_to_subpx(f, f->line_gap);
    f->cell_ascent_px = (asc_sub + 63) / 64;
    f->cell_height_px = ((asc_sub + dsc_sub + gap_sub) + 63) / 64;
    if (f->cell_height_px < TTF_PIXEL_SIZE) f->cell_height_px = TTF_PIXEL_SIZE;

    /* Fallback advance: use the advance of 'M' if present, else half-em. */
    uint16_t gid_m = cmap_lookup(f->cmap_fmt4, 'M');
    if (gid_m) {
        int32_t adv = funit_to_subpx(f, glyph_advance_funits(f, gid_m));
        f->cell_width_px = (adv + 32) / 64;
    } else {
        f->cell_width_px = TTF_PIXEL_SIZE / 2;
    }
    if (f->cell_width_px < 1) f->cell_width_px = 1;

    /* Rasterise .notdef (glyph 0) up front; used as the placeholder. */
    if (rasterise_glyph(f, 0, &f->notdef) != 0) {
        f->notdef.pixels = NULL;
        f->notdef.bitmap_w = f->notdef.bitmap_h = 0;
        f->notdef.advance = (uint16_t)f->cell_width_px;
    }

    /* Wire into out_font. */
    out_font->kind         = BITMAP_FONT_KIND_TTF;
    out_font->cell_width   = (uint8_t)(f->cell_width_px < 255 ? f->cell_width_px : 255);
    out_font->cell_height  = (uint8_t)(f->cell_height_px < 255 ? f->cell_height_px : 255);
    out_font->line_spacing = 2;
    out_font->data         = NULL;
    out_font->glyph_count  = f->num_glyphs;
    out_font->first_cp     = 0;
    out_font->last_cp      = 0xFF;
    out_font->priv         = f;

    return 0;
}

int ttf_get_glyph(const struct bitmap_font *font, uint32_t cp,
                  struct glyph_info *out)
{
    if (!font || !out || font->kind != BITMAP_FONT_KIND_TTF) return -1;
    struct ttf_face *f = (struct ttf_face *)font->priv;
    if (!f) return -1;

    /* Cache lookup for cp in flat range. */
    if (cp < CACHE_FLAT_N && f->cache[cp]) {
        *out = *f->cache[cp];
        return 0;
    }

    /* Resolve to glyph id. */
    uint16_t gid = cmap_lookup(f->cmap_fmt4, cp);
    if (gid == 0) {
        *out = f->notdef;
        return 0;
    }

    /* Rasterise. */
    struct glyph_info gi;
    if (rasterise_glyph(f, gid, &gi) != 0) {
        *out = f->notdef;
        return 0;
    }

    /* Cache. */
    if (cp < CACHE_FLAT_N) {
        struct glyph_info *slot = (struct glyph_info *)kmalloc(sizeof(*slot));
        if (slot) {
            *slot = gi;
            f->cache[cp] = slot;
        }
    }
    *out = gi;
    return 0;
}
