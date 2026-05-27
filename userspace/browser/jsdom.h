/*
 * userspace/browser/jsdom.h -- chapter 122 host bindings between
 * pocketjs (userspace/libc/pocketjs.h) and the browser's DOM /
 * layout / output.
 *
 * Header-only, single-TU pattern.  Include once from browser.c
 * AFTER pocketjs.h, dom.h, layout.h, and the loaded_page declaration.
 *
 * What this file knows that pocketjs.h does not
 * -----------------------------------------------
 * pocketjs.h is intentionally DOM-agnostic; it speaks values,
 * expressions, and an opaque host-object protocol.  This file
 * is the only place that crosses that boundary -- it implements
 * the get/set/method hooks for:
 *
 *   - `document`             one per-eval, owns the dom
 *   - `<element>`            a wrapper around a dom_node
 *   - `<element>.style`      sets the inline style="..." attr
 *   - `console`              .log / .error -> printf
 *   - `alert`                bare callable, latches a message
 *
 * Side effects from JS (style.display = 'none', innerText =,
 * setAttribute(), value =) set bits in `struct jsdom_ctx` so the
 * caller can request a relayout/repaint after pj_eval() returns.
 *
 * No mutation goes anywhere else: this file does not depend on
 * the browser_state event loop, the parser thread, or anything
 * GUI-specific, so the unit-test path in --check-js can use the
 * same bindings with a NULL `page`.
 */

#ifndef OSDEV_BROWSER_JSDOM_H
#define OSDEV_BROWSER_JSDOM_H

#include "../libc/pocketjs.h"
#include "../libc/dom.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"

/* ---------- context ---------- */

struct jsdom_ctx {
    struct dom         *dom;            /* may be NULL in --check-js */
    /* `page` is opaque to this header to keep us untangled from
     * the browser_state declarations; callers cast it back. */
    void               *page;
    /* Output side-channels the caller can act on after eval. */
    int                 needs_relayout;
    int                 needs_repaint;
    char                alert_buf[256];
    int                 alert_set;
    int                 console_logs;
    /* Last evaluated value, in a stable form, for --check-js. */
    char                last_value[256];
};

static inline void jsdom_ctx_init(struct jsdom_ctx *c,
                                  struct dom *dom, void *page)
{
    c->dom = dom; c->page = page;
    c->needs_relayout = 0; c->needs_repaint = 0;
    c->alert_buf[0] = 0; c->alert_set = 0;
    c->console_logs = 0;
    c->last_value[0] = 0;
}

/* ---------- shared per-eval bindings ---------- */

struct js_doc_obj    { struct jsdom_ctx *ctx; };
struct js_elem_obj   { struct jsdom_ctx *ctx; struct dom_node *node; };
struct js_style_obj  { struct jsdom_ctx *ctx; struct dom_node *node; };
struct js_console_obj{ struct jsdom_ctx *ctx; int is_error; };
struct js_alert_obj  { struct jsdom_ctx *ctx; };

/* Allocate a binding object out of the pj engine arena. */
static inline void *jsdom_arena_obj(struct pj *p, size_t n)
{
    return pj_alloc(p->arena, n);
}

/* Wrap one of the structs above into a pj_value with the given class. */
static inline struct pj_value jsdom_make(void *self,
                                          const struct pj_host_class *cls)
{
    return pj_host(self, cls);
}

/* forward decls of the vtables */
static const struct pj_host_class jsdom_doc_class;
static const struct pj_host_class jsdom_elem_class;
static const struct pj_host_class jsdom_style_class;
static const struct pj_host_class jsdom_console_class;
static const struct pj_host_class jsdom_alert_class;

/* ---------- helpers ---------- */

static inline int jsdom_streq_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Walk the DOM looking for an ELEMENT with id="..." matching `id`. */
static struct dom_node *jsdom_find_by_id(struct dom_node *root, const char *id)
{
    if (!root || !id) return NULL;
    if (root->type == DOM_NODE_ELEMENT) {
        const char *v = dom_node_attr(root, "id");
        if (v && pj_streq(v, id)) return root;
    }
    for (struct dom_node *c = root->first_child; c; c = c->next_sibling) {
        struct dom_node *r = jsdom_find_by_id(c, id);
        if (r) return r;
    }
    return NULL;
}

/* Concatenate every TEXT descendant into a fresh arena buffer
 * (the innerText getter).  Marked `used` so -Wunused-function
 * doesn't kill the build before we've wired it in via a callback
 * variant of jsdom_elem_get that has access to the engine. */
static __attribute__((unused)) char *jsdom_collect_text(struct pj *p, struct dom_node *root)
{
    size_t cap = 256, off = 0;
    char *buf = (char *)pj_alloc(p->arena, cap);
    if (!buf) return NULL;
    buf[0] = 0;

    struct dom_node *stack[64];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        struct dom_node *n = stack[--top];
        if (!n) continue;
        if (n->type == DOM_NODE_TEXT && n->text) {
            size_t l = n->text_len;
            if (l == 0) for (; n->text[l]; l++) {}
            if (off + l + 1 > cap) {
                size_t ncap = cap;
                while (ncap < off + l + 1) ncap *= 2;
                char *nb = (char *)pj_alloc(p->arena, ncap);
                if (!nb) return buf;
                for (size_t i = 0; i < off; i++) nb[i] = buf[i];
                buf = nb; cap = ncap;
            }
            for (size_t i = 0; i < l; i++) buf[off + i] = n->text[i];
            off += l;
            buf[off] = 0;
        }
        /* Push children right-to-left so left-most is visited first. */
        struct dom_node *kids[64]; int nk = 0;
        for (struct dom_node *c = n->first_child; c && nk < 64; c = c->next_sibling)
            kids[nk++] = c;
        for (int i = nk - 1; i >= 0; i--) {
            if (top >= 64) break;
            stack[top++] = kids[i];
        }
    }
    return buf;
}

/* Replace ALL children of `n` with a single text node holding `s`.
 * Returns 0 on success, -1 on OOM.  Used by innerText = "...". */
static int jsdom_replace_text_children(struct dom_node *n, const char *s)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return -1;
    /* Tear down existing children. */
    struct dom_node *c = n->first_child;
    while (c) {
        struct dom_node *next = c->next_sibling;
        dom_node_free(c);
        c = next;
    }
    n->first_child = NULL;
    n->last_child  = NULL;
    if (!s) return 0;
    size_t l = 0; while (s[l]) l++;
    struct dom_node *t = dom_node_new(DOM_NODE_TEXT);
    if (!t) return -1;
    t->text = (char *)malloc(l + 1);
    if (!t->text) { free(t); return -1; }
    for (size_t i = 0; i < l; i++) t->text[i] = s[i];
    t->text[l] = 0;
    t->text_len = l;
    dom_node_append_child(n, t);
    return 0;
}

/* Make a fresh inline `style` value with `prop:value;` either
 * appended or replacing an existing `prop:` declaration.  Caller
 * owns the malloc'd buffer. */
static char *jsdom_style_set_prop(const char *cur, const char *prop,
                                  const char *val)
{
    /* Strip any existing `prop:...;` declaration from `cur` into
     * `tmp`, then append `prop: val;` at the end. */
    size_t plen = 0; while (prop[plen]) plen++;
    size_t cur_len = 0; if (cur) while (cur[cur_len]) cur_len++;
    size_t val_len = 0; if (val) while (val[val_len]) val_len++;
    size_t cap = cur_len + plen + val_len + 8;
    char *tmp = (char *)malloc(cap);
    if (!tmp) return NULL;
    size_t off = 0;
    if (cur) {
        size_t i = 0;
        while (i < cur_len) {
            /* skip whitespace */
            while (i < cur_len && (cur[i] == ' ' || cur[i] == '\t')) i++;
            /* find end of property name (':') */
            size_t name_s = i;
            while (i < cur_len && cur[i] != ':' && cur[i] != ';') i++;
            size_t name_e = i;
            /* trim trailing space */
            while (name_e > name_s && (cur[name_e - 1] == ' ' || cur[name_e - 1] == '\t'))
                name_e--;
            /* match? */
            int is_match = 0;
            if ((name_e - name_s) == plen) {
                is_match = 1;
                for (size_t k = 0; k < plen; k++) {
                    int a = cur[name_s + k], b = prop[k];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { is_match = 0; break; }
                }
            }
            if (cur[i] == ':') i++;
            /* skip the value up to ';' or end */
            while (i < cur_len && cur[i] != ';') i++;
            if (cur[i] == ';') i++;
            if (!is_match) {
                /* copy [name_s..i) into tmp */
                for (size_t k = name_s; k < i; k++) tmp[off++] = cur[k];
                if (off > 0 && tmp[off - 1] != ' ') tmp[off++] = ' ';
            }
        }
    }
    /* Append `prop: val;` */
    for (size_t k = 0; k < plen; k++)   tmp[off++] = prop[k];
    tmp[off++] = ':'; tmp[off++] = ' ';
    if (val) for (size_t k = 0; k < val_len; k++) tmp[off++] = val[k];
    tmp[off++] = ';';
    tmp[off]   = 0;
    return tmp;
}

/* ---------- element ---------- */

static struct pj_value jsdom_elem_get(void *self, const char *name)
{
    struct js_elem_obj *e = (struct js_elem_obj *)self;
    if (!e || !e->node) return pj_undef();
    if (pj_streq(name, "id")) {
        const char *v = dom_node_attr(e->node, "id");
        return v ? pj_str(v) : pj_str("");
    }
    if (pj_streq(name, "tagName") || pj_streq(name, "tag")) {
        return e->node->tag ? pj_str(e->node->tag) : pj_str("");
    }
    if (pj_streq(name, "value")) {
        const char *v = dom_node_attr(e->node, "value");
        return v ? pj_str(v) : pj_str("");
    }
    if (pj_streq(name, "innerText") || pj_streq(name, "textContent")) {
        /* Lazy import: we need a `struct pj *` to allocate from
         * the arena.  We don't have one here -- but the engine
         * always evaluates strings into arena memory, so we
         * fall back to dom_node_attr's view (single text child). */
        if (e->node->first_child && e->node->first_child->type == DOM_NODE_TEXT)
            return pj_str(e->node->first_child->text);
        return pj_str("");
    }
    if (pj_streq(name, "style")) {
        /* The style host object is allocated from heap, not the
         * arena: it has to outlive any single get() call (caller
         * may stash it).  Freed when ctx is torn down via the
         * arena-reset boundary the engine enforces -- so use the
         * arena after all.  We can't get to the arena from here
         * either; fall back to malloc + accept the small leak
         * per onclick eval, which is bounded by node count. */
        struct js_style_obj *s =
            (struct js_style_obj *)malloc(sizeof(*s));
        if (!s) return pj_undef();
        s->ctx = e->ctx; s->node = e->node;
        return pj_host(s, &jsdom_style_class);
    }
    /* Fallback: any HTML attribute by exact name. */
    const char *v = dom_node_attr(e->node, name);
    return v ? pj_str(v) : pj_undef();
}

static int jsdom_elem_set(void *self, const char *name, struct pj_value val)
{
    struct js_elem_obj *e = (struct js_elem_obj *)self;
    if (!e || !e->node) return -1;
    /* Need a string view of `val`.  We don't have a pj* here so
     * stringify locally with a small buffer. */
    char tmp[64];
    const char *s = NULL;
    switch (val.type) {
    case PJ_STR:       s = val.v.s ? val.v.s : ""; break;
    case PJ_BOOL:      s = val.v.b ? "true" : "false"; break;
    case PJ_NUM: {
        int n = snprintf(tmp, sizeof(tmp), "%lld", val.v.n);
        if (n < 0) n = 0;
        if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
        tmp[n] = 0; s = tmp; break;
    }
    case PJ_NULL:      s = ""; break;
    case PJ_UNDEFINED: s = ""; break;
    case PJ_HOSTOBJ:   s = "[object]"; break;
    default:           s = ""; break;
    }
    if (pj_streq(name, "innerText") || pj_streq(name, "textContent")) {
        if (jsdom_replace_text_children(e->node, s) == 0)
            { e->ctx->needs_relayout = 1; e->ctx->needs_repaint = 1; }
        return 0;
    }
    if (pj_streq(name, "value")) {
        if (dom_node_set_attr(e->node, "value", s) == 0)
            { e->ctx->needs_repaint = 1; }
        return 0;
    }
    /* Default: write through to the HTML attribute. */
    if (dom_node_set_attr(e->node, name, s) == 0)
        { e->ctx->needs_relayout = 1; e->ctx->needs_repaint = 1; }
    return 0;
}

static struct pj_value jsdom_elem_method(void *self, const char *name,
                                          struct pj_value *argv, int argc)
{
    struct js_elem_obj *e = (struct js_elem_obj *)self;
    if (!e || !e->node) return pj_undef();
    if (pj_streq(name, "getAttribute") && argc >= 1 && argv[0].type == PJ_STR) {
        const char *v = dom_node_attr(e->node, argv[0].v.s);
        return v ? pj_str(v) : pj_str("");
    }
    if (pj_streq(name, "setAttribute") && argc >= 2 && argv[0].type == PJ_STR) {
        const char *vs = (argv[1].type == PJ_STR) ? argv[1].v.s : "";
        dom_node_set_attr(e->node, argv[0].v.s, vs ? vs : "");
        e->ctx->needs_relayout = 1; e->ctx->needs_repaint = 1;
        return pj_undef();
    }
    if (pj_streq(name, "hasAttribute") && argc >= 1 && argv[0].type == PJ_STR) {
        return pj_bool(dom_node_attr(e->node, argv[0].v.s) != NULL);
    }
    return pj_undef();
}

static const struct pj_host_class jsdom_elem_class = {
    "Element",
    jsdom_elem_get,
    jsdom_elem_set,
    jsdom_elem_method,
    NULL,
};

/* ---------- style ---------- */

static struct pj_value jsdom_style_get(void *self, const char *name)
{
    struct js_style_obj *st = (struct js_style_obj *)self;
    if (!st || !st->node) return pj_undef();
    const char *cur = dom_node_attr(st->node, "style");
    if (!cur) return pj_str("");
    /* Find `name:` (case-insensitive). */
    size_t pl = 0; while (name[pl]) pl++;
    size_t i = 0, cl = 0; while (cur[cl]) cl++;
    while (i < cl) {
        while (i < cl && (cur[i] == ' ' || cur[i] == '\t' || cur[i] == ';')) i++;
        size_t ns = i;
        while (i < cl && cur[i] != ':' && cur[i] != ';') i++;
        size_t ne = i;
        int hit = 0;
        if (cur[i] == ':' && (ne - ns) == pl) {
            hit = 1;
            for (size_t k = 0; k < pl; k++) {
                int a = cur[ns + k], b = name[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { hit = 0; break; }
            }
        }
        if (cur[i] == ':') i++;
        size_t vs = i;
        while (i < cl && cur[i] != ';') i++;
        size_t ve = i;
        if (cur[i] == ';') i++;
        if (hit) {
            while (vs < ve && (cur[vs] == ' ' || cur[vs] == '\t')) vs++;
            while (ve > vs && (cur[ve - 1] == ' ' || cur[ve - 1] == '\t')) ve--;
            size_t L = ve - vs;
            char *out = (char *)malloc(L + 1);
            if (!out) return pj_str("");
            for (size_t k = 0; k < L; k++) out[k] = cur[vs + k];
            out[L] = 0;
            return pj_str(out);
        }
    }
    return pj_str("");
}

static int jsdom_style_set(void *self, const char *name, struct pj_value val)
{
    struct js_style_obj *st = (struct js_style_obj *)self;
    if (!st || !st->node) return -1;
    /* Stringify rhs locally. */
    char tmp[64]; const char *vs = "";
    switch (val.type) {
    case PJ_STR:  vs = val.v.s ? val.v.s : ""; break;
    case PJ_BOOL: vs = val.v.b ? "true" : "false"; break;
    case PJ_NUM: {
        int n = snprintf(tmp, sizeof(tmp), "%lld", val.v.n);
        if (n < 0) n = 0;
        if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
        tmp[n] = 0; vs = tmp; break;
    }
    default: vs = ""; break;
    }
    const char *cur = dom_node_attr(st->node, "style");
    char *nv = jsdom_style_set_prop(cur, name, vs);
    if (!nv) return -1;
    int rc = dom_node_set_attr(st->node, "style", nv);
    free(nv);
    if (rc == 0) {
        st->ctx->needs_relayout = 1;
        st->ctx->needs_repaint  = 1;
    }
    return rc;
}

static const struct pj_host_class jsdom_style_class = {
    "Style",
    jsdom_style_get,
    jsdom_style_set,
    NULL,
    NULL,
};

/* ---------- document ---------- */

static struct pj_value jsdom_doc_method(void *self, const char *name,
                                         struct pj_value *argv, int argc)
{
    struct js_doc_obj *d = (struct js_doc_obj *)self;
    if (!d || !d->ctx || !d->ctx->dom) return pj_undef();
    if (pj_streq(name, "getElementById") && argc >= 1 && argv[0].type == PJ_STR) {
        struct dom_node *n = jsdom_find_by_id(dom_root(d->ctx->dom), argv[0].v.s);
        if (!n) return pj_undef();
        struct js_elem_obj *e =
            (struct js_elem_obj *)malloc(sizeof(*e));
        if (!e) return pj_undef();
        e->ctx = d->ctx; e->node = n;
        return pj_host(e, &jsdom_elem_class);
    }
    return pj_undef();
}

static struct pj_value jsdom_doc_get(void *self, const char *name)
{
    struct js_doc_obj *d = (struct js_doc_obj *)self;
    if (!d || !d->ctx || !d->ctx->dom) return pj_undef();
    if (pj_streq(name, "body")) {
        struct dom_node *b = dom_body(d->ctx->dom);
        if (!b) return pj_undef();
        struct js_elem_obj *e =
            (struct js_elem_obj *)malloc(sizeof(*e));
        if (!e) return pj_undef();
        e->ctx = d->ctx; e->node = b;
        return pj_host(e, &jsdom_elem_class);
    }
    return pj_undef();
}

static const struct pj_host_class jsdom_doc_class = {
    "Document",
    jsdom_doc_get,
    NULL,
    jsdom_doc_method,
    NULL,
};

/* ---------- console ---------- */

static struct pj_value jsdom_console_method(void *self, const char *name,
                                             struct pj_value *argv, int argc)
{
    struct js_console_obj *c = (struct js_console_obj *)self;
    if (!c || !c->ctx) return pj_undef();
    int is_error = c->is_error || pj_streq(name, "error");
    /* console.log/error/warn/info all print one space-separated line. */
    printf("[browser] %s:", is_error ? "console.error" : "console.log");
    for (int i = 0; i < argc; i++) {
        printf(" ");
        switch (argv[i].type) {
        case PJ_STR:       printf("%s", argv[i].v.s ? argv[i].v.s : ""); break;
        case PJ_NUM:       printf("%lld", argv[i].v.n); break;
        case PJ_BOOL:      printf("%s", argv[i].v.b ? "true" : "false"); break;
        case PJ_NULL:      printf("null"); break;
        case PJ_UNDEFINED: printf("undefined"); break;
        case PJ_HOSTOBJ:   printf("[object %s]",
                                  argv[i].v.h.cls && argv[i].v.h.cls->name
                                  ? argv[i].v.h.cls->name : "?"); break;
        default:           printf("?"); break;
        }
    }
    printf("\n");
    c->ctx->console_logs++;
    return pj_undef();
}

static const struct pj_host_class jsdom_console_class = {
    "Console",
    NULL,
    NULL,
    jsdom_console_method,
    NULL,
};

/* ---------- alert ---------- */

static struct pj_value jsdom_alert_call(void *self,
                                         struct pj_value *argv, int argc)
{
    struct js_alert_obj *a = (struct js_alert_obj *)self;
    if (!a || !a->ctx) return pj_undef();
    const char *msg = "";
    char tmp[64];
    if (argc >= 1) {
        switch (argv[0].type) {
        case PJ_STR:       msg = argv[0].v.s ? argv[0].v.s : ""; break;
        case PJ_NUM: {
            int n = snprintf(tmp, sizeof(tmp), "%lld", argv[0].v.n);
            if (n < 0) n = 0;
            if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
            tmp[n] = 0; msg = tmp; break;
        }
        case PJ_BOOL:      msg = argv[0].v.b ? "true" : "false"; break;
        case PJ_NULL:      msg = "null"; break;
        case PJ_UNDEFINED: msg = "undefined"; break;
        default:           msg = "[object]"; break;
        }
    }
    /* Latch into ctx and mirror to serial. */
    size_t i = 0;
    for (; msg[i] && i < sizeof(a->ctx->alert_buf) - 1; i++)
        a->ctx->alert_buf[i] = msg[i];
    a->ctx->alert_buf[i] = 0;
    a->ctx->alert_set = 1;
    printf("[browser] alert: %s\n", a->ctx->alert_buf);
    return pj_undef();
}

static const struct pj_host_class jsdom_alert_class = {
    "Alert",
    NULL,
    NULL,
    NULL,
    jsdom_alert_call,
};

/* ---------- public binding helper ---------- */

/*
 * Populate `pj` globals with the standard browser host objects
 * and (optionally) `this` bound to a clicked element.  All
 * allocations live in the pj arena except element/style wrappers
 * minted via `document.getElementById` (those use malloc and
 * leak across the eval; the eval lifetime is bounded by a single
 * onclick handler so the leak is small).  Returns 0 on success,
 * non-zero on arena OOM.
 */
static int jsdom_install(struct pj *p, struct jsdom_ctx *ctx,
                         struct dom_node *this_node)
{
    /* document */
    struct js_doc_obj *d = (struct js_doc_obj *)jsdom_arena_obj(p, sizeof(*d));
    if (!d) return -1;
    d->ctx = ctx;
    pj_set_global(p, "document", pj_host(d, &jsdom_doc_class));

    /* console */
    struct js_console_obj *c =
        (struct js_console_obj *)jsdom_arena_obj(p, sizeof(*c));
    if (!c) return -1;
    c->ctx = ctx; c->is_error = 0;
    pj_set_global(p, "console", pj_host(c, &jsdom_console_class));

    /* alert (a bare callable) */
    struct js_alert_obj *a =
        (struct js_alert_obj *)jsdom_arena_obj(p, sizeof(*a));
    if (!a) return -1;
    a->ctx = ctx;
    pj_set_global(p, "alert", pj_host(a, &jsdom_alert_class));

    /* this -- the clicked element, or undefined */
    if (this_node && this_node->type == DOM_NODE_ELEMENT) {
        struct js_elem_obj *e =
            (struct js_elem_obj *)jsdom_arena_obj(p, sizeof(*e));
        if (!e) return -1;
        e->ctx = ctx; e->node = this_node;
        pj_set_global(p, "this", pj_host(e, &jsdom_elem_class));
    }
    return 0;
}

#endif /* OSDEV_BROWSER_JSDOM_H */
