/*
 * userspace/libc/pocketjs.h -- chapter 122 pocket JavaScript.
 *
 * A header-only expression evaluator for the JS subset needed
 * by HTML onclick="..." handlers.  Not a real JS engine.
 *
 * Design constraints
 * ------------------
 * - Header-only, same convention as cookies.h / origin.h /
 *   url.h.  Include from exactly one .c per binary.
 * - Tree-walking evaluator over a Pratt-parsed expression AST.
 *   No bytecode, no GC, no closures, no control flow.
 * - All allocations come from a single caller-provided arena.
 *   After pj_eval() returns, the arena can be reset; any
 *   js_value strings the caller wants to keep must be copied
 *   out first.
 * - Hard caps live in #defines below: 4096 tokens, 256 AST
 *   nodes, 32-deep call stack.  A misbehaving onclick handler
 *   bounces off these limits rather than locking the browser.
 *
 * Language subset
 * ---------------
 *   value    ::= number | string | true | false | null | undefined
 *   primary  ::= value | identifier | "(" expr ")"
 *   postfix  ::= primary ( "." identifier | "(" args? ")" )*
 *   unary    ::= ("-" | "!") unary | postfix
 *   term     ::= unary  (("*" | "/") unary)*
 *   sum      ::= term   (("+" | "-") term)*
 *   cmp      ::= sum    (("<" | ">" | "<=" | ">=") sum)?
 *   eq       ::= cmp    (("==" | "!=") cmp)?
 *   and_     ::= eq     ("&&" eq)*
 *   or_      ::= and_   ("||" and_)*
 *   assign   ::= postfix "=" assign         // only LHS = member
 *              | or_
 *   stmt     ::= assign
 *   program  ::= stmt (";" stmt)* ";"?
 *
 * Returns the last statement's value.  An empty program returns
 * JS_UNDEFINED.
 *
 * Numbers are int64_t.  We deliberately don't ship a float type:
 * the only onclick handlers we care about are touching DOM
 * properties (strings) or counting clicks (integers).  Real JS's
 * IEEE-754 quirks bring zero value to the kind of pages this
 * engine targets.
 *
 * Strings are arena-allocated, NUL-terminated, and treated as
 * immutable -- concatenation (`+`) allocates a new buffer.
 *
 * Host objects
 * ------------
 * The interesting part of any embedded language is what you bind
 * to it.  The engine itself knows nothing about DOM, alert, or
 * console.  Everything that isn't a primitive is a JS_HOSTOBJ:
 * an opaque `void *self` plus a `struct pj_host_class` vtable.
 *
 *   - get(self, name)        -> obj.name             (rvalue)
 *   - set(self, name, val)   -> obj.name = val
 *   - method(self, name, ..) -> obj.name(args...)
 *   - call(self, ..)         -> obj(args...)
 *
 * Any of those four hooks may be NULL.  Engine fails the
 * operation (returns JS_UNDEFINED, logs to serial) if the hook
 * the program needs is missing.  jsdom.h in userspace/browser/
 * provides the bindings for document / element / style / alert /
 * console.
 *
 * Error handling
 * --------------
 * Errors don't throw.  The evaluator carries a pj_error string;
 * any operation that fails sets it, then continues by returning
 * JS_UNDEFINED.  Callers read `pj->err[0]` after pj_eval to see
 * if anything went wrong.  This matches the "tolerant of bad
 * markup" stance the rest of the browser takes.
 */

#ifndef OSDEV_LIBC_POCKETJS_H
#define OSDEV_LIBC_POCKETJS_H

#include "syscall.h"
#include "printf.h"
#include "malloc.h"
#include "freestanding.h"

#ifndef PJ_MAX_TOKENS
#define PJ_MAX_TOKENS    512
#endif
#ifndef PJ_MAX_NODES
#define PJ_MAX_NODES     256
#endif
#ifndef PJ_MAX_CALL_DEPTH
#define PJ_MAX_CALL_DEPTH 32
#endif
#ifndef PJ_MAX_ARGS
#define PJ_MAX_ARGS      8
#endif
#ifndef PJ_MAX_GLOBALS
#define PJ_MAX_GLOBALS   16
#endif
#ifndef PJ_ERR_MAX
#define PJ_ERR_MAX       96
#endif

/* ---- value types ------------------------------------------------ */

enum {
    PJ_UNDEFINED = 0,
    PJ_NULL      = 1,
    PJ_BOOL      = 2,
    PJ_NUM       = 3,
    PJ_STR       = 4,
    PJ_HOSTOBJ   = 5,
};

struct pj_host_class;

struct pj_value {
    int type;
    union {
        long long    n;     /* PJ_NUM */
        int          b;     /* PJ_BOOL */
        const char  *s;     /* PJ_STR, NUL-terminated, arena-owned */
        struct {
            void                       *self;
            const struct pj_host_class *cls;
        } h;                /* PJ_HOSTOBJ */
    } v;
};

typedef struct pj_value (*pj_get_fn)(void *self, const char *name);
typedef int             (*pj_set_fn)(void *self, const char *name,
                                     struct pj_value val);
typedef struct pj_value (*pj_method_fn)(void *self, const char *name,
                                        struct pj_value *argv, int argc);
typedef struct pj_value (*pj_call_fn)(void *self,
                                      struct pj_value *argv, int argc);

struct pj_host_class {
    const char    *name;        /* diagnostics only */
    pj_get_fn      get;
    pj_set_fn      set;
    pj_method_fn   method;
    pj_call_fn     call;
};

/* ---- arena ------------------------------------------------------ */

struct pj_arena {
    char   *base;
    size_t  cap;
    size_t  off;
};

static inline void pj_arena_init(struct pj_arena *a, void *buf, size_t cap)
{
    a->base = (char *)buf;
    a->cap  = cap;
    a->off  = 0;
}

static inline void pj_arena_reset(struct pj_arena *a) { a->off = 0; }

static inline void *pj_alloc(struct pj_arena *a, size_t n)
{
    size_t aligned = (a->off + 7u) & ~(size_t)7u;
    if (aligned + n > a->cap) return 0;
    void *p = a->base + aligned;
    a->off = aligned + n;
    return p;
}

static inline char *pj_strdup_n(struct pj_arena *a, const char *s, size_t n)
{
    char *p = (char *)pj_alloc(a, n + 1);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    p[n] = '\0';
    return p;
}

static inline size_t pj_strlen(const char *s)
{
    size_t n = 0; if (!s) return 0; while (s[n]) n++; return n;
}

static inline int pj_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* ---- tokens ----------------------------------------------------- */

enum {
    PJ_TK_END = 0,
    PJ_TK_NUM, PJ_TK_STR, PJ_TK_IDENT,
    PJ_TK_LPAREN, PJ_TK_RPAREN,
    PJ_TK_DOT, PJ_TK_COMMA, PJ_TK_SEMI,
    PJ_TK_ASSIGN,                           /* =  */
    PJ_TK_PLUS, PJ_TK_MINUS, PJ_TK_STAR, PJ_TK_SLASH,
    PJ_TK_NOT,                              /* !  */
    PJ_TK_EQ, PJ_TK_NE,                     /* == != */
    PJ_TK_LT, PJ_TK_GT, PJ_TK_LE, PJ_TK_GE,
    PJ_TK_AND, PJ_TK_OR,                    /* && || */
    PJ_TK_TRUE, PJ_TK_FALSE, PJ_TK_NULL, PJ_TK_UNDEFINED,
};

struct pj_token {
    int          kind;
    const char  *text;      /* arena-owned, NUL-terminated (IDENT/STR) */
    long long    num;       /* NUM */
};

/* ---- AST -------------------------------------------------------- */

enum {
    PJ_N_NUM, PJ_N_STR, PJ_N_BOOL, PJ_N_NULL, PJ_N_UNDEFINED,
    PJ_N_IDENT,
    PJ_N_MEMBER,        /* obj.name */
    PJ_N_CALL,          /* fn(args)  or obj.name(args)  -- see is_method */
    PJ_N_ASSIGN,        /* lhs (MEMBER or IDENT) = rhs */
    PJ_N_UNARY,         /* op . child */
    PJ_N_BINOP,         /* op, a, b */
    PJ_N_SEQ,           /* a; b */
};

struct pj_node {
    int kind;
    int op;                         /* token kind for UNARY/BINOP */
    int argc;                       /* PJ_N_CALL: arg count */
    int is_method;                  /* PJ_N_CALL: 1 = obj.name(args) */
    long long num;                  /* PJ_N_NUM, PJ_N_BOOL (0/1) */
    const char *str;                /* PJ_N_STR, PJ_N_IDENT, PJ_N_MEMBER */
    struct pj_node *a, *b;          /* children */
    /* CALL nodes store args via a linked list rooted in
     * first_arg and chained through every arg node's next_arg.
     * The chain reuses the AST's normal pj_node pool so we don't
     * carry per-node arg slots in the 90% non-CALL case. */
    struct pj_node *first_arg;
    struct pj_node *next_arg;
};

/* ---- engine state ----------------------------------------------- */

struct pj_global {
    const char       *name;
    struct pj_value   val;
};

struct pj {
    struct pj_arena *arena;

    /* lexer + parser */
    struct pj_token  tok[PJ_MAX_TOKENS];
    int              n_tok;
    int              pos;

    /* AST node pool */
    struct pj_node   nodes[PJ_MAX_NODES];
    int              n_nodes;

    /* globals */
    struct pj_global globals[PJ_MAX_GLOBALS];
    int              n_globals;

    /* eval call depth */
    int              call_depth;

    /* error */
    char             err[PJ_ERR_MAX];
};

/* ---- error helpers ---------------------------------------------- */

static inline void pj_set_err(struct pj *p, const char *msg)
{
    if (p->err[0]) return;                  /* keep first error */
    size_t n = pj_strlen(msg);
    if (n >= PJ_ERR_MAX) n = PJ_ERR_MAX - 1;
    for (size_t i = 0; i < n; i++) p->err[i] = msg[i];
    p->err[n] = '\0';
}

static inline struct pj_value pj_undef(void)
{
    struct pj_value v; v.type = PJ_UNDEFINED; v.v.n = 0; return v;
}

static inline struct pj_value pj_num(long long n)
{
    struct pj_value v; v.type = PJ_NUM; v.v.n = n; return v;
}

static inline struct pj_value pj_bool(int b)
{
    struct pj_value v; v.type = PJ_BOOL; v.v.b = b ? 1 : 0; return v;
}

static inline struct pj_value pj_str(const char *s)
{
    struct pj_value v; v.type = PJ_STR; v.v.s = s; return v;
}

static inline struct pj_value pj_host(void *self,
                                       const struct pj_host_class *cls)
{
    struct pj_value v;
    v.type = PJ_HOSTOBJ; v.v.h.self = self; v.v.h.cls = cls;
    return v;
}

/* ---- lexer ------------------------------------------------------ */

static inline int pj_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static inline int pj_is_alnum(int c)
{
    return pj_is_alpha(c) || (c >= '0' && c <= '9');
}

static inline int pj_is_digit(int c) { return c >= '0' && c <= '9'; }

static int pj_push_tok(struct pj *p, int kind)
{
    if (p->n_tok >= PJ_MAX_TOKENS) { pj_set_err(p, "token limit"); return -1; }
    struct pj_token *t = &p->tok[p->n_tok++];
    t->kind = kind; t->text = 0; t->num = 0;
    return 0;
}

static int pj_lex(struct pj *p, const char *src)
{
    p->n_tok = 0;
    size_t i = 0;
    while (src[i]) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }

        /* identifier / keyword */
        if (pj_is_alpha((unsigned char)c)) {
            size_t s = i;
            while (src[i] && pj_is_alnum((unsigned char)src[i])) i++;
            size_t len = i - s;
            const char *text = pj_strdup_n(p->arena, src + s, len);
            if (!text) { pj_set_err(p, "arena oom (ident)"); return -1; }
            int kind = PJ_TK_IDENT;
            if (pj_streq(text, "true"))           kind = PJ_TK_TRUE;
            else if (pj_streq(text, "false"))     kind = PJ_TK_FALSE;
            else if (pj_streq(text, "null"))      kind = PJ_TK_NULL;
            else if (pj_streq(text, "undefined")) kind = PJ_TK_UNDEFINED;
            if (pj_push_tok(p, kind) < 0) return -1;
            p->tok[p->n_tok - 1].text = text;
            continue;
        }

        /* number (integer only) */
        if (pj_is_digit((unsigned char)c)) {
            long long n = 0;
            while (src[i] && pj_is_digit((unsigned char)src[i])) {
                n = n * 10 + (src[i] - '0'); i++;
            }
            if (pj_push_tok(p, PJ_TK_NUM) < 0) return -1;
            p->tok[p->n_tok - 1].num = n;
            continue;
        }

        /* string -- single or double quote, backslash escapes */
        if (c == '\'' || c == '"') {
            char q = c;
            i++;
            /* worst-case length = remainder of source */
            size_t cap = pj_strlen(src + i) + 1;
            char *buf = (char *)pj_alloc(p->arena, cap);
            if (!buf) { pj_set_err(p, "arena oom (str)"); return -1; }
            size_t k = 0;
            while (src[i] && src[i] != q) {
                if (src[i] == '\\' && src[i + 1]) {
                    char e = src[i + 1];
                    char d = e;
                    if      (e == 'n')  d = '\n';
                    else if (e == 't')  d = '\t';
                    else if (e == 'r')  d = '\r';
                    else if (e == '\\') d = '\\';
                    else if (e == '\'') d = '\'';
                    else if (e == '"')  d = '"';
                    buf[k++] = d;
                    i += 2;
                } else {
                    buf[k++] = src[i++];
                }
            }
            if (src[i] != q) { pj_set_err(p, "unterminated string"); return -1; }
            i++;        /* closing quote */
            buf[k] = '\0';
            if (pj_push_tok(p, PJ_TK_STR) < 0) return -1;
            p->tok[p->n_tok - 1].text = buf;
            continue;
        }

        /* punctuation / operators */
        switch (c) {
        case '(': i++; if (pj_push_tok(p, PJ_TK_LPAREN) < 0) return -1; continue;
        case ')': i++; if (pj_push_tok(p, PJ_TK_RPAREN) < 0) return -1; continue;
        case '.': i++; if (pj_push_tok(p, PJ_TK_DOT)    < 0) return -1; continue;
        case ',': i++; if (pj_push_tok(p, PJ_TK_COMMA)  < 0) return -1; continue;
        case ';': i++; if (pj_push_tok(p, PJ_TK_SEMI)   < 0) return -1; continue;
        case '+': i++; if (pj_push_tok(p, PJ_TK_PLUS)   < 0) return -1; continue;
        case '-': i++; if (pj_push_tok(p, PJ_TK_MINUS)  < 0) return -1; continue;
        case '*': i++; if (pj_push_tok(p, PJ_TK_STAR)   < 0) return -1; continue;
        case '/': i++; if (pj_push_tok(p, PJ_TK_SLASH)  < 0) return -1; continue;
        case '=':
            if (src[i + 1] == '=') { i += 2; if (pj_push_tok(p, PJ_TK_EQ)     < 0) return -1; }
            else                   { i++;    if (pj_push_tok(p, PJ_TK_ASSIGN) < 0) return -1; }
            continue;
        case '!':
            if (src[i + 1] == '=') { i += 2; if (pj_push_tok(p, PJ_TK_NE)  < 0) return -1; }
            else                   { i++;    if (pj_push_tok(p, PJ_TK_NOT) < 0) return -1; }
            continue;
        case '<':
            if (src[i + 1] == '=') { i += 2; if (pj_push_tok(p, PJ_TK_LE) < 0) return -1; }
            else                   { i++;    if (pj_push_tok(p, PJ_TK_LT) < 0) return -1; }
            continue;
        case '>':
            if (src[i + 1] == '=') { i += 2; if (pj_push_tok(p, PJ_TK_GE) < 0) return -1; }
            else                   { i++;    if (pj_push_tok(p, PJ_TK_GT) < 0) return -1; }
            continue;
        case '&':
            if (src[i + 1] == '&') { i += 2; if (pj_push_tok(p, PJ_TK_AND) < 0) return -1; continue; }
            pj_set_err(p, "unexpected '&'"); return -1;
        case '|':
            if (src[i + 1] == '|') { i += 2; if (pj_push_tok(p, PJ_TK_OR) < 0) return -1; continue; }
            pj_set_err(p, "unexpected '|'"); return -1;
        default:
            pj_set_err(p, "unexpected character"); return -1;
        }
    }
    if (pj_push_tok(p, PJ_TK_END) < 0) return -1;
    return 0;
}

/* ---- parser ----------------------------------------------------- */

static struct pj_node *pj_node_new(struct pj *p, int kind)
{
    if (p->n_nodes >= PJ_MAX_NODES) { pj_set_err(p, "node limit"); return 0; }
    struct pj_node *n = &p->nodes[p->n_nodes++];
    n->kind = kind; n->op = 0; n->argc = 0; n->is_method = 0;
    n->num = 0; n->str = 0;
    n->a = n->b = 0;
    n->first_arg = 0; n->next_arg = 0;
    return n;
}

static inline struct pj_token *pj_peek(struct pj *p) { return &p->tok[p->pos]; }
static inline void              pj_advance(struct pj *p) { if (p->tok[p->pos].kind != PJ_TK_END) p->pos++; }
static inline int               pj_accept(struct pj *p, int k)
{ if (p->tok[p->pos].kind == k) { p->pos++; return 1; } return 0; }

/* forward decls */
static struct pj_node *pj_parse_assign(struct pj *p);
static struct pj_node *pj_parse_or    (struct pj *p);

static struct pj_node *pj_parse_primary(struct pj *p)
{
    struct pj_token *t = pj_peek(p);
    struct pj_node *n;
    switch (t->kind) {
    case PJ_TK_NUM:
        n = pj_node_new(p, PJ_N_NUM); if (!n) return 0; n->num = t->num;
        pj_advance(p); return n;
    case PJ_TK_STR:
        n = pj_node_new(p, PJ_N_STR); if (!n) return 0; n->str = t->text;
        pj_advance(p); return n;
    case PJ_TK_TRUE:
        n = pj_node_new(p, PJ_N_BOOL); if (!n) return 0; n->num = 1;
        pj_advance(p); return n;
    case PJ_TK_FALSE:
        n = pj_node_new(p, PJ_N_BOOL); if (!n) return 0; n->num = 0;
        pj_advance(p); return n;
    case PJ_TK_NULL:
        n = pj_node_new(p, PJ_N_NULL); pj_advance(p); return n;
    case PJ_TK_UNDEFINED:
        n = pj_node_new(p, PJ_N_UNDEFINED); pj_advance(p); return n;
    case PJ_TK_IDENT:
        n = pj_node_new(p, PJ_N_IDENT); if (!n) return 0; n->str = t->text;
        pj_advance(p); return n;
    case PJ_TK_LPAREN: {
        pj_advance(p);
        n = pj_parse_assign(p);
        if (!pj_accept(p, PJ_TK_RPAREN)) { pj_set_err(p, "missing ')'"); return 0; }
        return n;
    }
    default:
        pj_set_err(p, "unexpected token in expression"); return 0;
    }
}

static struct pj_node *pj_parse_args(struct pj *p, struct pj_node *callee)
{
    /* '(' already consumed */
    struct pj_node *call = pj_node_new(p, PJ_N_CALL);
    if (!call) return 0;
    call->a = callee;
    struct pj_node *tail = 0;
    if (pj_peek(p)->kind != PJ_TK_RPAREN) {
        for (;;) {
            if (call->argc >= PJ_MAX_ARGS) { pj_set_err(p, "too many args"); return 0; }
            struct pj_node *a = pj_parse_assign(p);
            if (!a) return 0;
            if (tail) tail->next_arg = a; else call->first_arg = a;
            tail = a;
            call->argc++;
            if (pj_accept(p, PJ_TK_COMMA)) continue;
            break;
        }
    }
    if (!pj_accept(p, PJ_TK_RPAREN)) { pj_set_err(p, "missing ')' in call"); return 0; }
    return call;
}

static struct pj_node *pj_parse_postfix(struct pj *p)
{
    struct pj_node *cur = pj_parse_primary(p);
    if (!cur) return 0;
    for (;;) {
        if (pj_accept(p, PJ_TK_DOT)) {
            struct pj_token *t = pj_peek(p);
            if (t->kind != PJ_TK_IDENT) { pj_set_err(p, "expected name after '.'"); return 0; }
            struct pj_node *m = pj_node_new(p, PJ_N_MEMBER);
            if (!m) return 0;
            m->a = cur; m->str = t->text;
            pj_advance(p);
            cur = m;
            continue;
        }
        if (pj_accept(p, PJ_TK_LPAREN)) {
            struct pj_node *call = pj_parse_args(p, cur);
            if (!call) return 0;
            call->is_method = (cur->kind == PJ_N_MEMBER);
            cur = call;
            continue;
        }
        break;
    }
    return cur;
}

static struct pj_node *pj_parse_unary(struct pj *p)
{
    struct pj_token *t = pj_peek(p);
    if (t->kind == PJ_TK_MINUS || t->kind == PJ_TK_NOT) {
        int op = t->kind; pj_advance(p);
        struct pj_node *child = pj_parse_unary(p);
        if (!child) return 0;
        struct pj_node *u = pj_node_new(p, PJ_N_UNARY);
        if (!u) return 0;
        u->op = op; u->a = child;
        return u;
    }
    return pj_parse_postfix(p);
}

static struct pj_node *pj_parse_binop_l(struct pj *p,
                                         struct pj_node *(*sub)(struct pj *),
                                         const int *ops, int n_ops)
{
    struct pj_node *left = sub(p);
    if (!left) return 0;
    for (;;) {
        int op = pj_peek(p)->kind, hit = 0;
        for (int i = 0; i < n_ops; i++) if (ops[i] == op) { hit = 1; break; }
        if (!hit) return left;
        pj_advance(p);
        struct pj_node *right = sub(p);
        if (!right) return 0;
        struct pj_node *b = pj_node_new(p, PJ_N_BINOP);
        if (!b) return 0;
        b->op = op; b->a = left; b->b = right;
        left = b;
    }
}

static struct pj_node *pj_parse_term(struct pj *p)
{
    static const int ops[] = { PJ_TK_STAR, PJ_TK_SLASH };
    return pj_parse_binop_l(p, pj_parse_unary, ops, 2);
}

static struct pj_node *pj_parse_sum(struct pj *p)
{
    static const int ops[] = { PJ_TK_PLUS, PJ_TK_MINUS };
    return pj_parse_binop_l(p, pj_parse_term, ops, 2);
}

static struct pj_node *pj_parse_cmp(struct pj *p)
{
    static const int ops[] = { PJ_TK_LT, PJ_TK_GT, PJ_TK_LE, PJ_TK_GE };
    return pj_parse_binop_l(p, pj_parse_sum, ops, 4);
}

static struct pj_node *pj_parse_eq(struct pj *p)
{
    static const int ops[] = { PJ_TK_EQ, PJ_TK_NE };
    return pj_parse_binop_l(p, pj_parse_cmp, ops, 2);
}

static struct pj_node *pj_parse_and(struct pj *p)
{
    static const int ops[] = { PJ_TK_AND };
    return pj_parse_binop_l(p, pj_parse_eq, ops, 1);
}

static struct pj_node *pj_parse_or(struct pj *p)
{
    static const int ops[] = { PJ_TK_OR };
    return pj_parse_binop_l(p, pj_parse_and, ops, 1);
}

static struct pj_node *pj_parse_assign(struct pj *p)
{
    struct pj_node *lhs = pj_parse_or(p);
    if (!lhs) return 0;
    if (pj_accept(p, PJ_TK_ASSIGN)) {
        if (lhs->kind != PJ_N_MEMBER && lhs->kind != PJ_N_IDENT) {
            pj_set_err(p, "invalid assignment target"); return 0;
        }
        struct pj_node *rhs = pj_parse_assign(p);
        if (!rhs) return 0;
        struct pj_node *a = pj_node_new(p, PJ_N_ASSIGN);
        if (!a) return 0;
        a->a = lhs; a->b = rhs;
        return a;
    }
    return lhs;
}

static struct pj_node *pj_parse_program(struct pj *p)
{
    /* program ::= assign (';' assign)* ';'?  -- empty is undefined. */
    if (pj_peek(p)->kind == PJ_TK_END) return pj_node_new(p, PJ_N_UNDEFINED);

    struct pj_node *first = pj_parse_assign(p);
    if (!first) return 0;
    struct pj_node *cur = first;
    while (pj_accept(p, PJ_TK_SEMI)) {
        if (pj_peek(p)->kind == PJ_TK_END) break;
        struct pj_node *next = pj_parse_assign(p);
        if (!next) return 0;
        struct pj_node *seq = pj_node_new(p, PJ_N_SEQ);
        if (!seq) return 0;
        seq->a = cur; seq->b = next;
        cur = seq;
    }
    if (pj_peek(p)->kind != PJ_TK_END) { pj_set_err(p, "trailing junk"); return 0; }
    return cur;
}

/* ---- evaluator -------------------------------------------------- */

static struct pj_value pj_eval_node(struct pj *p, struct pj_node *n);

static const char *pj_value_to_str(struct pj *p, struct pj_value v)
{
    char buf[64];
    int  len = 0;
    switch (v.type) {
    case PJ_UNDEFINED: return "undefined";
    case PJ_NULL:      return "null";
    case PJ_BOOL:      return v.v.b ? "true" : "false";
    case PJ_NUM:
        len = snprintf(buf, sizeof(buf), "%lld", v.v.n);
        if (len < 0) len = 0;
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        return pj_strdup_n(p->arena, buf, (size_t)len);
    case PJ_STR:       return v.v.s ? v.v.s : "";
    case PJ_HOSTOBJ: {
        const char *cn = (v.v.h.cls && v.v.h.cls->name) ? v.v.h.cls->name : "host";
        size_t cn_n = pj_strlen(cn);
        char *out = (char *)pj_alloc(p->arena, cn_n + 10);
        if (!out) return "[host]";
        const char *prefix = "[object ";
        size_t i = 0;
        for (; prefix[i]; i++) out[i] = prefix[i];
        for (size_t j = 0; j < cn_n; j++) out[i + j] = cn[j];
        out[i + cn_n] = ']'; out[i + cn_n + 1] = '\0';
        return out;
    }
    }
    return "";
}

static int pj_value_to_bool(struct pj_value v)
{
    switch (v.type) {
    case PJ_UNDEFINED:
    case PJ_NULL:    return 0;
    case PJ_BOOL:    return v.v.b;
    case PJ_NUM:     return v.v.n != 0;
    case PJ_STR:     return v.v.s && v.v.s[0];
    case PJ_HOSTOBJ: return 1;
    }
    return 0;
}

static long long pj_value_to_num(struct pj_value v)
{
    switch (v.type) {
    case PJ_NUM:  return v.v.n;
    case PJ_BOOL: return v.v.b;
    case PJ_STR: {
        if (!v.v.s) return 0;
        const char *s = v.v.s;
        int sign = 1;
        if (*s == '-') { sign = -1; s++; }
        long long n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
        return sign * n;
    }
    default: return 0;
    }
}

static int pj_value_eq(struct pj_value a, struct pj_value b)
{
    if (a.type == b.type) {
        switch (a.type) {
        case PJ_UNDEFINED:
        case PJ_NULL:     return 1;
        case PJ_BOOL:     return a.v.b == b.v.b;
        case PJ_NUM:      return a.v.n == b.v.n;
        case PJ_STR:
            if (!a.v.s || !b.v.s) return a.v.s == b.v.s;
            return pj_streq(a.v.s, b.v.s);
        case PJ_HOSTOBJ:  return a.v.h.self == b.v.h.self;
        }
    }
    /* loose equality: number-string coerces to number compare */
    if ((a.type == PJ_NUM && b.type == PJ_STR) ||
        (a.type == PJ_STR && b.type == PJ_NUM)) {
        return pj_value_to_num(a) == pj_value_to_num(b);
    }
    return 0;
}

static struct pj_value pj_lookup_global(struct pj *p, const char *name)
{
    for (int i = 0; i < p->n_globals; i++)
        if (pj_streq(p->globals[i].name, name))
            return p->globals[i].val;
    return pj_undef();
}

static int pj_assign_global(struct pj *p, const char *name, struct pj_value v)
{
    for (int i = 0; i < p->n_globals; i++) {
        if (pj_streq(p->globals[i].name, name)) {
            p->globals[i].val = v; return 0;
        }
    }
    /* Auto-create on assign (mirrors loose JS).  Fails silently on
     * overflow -- caller can check pj->err if they care. */
    if (p->n_globals >= PJ_MAX_GLOBALS) { pj_set_err(p, "globals full"); return -1; }
    p->globals[p->n_globals].name = name;
    p->globals[p->n_globals].val  = v;
    p->n_globals++;
    return 0;
}

static struct pj_value pj_eval_binop(struct pj *p, int op,
                                      struct pj_value A, struct pj_value B)
{
    if (op == PJ_TK_PLUS) {
        /* string concat if either side is string */
        if (A.type == PJ_STR || B.type == PJ_STR) {
            const char *sa = pj_value_to_str(p, A);
            const char *sb = pj_value_to_str(p, B);
            size_t la = pj_strlen(sa), lb = pj_strlen(sb);
            char *out = (char *)pj_alloc(p->arena, la + lb + 1);
            if (!out) { pj_set_err(p, "arena oom (concat)"); return pj_undef(); }
            for (size_t i = 0; i < la; i++) out[i] = sa[i];
            for (size_t i = 0; i < lb; i++) out[la + i] = sb[i];
            out[la + lb] = '\0';
            return pj_str(out);
        }
        return pj_num(pj_value_to_num(A) + pj_value_to_num(B));
    }
    if (op == PJ_TK_MINUS) return pj_num(pj_value_to_num(A) - pj_value_to_num(B));
    if (op == PJ_TK_STAR)  return pj_num(pj_value_to_num(A) * pj_value_to_num(B));
    if (op == PJ_TK_SLASH) {
        long long b = pj_value_to_num(B);
        if (b == 0) { pj_set_err(p, "div by zero"); return pj_undef(); }
        return pj_num(pj_value_to_num(A) / b);
    }
    if (op == PJ_TK_EQ) return pj_bool( pj_value_eq(A, B));
    if (op == PJ_TK_NE) return pj_bool(!pj_value_eq(A, B));
    if (op == PJ_TK_LT) return pj_bool(pj_value_to_num(A) <  pj_value_to_num(B));
    if (op == PJ_TK_GT) return pj_bool(pj_value_to_num(A) >  pj_value_to_num(B));
    if (op == PJ_TK_LE) return pj_bool(pj_value_to_num(A) <= pj_value_to_num(B));
    if (op == PJ_TK_GE) return pj_bool(pj_value_to_num(A) >= pj_value_to_num(B));
    if (op == PJ_TK_AND) return pj_value_to_bool(A) ? B : A;
    if (op == PJ_TK_OR)  return pj_value_to_bool(A) ? A : B;
    pj_set_err(p, "unknown binop"); return pj_undef();
}

static struct pj_value pj_eval_call(struct pj *p, struct pj_node *n)
{
    if (p->call_depth >= PJ_MAX_CALL_DEPTH) {
        pj_set_err(p, "call stack overflow"); return pj_undef();
    }
    struct pj_value argv[PJ_MAX_ARGS];
    int argc = 0;
    for (struct pj_node *a = n->first_arg; a && argc < PJ_MAX_ARGS; a = a->next_arg)
        argv[argc++] = pj_eval_node(p, a);

    p->call_depth++;
    struct pj_value rv = pj_undef();
    if (n->is_method) {
        /* obj.name(args) */
        struct pj_node *m = n->a;       /* PJ_N_MEMBER */
        struct pj_value obj = pj_eval_node(p, m->a);
        if (obj.type != PJ_HOSTOBJ || !obj.v.h.cls || !obj.v.h.cls->method) {
            pj_set_err(p, "method call on non-callable");
        } else {
            rv = obj.v.h.cls->method(obj.v.h.self, m->str, argv, argc);
        }
    } else {
        struct pj_value callee = pj_eval_node(p, n->a);
        if (callee.type != PJ_HOSTOBJ || !callee.v.h.cls || !callee.v.h.cls->call) {
            pj_set_err(p, "call on non-callable");
        } else {
            rv = callee.v.h.cls->call(callee.v.h.self, argv, argc);
        }
    }
    p->call_depth--;
    return rv;
}

static struct pj_value pj_eval_assign(struct pj *p, struct pj_node *n)
{
    struct pj_value rhs = pj_eval_node(p, n->b);
    if (n->a->kind == PJ_N_IDENT) {
        pj_assign_global(p, n->a->str, rhs);
        return rhs;
    }
    /* MEMBER */
    struct pj_value obj = pj_eval_node(p, n->a->a);
    if (obj.type != PJ_HOSTOBJ || !obj.v.h.cls || !obj.v.h.cls->set) {
        pj_set_err(p, "set on non-settable");
        return rhs;
    }
    obj.v.h.cls->set(obj.v.h.self, n->a->str, rhs);
    return rhs;
}

static struct pj_value pj_eval_node(struct pj *p, struct pj_node *n)
{
    if (!n) return pj_undef();
    if (p->err[0]) return pj_undef();

    switch (n->kind) {
    case PJ_N_NUM:       return pj_num(n->num);
    case PJ_N_STR:       return pj_str(n->str);
    case PJ_N_BOOL:      return pj_bool((int)n->num);
    case PJ_N_NULL: {    struct pj_value v; v.type = PJ_NULL; v.v.n = 0; return v; }
    case PJ_N_UNDEFINED: return pj_undef();
    case PJ_N_IDENT:     return pj_lookup_global(p, n->str);
    case PJ_N_MEMBER: {
        struct pj_value o = pj_eval_node(p, n->a);
        if (o.type != PJ_HOSTOBJ || !o.v.h.cls || !o.v.h.cls->get) {
            return pj_undef();
        }
        return o.v.h.cls->get(o.v.h.self, n->str);
    }
    case PJ_N_CALL:      return pj_eval_call(p, n);
    case PJ_N_ASSIGN:    return pj_eval_assign(p, n);
    case PJ_N_UNARY: {
        struct pj_value c = pj_eval_node(p, n->a);
        if (n->op == PJ_TK_MINUS) return pj_num(-pj_value_to_num(c));
        if (n->op == PJ_TK_NOT)   return pj_bool(!pj_value_to_bool(c));
        return pj_undef();
    }
    case PJ_N_BINOP: {
        if (n->op == PJ_TK_AND) {
            struct pj_value a = pj_eval_node(p, n->a);
            if (!pj_value_to_bool(a)) return a;
            return pj_eval_node(p, n->b);
        }
        if (n->op == PJ_TK_OR) {
            struct pj_value a = pj_eval_node(p, n->a);
            if (pj_value_to_bool(a)) return a;
            return pj_eval_node(p, n->b);
        }
        struct pj_value A = pj_eval_node(p, n->a);
        struct pj_value B = pj_eval_node(p, n->b);
        return pj_eval_binop(p, n->op, A, B);
    }
    case PJ_N_SEQ: {
        (void)pj_eval_node(p, n->a);
        return pj_eval_node(p, n->b);
    }
    }
    return pj_undef();
}

/* ---- public API ------------------------------------------------- */

static inline void pj_init(struct pj *p, struct pj_arena *a)
{
    p->arena = a;
    p->n_tok = 0; p->pos = 0;
    p->n_nodes = 0; p->n_globals = 0;
    p->call_depth = 0; p->err[0] = '\0';
}

static inline int pj_set_global(struct pj *p, const char *name, struct pj_value v)
{
    return pj_assign_global(p, name, v);
}

static inline struct pj_value pj_eval(struct pj *p, const char *src)
{
    p->n_tok = 0; p->pos = 0; p->n_nodes = 0;
    p->call_depth = 0; p->err[0] = '\0';
    if (pj_lex(p, src) < 0) return pj_undef();
    struct pj_node *prog = pj_parse_program(p);
    if (!prog) return pj_undef();
    return pj_eval_node(p, prog);
}

#endif /* OSDEV_LIBC_POCKETJS_H */
