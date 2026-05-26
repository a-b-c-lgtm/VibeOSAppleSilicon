/*
 * userspace/cssparse/cssparse.c — milestone-61 CSS-parser driver.
 *
 * Reads a CSS file path from argv[1] (or "/mnt/test.css" if missing),
 * slurps it into a heap buffer, runs `css.h`'s parser over it, and
 * prints one indented block per parsed rule:
 *
 *   [STYLESHEET] N rules, M decls, K bytes input
 *   [RULE 0] selectors=1 specificity=0
 *     [SEL] *
 *     [DECL] box-sizing: "border-box"
 *   [RULE 1] selectors=1 specificity=1
 *     [SEL] body
 *     [DECL] background: "white"
 *     [DECL] color: "black"
 *     ...
 *
 * If a SECOND argument is given (e.g. `cssparse /mnt/test.css
 * /mnt/test.html`) we additionally parse the HTML file with
 * html.h+dom.h and run the matcher: for each element node in the
 * DOM, we print which rule indices match it.
 *
 *   [MATCH] <p class="intro" id="lead"> matches rules 0, 1 (body p),
 *           5 (p.intro), 6 (p.intro#lead), 7 (#lead)
 */
#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/html.h"
#include "../libc/dom.h"
#include "../libc/css.h"

/* ---------- file slurp ---------- */

static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("cssparse: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("cssparse: oom\n"); close(fd); return 0; }
    char tmp[1024];
    long n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)n) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) { printf("cssparse: oom (grow)\n"); free(buf); close(fd); return 0; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf);
            buf = nb; cap = newcap;
        }
        for (long i = 0; i < n; i++) buf[len + (size_t)i] = tmp[i];
        len += (size_t)n;
    }
    close(fd);
    if (n < 0) { printf("cssparse: read error %ld\n", n); free(buf); return 0; }
    *out_len = len;
    return buf;
}

/* ---------- pretty-print rules ---------- */

static void print_simple(struct css_simple *s)
{
    switch (s->kind) {
    case CSS_SIMPLE_UNIVERSAL: printf("*"); break;
    case CSS_SIMPLE_TYPE:      printf("%s", s->name); break;
    case CSS_SIMPLE_CLASS:     printf(".%s", s->name); break;
    case CSS_SIMPLE_ID:        printf("#%s", s->name); break;
    default:                   printf("?"); break;
    }
}

static void print_chain(struct css_compound *cmp)
{
    /* Storage is right-to-left.  Walk to the leftmost end first,
     * collect into a tiny stack, then print left-to-right with the
     * combinator BETWEEN each pair (which lives on the right-hand
     * compound in our model). */
    struct css_compound *stk[32];
    int top = 0;
    for (struct css_compound *p = cmp; p && top < 32; p = p->next) stk[top++] = p;
    /* Print from leftmost (top-1) down to rightmost (0). */
    for (int i = top - 1; i >= 0; i--) {
        for (struct css_simple *s = stk[i]->simples; s; s = s->next)
            print_simple(s);
        if (i > 0) {
            int comb = stk[i - 1]->combinator;
            if (comb == CSS_COMB_CHILD)            printf(" > ");
            else                                    printf(" ");
        }
    }
}

static void print_quoted_z(const char *s)
{
    printf("\"");
    if (s) {
        for (size_t i = 0; s[i]; i++) {
            char c = s[i];
            switch (c) {
            case '\n': printf("\\n"); break;
            case '\t': printf("\\t"); break;
            case '\r': printf("\\r"); break;
            case '\\': printf("\\\\"); break;
            case '"':  printf("\\\""); break;
            default:
                if ((unsigned char)c < 0x20) printf("?");
                else                          printf("%c", c);
            }
        }
    }
    printf("\"");
}

static void print_rule(struct css_rule *r)
{
    int n_sel = 0, max_spec = 0;
    for (struct css_selector *s = r->selectors; s; s = s->next) {
        n_sel++;
        int sp = css_chain_specificity(s->chain);
        if (sp > max_spec) max_spec = sp;
    }
    printf("[RULE %d] selectors=%d specificity=%d\n",
           r->source_order, n_sel, max_spec);
    for (struct css_selector *s = r->selectors; s; s = s->next) {
        printf("  [SEL] ");
        print_chain(s->chain);
        printf("\n");
    }
    for (struct css_decl *d = r->decls; d; d = d->next) {
        printf("  [DECL] %s: ", d->property);
        print_quoted_z(d->value);
        printf("\n");
    }
}

/* ---------- matching against a DOM ---------- */

static void describe_node(const struct dom_node *n)
{
    printf("<%s", n->tag ? n->tag : "?");
    for (struct dom_attr *a = n->attrs; a; a = a->next) {
        printf(" %s=", a->name ? a->name : "?");
        print_quoted_z(a->value);
    }
    printf(">");
}

static void run_matches(const struct css_stylesheet *ss,
                        const struct dom_node *n)
{
    if (!n) return;
    if (n->type == DOM_NODE_ELEMENT) {
        int hits[64]; int n_hits = 0;
        for (struct css_rule *r = ss->rules; r; r = r->next) {
            if (n_hits >= 64) break;
            if (css_rule_matches(r, n)) hits[n_hits++] = r->source_order;
        }
        if (n_hits > 0) {
            printf("[MATCH] ");
            describe_node(n);
            printf(" -> rules");
            for (int i = 0; i < n_hits; i++) printf(" %d", hits[i]);
            printf("\n");
        }
    }
    for (struct dom_node *c = n->first_child; c; c = c->next_sibling)
        run_matches(ss, c);
}

int main(int argc, char **argv)
{
    const char *css_path  = "/mnt/test.css";
    const char *html_path = NULL;
    if (argc >= 2 && argv[1] && argv[1][0]) css_path  = argv[1];
    if (argc >= 3 && argv[2] && argv[2][0]) html_path = argv[2];

    /* --- parse the stylesheet --- */
    size_t css_len = 0;
    char  *css_buf = slurp(css_path, &css_len);
    if (!css_buf) return 1;

    struct css_stylesheet ss;
    css_init(&ss);
    int rc = css_parse(&ss, css_buf, css_len);
    if (rc < 0) printf("cssparse: oom during parse (partial result follows)\n");

    printf("[STYLESHEET] %lu rules, %lu decls, %lu bytes input\n",
           (unsigned long)ss.n_rules,
           (unsigned long)ss.n_decls,
           (unsigned long)css_len);
    for (struct css_rule *r = ss.rules; r; r = r->next) print_rule(r);

    /* --- optional: build DOM and run matcher --- */
    if (html_path) {
        size_t html_len = 0;
        char  *html_buf = slurp(html_path, &html_len);
        if (!html_buf) { css_destroy(&ss); free(css_buf); return 1; }

        struct html_tokenizer tz;
        html_tok_init(&tz, html_buf, html_len);

        struct html_token *scratch = (struct html_token *)malloc(sizeof(*scratch));
        if (!scratch) {
            printf("cssparse: oom (scratch)\n");
            free(html_buf); css_destroy(&ss); free(css_buf); return 1;
        }

        struct dom dom;
        if (dom_init(&dom) < 0) {
            printf("cssparse: oom (dom_init)\n");
            free(scratch); free(html_buf); css_destroy(&ss); free(css_buf); return 1;
        }
        int drc = dom_build(&dom, &tz, scratch);
        if (drc < 0) printf("cssparse: dom_build error rc=%d (continuing)\n", drc);

        run_matches(&ss, dom_root(&dom));

        dom_destroy(&dom);
        free(scratch);
        free(html_buf);
    }

    css_destroy(&ss);
    free(css_buf);
    return rc < 0 ? 1 : 0;
}
