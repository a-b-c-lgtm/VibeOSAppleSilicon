/*
 * userspace/libc/dom.h — header-only DOM builder.
 *
 * Single-translation-unit convention (matches printf.h, malloc.h,
 * url.h, http.h, html.h): include from one .c per binary.  Consumes
 * the html.h tokenizer; emits a tree.
 *
 * Scope and non-scope:
 *
 *   - We implement a *radically simplified* HTML5 tree-construction
 *     pass.  Real browsers run the WHATWG "insertion modes" state
 *     machine (initial → before-html → before-head → in-head → ...
 *     → after-after-body), with foster-parenting, formatting-element
 *     reconstruction, table fixups, the active-formatting list, etc.
 *     We don't.  Our model handles:
 *       * Documents with an explicit <html><head>...<body>... skeleton
 *         (the dominant well-formed case).
 *       * Documents WITHOUT a skeleton — we auto-insert <html>, <head>,
 *         <body> on demand and route content into the right one.
 *       * Void elements (br, img, meta, link, ...): inserted as
 *         children, never pushed onto the open-elements stack.
 *       * Self-closing slashes (<foo .../>): treated as void.
 *       * Mismatched </tag>: walk the open-elements stack; if we
 *         find a matching open element, pop everything down to and
 *         including it; otherwise drop the end tag.
 *     We do NOT handle:
 *       * Tables (<table>/<tr>/<td>) with any kind of fix-up.
 *       * Foster-parenting.
 *       * Active formatting reconstruction (the famous "Adoption
 *         Agency Algorithm").
 *       * SVG / MathML foreign content.
 *
 *   - The simplified head/body routing rule: a tag is "head-only" if
 *     its name is one of { meta, link, base, title, style, script }.
 *     If we see a head-only tag while the insertion point is the
 *     document or the implicit <html>, we implicitly open <head>
 *     and route it there.  If we see anything else while inside an
 *     implicit <head> (or at the implicit <html> with no head/body
 *     yet), we implicitly close head and open <body>.  This matches
 *     what every page in the wild expects.
 *
 * Memory:
 *
 *   Nodes and attributes are individually malloc'd from the userspace
 *   heap.  The cost of malloc-per-node is a non-issue at the page
 *   sizes we care about (tens of KiB → maybe a couple thousand nodes).
 *   The user calls dom_destroy() to free the whole tree in one walk.
 *
 *   The heap-allocated `struct html_token` (12 KiB) lives in the
 *   caller, NOT in this header — we want callers to be able to
 *   reuse one buffer across documents.
 *
 *   `struct dom` is small (< 64 bytes) and is safe on the stack.
 */
#ifndef USER_DOM_H
#define USER_DOM_H

#include <stdint.h>
#include <stddef.h>

#include "malloc.h"
#include "html.h"

/* ---------- node model ---------- */

enum dom_node_type {
    DOM_NODE_DOCUMENT = 1,
    DOM_NODE_ELEMENT  = 2,
    DOM_NODE_TEXT     = 3,
    DOM_NODE_COMMENT  = 4,
    DOM_NODE_DOCTYPE  = 5,
};

struct dom_attr {
    char            *name;          /* malloc'd, lowercased, NUL-terminated */
    char            *value;         /* malloc'd, NUL-terminated */
    struct dom_attr *next;
};

struct dom_node {
    int                  type;          /* enum dom_node_type */
    char                *tag;           /* ELEMENT: lowercased, NUL-term. NULL otherwise. */
    char                *text;          /* TEXT/COMMENT/DOCTYPE: NUL-term. NULL otherwise. */
    size_t               text_len;
    struct dom_attr     *attrs;         /* ELEMENT only; head of singly-linked list */
    struct dom_node     *parent;
    struct dom_node     *first_child;
    struct dom_node     *last_child;    /* O(1) append */
    struct dom_node     *next_sibling;
};

/* The DOM proper.  Holds shortcut pointers into the document tree
 * so callers (and the builder itself) don't have to walk to find
 * the canonical html/head/body. */
#define DOM_OPEN_STACK_MAX 64

struct dom {
    struct dom_node *root;          /* DOCUMENT node, owns the whole tree */
    struct dom_node *html;          /* the canonical <html> (NULL until first content) */
    struct dom_node *head;          /* the canonical <head> */
    struct dom_node *body;          /* the canonical <body> */
    size_t           n_nodes;       /* including root */

    /* Builder transient state — only meaningful between dom_init() and
     * dom_build_finish(); reset by dom_init.  Exposed in the struct
     * so the build loop is genuinely stateless inside its caller. */
    struct dom_node *open_stack[DOM_OPEN_STACK_MAX];
    int              open_top;      /* 0 = empty; index of next free slot */
};

/* ---------- helpers (file-local) ---------- */

static int dom_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Allocate and copy a NUL-terminated string of length `len` from
 * `src` (may not be NUL-terminated in the source).  Returns NULL
 * on OOM. */
static char *dom_strdup_n(const char *src, size_t len)
{
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < len; i++) p[i] = src[i];
    p[len] = 0;
    return p;
}

static int dom_is_void_tag(const char *tag)
{
    /* HTML5 §13.1.2 void elements.  Order chosen by perceived
     * frequency on the modern web so the early-return hits faster. */
    static const char *const VOID_TAGS[] = {
        "br", "img", "meta", "link", "input", "hr", "wbr",
        "area", "base", "col", "embed", "source", "track",
        NULL,
    };
    for (int i = 0; VOID_TAGS[i]; i++)
        if (dom_streq(tag, VOID_TAGS[i])) return 1;
    return 0;
}

static int dom_is_head_only_tag(const char *tag)
{
    /* Subset of the WHATWG "in head" insertion mode's tag set —
     * just the ones we expect to see in practice. */
    static const char *const HEAD_ONLY[] = {
        "meta", "link", "base", "title", "style", "script",
        NULL,
    };
    for (int i = 0; HEAD_ONLY[i]; i++)
        if (dom_streq(tag, HEAD_ONLY[i])) return 1;
    return 0;
}

/* ---------- node lifecycle ---------- */

static struct dom_node *dom_node_new(int type)
{
    struct dom_node *n = (struct dom_node *)malloc(sizeof(*n));
    if (!n) return NULL;
    n->type         = type;
    n->tag          = NULL;
    n->text         = NULL;
    n->text_len     = 0;
    n->attrs        = NULL;
    n->parent       = NULL;
    n->first_child  = NULL;
    n->last_child   = NULL;
    n->next_sibling = NULL;
    return n;
}

static void dom_node_append_child(struct dom_node *parent,
                                  struct dom_node *child)
{
    child->parent = parent;
    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
}

/* Free `n` and every descendant.  Iterative-via-recursion is fine
 * for our depth bound (HTML pages don't nest hundreds deep, and the
 * builder caps the open-elements stack at DOM_OPEN_STACK_MAX). */
static void dom_node_free(struct dom_node *n)
{
    if (!n) return;
    struct dom_node *c = n->first_child;
    while (c) {
        struct dom_node *next = c->next_sibling;
        dom_node_free(c);
        c = next;
    }
    if (n->tag)  free(n->tag);
    if (n->text) free(n->text);
    struct dom_attr *a = n->attrs;
    while (a) {
        struct dom_attr *next = a->next;
        if (a->name)  free(a->name);
        if (a->value) free(a->value);
        free(a);
        a = next;
    }
    free(n);
}

/* ---------- accessors ---------- */

static inline struct dom_node *dom_root(const struct dom *d)
{
    return d->root;
}

static inline struct dom_node *dom_html(const struct dom *d) { return d->html; }
static inline struct dom_node *dom_head(const struct dom *d) { return d->head; }
static inline struct dom_node *dom_body(const struct dom *d) { return d->body; }

static inline const char *dom_node_attr(const struct dom_node *n, const char *name)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return NULL;
    for (struct dom_attr *a = n->attrs; a; a = a->next)
        if (dom_streq(a->name, name)) return a->value;
    return NULL;
}

static inline int dom_node_n_attrs(const struct dom_node *n)
{
    int count = 0;
    if (!n || n->type != DOM_NODE_ELEMENT) return 0;
    for (struct dom_attr *a = n->attrs; a; a = a->next) count++;
    return count;
}

/* Set an attribute on an element node.  Replaces the existing
 * value if `name` is already present, else appends a new attr.
 * Strings are copied (malloc'd) -- caller retains ownership of
 * the input buffers.  Returns 0 on success, -1 on OOM or invalid
 * input.  Added in chapter 122 for the pocketjs DOM bridge.
 */
static inline int dom_node_set_attr(struct dom_node *n,
                                    const char *name,
                                    const char *value)
{
    if (!n || n->type != DOM_NODE_ELEMENT || !name) return -1;
    /* Replace existing attribute. */
    for (struct dom_attr *a = n->attrs; a; a = a->next) {
        if (!dom_streq(a->name, name)) continue;
        size_t vl = value ? 0 : 0; (void)vl;
        size_t nv = 0; if (value) while (value[nv]) nv++;
        char *nb = (char *)malloc(nv + 1);
        if (!nb) return -1;
        for (size_t i = 0; i < nv; i++) nb[i] = value[i];
        nb[nv] = 0;
        if (a->value) free(a->value);
        a->value = nb;
        return 0;
    }
    /* Append a new attribute. */
    struct dom_attr *a = (struct dom_attr *)malloc(sizeof(*a));
    if (!a) return -1;
    size_t nn = 0; while (name[nn]) nn++;
    size_t nv = 0; if (value) while (value[nv]) nv++;
    a->name  = (char *)malloc(nn + 1);
    a->value = (char *)malloc(nv + 1);
    if (!a->name || !a->value) {
        if (a->name) free(a->name);
        if (a->value) free(a->value);
        free(a);
        return -1;
    }
    for (size_t i = 0; i < nn; i++) a->name[i]  = name[i];
    a->name[nn] = 0;
    for (size_t i = 0; i < nv; i++) a->value[i] = value ? value[i] : 0;
    a->value[nv] = 0;
    a->next = NULL;
    /* Append at the end so the iteration order in dom_node_attr
     * stays predictable for callers that walk the attrs list. */
    if (!n->attrs) { n->attrs = a; return 0; }
    struct dom_attr *t = n->attrs;
    while (t->next) t = t->next;
    t->next = a;
    return 0;
}

/* ---------- builder: stack helpers ---------- */

static struct dom_node *dom_top(const struct dom *d)
{
    if (d->open_top == 0) return d->root;
    return d->open_stack[d->open_top - 1];
}

static int dom_push(struct dom *d, struct dom_node *n)
{
    if (d->open_top >= DOM_OPEN_STACK_MAX) return -1;
    d->open_stack[d->open_top++] = n;
    return 0;
}

static struct dom_node *dom_pop(struct dom *d)
{
    if (d->open_top == 0) return NULL;
    return d->open_stack[--d->open_top];
}

/* Walk the open-elements stack from top to bottom looking for an
 * element whose tag matches `name`.  Returns the index in the stack
 * (0..open_top-1) or -1 if not found. */
static int dom_find_open(const struct dom *d, const char *name)
{
    for (int i = d->open_top - 1; i >= 0; i--) {
        struct dom_node *e = d->open_stack[i];
        if (e->type == DOM_NODE_ELEMENT && dom_streq(e->tag, name))
            return i;
    }
    return -1;
}

/* ---------- builder: element creation ---------- */

/* Create an ELEMENT node from the START token.  Copies tag name and
 * all attributes via malloc.  Returns NULL on OOM. */
static struct dom_node *dom_make_element(const struct html_token *t)
{
    struct dom_node *n = dom_node_new(DOM_NODE_ELEMENT);
    if (!n) return NULL;
    n->tag = dom_strdup_n(t->tag_name, t->tag_name_len);
    if (!n->tag) { free(n); return NULL; }
    /* Walk attrs in source order so the resulting list iterates in
     * the same order — useful for deterministic test output. */
    struct dom_attr *tail = NULL;
    for (int i = 0; i < t->n_attrs; i++) {
        const struct html_attr *src = &t->attrs[i];
        struct dom_attr *a = (struct dom_attr *)malloc(sizeof(*a));
        if (!a) { dom_node_free(n); return NULL; }
        a->name  = dom_strdup_n(src->name,  src->name_len);
        a->value = dom_strdup_n(src->value, src->value_len);
        a->next  = NULL;
        if (!a->name || !a->value) {
            if (a->name)  free(a->name);
            if (a->value) free(a->value);
            free(a);
            dom_node_free(n);
            return NULL;
        }
        if (tail) tail->next = a;
        else      n->attrs   = a;
        tail = a;
    }
    return n;
}

/* ---------- builder: implicit-skeleton helpers ---------- */

/* Make sure d->html exists, is a child of d->root, and is on top of
 * the open-elements stack (or under whatever is currently above it
 * — we don't reorder, we only ensure presence).  Idempotent. */
static int dom_ensure_html(struct dom *d)
{
    if (d->html) return 0;
    struct dom_node *h = dom_node_new(DOM_NODE_ELEMENT);
    if (!h) return -1;
    h->tag = dom_strdup_n("html", 4);
    if (!h->tag) { free(h); return -1; }
    dom_node_append_child(d->root, h);
    d->html = h;
    d->n_nodes++;
    if (dom_push(d, h) < 0) return -1;
    return 0;
}

static int dom_ensure_head(struct dom *d)
{
    if (d->head) return 0;
    if (dom_ensure_html(d) < 0) return -1;
    struct dom_node *h = dom_node_new(DOM_NODE_ELEMENT);
    if (!h) return -1;
    h->tag = dom_strdup_n("head", 4);
    if (!h->tag) { free(h); return -1; }
    dom_node_append_child(d->html, h);
    d->head = h;
    d->n_nodes++;
    if (dom_push(d, h) < 0) return -1;
    return 0;
}

static int dom_ensure_body(struct dom *d)
{
    if (d->body) return 0;
    if (dom_ensure_html(d) < 0) return -1;
    /* If <head> is currently on the open-elements stack, pop it
     * first — entering body implicitly closes head. */
    int hi = dom_find_open(d, "head");
    if (hi >= 0) {
        while (d->open_top > hi) (void)dom_pop(d);
    }
    struct dom_node *b = dom_node_new(DOM_NODE_ELEMENT);
    if (!b) return -1;
    b->tag = dom_strdup_n("body", 4);
    if (!b->tag) { free(b); return -1; }
    dom_node_append_child(d->html, b);
    d->body = b;
    d->n_nodes++;
    if (dom_push(d, b) < 0) return -1;
    return 0;
}

/* Merge any attributes from a start-tag token onto an already-existing
 * element — used when the auto-skeleton inserted <html>/<head>/<body>
 * implicitly and the page later writes the explicit start tag with
 * its own attributes (e.g. `<html lang="en">`).  Existing attrs win
 * (this matches the spec's "for each attribute on the token, check
 * to see if the attribute is already present on the top element of
 * the stack of open elements; if it is not, add the attribute"
 * rule from the in-body insertion mode). */
static int dom_merge_attrs(struct dom_node *n, const struct html_token *t)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return 0;
    /* Find current tail so we append in source order. */
    struct dom_attr *tail = n->attrs;
    if (tail) while (tail->next) tail = tail->next;
    for (int i = 0; i < t->n_attrs; i++) {
        const struct html_attr *src = &t->attrs[i];
        /* Skip if attr already present on n. */
        int present = 0;
        for (struct dom_attr *a = n->attrs; a; a = a->next) {
            size_t k = 0;
            if (a->name) while (a->name[k]) k++;
            if (k == (size_t)src->name_len) {
                int eq = 1;
                for (size_t j = 0; j < k; j++)
                    if (a->name[j] != src->name[j]) { eq = 0; break; }
                if (eq) { present = 1; break; }
            }
        }
        if (present) continue;
        struct dom_attr *a = (struct dom_attr *)malloc(sizeof(*a));
        if (!a) return -1;
        a->name  = dom_strdup_n(src->name,  src->name_len);
        a->value = dom_strdup_n(src->value, src->value_len);
        a->next  = NULL;
        if (!a->name || !a->value) {
            if (a->name)  free(a->name);
            if (a->value) free(a->value);
            free(a);
            return -1;
        }
        if (tail) tail->next = a;
        else      n->attrs   = a;
        tail = a;
    }
    return 0;
}

/* ---------- builder: token handlers ---------- */

static int dom_handle_start(struct dom *d, const struct html_token *t)
{
    /* Re-route based on the canonical-skeleton rules.  We DON'T
     * want a literal duplicate <html> if the page wrote one and the
     * builder already auto-inserted one before something else
     * forced the issue.  In that case we just merge attributes
     * onto the existing one and ignore the new tag (this is what
     * the spec calls "in body, html start tag" handling, simplified). */
    if (dom_streq(t->tag_name, "html")) {
        if (dom_ensure_html(d) < 0) return -1;
        if (dom_merge_attrs(d->html, t) < 0) return -1;
        return 0;
    }
    if (dom_streq(t->tag_name, "head")) {
        if (dom_ensure_head(d) < 0) return -1;
        if (dom_merge_attrs(d->head, t) < 0) return -1;
        return 0;
    }
    if (dom_streq(t->tag_name, "body")) {
        if (dom_ensure_body(d) < 0) return -1;
        if (dom_merge_attrs(d->body, t) < 0) return -1;
        return 0;
    }

    /* Implicit-skeleton rules for any other tag. */
    if (dom_is_head_only_tag(t->tag_name)) {
        /* Goes inside <head>.  Open <head> if it isn't already; do
         * NOT switch to body. */
        if (dom_ensure_head(d) < 0) return -1;
        /* If the current insertion point is somewhere inside <body>
         * already (e.g. a <script> appearing mid-body), we leave it
         * where it is — head-only tags are legal in body too. */
        if (d->body == NULL) {
            /* still in head; force insertion point to head if the
             * stack got inflated by stray container tags */
            int hi = dom_find_open(d, "head");
            if (hi >= 0) {
                while (d->open_top > hi + 1) (void)dom_pop(d);
            }
        }
    } else {
        /* Anything not in HEAD_ONLY implies body.  If we're still
         * sitting at document or html or head, switch to body. */
        struct dom_node *cur = dom_top(d);
        if (cur == d->root || cur == d->html || cur == d->head) {
            if (dom_ensure_body(d) < 0) return -1;
        }
    }

    /* Now create the element and attach to whatever is now on top. */
    struct dom_node *e = dom_make_element(t);
    if (!e) return -1;
    dom_node_append_child(dom_top(d), e);
    d->n_nodes++;
    if (dom_is_void_tag(t->tag_name) || t->self_closing) {
        /* Void/self-closing: do not push.  No matching </tag> will
         * ever pop it, and the spec says it has no contents. */
        return 0;
    }
    if (dom_push(d, e) < 0) return -1;
    return 0;
}

static int dom_handle_end(struct dom *d, const struct html_token *t)
{
    /* </html>, </head>, </body>: the spec has elaborate "after-body"
     * etc modes; we just pop down to the matching open element if
     * any.  </body> in particular acts as a hard reset of the
     * insertion point back to <html>. */
    int idx = dom_find_open(d, t->tag_name);
    if (idx < 0) return 0;          /* stray end tag */
    while (d->open_top > idx) (void)dom_pop(d);
    return 0;
}

static int dom_handle_chars(struct dom *d, const struct html_token *t)
{
    /* Skip pure-whitespace text runs that land at the document or
     * html level — they're almost always indentation between tags
     * and would otherwise show up as garbage TEXT siblings of <html>.
     * Inside <head> / <body> we DO keep whitespace; it matters for
     * inline layout (collapsed at render time, not parse time). */
    struct dom_node *cur = dom_top(d);
    if (cur == d->root || cur == d->html) {
        int all_ws = 1;
        for (size_t i = 0; i < t->data_len; i++) {
            char c = t->data[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                all_ws = 0;
                break;
            }
        }
        if (all_ws) return 0;
        /* Non-whitespace content at this level forces body to open. */
        if (dom_ensure_body(d) < 0) return -1;
        cur = dom_top(d);
    }
    struct dom_node *n = dom_node_new(DOM_NODE_TEXT);
    if (!n) return -1;
    n->text = dom_strdup_n(t->data, t->data_len);
    if (!n->text) { free(n); return -1; }
    n->text_len = t->data_len;
    dom_node_append_child(cur, n);
    d->n_nodes++;
    return 0;
}

static int dom_handle_comment(struct dom *d, const struct html_token *t)
{
    struct dom_node *n = dom_node_new(DOM_NODE_COMMENT);
    if (!n) return -1;
    n->text = dom_strdup_n(t->data, t->data_len);
    if (!n->text) { free(n); return -1; }
    n->text_len = t->data_len;
    dom_node_append_child(dom_top(d), n);
    d->n_nodes++;
    return 0;
}

static int dom_handle_doctype(struct dom *d, const struct html_token *t)
{
    /* DOCTYPE always attaches to the document, NEVER inside a tag.
     * We don't validate that it appears before <html>; if the page
     * is buggy and emits a stray doctype mid-body we still attach
     * it to the document (ugly but harmless — render layer ignores
     * it). */
    struct dom_node *n = dom_node_new(DOM_NODE_DOCTYPE);
    if (!n) return -1;
    n->text = dom_strdup_n(t->data, t->data_len);
    if (!n->text) { free(n); return -1; }
    n->text_len = t->data_len;
    dom_node_append_child(d->root, n);
    d->n_nodes++;
    return 0;
}

/* ---------- public builder API ---------- */

/* One-shot init: zero the struct and create the DOCUMENT root.
 * Returns 0 on success, -1 on OOM. */
static int dom_init(struct dom *d)
{
    d->html      = NULL;
    d->head      = NULL;
    d->body      = NULL;
    d->n_nodes   = 0;
    d->open_top  = 0;
    d->root = dom_node_new(DOM_NODE_DOCUMENT);
    if (!d->root) return -1;
    d->n_nodes = 1;
    return 0;
}

/* Free everything — the whole tree, root included.  After this
 * call `d` is in the same state it was BEFORE dom_init was called;
 * call dom_init again to reuse. */
static void dom_destroy(struct dom *d)
{
    dom_node_free(d->root);
    d->root     = NULL;
    d->html     = NULL;
    d->head     = NULL;
    d->body     = NULL;
    d->n_nodes  = 0;
    d->open_top = 0;
}

/* Drive the tokenizer to EOF, building the tree as we go.  `tok`
 * must already be initialised against the source.  `scratch`
 * is a caller-owned token buffer (~12 KiB) we reuse for every
 * pulled token; we don't allocate one ourselves so callers can
 * batch-process many documents with a single allocation.
 *
 * Returns 0 on success, -1 on internal error (OOM, stack overflow,
 * tokenizer error).  On error the partial tree remains attached
 * to d->root and dom_destroy() is the right way to clean up. */
static int dom_build(struct dom *d,
                     struct html_tokenizer *tok,
                     struct html_token *scratch)
{
    for (;;) {
        int r = html_tok_next(tok, scratch);
        if (r < 0) return -1;
        if (r == 0) break;
        switch (scratch->type) {
        case HTML_TOK_CHARS:
            if (dom_handle_chars(d, scratch) < 0) return -1;
            break;
        case HTML_TOK_START:
            if (dom_handle_start(d, scratch) < 0) return -1;
            break;
        case HTML_TOK_END:
            if (dom_handle_end(d, scratch) < 0) return -1;
            break;
        case HTML_TOK_COMMENT:
            if (dom_handle_comment(d, scratch) < 0) return -1;
            break;
        case HTML_TOK_DOCTYPE:
            if (dom_handle_doctype(d, scratch) < 0) return -1;
            break;
        case HTML_TOK_EOF:
            return 0;
        default:
            /* HTML_TOK_NONE is unexpected — treat as EOF. */
            return 0;
        }
    }
    return 0;
}

#endif /* USER_DOM_H */
