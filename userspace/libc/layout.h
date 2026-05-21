/*
 * userspace/libc/layout.h — header-only CSS-driven layout engine.
 *
 * Single-translation-unit convention (matches printf.h, malloc.h,
 * url.h, http.h, html.h, dom.h, css.h): include from one .c per
 * binary.
 *
 * What this layer does:
 *
 *   1. CASCADE.  Combines three stylesheets (UA + author + inline
 *      style="..." attribute) into a fully resolved `struct
 *      computed_style` per DOM element.  Inheritance is performed
 *      for every property the spec marks as inherited (color,
 *      font-*, line-height, text-*, white-space, list-style,
 *      visibility).  Specificity + source-order tiebreaks match
 *      CSS 2.1 §6.4.
 *
 *   2. BOX TREE.  Walks the DOM (skipping `display: none` subtrees
 *      and head-only nodes) and produces a tree of `struct
 *      layout_box`.  Generates anonymous block boxes around inline
 *      runs that have block siblings, generates anonymous text
 *      boxes for DOM text nodes, and respects display: inline /
 *      inline-block / block / list-item / none.
 *
 *   3. LAYOUT.  Two formatting contexts:
 *
 *        - Block formatting context: children stack vertically.
 *          Margins between adjacent in-flow block siblings collapse
 *          per CSS 2.1 §8.3.1 (max of positive, plus most-negative).
 *          width/height resolve against containing block; `auto`
 *          width fills available space minus margin/border/padding.
 *
 *        - Inline formatting context: a sequence of inline boxes
 *          and text runs is broken into line boxes.  Word-wrap
 *          happens at whitespace.  Each line box is then aligned
 *          horizontally per text-align (left/center/right/justify).
 *          `line-height` controls the line box height; baseline
 *          alignment is approximated as cell-bottom alignment for
 *          our fixed monospace font.
 *
 *   4. PAINT.  Emits a flat array of `struct paint_cmd` in
 *      back-to-front order.  Each command is either a filled
 *      rectangle (background / border / list bullet) or a text run.
 *      The browser binary in M63 will play this list against the
 *      GUI server.
 *
 * What we do NOT model (yet):
 *
 *   - Floats (`float: left/right`).
 *   - Positioning other than `static` (no relative/absolute/fixed).
 *   - Flexbox, Grid, multi-column.
 *   - Tables (table-layout, table-cell baseline, row-spanning).
 *   - z-index / stacking contexts beyond document order.
 *   - Replaced elements with intrinsic size (we treat <img> as
 *     a placeholder rectangle of its `width` / `height` attribute,
 *     0 if missing).
 *   - Transforms, opacity, filters, animations.
 *   - Bidi / shaping / kerning — we use the kernel's 8x16 fixed
 *     cell font, scaled to integer multiples for font-size.
 *
 * Memory:
 *
 *   Computed styles, layout boxes, and paint commands are
 *   individually malloc'd from the userspace heap.  `layout_destroy`
 *   walks the tree and frees everything.  Cost is fine at
 *   document sizes we care about (a few thousand boxes).
 */
#ifndef USER_LAYOUT_H
#define USER_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

#include "malloc.h"
#include "dom.h"
#include "css.h"

/* ============================================================
 *   Intrinsic-size hook for replaced elements (chapter 98b)
 *
 *   When an <img> has no width=""/height="" attribute the layout
 *   pass historically defaulted to a 16x16 placeholder so the
 *   surrounding flow didn't collapse onto the baseline.  That
 *   was fine in the chapter-97 era when every <img> in our test
 *   corpus had explicit dimensions, but real-world pages routinely
 *   omit them and rely on the browser to use the image's intrinsic
 *   size from the decoded pixels.
 *
 *   Since the layout pass cannot reach into the browser's image
 *   cache directly (no kernel-side image API, no shared global
 *   state in a header-only library), we expose a function-pointer
 *   hook.  The browser sets it before calling layout_build_and_run
 *   and clears it afterwards.  Layout calls it once per <img>
 *   element that has at least one missing dimension; the callback
 *   returns 0 on cache-hit and writes the intrinsic w/h.
 *
 *   The hook variable is static-per-TU; setter and use both live
 *   in this header, so they share the same TU's copy when the
 *   header is included by browser.c (the only consumer that cares).
 * ============================================================ */

typedef int (*lay_img_size_fn)(const char *src, int *out_w, int *out_h,
                               void *ud);

static lay_img_size_fn lay__img_size_fn = 0;
static void          *lay__img_size_ud = 0;

static inline void layout_set_img_size_lookup(lay_img_size_fn fn, void *ud)
{
    lay__img_size_fn = fn;
    lay__img_size_ud = ud;
}

/* ============================================================
 *   PART 1 — value tags and the computed-style struct
 * ============================================================ */

/* `display` values we model.  Anything we don't recognise lands as
 * INLINE (the CSS default).  TABLE / TABLE_ROW / TABLE_CELL are
 * placeholders: we accept the keywords from CSS but currently lay
 * them out as BLOCK (no real table algorithm).  Recording them
 * separately lets a future M63+ chapter add table layout without
 * touching the parser. */
enum layout_display {
    LAY_DISPLAY_INLINE       = 0,    /* the CSS initial value */
    LAY_DISPLAY_BLOCK        = 1,
    LAY_DISPLAY_INLINE_BLOCK = 2,
    LAY_DISPLAY_LIST_ITEM    = 3,
    LAY_DISPLAY_NONE         = 4,
    LAY_DISPLAY_TABLE        = 5,
    LAY_DISPLAY_TABLE_ROW    = 6,
    LAY_DISPLAY_TABLE_CELL   = 7,
};

enum layout_font_weight {
    LAY_FW_NORMAL = 400,
    LAY_FW_BOLD   = 700,
};

enum layout_font_style {
    LAY_FS_NORMAL = 0,
    LAY_FS_ITALIC = 1,
};

enum layout_text_align {
    LAY_TA_LEFT    = 0,
    LAY_TA_CENTER  = 1,
    LAY_TA_RIGHT   = 2,
    LAY_TA_JUSTIFY = 3,
};

enum layout_text_decoration {
    LAY_TD_NONE        = 0,
    LAY_TD_UNDERLINE   = 1 << 0,
    LAY_TD_LINETHROUGH = 1 << 1,
    LAY_TD_OVERLINE    = 1 << 2,
};

enum layout_white_space {
    LAY_WS_NORMAL = 0,    /* collapse runs, wrap on whitespace */
    LAY_WS_PRE    = 1,    /* preserve whitespace + newlines, no wrap */
    LAY_WS_NOWRAP = 2,    /* collapse runs, no wrap */
    LAY_WS_PRE_WRAP = 3,  /* preserve whitespace, allow wrap */
};

enum layout_list_style {
    LAY_LS_NONE   = 0,
    LAY_LS_DISC   = 1,    /* default for <ul> */
    LAY_LS_CIRCLE = 2,
    LAY_LS_SQUARE = 3,
    LAY_LS_DECIMAL = 4,   /* default for <ol> */
};

/* A `length` is one of:
 *   - LAY_LEN_AUTO       — `auto` keyword (width/height/margin)
 *   - LAY_LEN_PX         — fixed pixels (any positive or negative)
 *   - LAY_LEN_PERCENT    — value in tenths-of-a-percent (so 12.5%
 *                          = 125), resolved against the containing
 *                          block dimension at use time
 *   - LAY_LEN_EM         — value in hundredths-of-an-em (1.5em = 150),
 *                          resolved against the element's own
 *                          (post-font-size-resolution) font size
 *
 * The value field is always an int32; the unit determines how it
 * is interpreted.  Storing units lets percentages reflow correctly
 * when the viewport changes width without re-running the cascade. */
enum layout_unit {
    LAY_LEN_AUTO    = 0,
    LAY_LEN_PX      = 1,
    LAY_LEN_PERCENT = 2,    /* value in tenths of a percent */
    LAY_LEN_EM      = 3,    /* value in hundredths of an em  */
    LAY_LEN_REM     = 4,    /* same encoding as EM, against root */
};

struct layout_length {
    int unit;       /* enum layout_unit */
    int v;          /* see comment above */
};

/* A colour is RGBA in 0xAARRGGBB (alpha in the high byte).  An
 * unset colour uses the sentinel below; the cascade replaces it
 * with the inherited or initial value before painting. */
typedef uint32_t layout_color_t;

#define LAY_COLOR_UNSET        0x00000000u   /* fully-transparent black; unused as a real colour */
#define LAY_COLOR_BLACK        0xFF000000u
#define LAY_COLOR_WHITE        0xFFFFFFFFu
#define LAY_COLOR_TRANSPARENT  0x00000000u

/* The computed style.  One of these is allocated per DOM element
 * during the cascade; layout reads it but never writes it. */
struct layout_computed {
    int                  display;        /* enum layout_display */
    enum layout_font_weight font_weight;
    int                  font_style;     /* enum layout_font_style */
    int                  font_size_px;   /* fully resolved (em/rem/% gone) */
    int                  text_align;     /* enum layout_text_align */
    int                  text_decoration;/* bitmask of enum layout_text_decoration */
    int                  white_space;    /* enum layout_white_space */
    int                  list_style;     /* enum layout_list_style */
    int                  line_height_px; /* fully resolved */

    /* Box-model lengths.  These keep their units so percentages
     * and `auto` survive into layout, which is the consumer that
     * knows the containing-block dimensions. */
    struct layout_length width;
    struct layout_length height;
    struct layout_length margin[4];      /* 0=top 1=right 2=bot 3=left */
    struct layout_length padding[4];
    int                  border_px[4];   /* in pixels, 0 means none */
    layout_color_t       border_color[4];

    /* Colours.  background may be transparent (the default) or set;
     * colour is always set after the cascade (initial = black). */
    layout_color_t       color;
    layout_color_t       background;

    /* Non-CSS flag that mirrors the HTML5 rendering rule:
     *     center, div[align=center i] { text-align: -webkit-center; }
     * `text-align: -webkit-center` is a legacy, browser-vendor
     * extension whose effect is "ALSO centre block-level
     * descendants in the containing block", not just inline content.
     * We model it as a separate flag so the block-flow loop in
     * layout_container_children can recentre each block child
     * after laying it out.  See layout_build_subtree where this
     * flag is set, and the centring branch in
     * layout_container_children where it is consumed.
     *
     * Why this matters: pages like plaintextworld.com wrap their
     * whole multi-column table layout in a single
     * `<div align="center">` and expect the inner content to stay
     * centred as the viewport widens.  Without this flag the
     * outer `<table>` (which has a fixed intrinsic max-content
     * width) just sits flush-left, producing a big empty gutter
     * on the right when the window is wider than the document. */
    int                  center_block_children;

    /* Second half of our `text-align: -webkit-center` model.
     * Inheritable: set by `layout_build_subtree` for `<center>`,
     * `<div align=center>` and `<body align=center>`, then
     * propagated to descendants via `layout_computed_inherit`.
     * After `layout_resolve` runs, `layout_build_subtree`
     * re-asserts `text-align: center` on any `<table>/<tr>/<td>/
     * <th>` whose computed text-align is the UA-default LEFT.
     * This makes the `<h2>` title in PTW's
     * `<div align="center"><table><tr><td><h2>...` centred within
     * its cell (so it tracks with the centred outer table when the
     * window is resized) without dragging cell content centring
     * onto pages like Hacker News that just want
     * `<table align="center">` to centre the table itself. */
    int                  text_align_center_inherit;
};

/* The four box-side indices, used by margin/padding/border. */
#define LAY_TOP    0
#define LAY_RIGHT  1
#define LAY_BOT    2
#define LAY_LEFT   3

/* ============================================================
 *   PART 2 — small helpers (string + numeric)
 * ============================================================ */

/* Freestanding mem* shims (memcpy / memset / memmove).  GCC
 * emits implicit calls to these when the optimizer recognises a
 * struct copy or zero-init bigger than its inlining threshold,
 * and userspace links without libc.  Centralised in
 * libc/freestanding.h so headers like cookies.h can include the
 * same definitions without producing duplicate `static memcpy`
 * symbols when both headers land in one .c.
 *
 * Documented in /memories/freestanding-c-memset-trap.md. */
#include "freestanding.h"

static inline int lay_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static inline int lay_streq_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (lay_lower((unsigned char)*a) != lay_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static inline int lay_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static inline int lay_streq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (a[i] == 0) return 0; }
    return 1;
}

static inline int lay_strlen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static inline int lay_is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static inline int lay_is_digit(int c) { return c >= '0' && c <= '9'; }

static inline int lay_is_hex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int lay_hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Parse a signed integer from src, advancing *pos.  Returns the
 * number of bytes consumed (0 if no digits). */
static inline int lay_parse_int(const char *src, int len, int *pos, int *out)
{
    int p = *pos;
    int sign = 1;
    if (p < len && src[p] == '-') { sign = -1; p++; }
    else if (p < len && src[p] == '+') { p++; }
    int start = p;
    long v = 0;
    while (p < len && lay_is_digit(src[p])) { v = v * 10 + (src[p] - '0'); p++; }
    if (p == start) return 0;
    *out = (int)(v * sign);
    int n = p - *pos;
    *pos = p;
    return n;
}

/* Parse a non-negative decimal number into hundredths.  "1.5" -> 150,
 * "12" -> 1200, ".25" -> 25.  Returns bytes consumed, 0 on failure.
 * Used for em / line-height multipliers. */
static inline int lay_parse_centi(const char *src, int len, int *pos, int *out)
{
    int p = *pos;
    int sign = 1;
    if (p < len && src[p] == '-') { sign = -1; p++; }
    else if (p < len && src[p] == '+') { p++; }
    int start = p;
    long whole = 0;
    while (p < len && lay_is_digit(src[p])) { whole = whole * 10 + (src[p] - '0'); p++; }
    long frac = 0;
    int frac_digits = 0;
    if (p < len && src[p] == '.') {
        p++;
        while (p < len && lay_is_digit(src[p]) && frac_digits < 4) {
            frac = frac * 10 + (src[p] - '0');
            p++; frac_digits++;
        }
        /* drop excess digits */
        while (p < len && lay_is_digit(src[p])) p++;
    }
    if (p == start) return 0;
    long centi = whole * 100;
    if (frac_digits == 1) centi += frac * 10;
    else if (frac_digits == 2) centi += frac;
    else if (frac_digits == 3) centi += (frac + 5) / 10;
    else if (frac_digits == 4) centi += (frac + 50) / 100;
    *out = (int)(centi * sign);
    int n = p - *pos;
    *pos = p;
    return n;
}

static inline char *lay_strdup_n(const char *s, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (!out) return 0;
    for (size_t i = 0; i < n; i++) out[i] = s[i];
    out[n] = 0;
    return out;
}

static inline char *lay_strdup(const char *s)
{
    int n = lay_strlen(s);
    return lay_strdup_n(s, (size_t)n);
}

/* Trim leading + trailing whitespace from a malloc'd string IN PLACE.
 * Returns the input pointer (so it composes with strdup). */
static inline char *lay_trim(char *s)
{
    if (!s) return s;
    int n = lay_strlen(s);
    int a = 0, b = n;
    while (a < b && lay_is_ws((unsigned char)s[a])) a++;
    while (b > a && lay_is_ws((unsigned char)s[b - 1])) b--;
    if (a > 0) for (int i = 0; i < b - a; i++) s[i] = s[a + i];
    s[b - a] = 0;
    return s;
}

/* ============================================================
 *   PART 3 — value parsers (color, length, keywords)
 * ============================================================ */

/* Parse a CSS colour from a value string.  Supports:
 *   - "transparent"
 *   - 17 CSS Level-1 named colours + a handful of common extras
 *   - #RGB, #RRGGBB, #RGBA, #RRGGBBAA
 *   - rgb(r,g,b), rgba(r,g,b,a)  (numeric components only)
 *
 * On success returns 1 and stores the colour in *out.  On failure
 * returns 0 and leaves *out untouched.  Leading whitespace is
 * tolerated; trailing tokens after the value are ignored. */

struct lay_named_color { const char *name; layout_color_t rgba; };

static inline const struct lay_named_color *lay_named_colors(int *n_out)
{
    static const struct lay_named_color tbl[] = {
        { "transparent", LAY_COLOR_TRANSPARENT },
        { "black",   0xFF000000u }, { "white",  0xFFFFFFFFu },
        { "red",     0xFFFF0000u }, { "green",  0xFF008000u },
        { "blue",    0xFF0000FFu }, { "yellow", 0xFFFFFF00u },
        { "navy",    0xFF000080u }, { "teal",   0xFF008080u },
        { "olive",   0xFF808000u }, { "purple", 0xFF800080u },
        { "maroon",  0xFF800000u }, { "lime",   0xFF00FF00u },
        { "aqua",    0xFF00FFFFu }, { "fuchsia",0xFFFF00FFu },
        { "silver",  0xFFC0C0C0u }, { "gray",   0xFF808080u },
        { "grey",    0xFF808080u }, { "orange", 0xFFFFA500u },
        { "pink",    0xFFFFC0CBu }, { "brown",  0xFFA52A2Au },
        { "gold",    0xFFFFD700u }, { "darkgray", 0xFFA9A9A9u },
        { "darkgrey", 0xFFA9A9A9u }, { "lightgray", 0xFFD3D3D3u },
        { "lightgrey", 0xFFD3D3D3u },
        { "darkred", 0xFF8B0000u }, { "darkgreen", 0xFF006400u },
        { "darkblue", 0xFF00008Bu }, { "lightblue", 0xFFADD8E6u },
        { "lightgreen", 0xFF90EE90u }, { "cornflowerblue", 0xFF6495EDu },
        { "skyblue", 0xFF87CEEBu }, { "steelblue", 0xFF4682B4u },
        { "royalblue", 0xFF4169E1u }, { "midnightblue", 0xFF191970u },
        { "indigo", 0xFF4B0082u }, { "violet", 0xFFEE82EEu },
        { "salmon", 0xFFFA8072u }, { "tomato", 0xFFFF6347u },
        { "coral", 0xFFFF7F50u }, { "khaki", 0xFFF0E68Cu },
        { "beige", 0xFFF5F5DCu }, { "ivory", 0xFFFFFFF0u },
        { "linen", 0xFFFAF0E6u }, { "snow", 0xFFFFFAFAu },
        { "ghostwhite", 0xFFF8F8FFu }, { "whitesmoke", 0xFFF5F5F5u },
        { "lavender", 0xFFE6E6FAu }, { "thistle", 0xFFD8BFD8u },
        { "plum", 0xFFDDA0DDu }, { "orchid", 0xFFDA70D6u },
        { "magenta", 0xFFFF00FFu }, { "cyan", 0xFF00FFFFu },
        { "chocolate", 0xFFD2691Eu }, { "saddlebrown", 0xFF8B4513u },
        { "sienna", 0xFFA0522Du }, { "peru", 0xFFCD853Fu },
        { "tan", 0xFFD2B48Cu }, { "wheat", 0xFFF5DEB3u },
        { "rosybrown", 0xFFBC8F8Fu }, { "darkorange", 0xFFFF8C00u },
        { "deepskyblue", 0xFF00BFFFu }, { "dodgerblue", 0xFF1E90FFu },
        { "powderblue", 0xFFB0E0E6u }, { "paleturquoise", 0xFFAFEEEEu },
        { "turquoise", 0xFF40E0D0u }, { "darkcyan", 0xFF008B8Bu },
        { "mediumseagreen", 0xFF3CB371u }, { "seagreen", 0xFF2E8B57u },
        { "forestgreen", 0xFF228B22u }, { "limegreen", 0xFF32CD32u },
        { "yellowgreen", 0xFF9ACD32u }, { "olivedrab", 0xFF6B8E23u },
        { "darkkhaki", 0xFFBDB76Bu }, { "moccasin", 0xFFFFE4B5u },
        { "papayawhip", 0xFFFFEFD5u }, { "peachpuff", 0xFFFFDAB9u },
        { "mistyrose", 0xFFFFE4E1u }, { "lavenderblush", 0xFFFFF0F5u },
        { "aliceblue", 0xFFF0F8FFu }, { "azure", 0xFFF0FFFFu },
        { "honeydew", 0xFFF0FFF0u }, { "mintcream", 0xFFF5FFFAu },
        { "seashell", 0xFFFFF5EEu }, { "floralwhite", 0xFFFFFAF0u },
        { "antiquewhite", 0xFFFAEBD7u }, { "oldlace", 0xFFFDF5E6u },
        { "blanchedalmond", 0xFFFFEBCDu }, { "bisque", 0xFFFFE4C4u },
        { "navajowhite", 0xFFFFDEADu }, { "lightyellow", 0xFFFFFFE0u },
        { "lemonchiffon", 0xFFFFFACDu }, { "lightgoldenrodyellow", 0xFFFAFAD2u },
        { "cornsilk", 0xFFFFF8DCu },
    };
    *n_out = (int)(sizeof(tbl) / sizeof(tbl[0]));
    return tbl;
}

static inline int lay_parse_color(const char *src, layout_color_t *out)
{
    if (!src) return 0;
    /* skip leading ws */
    int i = 0;
    while (src[i] && lay_is_ws((unsigned char)src[i])) i++;

    /* hex */
    if (src[i] == '#') {
        i++;
        int start = i;
        int n = 0;
        while (lay_is_hex((unsigned char)src[i])) { i++; n++; }
        const char *h = src + start;
        unsigned int r, g, b, a = 0xFF;
        if (n == 3) {
            r = lay_hex_digit(h[0]); g = lay_hex_digit(h[1]); b = lay_hex_digit(h[2]);
            r |= r << 4; g |= g << 4; b |= b << 4;
        } else if (n == 4) {
            r = lay_hex_digit(h[0]); g = lay_hex_digit(h[1]); b = lay_hex_digit(h[2]);
            a = lay_hex_digit(h[3]);
            r |= r << 4; g |= g << 4; b |= b << 4; a |= a << 4;
        } else if (n == 6) {
            r = (lay_hex_digit(h[0]) << 4) | lay_hex_digit(h[1]);
            g = (lay_hex_digit(h[2]) << 4) | lay_hex_digit(h[3]);
            b = (lay_hex_digit(h[4]) << 4) | lay_hex_digit(h[5]);
        } else if (n == 8) {
            r = (lay_hex_digit(h[0]) << 4) | lay_hex_digit(h[1]);
            g = (lay_hex_digit(h[2]) << 4) | lay_hex_digit(h[3]);
            b = (lay_hex_digit(h[4]) << 4) | lay_hex_digit(h[5]);
            a = (lay_hex_digit(h[6]) << 4) | lay_hex_digit(h[7]);
        } else {
            return 0;
        }
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        return 1;
    }

    /* rgb() / rgba() */
    if ((src[i] == 'r' || src[i] == 'R') &&
        (src[i+1] == 'g' || src[i+1] == 'G') &&
        (src[i+2] == 'b' || src[i+2] == 'B')) {
        int j = i + 3;
        int has_a = 0;
        if (src[j] == 'a' || src[j] == 'A') { has_a = 1; j++; }
        if (src[j] != '(') return 0;
        j++;
        int comps[4] = { 0, 0, 0, 255 };
        int need = has_a ? 4 : 3;
        for (int k = 0; k < need; k++) {
            while (src[j] && lay_is_ws((unsigned char)src[j])) j++;
            if (k == 3) {
                /* alpha is 0..1 (decimal); if it looks like 0..255 we
                 * treat it as such only if it's > 1 with no decimal. */
                int centi = 0;
                if (!lay_parse_centi(src, lay_strlen(src), &j, &centi)) return 0;
                if (centi <= 100 && centi >= 0)
                    comps[k] = (centi * 255 + 50) / 100;
                else
                    comps[k] = centi / 100;
                if (comps[k] < 0) comps[k] = 0;
                if (comps[k] > 255) comps[k] = 255;
            } else {
                int v = 0;
                if (!lay_parse_int(src, lay_strlen(src), &j, &v)) return 0;
                /* tolerate `100%` syntax */
                if (src[j] == '%') { j++; v = (v * 255 + 50) / 100; }
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                comps[k] = v;
            }
            while (src[j] && lay_is_ws((unsigned char)src[j])) j++;
            if (k < need - 1) {
                if (src[j] != ',' && src[j] != ' ') return 0;
                if (src[j] == ',') j++;
            }
        }
        while (src[j] && lay_is_ws((unsigned char)src[j])) j++;
        if (src[j] != ')') return 0;
        *out = ((unsigned int)comps[3] << 24) |
               ((unsigned int)comps[0] << 16) |
               ((unsigned int)comps[1] << 8)  |
               ((unsigned int)comps[2]);
        return 1;
    }

    /* named */
    int n_named = 0;
    const struct lay_named_color *tbl = lay_named_colors(&n_named);
    /* extract the keyword (alpha + dash) */
    int start = i;
    while (src[i] && (((unsigned char)src[i] >= 'a' && (unsigned char)src[i] <= 'z') ||
                      ((unsigned char)src[i] >= 'A' && (unsigned char)src[i] <= 'Z') ||
                      src[i] == '-')) i++;
    int klen = i - start;
    if (klen == 0) return 0;
    for (int t = 0; t < n_named; t++) {
        const char *nm = tbl[t].name;
        int nl = lay_strlen(nm);
        if (nl != klen) continue;
        int ok = 1;
        for (int p = 0; p < klen; p++) {
            if (lay_lower((unsigned char)nm[p]) != lay_lower((unsigned char)src[start + p])) { ok = 0; break; }
        }
        if (ok) { *out = tbl[t].rgba; return 1; }
    }
    return 0;
}

/* Parse a CSS length (or `auto`).  Returns 1 on success.  Accepts:
 *     auto, 0, 12px, 1.5em, 0.5rem, 50%, 100, -3px
 * Bare numbers are interpreted as PX (the convention many real
 * sites rely on, even though the spec says no). */
static inline int lay_parse_length(const char *src, struct layout_length *out)
{
    if (!src) return 0;
    int i = 0;
    while (src[i] && lay_is_ws((unsigned char)src[i])) i++;
    if (lay_streq_n(src + i, "auto", 4) &&
        (src[i+4] == 0 || lay_is_ws((unsigned char)src[i+4]))) {
        out->unit = LAY_LEN_AUTO; out->v = 0; return 1;
    }
    int len = lay_strlen(src);
    int centi = 0;
    int p = i;
    if (!lay_parse_centi(src, len, &p, &centi)) return 0;
    /* unit suffix */
    while (src[p] && lay_is_ws((unsigned char)src[p])) p++;
    if (src[p] == '%') {
        /* % is in tenths */
        out->unit = LAY_LEN_PERCENT;
        out->v   = (centi * 10) / 100;     /* centi units -> tenths of % */
        return 1;
    }
    if (src[p] == 'p' && src[p+1] == 'x') {
        out->unit = LAY_LEN_PX;
        out->v   = centi / 100;
        return 1;
    }
    if (src[p] == 'e' && src[p+1] == 'm') {
        out->unit = LAY_LEN_EM;
        out->v   = centi;
        return 1;
    }
    if (src[p] == 'r' && src[p+1] == 'e' && src[p+2] == 'm') {
        out->unit = LAY_LEN_REM;
        out->v   = centi;
        return 1;
    }
    if (src[p] == 'p' && src[p+1] == 't') {
        /* 1pt = 1.333px (96/72).  Approximate. */
        out->unit = LAY_LEN_PX;
        out->v   = ((centi * 4) / 3) / 100;
        return 1;
    }
    /* unitless number — treat 0 specially, otherwise px */
    out->unit = LAY_LEN_PX;
    out->v   = centi / 100;
    return 1;
}

/* Resolve a length to integer pixels at use time.
 *   parent_dim_px = width of the containing block for percentages
 *                   (height for vertical %s).  Pass 0 to treat
 *                   percentages as 0 (e.g. when there is no
 *                   containing dimension yet).
 *   own_font_size_px = element's resolved font-size in px (for em).
 *   root_font_size_px = root element's resolved font-size (for rem).
 *   default_for_auto = the value to return if unit is AUTO.
 */
static inline int lay_resolve_length(struct layout_length len,
                                     int parent_dim_px,
                                     int own_font_size_px,
                                     int root_font_size_px,
                                     int default_for_auto)
{
    switch (len.unit) {
    case LAY_LEN_AUTO:    return default_for_auto;
    case LAY_LEN_PX:      return len.v;
    case LAY_LEN_PERCENT: return (parent_dim_px * len.v) / 1000;
    case LAY_LEN_EM:      return (own_font_size_px * len.v) / 100;
    case LAY_LEN_REM:     return (root_font_size_px * len.v) / 100;
    }
    return default_for_auto;
}

/* Map a `display` keyword onto an enum value. */
static inline int lay_parse_display(const char *s)
{
    if (lay_streq_ci(s, "inline"))         return LAY_DISPLAY_INLINE;
    if (lay_streq_ci(s, "block"))          return LAY_DISPLAY_BLOCK;
    if (lay_streq_ci(s, "inline-block"))   return LAY_DISPLAY_INLINE_BLOCK;
    if (lay_streq_ci(s, "list-item"))      return LAY_DISPLAY_LIST_ITEM;
    if (lay_streq_ci(s, "none"))           return LAY_DISPLAY_NONE;
    if (lay_streq_ci(s, "table"))          return LAY_DISPLAY_TABLE;
    if (lay_streq_ci(s, "table-row"))      return LAY_DISPLAY_TABLE_ROW;
    if (lay_streq_ci(s, "table-cell"))     return LAY_DISPLAY_TABLE_CELL;
    return -1;
}

static inline int lay_parse_text_align(const char *s)
{
    if (lay_streq_ci(s, "left"))    return LAY_TA_LEFT;
    if (lay_streq_ci(s, "center"))  return LAY_TA_CENTER;
    if (lay_streq_ci(s, "right"))   return LAY_TA_RIGHT;
    if (lay_streq_ci(s, "justify")) return LAY_TA_JUSTIFY;
    return -1;
}

static inline int lay_parse_font_weight(const char *s)
{
    if (lay_streq_ci(s, "normal"))  return LAY_FW_NORMAL;
    if (lay_streq_ci(s, "bold"))    return LAY_FW_BOLD;
    if (lay_streq_ci(s, "bolder"))  return LAY_FW_BOLD;
    if (lay_streq_ci(s, "lighter")) return LAY_FW_NORMAL;
    /* numeric */
    int p = 0; int v = 0;
    if (lay_parse_int(s, lay_strlen(s), &p, &v) && v >= 100 && v <= 900) {
        return v >= 600 ? LAY_FW_BOLD : LAY_FW_NORMAL;
    }
    return -1;
}

static inline int lay_parse_font_style(const char *s)
{
    if (lay_streq_ci(s, "normal"))  return LAY_FS_NORMAL;
    if (lay_streq_ci(s, "italic"))  return LAY_FS_ITALIC;
    if (lay_streq_ci(s, "oblique")) return LAY_FS_ITALIC;
    return -1;
}

static inline int lay_parse_text_decoration(const char *s)
{
    /* Accept multi-keyword: "underline overline" => mask. */
    int mask = 0;
    int i = 0;
    int n = lay_strlen(s);
    while (i < n) {
        while (i < n && lay_is_ws((unsigned char)s[i])) i++;
        int start = i;
        while (i < n && !lay_is_ws((unsigned char)s[i])) i++;
        if (i == start) break;
        int klen = i - start;
        if (klen == 4 && lay_streq_n(s + start, "none", 4)) { /* clears */ }
        else if (klen == 9 && lay_streq_n(s + start, "underline", 9)) mask |= LAY_TD_UNDERLINE;
        else if (klen == 12 && lay_streq_n(s + start, "line-through", 12)) mask |= LAY_TD_LINETHROUGH;
        else if (klen == 8 && lay_streq_n(s + start, "overline", 8)) mask |= LAY_TD_OVERLINE;
    }
    return mask;
}

static inline int lay_parse_white_space(const char *s)
{
    if (lay_streq_ci(s, "normal"))   return LAY_WS_NORMAL;
    if (lay_streq_ci(s, "pre"))      return LAY_WS_PRE;
    if (lay_streq_ci(s, "nowrap"))   return LAY_WS_NOWRAP;
    if (lay_streq_ci(s, "pre-wrap")) return LAY_WS_PRE_WRAP;
    return -1;
}

static inline int lay_parse_list_style(const char *s)
{
    if (lay_streq_ci(s, "none"))    return LAY_LS_NONE;
    if (lay_streq_ci(s, "disc"))    return LAY_LS_DISC;
    if (lay_streq_ci(s, "circle"))  return LAY_LS_CIRCLE;
    if (lay_streq_ci(s, "square"))  return LAY_LS_SQUARE;
    if (lay_streq_ci(s, "decimal")) return LAY_LS_DECIMAL;
    return -1;
}

/* Parse a CSS shorthand of N lengths (1, 2, 3, or 4) and write
 * them to out[4] in TRBL order following CSS rules:
 *   1 value:  all four
 *   2 values: top/bot=v0, left/right=v1
 *   3 values: top=v0, left/right=v1, bot=v2
 *   4 values: top, right, bot, left
 * Accepts "auto" for any side. */
static inline int lay_parse_trbl_lengths(const char *src, struct layout_length out[4])
{
    int n = 0;
    struct layout_length tmp[4];
    int i = 0;
    int slen = lay_strlen(src);
    while (i < slen && n < 4) {
        while (i < slen && lay_is_ws((unsigned char)src[i])) i++;
        if (i >= slen) break;
        int start = i;
        while (i < slen && !lay_is_ws((unsigned char)src[i])) i++;
        int tlen = i - start;
        if (tlen == 0) break;
        char buf[64];
        if (tlen >= (int)sizeof(buf)) tlen = (int)sizeof(buf) - 1;
        for (int k = 0; k < tlen; k++) buf[k] = src[start + k];
        buf[tlen] = 0;
        if (!lay_parse_length(buf, &tmp[n])) {
            tmp[n].unit = LAY_LEN_PX; tmp[n].v = 0;
        }
        n++;
    }
    if (n == 0) return 0;
    if (n == 1) { out[0] = out[1] = out[2] = out[3] = tmp[0]; return n; }
    if (n == 2) { out[0] = out[2] = tmp[0]; out[1] = out[3] = tmp[1]; return n; }
    if (n == 3) { out[0] = tmp[0]; out[1] = out[3] = tmp[1]; out[2] = tmp[2]; return n; }
    out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2]; out[3] = tmp[3];
    return n;
}

/* Parse a `border` shorthand: "1px solid red".  Recognises any
 * of (length, style, colour) in any order.  Stores width to
 * *out_width_px, colour to *out_color, and returns 1 if at
 * least width or colour was found AND a valid style keyword
 * appeared.  Per CSS 2.1 the `border-style` initial value is
 * `none`, so a shorthand without an explicit style keyword
 * (e.g. `border: 2px red`) is invalid and must be ignored —
 * real browsers drop the whole declaration in that case.
 * Style keyword `none|hidden` cause width=0; everything else
 * paints as a solid 1px+ rule (we don't model dash patterns). */
static inline int lay_parse_border_shorthand(const char *src,
                                              int *out_width_px,
                                              layout_color_t *out_color)
{
    int width_set = 0, color_set = 0, style_set = 0, paints = 1;
    int width = 1;
    layout_color_t col = LAY_COLOR_BLACK;
    int i = 0;
    int n = lay_strlen(src);
    while (i < n) {
        while (i < n && lay_is_ws((unsigned char)src[i])) i++;
        int start = i;
        /* allow paren tokens as one (rgb(...)) */
        int depth = 0;
        while (i < n) {
            char c = src[i];
            if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (depth == 0 && lay_is_ws((unsigned char)c)) break;
            i++;
        }
        int tlen = i - start;
        if (tlen == 0) break;
        char tok[96];
        if (tlen >= (int)sizeof(tok)) tlen = (int)sizeof(tok) - 1;
        for (int k = 0; k < tlen; k++) tok[k] = src[start + k];
        tok[tlen] = 0;

        /* style keyword? */
        if (lay_streq_ci(tok, "none") || lay_streq_ci(tok, "hidden")) {
            width_set = 1; width = 0; style_set = 1; paints = 0;
            continue;
        }
        if (lay_streq_ci(tok, "solid") || lay_streq_ci(tok, "dashed") ||
            lay_streq_ci(tok, "dotted") || lay_streq_ci(tok, "double") ||
            lay_streq_ci(tok, "groove") || lay_streq_ci(tok, "ridge") ||
            lay_streq_ci(tok, "inset")  || lay_streq_ci(tok, "outset")) {
            style_set = 1;
            continue;
        }
        /* try colour first */
        layout_color_t c = 0;
        if (lay_parse_color(tok, &c)) { col = c; color_set = 1; continue; }
        /* try length */
        struct layout_length L;
        if (lay_parse_length(tok, &L)) {
            if (L.unit == LAY_LEN_PX) { width = L.v; width_set = 1; continue; }
        }
    }
    /* Without a valid style keyword the shorthand is invalid CSS
     * and must be discarded (per CSS 2.1 §8.5.4).  Pages that
     * write `border: 2px red` (real example: plaintextworld.com
     * uses `border: 2px red;` with no style keyword) get NO
     * border rendered, matching real browsers. */
    if (!style_set) return 0;
    if (!paints) {
        /* `none|hidden`: write width=0 so the box has no border. */
        if (out_width_px) *out_width_px = 0;
        return 1;
    }
    if (width_set) *out_width_px = width;
    if (color_set) *out_color    = col;
    return 1;
}

/* ============================================================
 *   PART 4 — UA stylesheet + cascade engine
 * ============================================================ */

/* The user-agent stylesheet.  Anything CSS-like that a default
 * browser does for a bare HTML element should appear here.  Real
 * browsers ship hundreds of lines (focus rings, form controls,
 * <details> open state, etc.); we cover the common element set
 * plus a few aesthetic defaults so untyped pages don't render as
 * a wall of unstyled text. */
static const char *layout_ua_stylesheet =
    "html { display: block; }\n"
    "head { display: none; }\n"
    "body { display: block; margin: 8px; color: black; "
    "       background: white; font-size: 16px; line-height: 1.4em; "
    "       font-family: serif; }\n"
    "div, section, article, main, header, footer, nav, aside, figure, "
    "figcaption, address, blockquote, dl, dd, fieldset, form, hr, "
    "noscript, pre { display: block; }\n"
    "p   { display: block; margin-top: 1em; margin-bottom: 1em; }\n"
    "h1  { display: block; font-size: 32px; font-weight: bold; "
    "      margin-top: 21px; margin-bottom: 21px; }\n"
    "h2  { display: block; font-size: 24px; font-weight: bold; "
    "      margin-top: 19px; margin-bottom: 19px; }\n"
    "h3  { display: block; font-size: 19px; font-weight: bold; "
    "      margin-top: 18px; margin-bottom: 18px; }\n"
    "h4  { display: block; font-size: 16px; font-weight: bold; "
    "      margin-top: 21px; margin-bottom: 21px; }\n"
    "h5  { display: block; font-size: 13px; font-weight: bold; "
    "      margin-top: 22px; margin-bottom: 22px; }\n"
    "h6  { display: block; font-size: 11px; font-weight: bold; "
    "      margin-top: 24px; margin-bottom: 24px; }\n"
    "ul, ol { display: block; margin-top: 1em; margin-bottom: 1em; "
    "         padding-left: 32px; }\n"
    "ul  { list-style: disc; }\n"
    "ol  { list-style: decimal; }\n"
    "li  { display: list-item; }\n"
    "dt  { display: block; font-weight: bold; }\n"
    "dd  { display: block; padding-left: 32px; }\n"
    "blockquote { display: block; margin-top: 1em; margin-bottom: 1em; "
    "             padding-left: 32px; padding-right: 32px; }\n"
    "pre  { display: block; white-space: pre; font-family: monospace; "
    "       margin-top: 1em; margin-bottom: 1em; }\n"
    "hr   { display: block; margin-top: 8px; margin-bottom: 8px; "
    "       border-top: 1px solid #888888; }\n"
    /* table / tr / td reset `text-align: left` so a parent's
     * `text-align: center` doesn't accidentally cascade through
     * the table and centre every cell's text.  This matches our
     * cell model: cells render as full-width blocks (not narrow
     * columns), so centring the parent shouldn't drag along link
     * lists, paragraphs, etc.  Pages that DO need cell content
     * centred (e.g. plaintextworld.com's `<h2>` title inside
     * `<div align="center"><table>...`) flow through the
     * `text_align_center_inherit` flag: `<center>` / `<div
     * align=center>` set that flag, it cascades down via
     * `layout_computed_inherit`, and the post-cascade pass in
     * `layout_build_subtree` re-asserts `text-align: center` on
     * `<table>/<tr>/<td>/<th>` whose resolved text-align is the
     * UA-default LEFT.  This mirrors `text-align: -webkit-center`
     * in real browsers without breaking ordinary tables. */
    "table { display: block; text-align: left; }\n"
    "tr   { display: table-row; text-align: left; }\n"
    "td, th { display: block; padding-left: 4px; padding-right: 4px; "
    "         text-align: left; vertical-align: top; }\n"
    "th   { font-weight: bold; }\n"
    "center { display: block; text-align: center; }\n"
    "dir, menu { display: block; margin-top: 1em; margin-bottom: 1em; "
    "            padding-left: 32px; }\n"
    "marquee { display: inline-block; }\n"
    "code, kbd, samp, tt { font-family: monospace; }\n"
    "b, strong { font-weight: bold; }\n"
    "i, em, cite, var { font-style: italic; }\n"
    "u, ins { text-decoration: underline; }\n"
    "s, strike, del { text-decoration: line-through; }\n"
    "small { font-size: 13px; }\n"
    "big   { font-size: 19px; }\n"
    "a    { color: #0050C0; text-decoration: underline; }\n"
    "img  { display: inline-block; }\n"
    "br   { display: inline; }\n"
    "script, style, title, meta, link { display: none; }\n"
    "input, button, textarea, select { display: inline-block; }\n";

/* User override sheet, applied at LAY_ORIG_USER (above author CSS).
 * This is where we put back UA defaults that real-world pages tend
 * to remove via author CSS but we (subjectively) want to keep.
 *
 * The current contents:
 *   - Force link underlines back on.  Without this, sites like
 *     Hacker News (which set `a:link { text-decoration: none; }`)
 *     render links indistinguishable from surrounding text in our
 *     limited renderer (we can't show hover or pointer cues).  The
 *     UA `a { ... underline }` rule has the same specificity as
 *     HN's `a:link` and loses on origin order, so we re-assert it
 *     here at user-override priority. */
static const char *layout_user_override_stylesheet =
    "a, a:link, a:visited { text-decoration: underline; }\n";

/* Initial / inherited values.  These match CSS 2.1 initial values
 * for properties we recognise. */
static inline void layout_computed_init(struct layout_computed *c)
{
    c->display          = LAY_DISPLAY_INLINE;
    c->font_weight      = LAY_FW_NORMAL;
    c->font_style       = LAY_FS_NORMAL;
    c->font_size_px     = 16;
    c->text_align       = LAY_TA_LEFT;
    c->text_decoration  = LAY_TD_NONE;
    c->white_space      = LAY_WS_NORMAL;
    c->list_style       = LAY_LS_DISC;
    c->line_height_px   = 0;        /* 0 = "use 1.2 * font_size" at use time */
    c->width.unit       = LAY_LEN_AUTO; c->width.v = 0;
    c->height.unit      = LAY_LEN_AUTO; c->height.v = 0;
    for (int i = 0; i < 4; i++) {
        c->margin[i].unit  = LAY_LEN_PX; c->margin[i].v  = 0;
        c->padding[i].unit = LAY_LEN_PX; c->padding[i].v = 0;
        c->border_px[i] = 0;
        c->border_color[i] = LAY_COLOR_BLACK;
    }
    c->color      = LAY_COLOR_BLACK;
    c->background = LAY_COLOR_TRANSPARENT;
    c->center_block_children = 0;
    c->text_align_center_inherit = 0;
}

/* Inherit the inheritable properties from `parent` onto `child`.
 * Per CSS 2.1, the inherited set is: color, font-*, line-height,
 * text-align, text-decoration (treated as inherited for our
 * subset; real CSS makes only some sub-properties inherited),
 * visibility, white-space, list-style, cursor, direction,
 * border-collapse, etc.  We inherit the ones we model. */
static inline void layout_computed_inherit(struct layout_computed *child,
                                            const struct layout_computed *parent)
{
    if (!parent) return;
    child->color           = parent->color;
    child->font_weight     = parent->font_weight;
    child->font_style      = parent->font_style;
    child->font_size_px    = parent->font_size_px;
    child->text_align      = parent->text_align;
    child->text_decoration = parent->text_decoration;
    child->white_space     = parent->white_space;
    child->list_style      = parent->list_style;
    child->line_height_px  = parent->line_height_px;
    /* `text_align_center_inherit` is the inheritable half of our
     * `-webkit-center` model — once set on a `<center>` or
     * `<div align=center>` ancestor, every descendant carries the
     * flag.  See the field's documentation in struct
     * layout_computed and the post-cascade re-assertion in
     * layout_build_subtree for how it gets consumed. */
    child->text_align_center_inherit = parent->text_align_center_inherit;
    /* `background` is NOT inherited; it stays transparent so layered
     * boxes paint correctly. */
}

/* Apply ONE declaration (property + value string) to a computed
 * style.  Containing-block dimensions for percent resolution are
 * passed in so values stored as PERCENT/EM/REM that are eligible
 * to resolve at cascade time can do so.  Properties that need
 * deferred resolution (margin, padding, width, height) are stored
 * in their original units so the layout pass can re-resolve.
 *
 * Returns 1 if the declaration was understood, 0 if the property
 * was ignored. */
static inline int layout_apply_decl(struct layout_computed *c,
                                     const char *prop,
                                     const char *value,
                                     int parent_font_size_px,
                                     int root_font_size_px)
{
    if (!prop || !value) return 0;

    /* font-size is special — must resolve early so that em / line-
     * height multipliers downstream see the right pixel value. */
    if (lay_streq_ci(prop, "font-size")) {
        struct layout_length L;
        if (!lay_parse_length(value, &L)) return 0;
        int px = lay_resolve_length(L, parent_font_size_px,
                                    parent_font_size_px,
                                    root_font_size_px,
                                    parent_font_size_px);
        if (px <= 0) px = 1;
        c->font_size_px = px;
        return 1;
    }
    if (lay_streq_ci(prop, "color")) {
        layout_color_t col;
        if (lay_parse_color(value, &col)) { c->color = col; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "background") ||
        lay_streq_ci(prop, "background-color")) {
        layout_color_t col;
        if (lay_parse_color(value, &col)) { c->background = col; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "display")) {
        int v = lay_parse_display(value);
        if (v >= 0) { c->display = v; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "font-weight")) {
        int v = lay_parse_font_weight(value);
        if (v >= 0) { c->font_weight = (enum layout_font_weight)v; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "font-style")) {
        int v = lay_parse_font_style(value);
        if (v >= 0) { c->font_style = v; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "text-align")) {
        int v = lay_parse_text_align(value);
        if (v >= 0) { c->text_align = v; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "text-decoration") ||
        lay_streq_ci(prop, "text-decoration-line")) {
        c->text_decoration = lay_parse_text_decoration(value);
        return 1;
    }
    if (lay_streq_ci(prop, "white-space")) {
        int v = lay_parse_white_space(value);
        if (v >= 0) { c->white_space = v; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "list-style") ||
        lay_streq_ci(prop, "list-style-type")) {
        int v = lay_parse_list_style(value);
        if (v >= 0) { c->list_style = v; return 1; }
        return 0;
    }
    if (lay_streq_ci(prop, "line-height")) {
        /* Number → multiplier of font_size.  Length → fixed.
         * Percent → fraction of font_size. */
        int slen = lay_strlen(value);
        int p = 0;
        int centi = 0;
        int npos = p;
        if (lay_parse_centi(value, slen, &p, &centi)) {
            while (p < slen && lay_is_ws((unsigned char)value[p])) p++;
            if (p == slen) {
                /* unitless multiplier */
                c->line_height_px = (c->font_size_px * centi) / 100;
                return 1;
            }
            (void)npos;
        }
        struct layout_length L;
        if (!lay_parse_length(value, &L)) return 0;
        int px = lay_resolve_length(L, c->font_size_px,
                                    c->font_size_px,
                                    root_font_size_px,
                                    (c->font_size_px * 12) / 10);
        if (px <= 0) px = c->font_size_px;
        c->line_height_px = px;
        return 1;
    }

    /* width / height */
    if (lay_streq_ci(prop, "width"))  { return lay_parse_length(value, &c->width); }
    if (lay_streq_ci(prop, "height")) { return lay_parse_length(value, &c->height); }

    /* margin / padding shorthands + per-side */
    if (lay_streq_ci(prop, "margin"))  return lay_parse_trbl_lengths(value, c->margin)  > 0;
    if (lay_streq_ci(prop, "padding")) return lay_parse_trbl_lengths(value, c->padding) > 0;

    if (lay_streq_ci(prop, "margin-top"))    return lay_parse_length(value, &c->margin[LAY_TOP]);
    if (lay_streq_ci(prop, "margin-right"))  return lay_parse_length(value, &c->margin[LAY_RIGHT]);
    if (lay_streq_ci(prop, "margin-bottom")) return lay_parse_length(value, &c->margin[LAY_BOT]);
    if (lay_streq_ci(prop, "margin-left"))   return lay_parse_length(value, &c->margin[LAY_LEFT]);

    if (lay_streq_ci(prop, "padding-top"))    return lay_parse_length(value, &c->padding[LAY_TOP]);
    if (lay_streq_ci(prop, "padding-right"))  return lay_parse_length(value, &c->padding[LAY_RIGHT]);
    if (lay_streq_ci(prop, "padding-bottom")) return lay_parse_length(value, &c->padding[LAY_BOT]);
    if (lay_streq_ci(prop, "padding-left"))   return lay_parse_length(value, &c->padding[LAY_LEFT]);

    /* border shorthand */
    if (lay_streq_ci(prop, "border")) {
        int w = 0; layout_color_t col = LAY_COLOR_BLACK;
        if (lay_parse_border_shorthand(value, &w, &col)) {
            for (int i = 0; i < 4; i++) {
                c->border_px[i] = w;
                c->border_color[i] = col;
            }
            return 1;
        }
        return 0;
    }
    if (lay_streq_ci(prop, "border-top")    || lay_streq_ci(prop, "border-right") ||
        lay_streq_ci(prop, "border-bottom") || lay_streq_ci(prop, "border-left")) {
        int side = lay_streq_ci(prop, "border-top") ? LAY_TOP :
                   lay_streq_ci(prop, "border-right") ? LAY_RIGHT :
                   lay_streq_ci(prop, "border-bottom") ? LAY_BOT : LAY_LEFT;
        int w = 0; layout_color_t col = LAY_COLOR_BLACK;
        if (lay_parse_border_shorthand(value, &w, &col)) {
            c->border_px[side] = w;
            c->border_color[side] = col;
            return 1;
        }
        return 0;
    }
    if (lay_streq_ci(prop, "border-width")) {
        struct layout_length tmp[4];
        if (lay_parse_trbl_lengths(value, tmp) <= 0) return 0;
        for (int i = 0; i < 4; i++) c->border_px[i] = tmp[i].v;
        return 1;
    }
    if (lay_streq_ci(prop, "border-color")) {
        layout_color_t col;
        if (!lay_parse_color(value, &col)) return 0;
        for (int i = 0; i < 4; i++) c->border_color[i] = col;
        return 1;
    }
    if (lay_streq_ci(prop, "border-top-width"))    { struct layout_length L; if (!lay_parse_length(value,&L)) return 0; c->border_px[LAY_TOP]=L.v;    return 1; }
    if (lay_streq_ci(prop, "border-right-width"))  { struct layout_length L; if (!lay_parse_length(value,&L)) return 0; c->border_px[LAY_RIGHT]=L.v;  return 1; }
    if (lay_streq_ci(prop, "border-bottom-width")) { struct layout_length L; if (!lay_parse_length(value,&L)) return 0; c->border_px[LAY_BOT]=L.v;    return 1; }
    if (lay_streq_ci(prop, "border-left-width"))   { struct layout_length L; if (!lay_parse_length(value,&L)) return 0; c->border_px[LAY_LEFT]=L.v;   return 1; }

    /* font shorthand: a tiny subset — we accept "<weight>? <size>
     * <family>" but only honour weight + size; family is ignored
     * because we have no font fallback. */
    if (lay_streq_ci(prop, "font")) {
        int slen = lay_strlen(value);
        int i = 0;
        int got_anything = 0;
        while (i < slen) {
            while (i < slen && lay_is_ws((unsigned char)value[i])) i++;
            int start = i;
            while (i < slen && !lay_is_ws((unsigned char)value[i])) i++;
            int tlen = i - start;
            if (tlen == 0) break;
            char tok[64];
            if (tlen >= (int)sizeof(tok)) tlen = (int)sizeof(tok) - 1;
            for (int k = 0; k < tlen; k++) tok[k] = value[start + k];
            tok[tlen] = 0;
            int w = lay_parse_font_weight(tok);
            if (w >= 0) { c->font_weight = (enum layout_font_weight)w; got_anything = 1; continue; }
            int st = lay_parse_font_style(tok);
            if (st >= 0) { c->font_style = st; got_anything = 1; continue; }
            struct layout_length L;
            if (lay_parse_length(tok, &L)) {
                int px = lay_resolve_length(L, parent_font_size_px,
                                             parent_font_size_px,
                                             root_font_size_px,
                                             parent_font_size_px);
                if (px > 0) { c->font_size_px = px; got_anything = 1; continue; }
            }
            /* anything else is treated as font-family (silently ignored) */
            got_anything = 1;
            break;
        }
        return got_anything;
    }

    /* Properties we recognise but ignore (they parse but have no
     * effect in our renderer — no warning). */
    if (lay_streq_ci(prop, "font-family") ||
        lay_streq_ci(prop, "cursor") ||
        lay_streq_ci(prop, "outline") || lay_streq_ci(prop, "outline-color") ||
        lay_streq_ci(prop, "outline-width") ||
        lay_streq_ci(prop, "box-sizing") ||
        lay_streq_ci(prop, "position") ||
        lay_streq_ci(prop, "float") || lay_streq_ci(prop, "clear") ||
        lay_streq_ci(prop, "overflow") || lay_streq_ci(prop, "overflow-x") ||
        lay_streq_ci(prop, "overflow-y") ||
        lay_streq_ci(prop, "z-index") ||
        lay_streq_ci(prop, "visibility") ||
        lay_streq_ci(prop, "vertical-align") ||
        lay_streq_ci(prop, "border-radius") ||
        lay_streq_ci(prop, "border-collapse") || lay_streq_ci(prop, "border-spacing") ||
        lay_streq_ci(prop, "min-width") || lay_streq_ci(prop, "max-width") ||
        lay_streq_ci(prop, "min-height") || lay_streq_ci(prop, "max-height") ||
        lay_streq_ci(prop, "opacity") ||
        lay_streq_ci(prop, "transition") || lay_streq_ci(prop, "transform")) {
        return 1;   /* understood but no-op */
    }
    return 0;
}

/* The cascade engine.  Stores parsed sheets, sorted on demand for
 * each resolve call.  Three origins: UA (lowest priority), AUTHOR
 * (the page's stylesheets), USER (our own override sheet —
 * conceptually the OS/user agent's user prefs, sits ABOVE author
 * CSS so we can put back UA defaults the page tried to remove),
 * INLINE (style="..." attribute, highest priority).  Within an
 * origin, ties broken by (specificity desc, source_order asc). */
enum layout_origin {
    LAY_ORIG_UA     = 0,
    LAY_ORIG_AUTHOR = 1,
    LAY_ORIG_USER   = 2,
    LAY_ORIG_INLINE = 3,
};

struct layout_engine_sheet {
    struct css_stylesheet      *ss;
    int                         origin;
    int                         owns;       /* 1 => engine frees on destroy */
    struct layout_engine_sheet *next;
};

struct layout_engine {
    struct layout_engine_sheet *sheets;
    int                         root_font_size_px;
};

static inline void layout_engine_init(struct layout_engine *e)
{
    e->sheets = 0;
    e->root_font_size_px = 16;
}

static inline void layout_engine_destroy(struct layout_engine *e)
{
    struct layout_engine_sheet *s = e->sheets;
    while (s) {
        struct layout_engine_sheet *n = s->next;
        if (s->owns && s->ss) { css_destroy(s->ss); free(s->ss); }
        free(s);
        s = n;
    }
    e->sheets = 0;
}

/* Take ownership of a parsed stylesheet at the given origin.
 * `owns` controls whether the engine frees it on destroy. */
static inline int layout_engine_add(struct layout_engine *e,
                                     struct css_stylesheet *ss,
                                     int origin, int owns)
{
    struct layout_engine_sheet *node = (struct layout_engine_sheet *)malloc(sizeof(*node));
    if (!node) return -1;
    node->ss = ss; node->origin = origin; node->owns = owns;
    /* append in order so iteration over sheets visits them in
     * cascade order (UA before AUTHOR before INLINE). */
    node->next = 0;
    if (!e->sheets) { e->sheets = node; return 0; }
    struct layout_engine_sheet *p = e->sheets;
    while (p->next) p = p->next;
    p->next = node;
    return 0;
}

/* A matching rule: pointer + per-chain specificity + origin + order.
 * The cascade picks the highest (origin, specificity, order) tuple
 * for each property. */
struct layout_match {
    struct css_rule *rule;
    struct css_selector *sel;     /* the matched chain (for spec) */
    int specificity;
    int origin;
    int source_order;
};

static inline int layout_match_cmp(const struct layout_match *a,
                                    const struct layout_match *b)
{
    /* ascending order: less-priority first.  origin asc, then
     * specificity asc, then source_order asc. */
    if (a->origin     != b->origin)     return a->origin     - b->origin;
    if (a->specificity != b->specificity) return a->specificity - b->specificity;
    return a->source_order - b->source_order;
}

/* Resolve the computed style of `node` from its parent's computed
 * style + the engine's stylesheets + an optional inline style
 * string.  `out` is initialised inside the function (caller does
 * not need to pre-init).
 *
 * Returns 1 on success.  Returns 0 if `node` is not an ELEMENT
 * (caller should not call this for text/comment nodes; they
 * inherit directly via layout_computed_inherit). */
static inline int layout_resolve(const struct layout_engine *e,
                                  const struct dom_node *node,
                                  const struct layout_computed *parent,
                                  const char *inline_css,
                                  struct layout_computed *out)
{
    if (!node || node->type != DOM_NODE_ELEMENT) return 0;

    /* Initial values + inheritance */
    layout_computed_init(out);
    layout_computed_inherit(out, parent);

    /* Collect matching rules across all sheets in cascade order. */
    enum { CAP = 256 };
    struct layout_match matches[CAP];
    int n = 0;
    for (struct layout_engine_sheet *s = e->sheets; s; s = s->next) {
        if (!s->ss) continue;
        for (struct css_rule *r = s->ss->rules; r; r = r->next) {
            for (struct css_selector *sel = r->selectors; sel; sel = sel->next) {
                if (!css_match_chain(sel->chain, node)) continue;
                if (n >= CAP) break;
                matches[n].rule        = r;
                matches[n].sel         = sel;
                matches[n].specificity = css_chain_specificity(sel->chain);
                matches[n].origin      = s->origin;
                matches[n].source_order = r->source_order;
                n++;
                break;  /* one chain match per rule is enough */
            }
        }
    }

    /* Sort ascending: bubble sort is fine for n < 256, n is usually
     * tiny per element, and we avoid pulling in qsort. */
    for (int i = 1; i < n; i++) {
        struct layout_match v = matches[i];
        int j = i - 1;
        while (j >= 0 && layout_match_cmp(&matches[j], &v) > 0) {
            matches[j + 1] = matches[j]; j--;
        }
        matches[j + 1] = v;
    }

    int parent_fs = parent ? parent->font_size_px : 16;
    int root_fs   = e->root_font_size_px;

    /* Apply in ascending priority so later wins. */
    for (int i = 0; i < n; i++) {
        struct css_rule *r = matches[i].rule;
        for (struct css_decl *d = r->decls; d; d = d->next)
            layout_apply_decl(out, d->property, d->value,
                              parent_fs, root_fs);
    }

    /* Inline style="..." attribute beats everything from sheets. */
    if (inline_css && inline_css[0]) {
        /* parse declarations from a synthetic block */
        int len = lay_strlen(inline_css);
        int p = 0;
        while (p < len) {
            while (p < len && lay_is_ws((unsigned char)inline_css[p])) p++;
            int prop_start = p;
            while (p < len && inline_css[p] != ':' && inline_css[p] != ';') p++;
            int prop_end = p;
            if (p >= len || inline_css[p] != ':') {
                /* recover: skip to next ; */
                while (p < len && inline_css[p] != ';') p++;
                if (p < len) p++;
                continue;
            }
            p++;       /* consume ':' */
            while (p < len && lay_is_ws((unsigned char)inline_css[p])) p++;
            int val_start = p;
            while (p < len && inline_css[p] != ';') p++;
            int val_end = p;
            if (p < len) p++;   /* consume ';' */
            /* trim trailing ws on prop and value */
            while (prop_end > prop_start &&
                   lay_is_ws((unsigned char)inline_css[prop_end - 1])) prop_end--;
            while (val_end > val_start &&
                   lay_is_ws((unsigned char)inline_css[val_end - 1])) val_end--;
            if (prop_end <= prop_start || val_end <= val_start) continue;
            char prop[64], val[256];
            int pl = prop_end - prop_start;
            int vl = val_end - val_start;
            if (pl >= (int)sizeof(prop)) pl = (int)sizeof(prop) - 1;
            if (vl >= (int)sizeof(val))  vl = (int)sizeof(val) - 1;
            for (int k = 0; k < pl; k++) prop[k] = lay_lower((unsigned char)inline_css[prop_start + k]);
            prop[pl] = 0;
            for (int k = 0; k < vl; k++) val[k] = inline_css[val_start + k];
            val[vl] = 0;
            layout_apply_decl(out, prop, val, parent_fs, root_fs);
        }
    }
    return 1;
}

/* ============================================================
 *   PART 5 — box tree
 * ============================================================ */

/* The box tree is the layout engine's working representation.
 * It mirrors the DOM but with the following transformations:
 *
 *   - `display: none` subtrees are pruned entirely.
 *   - DOM TEXT nodes become BOX_TEXT boxes with the parent's
 *     inherited computed style.  Pure-whitespace text between
 *     block siblings is dropped; whitespace inside an inline
 *     formatting context is collapsed by the inline pass.
 *   - When a block-display element has a mix of block and inline
 *     children, runs of inline children are wrapped in
 *     BOX_ANON_BLOCK boxes so the block layout pass only ever
 *     sees blocks.
 *   - `<li>` elements with `display: list-item` get a synthetic
 *     BULLET text box (currently "• " for disc/circle/square,
 *     "1. " for decimal — number tracking is per-parent).
 *   - `<br>` becomes a forced line-break in inline context.
 *   - Replaced elements (<img>) become BOX_REPLACED with width
 *     and height read from HTML attributes (no intrinsic image
 *     decoding).
 *
 * Each box owns its computed-style copy; text boxes share the
 * style with the element box they belong to (pointer copy, never
 * freed twice). */

enum layout_box_kind {
    LAY_BOX_BLOCK      = 1,    /* element with display:block / list-item */
    LAY_BOX_INLINE     = 2,    /* element with display:inline / inline-block */
    LAY_BOX_TEXT       = 3,    /* DOM text node, content in `text` */
    LAY_BOX_ANON_BLOCK = 4,    /* anonymous wrapper around inline run */
    LAY_BOX_REPLACED   = 5,    /* <img> placeholder */
    LAY_BOX_BR         = 6,    /* hard line break */
    LAY_BOX_BULLET     = 7,    /* synthetic list marker */
};

struct layout_box {
    int                     kind;
    const struct dom_node  *dom;        /* may be NULL for anon */
    struct layout_computed *style;      /* may be NULL for synthetic */
    char                   *text;       /* TEXT/BULLET only; malloc'd */
    int                     text_len;

    /* Layout output (pixel coords, document space) */
    int x, y, w, h;

    /* For replaced elements only */
    int replaced_w, replaced_h;
    char *replaced_alt;          /* malloc'd, NUL-term (for <img alt>) */

    /* For replaced elements that have decoded pixels: a borrowed
     * pointer to the decoded BGRA buffer (B,G,R,A in increasing
     * byte order, 4 bytes per pixel, tightly packed).  The pixels
     * live in the BROWSER's image cache, NOT in the layout box —
     * so layout_box_free_recursive must NOT free them.  When this
     * is non-NULL the paint emitter generates a LAY_PAINT_IMAGE
     * command instead of the placeholder grey rectangle.
     * `replaced_pixels_w/h` are the PNG's intrinsic dimensions, not
     * necessarily the same as the laid-out `w`/`h` (which may have
     * been scaled up to honour an explicit width/height attr; the
     * first cut just blits at intrinsic size and clips). */
    uint8_t *replaced_pixels;
    int      replaced_pixels_w, replaced_pixels_h;

    /* Tree links */
    struct layout_box *parent;
    struct layout_box *first_child;
    struct layout_box *last_child;
    struct layout_box *next_sibling;
};

static inline struct layout_box *layout_box_new(int kind)
{
    struct layout_box *b = (struct layout_box *)malloc(sizeof(*b));
    if (!b) return 0;
    b->kind = kind;
    b->dom = 0; b->style = 0;
    b->text = 0; b->text_len = 0;
    b->x = b->y = b->w = b->h = 0;
    b->replaced_w = b->replaced_h = 0; b->replaced_alt = 0;
    b->replaced_pixels = 0; b->replaced_pixels_w = b->replaced_pixels_h = 0;
    b->parent = b->first_child = b->last_child = b->next_sibling = 0;
    return b;
}

static inline void layout_box_append(struct layout_box *parent, struct layout_box *child)
{
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = 0;
    if (!parent->first_child) parent->first_child = child;
    else                       parent->last_child->next_sibling = child;
    parent->last_child = child;
}

/* The owning context for a layout pass.  Holds:
 *   - the cascade engine
 *   - the root style (initial values, used as the parent of <html>)
 *   - per-element computed style storage so layout passes can
 *     reach back into the DOM->style mapping cheaply
 *
 * The box tree is owned by `root_box`; layout_destroy walks it. */
struct layout_doc {
    struct layout_engine    engine;
    struct layout_computed  root_style;     /* implicit grand-parent */

    struct layout_box      *root_box;       /* anonymous BLOCK wrapping <html> */

    /* All malloc'd computed styles, kept in a flat list so destroy
     * can free them in one pass without walking the tree. */
    struct layout_style_node {
        struct layout_computed       *style;
        struct layout_style_node     *next;
    } *style_list;

    int viewport_px;        /* available width for the document */
    int doc_width_px;
    int doc_height_px;
};

static inline struct layout_computed *layout_doc_alloc_style(struct layout_doc *d)
{
    struct layout_computed *c = (struct layout_computed *)malloc(sizeof(*c));
    if (!c) return 0;
    layout_computed_init(c);
    struct layout_style_node *n = (struct layout_style_node *)malloc(sizeof(*n));
    if (!n) { free(c); return 0; }
    n->style = c; n->next = d->style_list; d->style_list = n;
    return c;
}

static inline void layout_doc_init(struct layout_doc *d, int viewport_px)
{
    layout_engine_init(&d->engine);
    layout_computed_init(&d->root_style);
    d->root_box = 0;
    d->style_list = 0;
    d->viewport_px = viewport_px;
    d->doc_width_px = 0;
    d->doc_height_px = 0;
}

static inline void layout_box_free_recursive(struct layout_box *b)
{
    if (!b) return;
    struct layout_box *c = b->first_child;
    while (c) {
        struct layout_box *n = c->next_sibling;
        layout_box_free_recursive(c);
        c = n;
    }
    /* Defensive: a pointer below the first 64 KiB cannot possibly be
     * a valid heap allocation (the user heap lives well above this).
     * Skip the free instead of crashing — paranoia in case a future
     * change reintroduces a use-after-free pattern like the one
     * fixed in M63 (REPLACED items dangling after pre-loop cleanup
     * in layout_inline_format). */
    if (b->text && (uintptr_t)b->text >= 0x10000u) free(b->text);
    if (b->replaced_alt && (uintptr_t)b->replaced_alt >= 0x10000u)
        free(b->replaced_alt);
    free(b);
}

static inline void layout_doc_destroy(struct layout_doc *d)
{
    layout_box_free_recursive(d->root_box);
    d->root_box = 0;
    /* Free all computed styles. */
    struct layout_style_node *n = d->style_list;
    while (n) {
        struct layout_style_node *nx = n->next;
        free(n->style); free(n);
        n = nx;
    }
    d->style_list = 0;
    layout_engine_destroy(&d->engine);
}

/* Whitespace-collapse a DOM text payload into a freshly-malloc'd
 * buffer.  Multiple ws-runs collapse to one space; leading and
 * trailing ws are stripped UNLESS preserve_ws is set.  Returns
 * NULL if nothing material remains (so the caller can drop the
 * box).  preserve_ws keeps newlines + tabs verbatim. */
static inline char *layout_collapse_text(const char *src, int len, int preserve_ws,
                                          int *out_len)
{
    if (!src || len <= 0) { if (out_len) *out_len = 0; return 0; }
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) { if (out_len) *out_len = 0; return 0; }
    int j = 0;
    if (preserve_ws) {
        for (int i = 0; i < len; i++) out[j++] = src[i];
    } else {
        /* CSS WS collapsing: runs of whitespace collapse to one
         * space, but we MUST keep leading/trailing space because
         * inline siblings (e.g. `<a>new</a> | <a>past</a>`) rely
         * on the boundary whitespace to separate words.  The
         * inline formatter coalesces adjacent SPACE items at line
         * boundaries, so leaving them here doesn't hurt. */
        int last_was_ws = 0;
        for (int i = 0; i < len; i++) {
            char c = src[i];
            int ws = lay_is_ws((unsigned char)c);
            if (ws) {
                if (!last_was_ws) out[j++] = ' ';
                last_was_ws = 1;
            } else {
                out[j++] = c;
                last_was_ws = 0;
            }
        }
    }
    out[j] = 0;
    if (j == 0) { free(out); if (out_len) *out_len = 0; return 0; }
    /* If the only content is whitespace, drop it — pure-WS text
     * between block siblings is collapsed away by emit_children. */
    int all_ws = 1;
    for (int i = 0; i < j; i++) if (!lay_is_ws((unsigned char)out[i])) { all_ws = 0; break; }
    if (all_ws && j == 1) {
        /* a lone collapsed space is meaningful (it separates two
         * inline siblings); keep it. */
    }
    if (out_len) *out_len = j;
    return out;
}

/* Forward declaration for build recursion. */
static inline struct layout_box *layout_build_subtree(struct layout_doc *d,
                                                       const struct dom_node *node,
                                                       struct layout_computed *parent_style);

/* Look up an attribute value on a DOM element (returns NULL if
 * not present). */
static inline const char *layout_dom_attr(const struct dom_node *n, const char *name)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return 0;
    for (struct dom_attr *a = n->attrs; a; a = a->next) {
        if (lay_streq_ci(a->name, name)) return a->value;
    }
    return 0;
}

/* HTML4 presentational attributes -> CSS.
 *
 * Many real-world pages (especially ones that pre-date CSS or
 * that use HTML4 quirks for compatibility — e.g. news.ycombinator
 * .com) put colour, alignment and sizing in attributes like
 * `bgcolor`, `text`, `align`, `width`, `height`, `cellpadding`.
 * Per the HTML5 spec these still have presentational hints with
 * specificity below an author stylesheet but above the UA sheet.
 *
 * We approximate that by synthesising a small CSS declaration
 * string from the attribute set and prepending it onto the
 * element's `style="..."` attribute (so any explicit `style=`
 * still wins).  No allocator: a 256-byte stack buffer is plenty
 * for the handful of properties we recognise per element.
 *
 * The function is intentionally generic — it doesn't special-case
 * any tag.  It just looks up each attribute and emits the
 * corresponding CSS declaration if the attribute is present and
 * makes sense for the tag (e.g. `cellpadding` only on tables).
 */
static inline int lay_pres_append(char *buf, int *pos, int cap,
                                   const char *prop, const char *val)
{
    int pl = lay_strlen(prop);
    int vl = lay_strlen(val);
    /* +5 for ": ;\0" plus a margin. */
    if (*pos + pl + vl + 5 >= cap) return 0;
    for (int i = 0; i < pl; i++) buf[(*pos)++] = prop[i];
    buf[(*pos)++] = ':'; buf[(*pos)++] = ' ';
    for (int i = 0; i < vl; i++) buf[(*pos)++] = val[i];
    buf[(*pos)++] = ';';
    buf[*pos] = 0;
    return 1;
}

/* Append a numeric attr value plus the given CSS unit.  Accepts
 * forms like "100", "100%", "100px".  We don't validate further
 * than that; an unrecognised suffix just gets dropped through to
 * layout_apply_decl which will fail-silently. */
static inline int lay_pres_append_dim(char *buf, int *pos, int cap,
                                       const char *prop, const char *val)
{
    if (!val || !val[0]) return 0;
    /* Trim leading ws */
    while (*val == ' ' || *val == '\t') val++;
    if (!*val) return 0;
    int n = lay_strlen(val);
    /* Detect explicit unit (% or trailing letter) */
    int has_unit = 0;
    for (int i = 0; i < n; i++) {
        char c = val[i];
        if (c == '%' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { has_unit = 1; break; }
    }
    char tmp[40];
    int t = 0;
    if (has_unit) {
        for (int i = 0; i < n && t < 38; i++) tmp[t++] = val[i];
    } else {
        for (int i = 0; i < n && t < 36; i++) tmp[t++] = val[i];
        tmp[t++] = 'p'; tmp[t++] = 'x';
    }
    tmp[t] = 0;
    return lay_pres_append(buf, pos, cap, prop, tmp);
}

/* If `s` looks like a 3- or 6-digit hex code without leading '#',
 * return a freshly-built `#RRGGBB` form in `out` (cap >= 8).
 * Otherwise copy as-is.  HTML4 colour attrs sometimes appear bare
 * ("ff6600"); CSS only accepts the leading '#'. */
static inline void lay_pres_normalize_color(const char *s, char *out, int cap)
{
    int n = lay_strlen(s);
    int hex_only = (n == 3 || n == 6);
    if (hex_only) {
        for (int i = 0; i < n; i++) {
            char c = s[i];
            int ok = (c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
            if (!ok) { hex_only = 0; break; }
        }
    }
    int o = 0;
    if (hex_only) {
        if (o + 1 < cap) out[o++] = '#';
        for (int i = 0; i < n && o + 1 < cap; i++) out[o++] = s[i];
    } else {
        for (int i = 0; i < n && o + 1 < cap; i++) out[o++] = s[i];
    }
    out[o] = 0;
}

/* Synthesise a CSS declaration list from the element's
 * presentational attributes.  Writes into `buf` (cap bytes)
 * and returns the number of bytes written (0 if nothing applies).
 *
 * Generic: each attribute is checked once and emitted iff present.
 * No tag-specific filtering except for the few attrs whose meaning
 * differs by tag (e.g. `border` on a table sets cell borders, on
 * an <img> it's the image frame width).  We emit border-width on
 * either; close enough for the unstyled-page rendering pass. */
static inline int layout_synth_pres_css(const struct dom_node *node,
                                          char *buf, int cap)
{
    if (!node || node->type != DOM_NODE_ELEMENT) { buf[0] = 0; return 0; }
    int pos = 0;
    buf[0] = 0;

    /* Colour-typed attributes.  We accept bare hex (HTML4 style). */
    const char *bg    = layout_dom_attr(node, "bgcolor");
    const char *fg    = layout_dom_attr(node, "color");
    const char *text  = layout_dom_attr(node, "text");
    char cbuf[16];
    if (bg && bg[0]) {
        lay_pres_normalize_color(bg, cbuf, sizeof(cbuf));
        lay_pres_append(buf, &pos, cap, "background-color", cbuf);
    }
    if (fg && fg[0]) {
        lay_pres_normalize_color(fg, cbuf, sizeof(cbuf));
        lay_pres_append(buf, &pos, cap, "color", cbuf);
    }
    /* `<body text="...">` sets the body text colour. */
    if (text && text[0]) {
        lay_pres_normalize_color(text, cbuf, sizeof(cbuf));
        lay_pres_append(buf, &pos, cap, "color", cbuf);
    }

    /* Sizing.  width/height accept "N", "N%", "Npx".  We don't
     * touch <img> here because its width/height drive the replaced
     * box dimensions directly; emitting CSS for them too is
     * harmless but wasteful. */
    const char *w = layout_dom_attr(node, "width");
    const char *h = layout_dom_attr(node, "height");
    int is_img = lay_streq_ci(node->tag, "img");
    if (!is_img) {
        if (w && w[0]) lay_pres_append_dim(buf, &pos, cap, "width", w);
        if (h && h[0]) lay_pres_append_dim(buf, &pos, cap, "height", h);
    }

    /* Alignment.  HTML4 `align="center"` -> text-align on block
     * containers; on <img> / <table> the spec is float, but we
     * map everything to text-align since we have no float impl.
     *
     * NOTE: we deliberately *skip* this on <td>/<th> because we
     * render cells as full-width blocks, not as proper table cells.
     * `<td align="right">` in real HTML is meant to right-align the
     * cell's content within a NARROW cell (e.g. a 40-px rank cell
     * on Hacker News).  In our model the cell spans the whole row,
     * so honoring align=right would push short content like "1." to
     * the far right of the page.  Letting `text-align` fall back to
     * the UA-sheet default of `left` produces a much more sensible
     * result for sites that use HTML4 table layout. */
    const char *align = layout_dom_attr(node, "align");
    int skip_align = node->tag &&
                     (lay_streq_ci(node->tag, "td") ||
                      lay_streq_ci(node->tag, "th"));
    if (align && align[0] && !skip_align) {
        if (lay_streq_ci(align, "center") ||
            lay_streq_ci(align, "left") ||
            lay_streq_ci(align, "right") ||
            lay_streq_ci(align, "justify")) {
            char low[16]; int la = 0;
            for (; align[la] && la < 15; la++) low[la] = lay_lower((unsigned char)align[la]);
            low[la] = 0;

            /* HTML4 `<table align="center">` is the legacy way to
             * centre an entire table in its containing block.
             * Real browsers translate it to `margin-left: auto;
             * margin-right: auto;` and explicitly do NOT also set
             * `text-align: center` — `text-align` is inheritable
             * and would centre every cell's content (Hacker News'
             * rank/title rows would all render centred instead of
             * left-aligned).  We follow the real-browser model:
             * for tables emit only the auto-margin pair; for
             * other elements emit text-align as before. */
            int is_table_align = (lay_streq_ci(align, "center") &&
                                  node->tag &&
                                  lay_streq_ci(node->tag, "table"));
            if (is_table_align) {
                lay_pres_append(buf, &pos, cap, "margin-left",  "auto");
                lay_pres_append(buf, &pos, cap, "margin-right", "auto");
            } else {
                lay_pres_append(buf, &pos, cap, "text-align", low);
            }
        }
    }

    /* Table-ish padding/spacing.  cellpadding -> padding on cells;
     * we apply it at the table level as a no-op for our renderer
     * (cells inherit padding from UA sheet) but `border` we map
     * to a 1px border for visual consistency. */
    const char *border = layout_dom_attr(node, "border");
    if (border && border[0]) {
        /* Only emit if the value is non-zero. */
        int nz = 0;
        for (int i = 0; border[i]; i++)
            if (border[i] >= '1' && border[i] <= '9') { nz = 1; break; }
        if (nz) {
            char dimbuf[16]; int t = 0;
            for (int i = 0; border[i] && t < 14; i++) dimbuf[t++] = border[i];
            dimbuf[t++] = 'p'; dimbuf[t++] = 'x'; dimbuf[t] = 0;
            lay_pres_append(buf, &pos, cap, "border-top-width", dimbuf);
            lay_pres_append(buf, &pos, cap, "border-right-width", dimbuf);
            lay_pres_append(buf, &pos, cap, "border-bottom-width", dimbuf);
            lay_pres_append(buf, &pos, cap, "border-left-width", dimbuf);
        }
    }

    /* <font face="..." color="..." size="..."> — color handled
     * above; map size 1..7 to a CSS pixel size approximating
     * HTML4's relative font sizing. */
    const char *fsize = layout_dom_attr(node, "size");
    if (fsize && fsize[0] && lay_streq_ci(node->tag, "font")) {
        int sgn = 0, n = 0, p = 0; (void)sgn;
        if (fsize[0] == '+' || fsize[0] == '-') p++;
        if (lay_parse_int(fsize, lay_strlen(fsize), &p, &n)) {
            int sz = 16;
            if (fsize[0] == '+') sz = 16 + n * 4;
            else if (fsize[0] == '-') sz = 16 - n * 2;
            else {
                static const int table[] = { 9, 11, 13, 16, 19, 24, 32 };
                if (n >= 1 && n <= 7) sz = table[n - 1];
            }
            if (sz < 8) sz = 8;
            if (sz > 64) sz = 64;
            char tmp[16]; int t = 0;
            int v = sz; char digits[8]; int dn = 0;
            if (v == 0) digits[dn++] = '0';
            while (v > 0 && dn < 7) { digits[dn++] = '0' + (v % 10); v /= 10; }
            for (int i = dn - 1; i >= 0 && t < 14; i--) tmp[t++] = digits[i];
            tmp[t++] = 'p'; tmp[t++] = 'x'; tmp[t] = 0;
            lay_pres_append(buf, &pos, cap, "font-size", tmp);
        }
    }

    /* `<td nowrap>` / `<nobr>` — preserve runs of inline content
     * on a single line.  We map to white-space:nowrap. */
    const char *nowrap = layout_dom_attr(node, "nowrap");
    if (nowrap || lay_streq_ci(node->tag, "nobr")) {
        lay_pres_append(buf, &pos, cap, "white-space", "nowrap");
    }

    return pos;
}

/* Build the combined inline CSS for an element: presentational
 * attrs first, then `style="..."`.  Returns a pointer into `buf`
 * (which must be at least 1024 bytes). */
static inline const char *layout_combined_inline_css(const struct dom_node *node,
                                                       char *buf, int cap)
{
    int pos = layout_synth_pres_css(node, buf, cap);
    const char *s = layout_dom_attr(node, "style");
    if (s && s[0]) {
        int sl = lay_strlen(s);
        /* Make sure we have a separating ';' if the pres CSS is
         * non-empty; harmless if not (parser tolerates leading ;). */
        if (pos > 0 && pos + 1 < cap) buf[pos++] = ';';
        for (int i = 0; i < sl && pos + 1 < cap; i++) buf[pos++] = s[i];
        buf[pos] = 0;
    }
    return pos > 0 ? buf : 0;
}


/* Decide if a DOM tag has element-specific styling that the UA
 * stylesheet wouldn't catch — currently used for <li> bullet
 * generation. */
static inline int layout_is_list_item_tag(const char *tag)
{
    return tag && lay_streq_ci(tag, "li");
}

/* Children-pass: walk DOM children and emit boxes, generating
 * anonymous block wrappers around inline runs when the parent
 * itself is a block container with mixed children.
 *
 * The two-pass anon-block algorithm (CSS 2.1 §9.2.1):
 *   - If all children are inline, emit them inline (no anon).
 *   - If at least one child is block, wrap each maximal run of
 *     inline children in an anon-block.
 *
 * "Inline child" = box of kind INLINE / TEXT / REPLACED / BR /
 * BULLET.  "Block child" = BLOCK / ANON_BLOCK / LIST_ITEM. */
static inline void layout_emit_children(struct layout_doc *d,
                                         struct layout_box *parent,
                                         const struct dom_node *dom_parent,
                                         struct layout_computed *parent_style)
{
    /* Pass 1: build children into a transient linked list,
     * count block vs inline. */
    struct layout_box *head = 0, *tail = 0;
    int n_block = 0, n_inline = 0;
    for (const struct dom_node *c = dom_parent->first_child; c; c = c->next_sibling) {
        struct layout_box *cb = layout_build_subtree(d, c, parent_style);
        if (!cb) continue;
        cb->parent = 0; cb->next_sibling = 0;
        if (!head) head = cb; else tail->next_sibling = cb;
        tail = cb;

        int is_block = (cb->kind == LAY_BOX_BLOCK || cb->kind == LAY_BOX_ANON_BLOCK);
        if (is_block) n_block++; else n_inline++;
    }

    if (n_block == 0) {
        /* All inline — attach directly. */
        struct layout_box *cb = head;
        while (cb) {
            struct layout_box *nx = cb->next_sibling;
            cb->next_sibling = 0;
            layout_box_append(parent, cb);
            cb = nx;
        }
        return;
    }

    /* Mixed — wrap inline runs.  We never emit an empty anon
     * block (no run = no wrapper). */
    struct layout_box *cb = head;
    struct layout_box *anon = 0;
    while (cb) {
        struct layout_box *nx = cb->next_sibling;
        cb->next_sibling = 0;
        int is_block = (cb->kind == LAY_BOX_BLOCK || cb->kind == LAY_BOX_ANON_BLOCK);
        if (is_block) {
            anon = 0;     /* close any open anon */
            layout_box_append(parent, cb);
        } else {
            /* Pure-whitespace TEXT between block siblings is
             * dropped (don't open an anon block for nothing). */
            if (cb->kind == LAY_BOX_TEXT) {
                int all_ws = 1;
                for (int i = 0; i < cb->text_len; i++)
                    if (!lay_is_ws((unsigned char)cb->text[i])) { all_ws = 0; break; }
                if (all_ws) {
                    layout_box_free_recursive(cb);
                    cb = nx; continue;
                }
            }
            if (!anon) {
                anon = layout_box_new(LAY_BOX_ANON_BLOCK);
                anon->style = parent_style;
                layout_box_append(parent, anon);
            }
            layout_box_append(anon, cb);
        }
        cb = nx;
    }
}

/* Build a box subtree from a DOM node.  Returns NULL if the node
 * should be skipped (display:none, head-only nodes inside head,
 * comments). */
static inline struct layout_box *layout_build_subtree(struct layout_doc *d,
                                                       const struct dom_node *node,
                                                       struct layout_computed *parent_style)
{
    if (!node) return 0;

    if (node->type == DOM_NODE_COMMENT || node->type == DOM_NODE_DOCTYPE)
        return 0;

    if (node->type == DOM_NODE_TEXT) {
        if (!node->text) return 0;
        int preserve = parent_style && parent_style->white_space == LAY_WS_PRE;
        int len = (int)node->text_len;
        int olen = 0;
        char *t = layout_collapse_text(node->text, len, preserve, &olen);
        if (!t) return 0;
        struct layout_box *b = layout_box_new(LAY_BOX_TEXT);
        b->text = t; b->text_len = olen;
        b->dom = node;
        b->style = parent_style;
        return b;
    }

    if (node->type == DOM_NODE_ELEMENT) {
        /* Build the effective inline-CSS for this element by
         * combining HTML4 presentational attrs (bgcolor, align,
         * width, ...) with the explicit `style="..."` attribute.
         * Any explicit `style` declaration wins because it appears
         * later in the combined string.
         *
         * The combined buffer lives on the heap rather than the
         * stack because layout_build_subtree recurses to the depth
         * of the DOM tree and the user thread stack is only 16 KiB
         * (chapter 11) — a 1 KiB stack array per frame quickly
         * overflows on real-world pages. */
        char *inline_buf = (char *)malloc(1024);
        const char *inline_css = inline_buf
            ? layout_combined_inline_css(node, inline_buf, 1024)
            : layout_dom_attr(node, "style");   /* fallback if oom */
        struct layout_computed *style = layout_doc_alloc_style(d);
        if (!style) { if (inline_buf) free(inline_buf); return 0; }
        layout_resolve(&d->engine, node, parent_style, inline_css, style);
        if (inline_buf) free(inline_buf);

        if (style->display == LAY_DISPLAY_NONE) {
            return 0;
        }

        /* HTML5 rendering compatibility: `<center>` and
         * `<div align="center">` (and `<body align="center">`)
         * both centre block-level descendants in addition to
         * their inline content.  Real browsers model this via
         * the legacy CSS value `text-align: -webkit-center`,
         * which our cascade doesn't grok; do it manually here
         * by setting the flag the block-flow loop consumes.
         *
         * Limiting this to <center>/<div>/<body> matches the HTML
         * Living Standard's rendering rules.  Other elements with
         * `align="center"` (e.g. <p align="center">) get plain
         * text-align: center via the presentational-CSS path. */
        if (node->tag) {
            int is_center_tag =
                lay_streq_ci(node->tag, "center");
            int is_align_center_container = 0;
            if (lay_streq_ci(node->tag, "div") ||
                lay_streq_ci(node->tag, "body")) {
                const char *al = layout_dom_attr(node, "align");
                if (al && lay_streq_ci(al, "center"))
                    is_align_center_container = 1;
            }
            if (is_center_tag || is_align_center_container) {
                style->center_block_children     = 1;
                /* Cascade the centring intent to descendants so a
                 * later heading/paragraph (whose containing `<td>`
                 * had its UA text-align reset to LEFT) can re-
                 * assert centre below. */
                style->text_align_center_inherit = 1;
                /* NB: we deliberately do NOT also set
                 * `style->text_align = LAY_TA_CENTER` on this
                 * container — the UA sheet already pins `<center>`
                 * to text-align: center, and `synth_pres_css`
                 * emits the same for `<div align=center>` /
                 * `<body align=center>`.  Setting it manually is
                 * redundant, but more importantly it bypasses the
                 * cascade — which means an author rule
                 * (`center { text-align: left }`, etc.) would lose
                 * to our hard-coded value.  Keep the cascade as
                 * the single source of truth for text-align. */
            }
        }

        /* Post-cascade re-assertion of `-webkit-center` semantics.
         * Our UA stylesheet pins `text-align: left` on table /
         * tr / td / th to stop a centred parent from cascading
         * cell-content centring onto pages like Hacker News
         * (HN wraps the whole page in `<center>` but expects
         * cell content to render left-aligned; real browsers
         * follow the same rule).
         *
         * But pages that put a heading or description directly
         * inside a centring container (PTW: `<div align=center>
         * <table><tr><td><h2>...`) DO want the heading centred.
         * Real browsers achieve this with `-webkit-center` having
         * different semantics for inline vs. block descendants.
         * We approximate it by re-asserting text-align: center on
         * a small allow-list of "content" elements (headings,
         * paragraphs) that lost the inheritance because their
         * containing `<td>` reset to LEFT.  Table elements
         * themselves are deliberately NOT in the allow-list:
         * re-asserting on `<td>` would break HN.
         *
         * The condition `text_align == LAY_TA_LEFT` ensures we
         * only override the UA default; an author who explicitly
         * set `text-align: right` on `.title` still wins. */
        if (node->tag && style->text_align_center_inherit &&
            style->text_align == LAY_TA_LEFT) {
            int is_heading =
                lay_streq_ci(node->tag, "h1") ||
                lay_streq_ci(node->tag, "h2") ||
                lay_streq_ci(node->tag, "h3") ||
                lay_streq_ci(node->tag, "h4") ||
                lay_streq_ci(node->tag, "h5") ||
                lay_streq_ci(node->tag, "h6");
            int is_paragraph =
                lay_streq_ci(node->tag, "p");
            if (is_heading || is_paragraph)
                style->text_align = LAY_TA_CENTER;
        }

        /* <br> */
        if (lay_streq_ci(node->tag, "br")) {
            struct layout_box *b = layout_box_new(LAY_BOX_BR);
            b->style = style; b->dom = node;
            return b;
        }

        /* <img> — replaced element.  Three sources for the box
         * dimensions, tried in order:
         *
         *   1. Explicit `width=""` / `height=""` attributes on the
         *      HTML element.  Win immediately if both present.
         *
         *   2. Intrinsic size from the browser's image cache via
         *      the layout_set_img_size_lookup hook (chapter 98b).
         *      Used when an attribute is missing AND the browser
         *      has pre-decoded the image.
         *
         *   3. A 16x16 placeholder so the surrounding flow doesn't
         *      collapse onto the baseline.  Last-resort fallback
         *      for un-decoded / unknown images. */
        if (lay_streq_ci(node->tag, "img")) {
            struct layout_box *b = layout_box_new(LAY_BOX_REPLACED);
            b->style = style; b->dom = node;
            const char *w_attr = layout_dom_attr(node, "width");
            const char *h_attr = layout_dom_attr(node, "height");
            int p = 0;
            if (w_attr) lay_parse_int(w_attr, lay_strlen(w_attr), &p, &b->replaced_w);
            p = 0;
            if (h_attr) lay_parse_int(h_attr, lay_strlen(h_attr), &p, &b->replaced_h);
            if ((b->replaced_w <= 0 || b->replaced_h <= 0) && lay__img_size_fn) {
                const char *src = layout_dom_attr(node, "src");
                int iw = 0, ih = 0;
                if (src && lay__img_size_fn(src, &iw, &ih, lay__img_size_ud) == 0) {
                    if (b->replaced_w <= 0 && iw > 0) b->replaced_w = iw;
                    if (b->replaced_h <= 0 && ih > 0) b->replaced_h = ih;
                }
            }
            if (b->replaced_w <= 0) b->replaced_w = 16;
            if (b->replaced_h <= 0) b->replaced_h = 16;
            const char *alt = layout_dom_attr(node, "alt");
            if (alt) b->replaced_alt = lay_strdup(alt);
            return b;
        }

        /* Decide block vs inline kind. */
        int kind = LAY_BOX_INLINE;
        if (style->display == LAY_DISPLAY_BLOCK ||
            style->display == LAY_DISPLAY_LIST_ITEM ||
            style->display == LAY_DISPLAY_TABLE ||
            style->display == LAY_DISPLAY_TABLE_ROW ||
            style->display == LAY_DISPLAY_TABLE_CELL) {
            kind = LAY_BOX_BLOCK;
        }
        struct layout_box *b = layout_box_new(kind);
        b->style = style; b->dom = node;

        /* List bullet for display:list-item */
        if (style->display == LAY_DISPLAY_LIST_ITEM ||
            layout_is_list_item_tag(node->tag)) {
            const char *bullet = "• ";
            if (style->list_style == LAY_LS_CIRCLE) bullet = "◦ ";
            else if (style->list_style == LAY_LS_SQUARE) bullet = "▪ ";
            else if (style->list_style == LAY_LS_NONE) bullet = "";
            else if (style->list_style == LAY_LS_DECIMAL) bullet = "1. ";
            if (bullet[0]) {
                struct layout_box *bm = layout_box_new(LAY_BOX_BULLET);
                bm->style = style;
                bm->text = lay_strdup(bullet);
                bm->text_len = lay_strlen(bm->text);
                layout_box_append(b, bm);
            }
        }

        /* Children */
        layout_emit_children(d, b, node, style);
        return b;
    }

    if (node->type == DOM_NODE_DOCUMENT) {
        struct layout_box *b = layout_box_new(LAY_BOX_BLOCK);
        b->style = &d->root_style;
        b->dom = node;
        layout_emit_children(d, b, node, &d->root_style);
        return b;
    }
    return 0;
}

/* ============================================================
 *   PART 6 — layout passes (block + inline)
 * ============================================================ */

/* Font metrics.  Our renderer ships only the kernel's 8x16 fixed
 * cell font (chapter 23).  For larger font-sizes we scale glyph
 * width and advance proportionally — this is a simplification
 * (real fonts have per-glyph metrics and aren't square-scalable)
 * but it lets the layout pass produce visibly different sizes for
 * different headings without needing a real font subsystem.
 *
 * The base cell at font-size 16 is exactly 8 wide.  We round
 * `advance = font_size_px / 2` (so 16->8, 24->12, 32->16).  The
 * line height is `font_size_px` itself if line-height is unset,
 * else the resolved line-height. */
#define LAYOUT_BASE_FONT_PX  16
#define LAYOUT_BASE_GLYPH_W  9

static inline int layout_glyph_width(int font_size_px)
{
    /* Layout's glyph advance is fs/2 in principle, but our renderer
     * (both the GUI 8x16 font and the text-mode character grid)
     * cannot draw glyphs narrower than 8 px / 1 cell — so we floor
     * the advance at LAYOUT_BASE_GLYPH_W (currently 9 px = 8-px
     * font glyph + 1-px right side bearing).  Without this floor,
     * `font-size:13px` (advance 6) produced glyphs that overlapped
     * each other in the GUI ("Inline" → "In li ne") and stomped
     * neighbouring cells in text mode.  Bumping the floor from 8 to
     * 9 also gives a 1-px gap between every character in the GUI
     * 8x16 font so adjacent letters no longer touch ("Architecture"
     * was unreadable when packed at 8-px pitch). */
    int w = font_size_px / 2;
    if (w < LAYOUT_BASE_GLYPH_W) w = LAYOUT_BASE_GLYPH_W;
    return w;
}

static inline int layout_text_width(const char *s, int n, int font_size_px)
{
    int gw = layout_glyph_width(font_size_px);
    return n * gw;
    (void)s;     /* every glyph is the same advance */
}

static inline int layout_line_box_height(const struct layout_computed *st)
{
    if (!st) return LAYOUT_BASE_FONT_PX;
    int lh = st->line_height_px > 0
                 ? st->line_height_px
                 : (st->font_size_px * 12) / 10;     /* CSS default ~1.2 */
    /* The kernel's only font is fixed 8x16, so a glyph always
     * occupies 16 px of vertical room no matter what `font-size`
     * the layout asked for.  If we honoured the CSS line-box height
     * literally for fs<16, the glyph would overflow the box and the
     * underline (which we clamp into the box) would land mid-glyph.
     * Floor the line box at the glyph height so small-font lines
     * still get a slim "leading" gap below for descenders + rule. */
    int min_lh = LAYOUT_BASE_FONT_PX + 2;            /* +2 for underline */
    if (lh < min_lh) lh = min_lh;
    return lh;
}

/* Resolution helpers: turn a stored layout_length into pixels in
 * the context of a particular containing-block dim and font. */
static inline int layout_len_px(struct layout_length L,
                                 int parent_dim_px,
                                 int own_font_px,
                                 int root_font_px,
                                 int default_for_auto)
{
    return lay_resolve_length(L, parent_dim_px, own_font_px,
                              root_font_px, default_for_auto);
}

/* ---------- inline layout (line boxes) ---------- */

/* While laying out an inline run we accumulate "items" — atomic
 * pieces that can't be split internally — then break the run
 * into line boxes.  An item is one of:
 *   - a word (run of non-whitespace)
 *   - a forced break (<br>)
 *   - a bullet (rendered as text, not breakable)
 *   - a replaced atom (<img>, fixed width)
 *   - a soft-space (one space; collapses if line-broken at that
 *     position; otherwise renders as one space).
 *
 * After collection, we walk left-to-right placing items on lines;
 * when adding the next item would overflow `content_w`, we cut
 * the line at the most recent soft-space (or, if the current
 * item itself is wider than the line, we emit it on a line of
 * its own). */

enum {
    LAY_ITEM_WORD = 1,
    LAY_ITEM_SPACE = 2,
    LAY_ITEM_BR = 3,
    LAY_ITEM_REPLACED = 4,
};

struct layout_item {
    int kind;
    /* For WORD: pointer into a malloc'd buffer + length.  For
     * SPACE: width in px (depends on style at the point of
     * insertion).  For REPLACED / WORD: precomputed width. */
    const char *text;
    int         text_len;
    int         owns_text;       /* 1 => layout_inline_format frees text */
    int         width_px;
    int         height_px;       /* for vertical alignment / line height */
    struct layout_computed *style;   /* style at time of emission */
    /* For REPLACED, the underlying box (to write back x/y/w/h
     * after wrapping). */
    struct layout_box *src_box;
    /* For WORD: source box (TEXT or BULLET) so we can produce
     * paint commands later. */
    struct layout_box *box_src;
    /* Snapshots captured at collection time so the wrap loop can
     * read them safely even after `src_box` / `box_src` are freed
     * by the pre-loop cleanup pass.  `dom_snap` is borrowed (the
     * DOM tree outlives layout); `alt_snap` is owned and freed
     * with the item. */
    const struct dom_node *dom_snap;
    char       *alt_snap;
};

struct layout_inline_buf {
    struct layout_item *items;
    int                 n;
    int                 cap;
};

static inline int layout_inline_push(struct layout_inline_buf *buf, struct layout_item it)
{
    if (buf->n == buf->cap) {
        int nc = buf->cap ? buf->cap * 2 : 32;
        struct layout_item *n = (struct layout_item *)malloc((size_t)nc * sizeof(*n));
        if (!n) return -1;
        for (int i = 0; i < buf->n; i++) n[i] = buf->items[i];
        if (buf->items) free(buf->items);
        buf->items = n; buf->cap = nc;
    }
    buf->items[buf->n++] = it;
    return 0;
}

/* Walk the inline children of `container`, splitting TEXT boxes
 * into word + space items.  Inline element boxes recurse, with
 * the element's style applied to its descendants. */
static inline void layout_collect_inline(struct layout_box *container,
                                          struct layout_inline_buf *buf)
{
    for (struct layout_box *c = container->first_child; c; c = c->next_sibling) {
        if (c->kind == LAY_BOX_BR) {
            struct layout_item it; it.kind = LAY_ITEM_BR;
            it.text = 0; it.text_len = 0; it.owns_text = 0; it.width_px = 0;
            it.style = c->style;
            it.height_px = layout_line_box_height(c->style);
            it.src_box = c; it.box_src = 0;
            it.dom_snap = c->dom; it.alt_snap = 0;
            layout_inline_push(buf, it);
            continue;
        }
        if (c->kind == LAY_BOX_REPLACED) {
            struct layout_item it; it.kind = LAY_ITEM_REPLACED;
            it.text = 0; it.text_len = 0; it.owns_text = 0;
            it.width_px = c->replaced_w;
            it.height_px = c->replaced_h > 0 ? c->replaced_h : layout_line_box_height(c->style);
            it.style = c->style;
            it.src_box = c; it.box_src = c;
            it.dom_snap = c->dom;
            it.alt_snap = 0;
            if (c->replaced_alt) {
                int an = lay_strlen(c->replaced_alt);
                char *ac = (char *)malloc((size_t)an + 1);
                if (ac) {
                    for (int k = 0; k < an; k++) ac[k] = c->replaced_alt[k];
                    ac[an] = 0;
                    it.alt_snap = ac;
                }
            }
            layout_inline_push(buf, it);
            continue;
        }
        if (c->kind == LAY_BOX_TEXT || c->kind == LAY_BOX_BULLET) {
            const char *s = c->text;
            int n = c->text_len;
            int fs = c->style ? c->style->font_size_px : LAYOUT_BASE_FONT_PX;
            int lh = layout_line_box_height(c->style);
            int gw = layout_glyph_width(fs);
            /* Bullet: emit as a single non-breakable WORD.  Always
             * own a copy so the source box can be freed safely. */
            if (c->kind == LAY_BOX_BULLET) {
                struct layout_item it; it.kind = LAY_ITEM_WORD;
                char *t = (char *)malloc((size_t)n + 1);
                if (t) { for (int k = 0; k < n; k++) t[k] = s[k]; t[n] = 0; }
                it.text = t; it.text_len = n; it.owns_text = 1;
                it.width_px = layout_text_width(s, n, fs);
                it.height_px = lh; it.style = c->style;
                it.src_box = c; it.box_src = c;
                it.dom_snap = c->dom; it.alt_snap = 0;
                layout_inline_push(buf, it);
                continue;
            }
            /* Text: tokenize on whitespace into WORD + SPACE items.
             * Each WORD owns a freshly-malloc'd copy of its slice
             * because we will free the source TEXT box BEFORE the
             * wrap loop reads back from these items (the source
             * pointer would dangle otherwise). */
            int i = 0;
            int leading = 0;
            while (i < n) {
                if (lay_is_ws((unsigned char)s[i])) {
                    if (!leading || buf->n > 0) {
                        struct layout_item it; it.kind = LAY_ITEM_SPACE;
                        it.text = 0; it.text_len = 0; it.owns_text = 0;
                        it.width_px = gw;
                        it.height_px = lh; it.style = c->style;
                        it.src_box = c; it.box_src = c;
                        it.dom_snap = c->dom; it.alt_snap = 0;
                        layout_inline_push(buf, it);
                    }
                    while (i < n && lay_is_ws((unsigned char)s[i])) i++;
                    leading = 0;
                    continue;
                }
                int start = i;
                while (i < n && !lay_is_ws((unsigned char)s[i])) i++;
                int wlen = i - start;
                struct layout_item it; it.kind = LAY_ITEM_WORD;
                char *t = (char *)malloc((size_t)wlen + 1);
                if (t) { for (int k = 0; k < wlen; k++) t[k] = s[start + k]; t[wlen] = 0; }
                it.text = t; it.text_len = wlen; it.owns_text = 1;
                it.width_px = layout_text_width(it.text, wlen, fs);
                it.height_px = lh; it.style = c->style;
                it.src_box = c; it.box_src = c;
                it.dom_snap = c->dom; it.alt_snap = 0;
                layout_inline_push(buf, it);
                leading = 1;
            }
            continue;
        }
        if (c->kind == LAY_BOX_INLINE) {
            /* Recurse, inline children flatten. */
            layout_collect_inline(c, buf);
            continue;
        }
        /* Anonymous block / block child shouldn't appear in an
         * inline run (anon-block wrapping handled this).  Skip
         * defensively. */
    }
}

/* Per-line accumulated state during the wrap pass. */
struct layout_line {
    int first;          /* index into items[] */
    int last;           /* exclusive */
    int width_px;       /* sum widths of items, including soft-spaces */
    int height_px;      /* max(item.height_px) for items on the line */
    int n_words;
    int n_spaces;
};

/* Lay out an inline formatting context inside `container`, anchored
 * at (x, y) document-space, content width = content_w.  Returns the
 * total height consumed by the line stack.
 *
 * Positions every WORD / REPLACED / BULLET item by stamping
 * absolute (x,y,w,h) onto its source box (LAY_BOX_TEXT, BULLET, or
 * REPLACED).  For TEXT boxes we cope with multiple items belonging
 * to the same TEXT box by *creating new boxes per item* — see
 * layout_emit_inline_word, which always emits a fresh BOX_TEXT. */
static inline struct layout_box *layout_emit_inline_word(struct layout_box *anon_parent,
                                                           const struct layout_item *it,
                                                           int x, int y, int line_h)
{
    /* Reuse the source box if we can (first time it's painted in
     * this layout run); otherwise allocate a new TEXT box. */
    struct layout_box *box = layout_box_new(LAY_BOX_TEXT);
    if (!box) return 0;
    box->style = it->style;
    box->dom   = it->dom_snap;
    /* malloc its own copy of the slice text */
    char *t = (char *)malloc((size_t)it->text_len + 1);
    if (t) {
        for (int k = 0; k < it->text_len; k++) t[k] = it->text[k];
        t[it->text_len] = 0;
    }
    box->text = t; box->text_len = it->text_len;
    box->x = x; box->y = y;
    box->w = it->width_px;
    box->h = line_h;
    layout_box_append(anon_parent, box);
    return box;
}

static inline int layout_inline_format(struct layout_box *container,
                                        int x_origin, int y_origin,
                                        int content_w)
{
    struct layout_inline_buf buf = { 0, 0, 0 };
    layout_collect_inline(container, &buf);

    /* NOTE: we used to clamp oversized REPLACED items (typically
     * `<img width=N>` larger than the inline container) to
     * content_w with aspect ratio preserved.  That was a hack
     * for the era before horizontal scrolling — without it a
     * 288-px image inside a 244-px cell stomped its sibling.
     * Now that M63 supports horizontal scroll, we let the image
     * keep its natural pixel width: real browsers overflow in
     * this case and the user can scroll to see the rest. */

    /* We are going to REPLACE the children of `container` with
     * positioned WORD / REPLACED boxes (one per item per line).
     * The original collection is freed first. */
    struct layout_box *old = container->first_child;
    container->first_child = container->last_child = 0;
    while (old) {
        struct layout_box *nx = old->next_sibling;
        layout_box_free_recursive(old);
        old = nx;
    }

    if (buf.n == 0) {
        if (buf.items) free(buf.items);
        return 0;
    }

    int y = y_origin;
    int total_h = 0;
    int i = 0;
    while (i < buf.n) {
        /* Build one line: greedy fit until next item would overflow.
         * Skip leading SPACEs at the start of a new line. */
        while (i < buf.n && buf.items[i].kind == LAY_ITEM_SPACE) i++;
        if (i >= buf.n) break;

        int line_start = i;
        int line_w = 0;
        int line_h = 0;
        int last_break = -1;       /* index just before a SPACE */
        int line_w_at_break = 0;
        int forced_break = 0;
        while (i < buf.n) {
            struct layout_item *it = &buf.items[i];
            if (it->kind == LAY_ITEM_BR) { forced_break = 1; i++; break; }
            int next_w = it->width_px;
            int hp = it->height_px;
            if (line_w + next_w > content_w && i > line_start) {
                /* overflow.  if we have a break point, retreat to it.
                 * else hard-break here. */
                if (last_break >= line_start) {
                    int j = last_break;
                    /* rewind */
                    line_w = line_w_at_break;
                    /* line ends at j, next line starts at j+1 (skip space) */
                    i = j + 1;
                }
                break;
            }
            if (it->kind == LAY_ITEM_SPACE) {
                last_break = i;
                line_w_at_break = line_w;
            }
            line_w += next_w;
            if (hp > line_h) line_h = hp;
            i++;
        }
        int line_end = i;     /* exclusive */
        if (forced_break) line_end = i - 1;     /* don't render the BR itself */

        /* trim trailing SPACEs on this line */
        while (line_end > line_start &&
               buf.items[line_end - 1].kind == LAY_ITEM_SPACE) line_end--;

        if (line_h == 0) line_h = container->style ? layout_line_box_height(container->style)
                                                    : LAYOUT_BASE_FONT_PX;

        /* Recompute line_w over the actually-emitted range. */
        int real_w = 0;
        int n_spaces = 0;
        for (int j = line_start; j < line_end; j++) {
            real_w += buf.items[j].width_px;
            if (buf.items[j].kind == LAY_ITEM_SPACE) n_spaces++;
        }
        /* Horizontal alignment */
        int x_pen = x_origin;
        int extra_per_space = 0;
        int extra_first_n   = 0;
        int container_align = container->style ? container->style->text_align : LAY_TA_LEFT;
        if (container_align == LAY_TA_CENTER) {
            x_pen = x_origin + (content_w - real_w) / 2;
        } else if (container_align == LAY_TA_RIGHT) {
            x_pen = x_origin + (content_w - real_w);
        } else if (container_align == LAY_TA_JUSTIFY && n_spaces > 0 &&
                   line_end < buf.n /* not the last line */) {
            int slack = content_w - real_w;
            extra_per_space = slack / n_spaces;
            extra_first_n   = slack - extra_per_space * n_spaces;
        }
        if (x_pen < x_origin) x_pen = x_origin;

        for (int j = line_start; j < line_end; j++) {
            struct layout_item *it = &buf.items[j];
            if (it->kind == LAY_ITEM_SPACE) {
                int extra = extra_per_space;
                if (extra_first_n > 0) { extra++; extra_first_n--; }
                /* emit a tiny TEXT box for the space so painting
                 * over the underline works uniformly.  But if the
                 * style has no underline / decoration we can skip. */
                if (it->style && (it->style->text_decoration & LAY_TD_UNDERLINE)) {
                    struct layout_item synth = *it;
                    synth.width_px = it->width_px + extra;
                    layout_emit_inline_word(container, &synth, x_pen, y, line_h);
                }
                x_pen += it->width_px + extra;
                continue;
            }
            if (it->kind == LAY_ITEM_WORD) {
                layout_emit_inline_word(container, it, x_pen, y, line_h);
                x_pen += it->width_px;
                continue;
            }
            if (it->kind == LAY_ITEM_REPLACED) {
                /* Emit a fresh REPLACED box rather than re-parenting
                 * `it->src_box` — the source was already destroyed
                 * by the pre-loop cleanup that freed `old` children
                 * recursively, so its memory is now dangling. */
                struct layout_box *rb = layout_box_new(LAY_BOX_REPLACED);
                if (rb) {
                    rb->style = it->style;
                    rb->dom = it->dom_snap;
                    rb->x = x_pen; rb->y = y;
                    rb->w = it->width_px; rb->h = it->height_px;
                    rb->replaced_w = it->width_px;
                    rb->replaced_h = it->height_px;
                    if (it->alt_snap) {
                        int an = lay_strlen(it->alt_snap);
                        char *ac = (char *)malloc((size_t)an + 1);
                        if (ac) {
                            for (int k = 0; k < an; k++) ac[k] = it->alt_snap[k];
                            ac[an] = 0;
                            rb->replaced_alt = ac;
                        }
                    }
                    layout_box_append(container, rb);
                }
                x_pen += it->width_px;
                continue;
            }
        }
        y += line_h;
        total_h += line_h;

        /* If we forced-broke on BR, advance past it. */
        if (forced_break && line_end == i - 1) { /* already consumed */ }
        if (line_start == line_end && i == line_start) {
            /* No progress (single item too wide and not breakable) — move on. */
            i++;
            y += LAYOUT_BASE_FONT_PX;
            total_h += LAYOUT_BASE_FONT_PX;
        }
    }

    if (buf.items) {
        for (int j = 0; j < buf.n; j++) {
            if (buf.items[j].owns_text && buf.items[j].text)
                free((void *)buf.items[j].text);
            if (buf.items[j].alt_snap)
                free(buf.items[j].alt_snap);
        }
        free(buf.items);
    }
    return total_h;
}

/* ---------- block layout ---------- */

/* Forward declaration: a block layouts into its own (x,y,w) and
 * returns the total height including margins/padding/border. */
static inline int layout_block(struct layout_box *box,
                                int x, int y, int containing_w,
                                int root_fs);

/* Walk a box subtree and return the maximum `x + w` of any box
 * in it (the rightmost painted extent, in absolute coordinates).
 * Used by the centering logic in `layout_container_children` to
 * decide how much slack to distribute when sliding a centred
 * child.  Just using the child's own `c->w` is wrong for tables
 * whose inner rows overflow horizontally (table-row strategy A
 * accepts overflow rather than shrinking rigid cells); centring
 * on the nominal `c->w` then pushes the visible content off the
 * right edge of the viewport.  Walking the subtree captures the
 * actual painted right edge regardless of overflow. */
static inline int layout_subtree_right_extent(struct layout_box *root)
{
    if (!root) return 0;
    int cap = 64, top = 0;
    struct layout_box **stk =
        (struct layout_box **)malloc(sizeof(*stk) * (size_t)cap);
    if (!stk) {
        /* OOM — fall back to the box's own right edge.  This
         * matches the pre-overflow-aware behaviour and is at
         * least conservative (no shift if the table is wider
         * than its container). */
        return root->x + root->w;
    }
    stk[top++] = root;
    int max_right = root->x + root->w;
    while (top > 0) {
        struct layout_box *b = stk[--top];
        int br = b->x + b->w;
        if (br > max_right) max_right = br;
        for (struct layout_box *c = b->first_child;
             c; c = c->next_sibling) {
            if (top >= cap) {
                int new_cap = cap * 2;
                struct layout_box **n =
                    (struct layout_box **)malloc(sizeof(*n) * (size_t)new_cap);
                if (!n) { top = 0; break; }
                for (int i = 0; i < top; i++) n[i] = stk[i];
                free(stk); stk = n; cap = new_cap;
            }
            stk[top++] = c;
        }
    }
    free(stk);
    return max_right;
}

/* Recursively shift a box subtree by `dx` along the x axis.  Used
 * by the block-flow loop to recentre a child after laying it out
 * inside a `<center>` / `<div align="center">` container — the
 * child was first positioned at the container's left edge, then
 * the whole subtree is slid right by half the slack.  Walks the
 * tree iteratively (explicit malloc'd stack) to keep the user
 * thread's 16-KiB stack budget intact on deeply nested pages. */
static inline void layout_shift_subtree_x(struct layout_box *root, int dx)
{
    if (!root || dx == 0) return;
    int cap = 64, top = 0;
    struct layout_box **stk =
        (struct layout_box **)malloc(sizeof(*stk) * (size_t)cap);
    if (!stk) {
        /* OOM — fall back to recursive shift.  Stack depth is
         * usually small; this only fires on very deep DOMs and
         * the system is already in trouble if malloc fails. */
        root->x += dx;
        for (struct layout_box *c = root->first_child;
             c; c = c->next_sibling)
            layout_shift_subtree_x(c, dx);
        return;
    }
    stk[top++] = root;
    while (top > 0) {
        struct layout_box *b = stk[--top];
        b->x += dx;
        for (struct layout_box *c = b->first_child;
             c; c = c->next_sibling) {
            if (top >= cap) {
                int new_cap = cap * 2;
                struct layout_box **n =
                    (struct layout_box **)malloc(sizeof(*n) * (size_t)new_cap);
                if (!n) { top = 0; break; }
                for (int i = 0; i < top; i++) n[i] = stk[i];
                free(stk); stk = n; cap = new_cap;
            }
            stk[top++] = c;
        }
    }
    free(stk);
}

/* Approximate the CSS "max-content" intrinsic width of a subtree:
 * the width the subtree would take if it never wrapped.  Used for
 * the table-row sizing below.  We deliberately measure on the box
 * tree (which has already been style-resolved by layout_build_subtree)
 * but BEFORE layout_block has stamped final positions, so we can
 * size cells without doing two full layout passes.
 *
 * Inline children sum (single line); block children take the max
 * (each block starts a new line).  TEXT/BULLET width is text_len
 * times the glyph advance for its style; REPLACED is its placeholder
 * width.
 *
 * `depth` is a hard cap that prevents this routine from blowing the
 * 16-KiB user stack on deeply-nested HTML (HN nests <table> inside
 * <td> inside <table>...).  Beyond LAY_INTRINSIC_DEPTH we just
 * return 0 and let the table-row sizer fall back to its slack-to-
 * widest-cell heuristic — slightly less accurate but never crashy. */
#define LAY_INTRINSIC_DEPTH  12
static inline int layout_intrinsic_max_w(const struct layout_box *b,
                                          int root_fs, int depth)
{
    if (!b || depth >= LAY_INTRINSIC_DEPTH) return 0;
    int fs = b->style ? b->style->font_size_px : LAYOUT_BASE_FONT_PX;
    int gw = layout_glyph_width(fs);

    /* Explicit pixel width is the strongest signal we have for
     * intrinsic max-content width — `width: 320px` on a `<table>`
     * means "this box is at least 320 px wide regardless of its
     * children's text".  Use it directly (plus padding/border)
     * and don't recurse into children: their own intrinsic widths
     * may be smaller (e.g. a 320-px table containing a one-word
     * `<a>` link), but the BOX itself still wants 320 px. */
    if (b->style && b->style->width.unit == LAY_LEN_PX &&
        b->style->width.v > 0) {
        int w = b->style->width.v;
        /* Add typical padding/border (matches the "+ 8" hand-wave
         * in the descendant path below). */
        return w + 8;
    }

    int own = 0;
    if (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_BULLET) {
        if (b->text_len > 0) own = b->text_len * gw;
    } else if (b->kind == LAY_BOX_REPLACED) {
        own = b->replaced_w > 0 ? b->replaced_w : 16;
    } else if (b->kind == LAY_BOX_BR) {
        own = 0;
    } else if (b->kind == LAY_BOX_INLINE) {
        /* `<a><br></a>` is rare but legal — break the running sum
         * at each <br>, take max-of-segments.  See the parallel
         * comment in the !has_block path below for why this is
         * the right semantics for max-content. */
        int seg = 0;
        for (struct layout_box *c = b->first_child; c; c = c->next_sibling) {
            if (c->kind == LAY_BOX_BR) {
                if (seg > own) own = seg;
                seg = 0;
                continue;
            }
            seg += layout_intrinsic_max_w(c, root_fs, depth + 1);
        }
        if (seg > own) own = seg;
    } else {
        /* BLOCK / ANON_BLOCK.  If every child is an inline-level
         * box (TEXT, INLINE, REPLACED, BULLET, BR) they share a
         * single inline formatting context, so the max-content
         * width is the longest <br>-delimited segment (since
         * <br> forces a line break and is the only inline that
         * does so in our model).  Without this we'd sum a
         * cell-full of `<a>...</a><br><a>...</a><br>...` as a
         * single huge line, drastically over-estimating the
         * width and defeating the table shrink-to-fit below.
         *
         * If any child is a block-level box each starts its own
         * line, so take MAX of children directly. */
        int has_block = 0;
        for (struct layout_box *c = b->first_child; c; c = c->next_sibling) {
            if (c->kind == LAY_BOX_BLOCK ||
                c->kind == LAY_BOX_ANON_BLOCK) { has_block = 1; break; }
        }
        if (!has_block) {
            int seg = 0;
            for (struct layout_box *c = b->first_child;
                 c; c = c->next_sibling) {
                if (c->kind == LAY_BOX_BR) {
                    if (seg > own) own = seg;
                    seg = 0;
                    continue;
                }
                seg += layout_intrinsic_max_w(c, root_fs, depth + 1);
            }
            if (seg > own) own = seg;
        } else if (b->style &&
                   b->style->display == LAY_DISPLAY_TABLE_ROW) {
            /* `<tr>` lays its cells out horizontally, not
             * vertically — so the max-content of a row is the
             * SUM of its cells' max-contents (plus a small
             * per-cell gutter for cellspacing/padding).  Without
             * this branch, an outer table containing a row of
             * three 330-px columns would report max-content as
             * just 330 (the max of one column) and shrink-to-fit
             * would clamp the table to a single-column width;
             * the inner row then overflows the table to the
             * right and the centring shift puts everything in
             * the wrong place. */
            int sum = 0;
            for (struct layout_box *c = b->first_child;
                 c; c = c->next_sibling) {
                if (c->kind == LAY_BOX_BLOCK ||
                    c->kind == LAY_BOX_ANON_BLOCK)
                    sum += layout_intrinsic_max_w(c, root_fs, depth + 1);
            }
            own = sum;
        } else {
            int max_w = 0;
            for (struct layout_box *c = b->first_child;
                 c; c = c->next_sibling) {
                int cw = layout_intrinsic_max_w(c, root_fs, depth + 1);
                if (cw > max_w) max_w = cw;
            }
            own = max_w;
        }
        /* Hard-code typical horizontal padding/border instead of
         * resolving from style — keeps this routine's stack frame
         * minimal (no nested layout_len_px calls). */
        own += 8;
    }
    return own;
}

/* Return the largest "rigid" width found in this subtree — that is,
 * the largest descendant `width: Npx` value (or the box's own
 * explicit pixel width).  Returns 0 if no descendant has an
 * explicit pixel width.
 *
 * This is the signal we use in the table-row sizer to decide
 * whether a cell whose natural max-content exceeds its
 * proportional share is "rigid" (must keep its width and force
 * the row to overflow) or "flexible" (just text-heavy, can
 * shrink and reflow). */
static inline int layout_rigid_min_w(const struct layout_box *b,
                                      int depth)
{
    if (!b || depth >= LAY_INTRINSIC_DEPTH) return 0;
    int own = 0;
    if (b->style && b->style->width.unit == LAY_LEN_PX &&
        b->style->width.v > 0) {
        own = b->style->width.v;
    }
    int max_d = 0;
    for (struct layout_box *c = b->first_child;
         c; c = c->next_sibling) {
        int cw = layout_rigid_min_w(c, depth + 1);
        if (cw > max_d) max_d = cw;
    }
    return own > max_d ? own : max_d;
}

/* The container's children may be all blocks, all inline, or
 * (after anon-wrapping) all blocks.  If the container has only
 * inline children we go straight to the inline pass; otherwise
 * we walk blocks and recurse. */
static inline int layout_container_children(struct layout_box *container,
                                             int content_x, int content_y,
                                             int content_w, int root_fs)
{
    /* Special case: a `<tr>` (or any element with `display: table-row`)
     * lays its children out horizontally instead of vertically.
     *
     * This is a deliberately tiny shrink-of-CSS-tables: we don't do
     * a real two-pass auto-layout with min/max-content per cell.
     * Instead we estimate each cell's "max-content" width from its
     * box subtree (longest single line) and:
     *
     *   1. If the cells fit comfortably (sum + slack <= row width),
     *      give each cell its max-content width and donate any
     *      remaining slack to the widest cell (which is almost
     *      always the "main" content cell on real HTML4 layouts
     *      like Hacker News' rank / votes / title rows).
     *   2. Otherwise (sum > row width), give each cell its
     *      proportional share of the row width.
     *
     * This makes a `<tr><td>1.</td><td>^</td><td>long story title…
     * </td></tr>` row render as a tiny rank cell, a tiny votes cell,
     * and a wide title cell taking the remainder — instead of every
     * cell getting row_w / 3. */
    if (container->style &&
        container->style->display == LAY_DISPLAY_TABLE_ROW) {
        int n_cells = 0;
        for (struct layout_box *c = container->first_child;
             c; c = c->next_sibling) {
            if (c->kind == LAY_BOX_BLOCK || c->kind == LAY_BOX_ANON_BLOCK)
                n_cells++;
        }
        if (n_cells > 0) {
            /* Allocate the per-cell scratch arrays on the heap.
             * Stacking 3 * n_cells * sizeof(int) (plus a pointer
             * array) per recursion would tear through the 16-KiB
             * user stack in deeply-nested table layouts. */
            struct layout_box **cells =
                (struct layout_box **)malloc(sizeof(*cells) * (size_t)n_cells);
            int *natural  = (int *)malloc(sizeof(int) * (size_t)n_cells);
            int *assigned = (int *)malloc(sizeof(int) * (size_t)n_cells);
            if (!cells || !natural || !assigned) {
                /* OOM -> fall back to the simple equal-share path. */
                if (cells)    free(cells);
                if (natural)  free(natural);
                if (assigned) free(assigned);
                int cell_w = content_w / n_cells;
                if (cell_w < 16) cell_w = 16;
                int cx = content_x;
                int max_h = 0;
                for (struct layout_box *c = container->first_child;
                     c; c = c->next_sibling) {
                    if (c->kind != LAY_BOX_BLOCK &&
                        c->kind != LAY_BOX_ANON_BLOCK) continue;
                    int ch = layout_block(c, cx, content_y, cell_w, root_fs);
                    if (ch > max_h) max_h = ch;
                    cx += cell_w;
                }
                return max_h;
            }

            int idx = 0;
            int total_natural = 0;
            int widest = 0;
            int widest_idx = 0;
            for (struct layout_box *c = container->first_child;
                 c; c = c->next_sibling) {
                if (c->kind != LAY_BOX_BLOCK &&
                    c->kind != LAY_BOX_ANON_BLOCK) continue;
                cells[idx] = c;
                int nw = layout_intrinsic_max_w(c, root_fs, 0);
                if (nw < 16) nw = 16;     /* minimum sane cell */
                natural[idx] = nw;
                total_natural += nw;
                if (nw > widest) { widest = nw; widest_idx = idx; }
                idx++;
            }

            /* Inter-cell gutter.  HTML4 tables default to
             * `border-spacing: 2px` between cells (cellspacing=2);
             * real renderers ALSO add cellpadding (default 1) so
             * the visible gap is small but present.  We honour
             * that 2-px default — multi-column page layouts (e.g.
             * plaintextworld.com) want columns to sit close to
             * each other.  Wider gutters make the page look like
             * spaced-out unrelated blocks instead of a single
             * multi-column document.
             *
             * Visual separation between adjacent same-coloured
             * cells comes from the cells' own padding/borders
             * (UA sheet gives td 4-px L/R padding), not from the
             * inter-cell space.  Total gutter consumed =
             * (n_cells - 1) * GUTTER. */
            const int GUTTER = 2;
            int gutter_total = (n_cells - 1) * GUTTER;
            if (gutter_total < 0) gutter_total = 0;
            int avail_for_cells = content_w - gutter_total;
            if (avail_for_cells < n_cells * 16) {
                /* Too narrow to honour the gutter — drop it. */
                avail_for_cells = content_w;
                gutter_total = 0;
            }

            if (total_natural <= avail_for_cells) {
                /* Plenty of room: give each cell its natural width,
                 * then dump the leftover slack into the widest cell. */
                int slack = avail_for_cells - total_natural;
                for (int i = 0; i < n_cells; i++)
                    assigned[i] = natural[i];
                assigned[widest_idx] += slack;
            } else if (n_cells == 1) {
                /* Single-cell row: just take the whole available
                 * width.  The "rigid vs flexible" heuristic below
                 * is meaningless when there's nothing to balance
                 * against — a single cell should always reflow its
                 * inline content into the parent's width, NOT
                 * overflow it. */
                assigned[0] = avail_for_cells;
            } else {
                /* Squeezed: this row's cells together want more
                 * room than the parent can give.  Two strategies:
                 *
                 *   A. If at least one cell carries a "rigid"
                 *      width signal — a descendant with an
                 *      explicit `width: Npx` — honour every
                 *      cell's natural width and let the row
                 *      OVERFLOW its parent.  Real browsers do
                 *      this: the page becomes horizontally
                 *      scrollable.  M63's renderer supports
                 *      horizontal scroll, so overflow is the
                 *      user-visible-correct behaviour for a
                 *      fixed-layout site like
                 *      plaintextworld.com.
                 *
                 *   B. Otherwise (all cells are flexible — text
                 *      content with no explicit width), distribute
                 *      proportionally so the cells fit.  This is
                 *      what HN-style tables want: the title cell
                 *      shrinks to make room for rank + votes.
                 *
                 * We use `layout_rigid_min_w` (above) as the
                 * signal, NOT the natural max-content width — a
                 * cell stuffed with text has a huge `natural` but
                 * IS still flexible, while a cell with a single
                 * `<table width="320">` child has rigid min 320
                 * even if its natural-content is also 320. */
                int rigid_total = 0;
                for (int i = 0; i < n_cells; i++) {
                    int r = layout_rigid_min_w(cells[i], 0);
                    if (r > 0) {
                        /* Floor each cell's allocation at its
                         * rigid minimum so its fixed-width child
                         * fits without overflow into siblings. */
                        if (natural[i] < r) natural[i] = r;
                    }
                    rigid_total += (r > 0 ? (natural[i]) : 0);
                }
                if (rigid_total > 0 && rigid_total >= avail_for_cells) {
                    /* Strategy A: honour naturals, overflow row. */
                    for (int i = 0; i < n_cells; i++)
                        assigned[i] = natural[i];
                } else {
                    /* Strategy B: distribute proportionally.
                     * Use 64-bit math to avoid overflow on big rows. */
                    int used = 0;
                    for (int i = 0; i < n_cells - 1; i++) {
                        long long w =
                            (long long)natural[i] * (long long)avail_for_cells
                            / (long long)total_natural;
                        if (w < 16) w = 16;
                        assigned[i] = (int)w;
                        used += assigned[i];
                    }
                    int last = avail_for_cells - used;
                    if (last < 16) last = 16;
                    assigned[n_cells - 1] = last;
                }
            }

            int cell_gap = (gutter_total > 0) ? GUTTER : 0;
            int cx = content_x;
            int max_h = 0;
            for (int i = 0; i < n_cells; i++) {
                int ch = layout_block(cells[i], cx, content_y,
                                       assigned[i], root_fs);
                if (ch > max_h) max_h = ch;
                cx += assigned[i];
                if (i + 1 < n_cells) cx += cell_gap;
            }
            free(cells);
            free(natural);
            free(assigned);
            return max_h;
        }
    }

    int has_block = 0;
    int has_inline = 0;
    for (struct layout_box *c = container->first_child; c; c = c->next_sibling) {
        if (c->kind == LAY_BOX_BLOCK || c->kind == LAY_BOX_ANON_BLOCK) has_block = 1;
        else has_inline = 1;
    }

    if (!has_block && has_inline) {
        return layout_inline_format(container, content_x, content_y, content_w);
    }

    /* Block flow with margin collapsing between adjacent siblings. */
    int y = content_y;
    int prev_margin_bot = 0;     /* signed value carried forward */
    int first_in_flow = 1;
    int total_h = 0;
    /* Track whether the previous in-flow child was a `<table>` so
     * we can synthesise a small black gap between two adjacent
     * sibling tables.  Real browsers do this implicitly via
     * `border-spacing: 2px` on each table (the inset around the
     * inner cells creates a 2-px halo on each side, totalling 4 px
     * of visible body-bg between two tables stacked in the same
     * container).  Author stylesheets routinely set `padding: 0;
     * margin: 0;` on tables (e.g. plaintextworld.com), which
     * stomps any UA-sheet padding/margin we might add.  Inserting
     * the gap directly in the block-flow loop sidesteps the
     * cascade and gives multi-card layouts visible separation
     * regardless of what the author CSS specifies. */
    int prev_was_table = 0;
    for (struct layout_box *c = container->first_child; c; c = c->next_sibling) {
        if (c->kind != LAY_BOX_BLOCK && c->kind != LAY_BOX_ANON_BLOCK) {
            /* shouldn't appear in block flow after anon-wrapping;
             * defensively lay out as inline-block of zero size. */
            continue;
        }
        struct layout_computed *st = c->style;
        int own_fs = st ? st->font_size_px : LAYOUT_BASE_FONT_PX;
        int mt = st ? layout_len_px(st->margin[LAY_TOP], content_w, own_fs, root_fs, 0) : 0;
        int mb = st ? layout_len_px(st->margin[LAY_BOT], content_w, own_fs, root_fs, 0) : 0;

        /* Collapse margin with previous sibling: max of positives,
         * then add most-negative.  Simplified for our subset. */
        int collapsed = 0;
        if (!first_in_flow) {
            int a = prev_margin_bot, b = mt;
            int pos = (a > b ? a : b); if (pos < 0) pos = 0;
            int neg = (a < b ? a : b); if (neg > 0) neg = 0;
            collapsed = pos + neg;
            y += collapsed;
        } else {
            /* The first child in a block container does NOT collapse
             * its margin-top with its parent (we'd need the parent
             * to have no padding/border to do that).  We just apply
             * the margin. */
            y += mt;
            first_in_flow = 0;
        }
        total_h += (collapsed > 0 ? collapsed : (first_in_flow ? mt : 0));

        /* Synthetic inter-table gap.  See `prev_was_table` doc. */
        int this_is_table = (c->dom && c->dom->tag &&
                              lay_streq_ci(c->dom->tag, "table"));
        if (prev_was_table && this_is_table) {
            y       += 4;
            total_h += 4;
        }

        int child_h = layout_block(c, content_x, y, content_w, root_fs);

        /* If the container is a `<center>` / `<div align="center">`,
         * mirror the HTML5 rendering rule by sliding this child's
         * subtree right so it sits centred in the available width.
         *
         * We must measure the subtree's ACTUAL rightmost extent
         * rather than the child's nominal `c->w`, because tables
         * routinely overflow themselves: the table-row sizer's
         * "rigid overflow" path (used when rigid cells already sum
         * to more than the container can offer) keeps natural cell
         * widths and lets the row stick out to the right of the
         * table box.  Centring on the nominal `c->w` then shifts
         * the visible content off the right edge of the viewport.
         * Walking the subtree captures the painted right edge
         * regardless of any internal overflow.
         *
         * If the actual extent is wider than the container we just
         * leave the child alone and let it horizontally overflow
         * — there's no slack to distribute. */
        if (container->style &&
            container->style->center_block_children) {
            int right = layout_subtree_right_extent(c);
            int extent = right - content_x;     /* width of subtree */
            if (extent > 0 && extent < content_w) {
                int slack = content_w - extent;
                layout_shift_subtree_x(c, slack / 2);
            }
        }

        y += child_h;
        total_h = y - content_y;
        prev_margin_bot = mb;
        prev_was_table  = this_is_table;
    }
    /* Final bottom margin sticks out (caller may collapse with
     * its own bottom). */
    if (!first_in_flow) {
        y += prev_margin_bot;
        total_h = y - content_y;
    }
    return total_h;
}

/* Lay out a block-level box at (x, y) with available containing
 * block width `containing_w`.  Stamps the box's own absolute
 * (x,y,w,h) and returns its outer height (border + padding +
 * content; margins are handled by the caller). */
static inline int layout_block(struct layout_box *box,
                                int x, int y, int containing_w,
                                int root_fs)
{
    struct layout_computed *st = box->style;
    int own_fs = st ? st->font_size_px : LAYOUT_BASE_FONT_PX;

    int ml = st ? layout_len_px(st->margin[LAY_LEFT],  containing_w, own_fs, root_fs, 0) : 0;
    int mr = st ? layout_len_px(st->margin[LAY_RIGHT], containing_w, own_fs, root_fs, 0) : 0;
    int pl = st ? layout_len_px(st->padding[LAY_LEFT], containing_w, own_fs, root_fs, 0) : 0;
    int pr = st ? layout_len_px(st->padding[LAY_RIGHT],containing_w, own_fs, root_fs, 0) : 0;
    int pt = st ? layout_len_px(st->padding[LAY_TOP],  containing_w, own_fs, root_fs, 0) : 0;
    int pb = st ? layout_len_px(st->padding[LAY_BOT],  containing_w, own_fs, root_fs, 0) : 0;
    int bl = st ? st->border_px[LAY_LEFT]  : 0;
    int br = st ? st->border_px[LAY_RIGHT] : 0;
    int bt = st ? st->border_px[LAY_TOP]   : 0;
    int bb = st ? st->border_px[LAY_BOT]   : 0;

    /* Resolve width: auto fills available; fixed/percent uses the
     * stored length.  CSS width applies to the content box.
     *
     * We honour explicit widths LITERALLY, even when they exceed
     * the parent's available space.  Real browsers do the same:
     * `width: 320px` is 320 px regardless of the viewport, and the
     * page becomes horizontally scrollable.  M63's GUI renderer
     * supports horizontal scrolling, so a fixed-width column on
     * a site like plaintextworld.com (three 320-px tables in a
     * row, total ~960 px) keeps its columns the right shape and
     * the user pans to see the rightmost one when the window is
     * narrower than 960 px.
     *
     * `width: auto` (the default for blocks) still fills the
     * parent's content_w, so paragraphs, headings, etc. continue
     * to reflow as the window resizes. */
    int avail = containing_w - ml - mr - bl - br - pl - pr;
    if (avail < 0) avail = 0;
    int content_w = avail;
    if (st) {
        if (st->width.unit != LAY_LEN_AUTO) {
            content_w = layout_len_px(st->width, containing_w, own_fs, root_fs, avail);
            if (content_w < 0) content_w = 0;
        } else if (box->dom && box->dom->tag &&
                   lay_streq_ci(box->dom->tag, "table")) {
            /* CSS 2.1 §17.5.2 / shrink-to-fit:
             * `<table>` with `width: auto` doesn't fill its
             * container the way a regular block does — it sizes
             * itself to its intrinsic max-content width (clamped
             * to the available width as an upper bound).  A
             * stretched-to-fill table has no slack to distribute,
             * so without this branch a `<center>` /
             * `<div align="center">` wrapping a multi-card table
             * (e.g. plaintextworld.com) renders flush-left.
             *
             * We only run shrink-to-fit when the table is a
             * descendant of a centring container.  This is a
             * deliberate scope narrowing:
             *
             *   - Real browsers shrink-to-fit ALL auto-width
             *     tables.  Our `layout_intrinsic_max_w` is an
             *     approximation (max-content per <br>-delimited
             *     segment); it tracks reality well enough for
             *     centring but mis-estimates in edge cases — e.g.
             *     a `<td>` of unbroken whitespace-separated text
             *     with no `<br>` reports max-content as the full
             *     length of the line.  Limiting the algorithm to
             *     the cases where centring needs it avoids
             *     paying that cost on pages that don't.
             *
             *   - For plaintextworld.com the inner card tables
             *     pick up natural widths from their cells'
             *     longest <br>-delimited line (~280-300 px); the
             *     outer table sums those across its three
             *     columns plus gutters and lands near 960 px,
             *     matching what real browsers compute from
             *     `.quarterlist { width: 320px }` in the
             *     external stylesheet (which we don't fetch). */
            int has_center_ancestor = 0;
            for (struct layout_box *p = box->parent; p; p = p->parent) {
                if (p->style && p->style->center_block_children) {
                    has_center_ancestor = 1;
                    break;
                }
            }
            if (has_center_ancestor) {
                int max_content = layout_intrinsic_max_w(box, root_fs, 0);
                int target = max_content - bl - pl - br - pr;
                if (target < 0)     target = 0;
                if (target > avail) target = avail;
                if (target > 0)     content_w = target;
            }
        }
    }

    /* CSS 2.1 §10.3.3: if the box has an explicit (non-auto) width
     * and both `margin-left` and `margin-right` are `auto`, the
     * extra space is distributed equally — i.e. the block is
     * centred in its containing block.  This is what `margin: 0
     * auto` does in real browsers and is also how
     * `<table align="center">` gets centred via the
     * presentational-CSS path above.
     *
     * We deliberately only apply the centring split when width is
     * fixed — for `width: auto` boxes there is no slack to
     * distribute (content_w just expanded to fill avail) and the
     * box always wants the full containing width.
     *
     * The order matters: we resolved `ml`/`mr` as 0 for AUTO above
     * (so the width math saw the full containing width); now we
     * retro-actively split the slack between them. */
    if (st &&
        st->margin[LAY_LEFT].unit  == LAY_LEN_AUTO &&
        st->margin[LAY_RIGHT].unit == LAY_LEN_AUTO &&
        st->width.unit != LAY_LEN_AUTO) {
        int slack = containing_w - bl - br - pl - pr - content_w;
        if (slack > 0) {
            ml = slack / 2;
            mr = slack - ml;
        }
    }

    int box_x = x + ml;
    int box_y = y;
    int border_x = box_x;
    int border_y = box_y;
    int content_x = border_x + bl + pl;
    int content_y = border_y + bt + pt;

    /* Recurse into children */
    int content_h = layout_container_children(box, content_x, content_y,
                                              content_w, root_fs);

    /* Resolve height: auto = content_h; fixed/percent uses stored. */
    int height_h = content_h;
    if (st && st->height.unit != LAY_LEN_AUTO) {
        height_h = layout_len_px(st->height, 0 /* containing height not tracked */,
                                  own_fs, root_fs, content_h);
        if (height_h < content_h) height_h = content_h;     /* no overflow clipping */
    }

    int outer_w = bl + pl + content_w + pr + br;
    int outer_h = bt + pt + height_h + pb + bb;

    box->x = border_x;
    box->y = border_y;
    box->w = outer_w;
    box->h = outer_h;

    return outer_h;
}

/* Public entry: lay out the document at the given viewport width.
 * Stamps absolute pixel positions onto every box and updates
 * d->doc_width_px and d->doc_height_px. */
static inline int layout_run(struct layout_doc *d)
{
    if (!d->root_box) return -1;
    /* Root box is the implicit BLOCK around the document.  It
     * has no margins/padding/border itself; it just frames the
     * viewport. */
    int root_fs = d->root_style.font_size_px;
    if (root_fs <= 0) root_fs = LAYOUT_BASE_FONT_PX;
    d->engine.root_font_size_px = root_fs;

    int h = layout_container_children(d->root_box,
                                       0, 0, d->viewport_px, root_fs);
    d->root_box->x = 0; d->root_box->y = 0;
    d->root_box->w = d->viewport_px;
    d->root_box->h = h;

    /* Compute the document's actual width: the maximum right-edge
     * of any laid-out box.  Boxes with explicit pixel widths can
     * exceed the viewport, in which case the document is wider
     * than the window and the renderer needs to surface that to
     * the user (horizontal scrollbar in M63).  Walk the box tree
     * iteratively to keep the user-stack budget under control on
     * deeply nested HTML4 table layouts. */
    int max_right = d->viewport_px;
    {
        /* Iterative DFS using an explicit malloc'd stack.  Falls
         * back to viewport_px on OOM (the page just won't horizontal-
         * scroll). */
        int cap = 64, top = 0;
        struct layout_box **stk =
            (struct layout_box **)malloc(sizeof(*stk) * (size_t)cap);
        if (stk) {
            stk[top++] = d->root_box;
            while (top > 0) {
                struct layout_box *b = stk[--top];
                int right = b->x + b->w;
                if (right > max_right) max_right = right;
                for (struct layout_box *c = b->first_child;
                     c; c = c->next_sibling) {
                    if (top >= cap) {
                        int new_cap = cap * 2;
                        struct layout_box **n =
                            (struct layout_box **)malloc(sizeof(*n) * (size_t)new_cap);
                        if (!n) { top = 0; break; }
                        for (int i = 0; i < top; i++) n[i] = stk[i];
                        free(stk); stk = n; cap = new_cap;
                    }
                    stk[top++] = c;
                }
            }
            free(stk);
        }
    }

    d->doc_width_px  = max_right;
    d->doc_height_px = h;
    return 0;
}

/* ============================================================
 *   PART 7 — paint command stream
 * ============================================================ */

enum layout_paint_kind {
    LAY_PAINT_RECT      = 1,    /* solid filled rectangle (bg/border) */
    LAY_PAINT_TEXT      = 2,    /* glyph run */
    LAY_PAINT_UNDERLINE = 3,    /* horizontal rule beneath text */
    LAY_PAINT_IMAGE     = 4,    /* BGRA blit (decoded <img> pixels) */
};

struct layout_paint_cmd {
    int kind;
    int x, y, w, h;
    layout_color_t color;
    const struct dom_node *dom;    /* source node for this paint item */
    /* TEXT only */
    const char *text;       /* points into the box's text buffer */
    int         text_len;
    int         font_size_px;
    int         font_weight;    /* LAY_FW_NORMAL or LAY_FW_BOLD */
    int         font_style;     /* LAY_FS_NORMAL or LAY_FS_ITALIC */
    /* IMAGE only — borrowed pointer into the browser's image cache.
     * `image_w` / `image_h` are the PNG's intrinsic dimensions; the
     * blitter clips to (x, y, w, h). */
    const void *image_pixels;
    int         image_w;
    int         image_h;
};

struct layout_paint_buf {
    struct layout_paint_cmd *cmds;
    int                      n;
    int                      cap;
};

static inline void layout_paint_buf_init(struct layout_paint_buf *p)
{
    p->cmds = 0; p->n = 0; p->cap = 0;
}

static inline void layout_paint_buf_destroy(struct layout_paint_buf *p)
{
    if (p->cmds) free(p->cmds);
    p->cmds = 0; p->n = p->cap = 0;
}

static inline int layout_paint_push(struct layout_paint_buf *p, struct layout_paint_cmd c)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        struct layout_paint_cmd *nb = (struct layout_paint_cmd *)malloc((size_t)nc * sizeof(*nb));
        if (!nb) return -1;
        for (int i = 0; i < p->n; i++) nb[i] = p->cmds[i];
        if (p->cmds) free(p->cmds);
        p->cmds = nb; p->cap = nc;
    }
    p->cmds[p->n++] = c;
    return 0;
}

/* Walk the box tree depth-first and emit paint commands in
 * back-to-front order: first the background of each box, then
 * its border, then descendants, then any text painted by the
 * box itself. */
static inline void layout_paint_box(struct layout_box *b, struct layout_paint_buf *out)
{
    if (!b) return;
    struct layout_computed *st = b->style;

    /* Background (solid colour, only if not transparent) */
    if (st && (st->background >> 24) != 0 && b->w > 0 && b->h > 0 &&
        b->kind != LAY_BOX_TEXT && b->kind != LAY_BOX_BR) {
        struct layout_paint_cmd c;
        c.kind = LAY_PAINT_RECT;
        c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
        c.color = st->background;
        c.dom = b->dom;
        c.text = 0; c.text_len = 0;
        c.font_size_px = 0; c.font_weight = 0; c.font_style = 0;
        c.image_pixels = 0; c.image_w = 0; c.image_h = 0;
        layout_paint_push(out, c);
    }

    /* Border (four sides, each as its own rectangle) */
    if (st && b->kind != LAY_BOX_TEXT && b->kind != LAY_BOX_BR) {
        for (int side = 0; side < 4; side++) {
            int t = st->border_px[side];
            if (t <= 0) continue;
            struct layout_paint_cmd c;
            c.kind = LAY_PAINT_RECT;
            c.color = st->border_color[side];
            c.dom = b->dom;
            c.text = 0; c.text_len = 0;
            c.font_size_px = 0; c.font_weight = 0; c.font_style = 0;
            c.image_pixels = 0; c.image_w = 0; c.image_h = 0;
            switch (side) {
            case LAY_TOP:
                c.x = b->x; c.y = b->y; c.w = b->w; c.h = t; break;
            case LAY_RIGHT:
                c.x = b->x + b->w - t; c.y = b->y; c.w = t; c.h = b->h; break;
            case LAY_BOT:
                c.x = b->x; c.y = b->y + b->h - t; c.w = b->w; c.h = t; break;
            case LAY_LEFT:
                c.x = b->x; c.y = b->y; c.w = t; c.h = b->h; break;
            }
            layout_paint_push(out, c);
        }
    }

    /* Children before own text (text is always at this box's
     * level for TEXT kind; no other kinds have own text). */
    for (struct layout_box *c = b->first_child; c; c = c->next_sibling)
        layout_paint_box(c, out);

    if ((b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_BULLET) &&
        b->text && b->text_len > 0 && st) {
        struct layout_paint_cmd c;
        c.kind = LAY_PAINT_TEXT;
        c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
        c.color = st->color;
        c.dom = b->dom;
        c.text = b->text; c.text_len = b->text_len;
        c.font_size_px = st->font_size_px;
        c.font_weight = st->font_weight;
        c.font_style  = st->font_style;
        c.image_pixels = 0; c.image_w = 0; c.image_h = 0;
        layout_paint_push(out, c);
        /* underline rule if requested.  Position it just below the
         * actual rendered glyph.  The kernel font is fixed 8x16 px
         * regardless of the requested font-size, so the glyph always
         * occupies y..y+max(fs,16); using `font_size_px` alone would
         * place the underline mid-glyph for any small font (e.g.
         * Hacker News' `.subtext { font-size: 7pt }` was rendering
         * the rule through the middle of "9 hours ago" because the
         * glyph is still 16 px tall but layout asked for fs=9).
         * Clamp into the line box so we never paint outside it. */
        if (st->text_decoration & LAY_TD_UNDERLINE) {
            struct layout_paint_cmd u;
            u.kind = LAY_PAINT_UNDERLINE;
            int gw = layout_glyph_width(st->font_size_px);
            int glyph_h = st->font_size_px;
            if (glyph_h < 16) glyph_h = 16;     /* font is fixed 8x16 */
            int rule_y = b->y + glyph_h;
            int max_y  = b->y + b->h - 1;
            if (rule_y > max_y) rule_y = max_y;
            u.x = b->x; u.y = rule_y;
            u.w = b->text_len * gw;
            u.h = (st->font_size_px / 12) > 0 ? (st->font_size_px / 12) : 1;
            u.color = st->color;
            u.dom = b->dom;
            u.text = 0; u.text_len = 0;
            u.font_size_px = 0; u.font_weight = 0; u.font_style = 0;
            u.image_pixels = 0; u.image_w = 0; u.image_h = 0;
            layout_paint_push(out, u);
        }
    }

    /* For replaced elements, paint either:
     *   * a LAY_PAINT_IMAGE command if the browser has decoded
     *     pixels for this element (set b->replaced_pixels), OR
     *   * a placeholder grey rectangle + alt text overlay if
     *     pixels are not (yet) available (decode failed, asset
     *     missing, src couldn't be fetched, ...).
     *
     * The fall-back is the same one users see in classic browsers
     * when an image 404s — a useful UX even when the failure is
     * permanent. */
    if (b->kind == LAY_BOX_REPLACED && b->w > 0 && b->h > 0) {
        if (b->replaced_pixels && b->replaced_pixels_w > 0 &&
            b->replaced_pixels_h > 0) {
            struct layout_paint_cmd img;
            img.kind = LAY_PAINT_IMAGE;
            img.x = b->x; img.y = b->y; img.w = b->w; img.h = b->h;
            img.color = 0;
            img.dom = b->dom;
            img.text = 0; img.text_len = 0;
            img.font_size_px = 0; img.font_weight = 0; img.font_style = 0;
            img.image_pixels = b->replaced_pixels;
            img.image_w = b->replaced_pixels_w;
            img.image_h = b->replaced_pixels_h;
            layout_paint_push(out, img);
        } else {
            struct layout_paint_cmd c;
            c.kind = LAY_PAINT_RECT;
            c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
            c.color = 0xFFE0E0E0u;     /* light grey */
            c.dom = b->dom;
            c.text = 0; c.text_len = 0;
            c.font_size_px = 0; c.font_weight = 0; c.font_style = 0;
            c.image_pixels = 0; c.image_w = 0; c.image_h = 0;
            layout_paint_push(out, c);
            if (b->replaced_alt && b->replaced_alt[0]) {
                struct layout_paint_cmd t;
                t.kind = LAY_PAINT_TEXT;
                t.x = b->x + 2; t.y = b->y + 2;
                t.w = b->w - 4; t.h = b->h - 4;
                t.color = 0xFF606060u;
                t.dom = b->dom;
                t.text = b->replaced_alt;
                t.text_len = lay_strlen(b->replaced_alt);
                t.font_size_px = st ? st->font_size_px : LAYOUT_BASE_FONT_PX;
                t.font_weight = LAY_FW_NORMAL; t.font_style = LAY_FS_NORMAL;
                t.image_pixels = 0; t.image_w = 0; t.image_h = 0;
                layout_paint_push(out, t);
            }
        }
    }
}

static inline void layout_paint_collect(struct layout_doc *d, struct layout_paint_buf *out)
{
    layout_paint_buf_init(out);
    layout_paint_box(d->root_box, out);
}

/* ============================================================
 *   PART 8 — public API
 * ============================================================ */

/* Convenience: build a layout_doc from a DOM tree + an optional
 * author stylesheet string, then run the layout pass.
 *
 *   - `dom` is the root DOM node (DOCUMENT or ELEMENT).
 *   - `author_css` / `author_css_len` is the concatenation of all
 *     <style> blocks (and any external sheets the caller has
 *     already fetched).  Pass NULL/0 for none.
 *   - `viewport_px` is the available content width.
 *
 * On return, `d` is populated.  Caller calls layout_doc_destroy
 * to free everything when done. */
static inline int layout_build_and_run(struct layout_doc *d,
                                        const struct dom_node *dom,
                                        const char *author_css, int author_css_len,
                                        int viewport_px)
{
    layout_doc_init(d, viewport_px);

    /* Install UA stylesheet (always). */
    {
        struct css_stylesheet *ss = (struct css_stylesheet *)malloc(sizeof(*ss));
        if (!ss) { layout_doc_destroy(d); return -1; }
        css_init(ss);
        int ua_len = lay_strlen(layout_ua_stylesheet);
        css_parse(ss, layout_ua_stylesheet, (size_t)ua_len);
        layout_engine_add(&d->engine, ss, LAY_ORIG_UA, 1);
    }

    /* Author stylesheet (if any). */
    if (author_css && author_css_len > 0) {
        struct css_stylesheet *ss = (struct css_stylesheet *)malloc(sizeof(*ss));
        if (!ss) { layout_doc_destroy(d); return -1; }
        css_init(ss);
        css_parse(ss, author_css, (size_t)author_css_len);
        layout_engine_add(&d->engine, ss, LAY_ORIG_AUTHOR, 1);
    }

    /* User override stylesheet (always, sits above author CSS).
     * Restores UA defaults the page tried to remove. */
    {
        struct css_stylesheet *ss = (struct css_stylesheet *)malloc(sizeof(*ss));
        if (!ss) { layout_doc_destroy(d); return -1; }
        css_init(ss);
        int u_len = lay_strlen(layout_user_override_stylesheet);
        css_parse(ss, layout_user_override_stylesheet, (size_t)u_len);
        layout_engine_add(&d->engine, ss, LAY_ORIG_USER, 1);
    }

    /* Build box tree */
    d->root_box = layout_build_subtree(d, dom, &d->root_style);
    if (!d->root_box) { layout_doc_destroy(d); return -1; }

    /* Run layout */
    return layout_run(d);
}

/* Walk the DOM looking for inline <style> blocks; concatenate
 * them into a freshly-malloc'd buffer.  Returns NULL if no
 * style blocks are found.  Caller free()s. */
static inline char *layout_collect_inline_styles(const struct dom_node *root, int *out_len)
{
    if (out_len) *out_len = 0;
    /* Two-pass: count, then copy. */
    int total = 0;
    /* simple recursive walker via stack allocation */
    /* count */
    {
        const struct dom_node *stack[256];
        int top = 0;
        stack[top++] = root;
        while (top > 0) {
            const struct dom_node *n = stack[--top];
            if (!n) continue;
            if (n->type == DOM_NODE_ELEMENT && lay_streq_ci(n->tag, "style")) {
                for (struct dom_node *c = n->first_child; c; c = c->next_sibling) {
                    if (c->type == DOM_NODE_TEXT && c->text)
                        total += (int)c->text_len + 1;
                }
            }
            for (struct dom_node *c = n->first_child; c && top < 256; c = c->next_sibling) {
                stack[top++] = c;
            }
        }
    }
    if (total == 0) return 0;
    char *buf = (char *)malloc((size_t)total + 1);
    if (!buf) return 0;
    int j = 0;
    {
        const struct dom_node *stack[256];
        int top = 0;
        stack[top++] = root;
        while (top > 0) {
            const struct dom_node *n = stack[--top];
            if (!n) continue;
            if (n->type == DOM_NODE_ELEMENT && lay_streq_ci(n->tag, "style")) {
                for (struct dom_node *c = n->first_child; c; c = c->next_sibling) {
                    if (c->type == DOM_NODE_TEXT && c->text) {
                        for (size_t k = 0; k < c->text_len; k++) buf[j++] = c->text[k];
                        buf[j++] = '\n';
                    }
                }
            }
            for (struct dom_node *c = n->first_child; c && top < 256; c = c->next_sibling) {
                stack[top++] = c;
            }
        }
    }
    buf[j] = 0;
    if (out_len) *out_len = j;
    return buf;
}

#endif /* USER_LAYOUT_H */
