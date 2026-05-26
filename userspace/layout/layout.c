/*
 * userspace/layout/layout.c — milestone-62 layout driver.
 *
 * Reads an HTML file (default /mnt/test_layout.html), runs:
 *
 *   html.h tokenizer  -> dom.h tree builder
 *   layout.h cascade  (UA + author <style> + inline style="...")
 *   layout.h box tree
 *   layout.h block + inline layout
 *   layout.h paint command stream
 *
 * and prints one line per paint command in painting order, plus
 * a summary header.  This is the M62 contribution to the
 * eventual /bin/browser pipeline (M63 will replace stdout with
 * gui_draw_text / gui_fill_rect calls).
 *
 * Output format (parsable by scripts/test_layout.py):
 *
 *   [DOC] viewport=W height=H boxes=N paints=M
 *   [BOX#i] kind=BLOCK x=.. y=.. w=.. h=..  tag=p
 *   [PAINT#i] RECT x y w h color=#AARRGGBB
 *   [PAINT#i] TEXT x y w h color=#AARRGGBB fs=16 fw=400 fst=0 "the text"
 *   [PAINT#i] UNDERLINE x y w h color=#AARRGGBB
 *
 * Optional second arg: viewport width (default 800).
 */
#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/html.h"
#include "../libc/dom.h"
#include "../libc/css.h"
#include "../libc/layout.h"

/* ---------- file slurp ---------- */

static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("layout: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("layout: oom\n"); close(fd); return 0; }
    char tmp[1024];
    long n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)n) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) { printf("layout: oom (grow)\n"); free(buf); close(fd); return 0; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf);
            buf = nb; cap = newcap;
        }
        for (long i = 0; i < n; i++) buf[len + (size_t)i] = tmp[i];
        len += (size_t)n;
    }
    close(fd);
    if (n < 0) { printf("layout: read error %ld\n", n); free(buf); return 0; }
    *out_len = len;
    return buf;
}

/* ---------- tiny atoi (no <stdlib.h>) ---------- */
static int my_atoi(const char *s)
{
    int v = 0; int sign = 1;
    if (!s) return 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

/* ---------- box-tree pretty printer ---------- */
static const char *kind_name(int k)
{
    switch (k) {
    case LAY_BOX_BLOCK:      return "BLOCK";
    case LAY_BOX_INLINE:     return "INLINE";
    case LAY_BOX_TEXT:       return "TEXT";
    case LAY_BOX_ANON_BLOCK: return "ANON_BLOCK";
    case LAY_BOX_REPLACED:   return "REPLACED";
    case LAY_BOX_BR:         return "BR";
    case LAY_BOX_BULLET:     return "BULLET";
    }
    return "?";
}

static int g_box_index = 0;
static int g_box_count = 0;

static void count_boxes(struct layout_box *b) {
    if (!b) return;
    g_box_count++;
    for (struct layout_box *c = b->first_child; c; c = c->next_sibling)
        count_boxes(c);
}

static void print_quoted(const char *s, int n)
{
    printf("\"");
    if (s) {
        for (int i = 0; i < n; i++) {
            char c = s[i];
            if (c == '"')  printf("\\\"");
            else if (c == '\\') printf("\\\\");
            else if (c == '\n') printf("\\n");
            else if (c == '\t') printf("\\t");
            else if (c == '\r') printf("\\r");
            else if ((unsigned char)c < 0x20) printf("?");
            else printf("%c", c);
        }
    }
    printf("\"");
}

static void print_box(struct layout_box *b)
{
    if (!b) return;
    int idx = g_box_index++;
    const char *tag = "?";
    if (b->dom && b->dom->type == DOM_NODE_ELEMENT && b->dom->tag) tag = b->dom->tag;
    else if (b->dom && b->dom->type == DOM_NODE_TEXT) tag = "#text";
    else if (b->dom && b->dom->type == DOM_NODE_DOCUMENT) tag = "#doc";
    else if (b->kind == LAY_BOX_ANON_BLOCK) tag = "#anon";
    printf("[BOX#%d] kind=%s x=%d y=%d w=%d h=%d tag=%s",
           idx, kind_name(b->kind), b->x, b->y, b->w, b->h, tag);
    if (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_BULLET) {
        printf(" text=");
        print_quoted(b->text, b->text_len);
    }
    if (b->kind == LAY_BOX_REPLACED) {
        printf(" rw=%d rh=%d", b->replaced_w, b->replaced_h);
    }
    printf("\n");
    for (struct layout_box *c = b->first_child; c; c = c->next_sibling)
        print_box(c);
}

static void print_color(unsigned int c) {
    /* Print as 0xAARRGGBB so the harness can match exact pixels. */
    static const char *hex = "0123456789ABCDEF";
    char out[9];
    for (int i = 7; i >= 0; i--) { out[i] = hex[c & 0xF]; c >>= 4; }
    out[8] = 0;
    printf("#%s", out);
}

static void print_paints(struct layout_paint_buf *p)
{
    for (int i = 0; i < p->n; i++) {
        struct layout_paint_cmd *c = &p->cmds[i];
        const char *kind = c->kind == LAY_PAINT_RECT ? "RECT" :
                           c->kind == LAY_PAINT_TEXT ? "TEXT" :
                           c->kind == LAY_PAINT_UNDERLINE ? "UNDERLINE" : "?";
        printf("[PAINT#%d] %s x=%d y=%d w=%d h=%d color=", i, kind,
               c->x, c->y, c->w, c->h);
        print_color(c->color);
        if (c->kind == LAY_PAINT_TEXT) {
            printf(" fs=%d fw=%d fst=%d ", c->font_size_px, c->font_weight, c->font_style);
            print_quoted(c->text, c->text_len);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *html_path = "/mnt/test_layout.html";
    int viewport = 800;
    if (argc >= 2 && argv[1] && argv[1][0]) html_path = argv[1];
    if (argc >= 3 && argv[2] && argv[2][0]) viewport = my_atoi(argv[2]);
    if (viewport < 64) viewport = 64;

    /* --- slurp + tokenize + DOM --- */
    size_t html_len = 0;
    char *html_buf = slurp(html_path, &html_len);
    if (!html_buf) return 1;

    struct html_tokenizer tz;
    html_tok_init(&tz, html_buf, html_len);

    struct html_token *scratch = (struct html_token *)malloc(sizeof(*scratch));
    if (!scratch) { printf("layout: oom (scratch)\n"); free(html_buf); return 1; }

    struct dom dom;
    if (dom_init(&dom) < 0) {
        printf("layout: oom (dom_init)\n");
        free(scratch); free(html_buf); return 1;
    }
    int drc = dom_build(&dom, &tz, scratch);
    if (drc < 0) printf("layout: dom_build error rc=%d (continuing)\n", drc);

    /* --- author CSS from inline <style> blocks --- */
    int author_len = 0;
    char *author_css = layout_collect_inline_styles(dom_root(&dom), &author_len);

    /* --- run layout --- */
    struct layout_doc doc;
    int rc = layout_build_and_run(&doc, dom_root(&dom),
                                   author_css, author_len, viewport);
    if (rc < 0) {
        printf("layout: build_and_run failed\n");
        if (author_css) free(author_css);
        dom_destroy(&dom);
        free(scratch); free(html_buf);
        return 1;
    }

    /* --- emit paint stream --- */
    struct layout_paint_buf pb;
    layout_paint_collect(&doc, &pb);

    g_box_count = 0;
    count_boxes(doc.root_box);

    printf("[DOC] viewport=%d height=%d boxes=%d paints=%d\n",
           doc.doc_width_px, doc.doc_height_px, g_box_count, pb.n);

    g_box_index = 0;
    print_box(doc.root_box);
    print_paints(&pb);

    layout_paint_buf_destroy(&pb);
    layout_doc_destroy(&doc);

    if (author_css) free(author_css);
    dom_destroy(&dom);
    free(scratch);
    free(html_buf);
    return 0;
}
