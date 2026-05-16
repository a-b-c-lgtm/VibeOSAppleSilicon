/*
 * userspace/libc/css.h — header-only CSS parser + selector matcher.
 *
 * Single-translation-unit convention (matches printf.h, malloc.h,
 * url.h, http.h, html.h, dom.h): include from one .c per binary.
 * Consumes dom.h node types for the matcher half; the parser half
 * has no dom.h dependency at all and could be used standalone.
 *
 * Scope and non-scope:
 *
 *   We implement a *radically simplified* CSS Level-3 selector
 *   subset and a flat declaration model.
 *
 *   What we parse:
 *     - Type selectors:        h1, body, p, ...
 *     - Class selectors:       .intro, .warn
 *     - ID selectors:          #lead
 *     - Universal:             *
 *     - Compound selectors:    p.intro#lead   (no whitespace)
 *     - Descendant combinator: ul li
 *     - Child combinator:      ul > li
 *     - Comma-separated lists: .a, .b { ... }   -> two chains
 *     - Block declarations:    prop: value; prop: value;
 *     - Block comments:        / *  ...  * /  (anywhere whitespace fits)
 *     - At-rules:              skipped to next matching }, content lost
 *     - Bad rules:             skipped to next }, content lost
 *
 *   What we do NOT parse:
 *     - Pseudo-classes (:hover, :nth-child)
 *     - Pseudo-elements (::before, ::first-line)
 *     - Attribute selectors ([type="text"])
 *     - Sibling combinators (~, +)
 *     - Media queries / @media nesting
 *     - calc(), var(), nested function values
 *     - Shorthand expansion (margin: 1px 2px 3px 4px stays as one value)
 *     - !important   (we accept it syntactically by leaving it in the
 *                     value string, but no special precedence)
 *
 *   Values are kept as raw strings (lightly trimmed).  The layout
 *   engine in M62 will tokenize them at use time.  This is a
 *   deliberate split: the parser has zero opinion about what
 *   "10px" or "navy" means.
 *
 * Memory:
 *
 *   Rules, selectors, compounds, simples, and declarations are
 *   individually malloc'd from the userspace heap.  css_destroy()
 *   walks the tree and frees everything in a single pass.  Cost
 *   is fine at the stylesheet sizes we expect (a few KiB →
 *   a few hundred allocations).
 *
 *   `struct css_stylesheet` is small (< 32 bytes) and is safe on
 *   the stack.
 *
 * Selector storage convention:
 *
 *   A chain like "ul > li" is parsed left-to-right but stored
 *   *right-to-left*.  The head of the linked list is the rightmost
 *   compound — the one that must match the candidate node.  Each
 *   compound carries a `combinator` field describing how to reach
 *   its `next` (= LEFT neighbor in source order = ANCESTOR in DOM).
 *
 *   For "ul > li":
 *     head -> { simples: li, combinator: '>', next: ul }
 *           -> { simples: ul, combinator: 0,   next: NULL }
 *
 *   The matcher walks head-first: match the candidate against the
 *   head compound; if pass, climb DOM ancestors per combinator
 *   looking for a node matching the next compound; repeat.
 */
#ifndef USER_CSS_H
#define USER_CSS_H

#include <stdint.h>
#include <stddef.h>

#include "malloc.h"

/* dom.h is only required by the matcher half; the parser half is
 * standalone.  Including it here is fine — dom.h is also header-only
 * and itself includes only malloc.h + html.h.  The matcher
 * functions guard their dom_node-typed parameters with `struct
 * dom_node *` so this compiles even if a caller has not yet built a
 * DOM (i.e. only css_parse / css_destroy are used). */
#include "dom.h"

/* ---------- model ---------- */

/* A "simple" is one of the atomic selector pieces. */
enum css_simple_kind {
    CSS_SIMPLE_UNIVERSAL = 1,   /* *                    */
    CSS_SIMPLE_TYPE      = 2,   /* p, h1, body, ...     */
    CSS_SIMPLE_CLASS     = 3,   /* .intro               */
    CSS_SIMPLE_ID        = 4,   /* #lead                */
    /* Pseudo-classes / pseudo-elements we don't model semantically.
     * We collapse them to one of two outcomes at parse time so the
     * matcher doesn't have to understand any pseudo grammar:
     *   PSEUDO_TRUE  always matches  (e.g. :link on an <a>, since
     *                we have no concept of visited/unvisited; and
     *                :root on the root element approximated as TRUE).
     *   PSEUDO_FALSE never matches  (e.g. :hover, :focus, :active,
     *                ::before/::after pseudo-elements).
     * This lets real-world stylesheets that use `a:link` actually
     * apply their declarations to `<a>` elements instead of being
     * silently dropped by the parser. */
    CSS_SIMPLE_PSEUDO_TRUE  = 5,
    CSS_SIMPLE_PSEUDO_FALSE = 6,
};

struct css_simple {
    int                  kind;       /* enum css_simple_kind */
    char                *name;       /* tag/class/id name; NULL for universal */
    struct css_simple   *next;       /* next simple in this compound */
};

/* A "compound" is one or more simples with no whitespace between
 * them, e.g. "p.intro#lead".  Stored right-to-left in the chain;
 * `combinator` describes how to reach the *next* compound (which
 * is the LEFT neighbor in source order = ANCESTOR in the DOM). */
#define CSS_COMB_NONE       0
#define CSS_COMB_DESCENDANT ' '
#define CSS_COMB_CHILD      '>'

struct css_compound {
    struct css_simple   *simples;     /* head of singly-linked list */
    int                  combinator;  /* CSS_COMB_* */
    struct css_compound *next;        /* leftward in source order */
};

/* One comma-separated selector chain (= one compound list). */
struct css_selector {
    struct css_compound *chain;       /* head = rightmost compound */
    struct css_selector *next;        /* next chain in same rule (comma-sep) */
};

struct css_decl {
    char                *property;    /* lowercased, NUL-terminated */
    char                *value;       /* trimmed, NUL-terminated */
    struct css_decl     *next;
};

struct css_rule {
    struct css_selector *selectors;   /* head of singly-linked list */
    struct css_decl     *decls;
    int                  source_order;/* 0-based; tiebreak in cascade */
    struct css_rule     *next;
};

struct css_stylesheet {
    struct css_rule     *rules;       /* head of singly-linked list */
    struct css_rule     *rules_tail;  /* O(1) append during parse */
    size_t               n_rules;
    size_t               n_decls;     /* sum across all rules */
};

/* ---------- helpers (file-local, all static inline) ---------- */

static inline int css_streq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 0;     /* a shorter than n */
    }
    return 1;
}

static inline int css_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static inline int css_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static inline int css_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline int css_is_digit(char c) { return c >= '0' && c <= '9'; }

/* Identifier byte: per spec, [a-zA-Z0-9_-] plus most non-ASCII.  We
 * accept the ASCII subset only; non-ASCII tag/class names are vanishingly
 * rare in the corpus we render and the layout engine doesn't care. */
static inline int css_is_ident(char c)
{
    return css_is_alpha(c) || css_is_digit(c) || c == '-' || c == '_';
}

static inline char css_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static inline char *css_strdup_n(const char *src, size_t len)
{
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < len; i++) p[i] = src[i];
    p[len] = 0;
    return p;
}

static inline char *css_strdup_lower_n(const char *src, size_t len)
{
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < len; i++) p[i] = css_lower(src[i]);
    p[len] = 0;
    return p;
}

/* ---------- destroy ---------- */

static inline void css_simple_free(struct css_simple *s)
{
    while (s) {
        struct css_simple *next = s->next;
        if (s->name) free(s->name);
        free(s);
        s = next;
    }
}

static inline void css_compound_free(struct css_compound *c)
{
    while (c) {
        struct css_compound *next = c->next;
        css_simple_free(c->simples);
        free(c);
        c = next;
    }
}

static inline void css_selector_free(struct css_selector *sel)
{
    while (sel) {
        struct css_selector *next = sel->next;
        css_compound_free(sel->chain);
        free(sel);
        sel = next;
    }
}

static inline void css_decl_free(struct css_decl *d)
{
    while (d) {
        struct css_decl *next = d->next;
        if (d->property) free(d->property);
        if (d->value)    free(d->value);
        free(d);
        d = next;
    }
}

static inline void css_rule_free(struct css_rule *r)
{
    while (r) {
        struct css_rule *next = r->next;
        css_selector_free(r->selectors);
        css_decl_free(r->decls);
        free(r);
        r = next;
    }
}

static inline void css_init(struct css_stylesheet *ss)
{
    ss->rules      = NULL;
    ss->rules_tail = NULL;
    ss->n_rules    = 0;
    ss->n_decls    = 0;
}

static inline void css_destroy(struct css_stylesheet *ss)
{
    css_rule_free(ss->rules);
    ss->rules      = NULL;
    ss->rules_tail = NULL;
    ss->n_rules    = 0;
    ss->n_decls    = 0;
}

/* ---------- parser ---------- */

struct css_parser {
    const char *src;
    size_t      len;
    size_t      pos;
};

static inline void css_skip_ws_and_comments(struct css_parser *p)
{
    for (;;) {
        while (p->pos < p->len && css_is_ws(p->src[p->pos])) p->pos++;
        /* CSS block comment: slash-star ... star-slash. */
        if (p->pos + 1 < p->len &&
            p->src[p->pos] == '/' && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len &&
                   !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/'))
                p->pos++;
            if (p->pos + 1 < p->len) p->pos += 2;   /* consume closer */
            else                     p->pos = p->len;  /* unterminated */
            continue;
        }
        return;
    }
}

/* Consume an identifier ([a-zA-Z_-][a-zA-Z0-9_-]*).  Returns 0 if
 * no identifier present; otherwise writes the malloc'd lowercased
 * result into *out and returns 1. */
static inline int css_take_ident(struct css_parser *p, char **out)
{
    if (p->pos >= p->len) return 0;
    char c0 = p->src[p->pos];
    if (!css_is_alpha(c0) && c0 != '-' && c0 != '_') return 0;
    size_t start = p->pos;
    p->pos++;
    while (p->pos < p->len && css_is_ident(p->src[p->pos])) p->pos++;
    *out = css_strdup_lower_n(p->src + start, p->pos - start);
    return *out != NULL;
}

/* Skip past a string literal terminated by `quote`.  Used by both
 * value scanning and brace-skipping.  Pos is left ON the closing
 * quote (caller's responsibility to advance past it). */
static inline void css_skip_string(struct css_parser *p, char quote)
{
    while (p->pos < p->len && p->src[p->pos] != quote) {
        if (p->src[p->pos] == '\\' && p->pos + 1 < p->len) p->pos++;
        p->pos++;
    }
}

/* Skip from current position past the matching '}', honoring nested
 * braces and string literals.  Used to recover from at-rules and
 * malformed rules.  Caller is at or past the opening '{'. */
static inline void css_skip_block(struct css_parser *p)
{
    int depth = 1;
    while (p->pos < p->len && depth > 0) {
        char c = p->src[p->pos];
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len &&
                   !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/'))
                p->pos++;
            if (p->pos + 1 < p->len) p->pos += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            p->pos++;
            css_skip_string(p, q);
            if (p->pos < p->len) p->pos++;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') depth--;
        p->pos++;
    }
}

/* Parse a single compound selector.  Returns NULL on no compound
 * available (caller should treat that as end-of-chain).  Stops at
 * the first whitespace, '>', ',', or '{'. */
static inline struct css_compound *css_parse_compound(struct css_parser *p)
{
    if (p->pos >= p->len) return NULL;
    char c = p->src[p->pos];
    if (c == ',' || c == '{' || c == '>' || c == '}' || css_is_ws(c))
        return NULL;

    struct css_compound *cmp = (struct css_compound *)malloc(sizeof(*cmp));
    if (!cmp) return NULL;
    cmp->simples    = NULL;
    cmp->combinator = CSS_COMB_NONE;
    cmp->next       = NULL;

    struct css_simple *tail = NULL;
    while (p->pos < p->len) {
        char ch = p->src[p->pos];
        if (ch == ',' || ch == '{' || ch == '>' || ch == '}' ||
            css_is_ws(ch))
            break;

        struct css_simple *s = (struct css_simple *)malloc(sizeof(*s));
        if (!s) { css_compound_free(cmp); return NULL; }
        s->kind = 0; s->name = NULL; s->next = NULL;

        if (ch == '*') {
            s->kind = CSS_SIMPLE_UNIVERSAL;
            p->pos++;
        } else if (ch == '.') {
            p->pos++;
            char *name = NULL;
            if (!css_take_ident(p, &name)) {
                free(s); css_compound_free(cmp); return NULL;
            }
            s->kind = CSS_SIMPLE_CLASS;
            s->name = name;
        } else if (ch == '#') {
            p->pos++;
            char *name = NULL;
            if (!css_take_ident(p, &name)) {
                free(s); css_compound_free(cmp); return NULL;
            }
            s->kind = CSS_SIMPLE_ID;
            s->name = name;
        } else if (ch == ':') {
            /* Pseudo-class (`:link`) or pseudo-element (`::before`).
             * We don't model state/generated-content, so we collapse
             * to a TRUE/FALSE simple at parse time:
             *   - state pseudos that we can fairly call "true for
             *     the bare element": :link, :visited, :any-link
             *   - structural pseudos approximated as TRUE because
             *     we'd rather over-match than drop the rule:
             *     :root, :first-child, :last-child, :only-child,
             *     :nth-child(...) (eats its parens)
             *   - everything else (incl. all pseudo-elements and
             *     interaction pseudos like :hover/:focus/:active):
             *     FALSE so the rule never fires.
             * Eats one extra ':' (so `::before` -> single pseudo).
             * Also eats a parenthesised arg if present so we don't
             * choke on `:nth-child(2n+1)`. */
            p->pos++;
            if (p->pos < p->len && p->src[p->pos] == ':') p->pos++;
            char *name = NULL;
            if (!css_take_ident(p, &name)) {
                free(s); css_compound_free(cmp); return NULL;
            }
            /* eat optional functional argument */
            if (p->pos < p->len && p->src[p->pos] == '(') {
                int depth = 0;
                while (p->pos < p->len) {
                    char qc = p->src[p->pos++];
                    if (qc == '(') depth++;
                    else if (qc == ')') { depth--; if (depth == 0) break; }
                }
            }
            int truthy =
                css_streq(name, "link") ||
                css_streq(name, "visited") ||
                css_streq(name, "any-link") ||
                css_streq(name, "local-link") ||
                css_streq(name, "root") ||
                css_streq(name, "first-child") ||
                css_streq(name, "last-child") ||
                css_streq(name, "only-child") ||
                css_streq(name, "first-of-type") ||
                css_streq(name, "last-of-type") ||
                css_streq(name, "nth-child") ||
                css_streq(name, "nth-of-type") ||
                css_streq(name, "nth-last-child") ||
                css_streq(name, "nth-last-of-type") ||
                css_streq(name, "not");
            free(name);
            s->kind = truthy ? CSS_SIMPLE_PSEUDO_TRUE
                              : CSS_SIMPLE_PSEUDO_FALSE;
            s->name = NULL;
        } else if (css_is_alpha(ch) || ch == '-' || ch == '_') {
            char *name = NULL;
            if (!css_take_ident(p, &name)) {
                free(s); css_compound_free(cmp); return NULL;
            }
            s->kind = CSS_SIMPLE_TYPE;
            s->name = name;
        } else {
            /* Unknown selector byte — bail.  Caller's error recovery
             * (skip-to-`}`) will resync. */
            free(s); css_compound_free(cmp); return NULL;
        }

        if (tail) tail->next = s;
        else      cmp->simples = s;
        tail = s;
    }

    if (!cmp->simples) { free(cmp); return NULL; }
    return cmp;
}

/* Parse a selector chain (one comma-separated alternative).  Returns
 * a css_selector with chain stored RIGHT-TO-LEFT (head = rightmost
 * compound).  Returns NULL on parse error.
 *
 * Storage convention: each compound's `combinator` describes the
 * relationship to its `next` (which is the source-LEFT neighbor =
 * ancestor in the DOM).  We achieve this by *prepending* each new
 * compound to head, with the combinator we just parsed attached to
 * the new compound (because that combinator describes how to reach
 * the OLD head, which is now the new compound's `next`). */
static inline struct css_selector *css_parse_selector(struct css_parser *p)
{
    css_skip_ws_and_comments(p);
    struct css_compound *first = css_parse_compound(p);
    if (!first) return NULL;
    first->combinator = CSS_COMB_NONE;
    first->next       = NULL;

    struct css_compound *head = first;     /* head = current rightmost */

    for (;;) {
        /* Look for a combinator (' ' or '>') leading to the next
         * compound in source order.  Whitespace alone = descendant;
         * whitespace plus '>' (with optional surrounding whitespace)
         * = child. */
        size_t mark = p->pos;
        int saw_ws = 0;
        while (p->pos < p->len && css_is_ws(p->src[p->pos])) {
            saw_ws = 1; p->pos++;
        }
        if (p->pos >= p->len) break;
        char ch = p->src[p->pos];
        if (ch == ',' || ch == '{') {
            /* End of this chain.  Restore so the outer parser sees
             * the punctuation. */
            p->pos = mark;
            break;
        }
        int comb = CSS_COMB_NONE;
        if (ch == '>') {
            comb = CSS_COMB_CHILD;
            p->pos++;
            css_skip_ws_and_comments(p);
        } else if (saw_ws) {
            comb = CSS_COMB_DESCENDANT;
        } else {
            /* No combinator found and we didn't move — bail to
             * avoid an infinite loop on garbage input. */
            break;
        }

        struct css_compound *next_cmp = css_parse_compound(p);
        if (!next_cmp) {
            /* Combinator with no following compound — malformed. */
            css_compound_free(head);
            return NULL;
        }
        /* Prepend: new compound becomes the new rightmost.  The
         * combinator we just parsed sits between the OLD head (which
         * is now the source-LEFT neighbor of next_cmp) and next_cmp.
         * Per the storage rule, the combinator goes on next_cmp,
         * describing how to reach next_cmp->next = old head. */
        next_cmp->combinator = comb;
        next_cmp->next       = head;
        head = next_cmp;
    }

    struct css_selector *sel = (struct css_selector *)malloc(sizeof(*sel));
    if (!sel) { css_compound_free(head); return NULL; }
    sel->chain = head;
    sel->next  = NULL;
    return sel;
}

/* Parse the body of a declaration block (we're past the `{`).
 * Stops at the matching `}`.  Returns 0 on success, -1 on OOM. */
static inline int css_parse_decls(struct css_parser *p,
                                  struct css_decl **out_head,
                                  size_t *out_count)
{
    struct css_decl *head = NULL, *tail = NULL;
    size_t count = 0;

    for (;;) {
        css_skip_ws_and_comments(p);
        if (p->pos >= p->len) break;
        if (p->src[p->pos] == '}') break;
        if (p->src[p->pos] == ';') { p->pos++; continue; }

        char *prop = NULL;
        if (!css_take_ident(p, &prop)) {
            /* Junk before colon — skip this declaration. */
            while (p->pos < p->len &&
                   p->src[p->pos] != ';' && p->src[p->pos] != '}')
                p->pos++;
            continue;
        }
        css_skip_ws_and_comments(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            free(prop);
            while (p->pos < p->len &&
                   p->src[p->pos] != ';' && p->src[p->pos] != '}')
                p->pos++;
            continue;
        }
        p->pos++;   /* consume ':' */
        css_skip_ws_and_comments(p);

        /* Value: everything until ';' or '}', honoring strings.
         * Then trim trailing whitespace. */
        size_t v_start = p->pos;
        while (p->pos < p->len) {
            char ch = p->src[p->pos];
            if (ch == ';' || ch == '}') break;
            if (ch == '"' || ch == '\'') {
                char q = ch; p->pos++;
                css_skip_string(p, q);
                if (p->pos < p->len) p->pos++;
                continue;
            }
            if (ch == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
                p->pos += 2;
                while (p->pos + 1 < p->len &&
                       !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/'))
                    p->pos++;
                if (p->pos + 1 < p->len) p->pos += 2;
                continue;
            }
            p->pos++;
        }
        size_t v_end = p->pos;
        while (v_end > v_start && css_is_ws(p->src[v_end - 1])) v_end--;

        char *value = css_strdup_n(p->src + v_start, v_end - v_start);
        if (!value) { free(prop); css_decl_free(head); return -1; }

        struct css_decl *d = (struct css_decl *)malloc(sizeof(*d));
        if (!d) { free(prop); free(value); css_decl_free(head); return -1; }
        d->property = prop;
        d->value    = value;
        d->next     = NULL;
        if (tail) tail->next = d;
        else      head       = d;
        tail = d;
        count++;
    }

    *out_head  = head;
    *out_count = count;
    return 0;
}

/* css_parse — append rules from `src`/`len` into the stylesheet.
 * Returns 0 on success.  On OOM we return -1 but leave whatever
 * we already parsed in place (best effort).  Malformed at-rules
 * and blocks are silently skipped per the spec's "rule that doesn't
 * parse, drop" recovery. */
static inline int css_parse(struct css_stylesheet *ss,
                            const char *src, size_t len)
{
    struct css_parser p;
    p.src = src; p.len = len; p.pos = 0;

    while (p.pos < p.len) {
        css_skip_ws_and_comments(&p);
        if (p.pos >= p.len) break;
        char ch = p.src[p.pos];

        if (ch == '@') {
            /* At-rule.  Skip name + prelude up to ';' or '{...}'. */
            p.pos++;
            while (p.pos < p.len &&
                   p.src[p.pos] != ';' && p.src[p.pos] != '{')
                p.pos++;
            if (p.pos < p.len && p.src[p.pos] == '{') {
                p.pos++;
                css_skip_block(&p);
            } else if (p.pos < p.len) {
                p.pos++;   /* consume ';' */
            }
            continue;
        }
        if (ch == '}') { p.pos++; continue; }  /* stray close — skip */

        /* Parse one rule = comma-separated selector list, then block. */
        struct css_selector *sel_head = NULL, *sel_tail = NULL;
        for (;;) {
            css_skip_ws_and_comments(&p);
            struct css_selector *sel = css_parse_selector(&p);
            if (!sel) {
                /* Bad selector — recover by skipping to next '}'
                 * (drops any partial sel_head). */
                css_selector_free(sel_head);
                sel_head = NULL;
                while (p.pos < p.len && p.src[p.pos] != '}') {
                    if (p.src[p.pos] == '{') {
                        p.pos++;
                        css_skip_block(&p);
                        goto next_rule;
                    }
                    p.pos++;
                }
                if (p.pos < p.len) p.pos++;
                goto next_rule;
            }
            if (sel_tail) sel_tail->next = sel;
            else          sel_head       = sel;
            sel_tail = sel;

            css_skip_ws_and_comments(&p);
            if (p.pos < p.len && p.src[p.pos] == ',') { p.pos++; continue; }
            break;
        }

        css_skip_ws_and_comments(&p);
        if (p.pos >= p.len || p.src[p.pos] != '{') {
            css_selector_free(sel_head);
            continue;
        }
        p.pos++;   /* consume '{' */

        struct css_decl *decls = NULL;
        size_t           ndecl = 0;
        if (css_parse_decls(&p, &decls, &ndecl) < 0) {
            css_selector_free(sel_head);
            return -1;
        }
        if (p.pos < p.len && p.src[p.pos] == '}') p.pos++;

        struct css_rule *r = (struct css_rule *)malloc(sizeof(*r));
        if (!r) { css_selector_free(sel_head); css_decl_free(decls); return -1; }
        r->selectors    = sel_head;
        r->decls        = decls;
        r->source_order = (int)ss->n_rules;
        r->next         = NULL;
        if (ss->rules_tail) ss->rules_tail->next = r;
        else                ss->rules            = r;
        ss->rules_tail = r;
        ss->n_rules++;
        ss->n_decls += ndecl;

    next_rule: ;
    }

    return 0;
}

/* ---------- selector matcher ---------- */

/* Test whether the candidate's class attribute (a space-separated
 * list, e.g. "intro lead") contains `name`. */
static inline int css_node_has_class(const struct dom_node *n, const char *name)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return 0;
    for (struct dom_attr *a = n->attrs; a; a = a->next) {
        if (!css_streq(a->name, "class")) continue;
        const char *v = a->value;
        if (!v) return 0;
        size_t nl = 0; while (name[nl]) nl++;
        size_t i = 0;
        while (v[i]) {
            while (v[i] && css_is_ws(v[i])) i++;
            size_t s = i;
            while (v[i] && !css_is_ws(v[i])) i++;
            if (i - s == nl) {
                int eq = 1;
                for (size_t k = 0; k < nl; k++)
                    if (v[s + k] != name[k]) { eq = 0; break; }
                if (eq) return 1;
            }
        }
        return 0;
    }
    return 0;
}

static inline int css_node_id_eq(const struct dom_node *n, const char *name)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return 0;
    for (struct dom_attr *a = n->attrs; a; a = a->next)
        if (css_streq(a->name, "id"))
            return a->value && css_streq(a->value, name);
    return 0;
}

/* Match a single compound (all simples must hit) against one node. */
static inline int css_match_compound(const struct css_compound *cmp,
                                     const struct dom_node *n)
{
    if (!n || n->type != DOM_NODE_ELEMENT) return 0;
    for (struct css_simple *s = cmp->simples; s; s = s->next) {
        switch (s->kind) {
        case CSS_SIMPLE_UNIVERSAL:
            break;
        case CSS_SIMPLE_TYPE:
            if (!css_streq(n->tag, s->name)) return 0;
            break;
        case CSS_SIMPLE_CLASS:
            if (!css_node_has_class(n, s->name)) return 0;
            break;
        case CSS_SIMPLE_ID:
            if (!css_node_id_eq(n, s->name)) return 0;
            break;
        case CSS_SIMPLE_PSEUDO_TRUE:
            /* Always-match pseudo (e.g. :link).  Nothing to check. */
            break;
        case CSS_SIMPLE_PSEUDO_FALSE:
            /* Never-match pseudo (e.g. :hover).  Whole compound
             * fails so the rule does not apply. */
            return 0;
        default:
            return 0;
        }
    }
    return 1;
}

/* Match a compound chain against a node.  Walks the chain head-first
 * (which is right-to-left in source order); for each step climbs DOM
 * ancestors per the combinator. */
static inline int css_match_chain(const struct css_compound *cmp,
                                  const struct dom_node *n)
{
    if (!cmp) return 1;
    if (!css_match_compound(cmp, n)) return 0;
    if (!cmp->next) return 1;

    /* Climb to find a node matching cmp->next per cmp->combinator. */
    if (cmp->combinator == CSS_COMB_CHILD) {
        struct dom_node *parent = n->parent;
        if (!parent) return 0;
        return css_match_chain(cmp->next, parent);
    }
    /* Default: descendant.  Walk all ancestors. */
    for (struct dom_node *anc = n->parent; anc; anc = anc->parent)
        if (css_match_chain(cmp->next, anc)) return 1;
    return 0;
}

/* Test whether ANY of the rule's comma-separated selectors matches
 * the node.  Returns 1 if so, 0 otherwise. */
static inline int css_rule_matches(const struct css_rule *r,
                                   const struct dom_node *n)
{
    for (struct css_selector *sel = r->selectors; sel; sel = sel->next)
        if (css_match_chain(sel->chain, n)) return 1;
    return 0;
}

/* Specificity of a single chain: pack (#id_count, .class_count,
 * type_count) into one int as a*10000 + b*100 + c.  Universal
 * counts as zero everywhere. */
static inline int css_chain_specificity(const struct css_compound *cmp)
{
    int a = 0, b = 0, c = 0;
    for (const struct css_compound *p = cmp; p; p = p->next) {
        for (struct css_simple *s = p->simples; s; s = s->next) {
            switch (s->kind) {
            case CSS_SIMPLE_ID:    a++; break;
            case CSS_SIMPLE_CLASS: b++; break;
            /* Pseudo-classes count like classes per the spec
             * (`a:link` should beat plain `a`).  Pseudo-elements
             * count like a type, but our parser collapses them
             * to FALSE so they never reach here in a matching
             * rule — group them with classes for safety. */
            case CSS_SIMPLE_PSEUDO_TRUE:
            case CSS_SIMPLE_PSEUDO_FALSE:
                                   b++; break;
            case CSS_SIMPLE_TYPE:  c++; break;
            default:               break;
            }
        }
    }
    return a * 10000 + b * 100 + c;
}

/* ---------- accessors ---------- */

static inline const char *css_decl_lookup(const struct css_rule *r,
                                          const char *property)
{
    for (struct css_decl *d = r->decls; d; d = d->next)
        if (css_streq(d->property, property))
            return d->value;
    return NULL;
}

static inline size_t css_n_simples_in_chain(const struct css_compound *cmp)
{
    size_t n = 0;
    for (const struct css_compound *p = cmp; p; p = p->next)
        for (struct css_simple *s = p->simples; s; s = s->next) n++;
    return n;
}

#endif /* USER_CSS_H */
