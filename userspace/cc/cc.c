/*
 * userspace/cc/cc.c — chapter 157's one-chapter native C compiler.
 *
 * A *deliberately tiny* C compiler.  Curated subset:
 *
 *   - One function, must be:
 *         int main(void) { ... }
 *     or  int main()     { ... }
 *     or  int main(int argc, char **argv) { ... }
 *   - Statements (semicolon-terminated):
 *         printf("LITERAL");
 *         puts("LITERAL");
 *         write(FD, "LITERAL", LEN);
 *         return INT_LITERAL;
 *   - C99 // line comments and C-style block comments.
 *   - Free whitespace.  Tabs and CRLF both fine.
 *
 * Not supported (lands in chapter 159's GCC port):
 *   - Variables, expressions, control flow, function defs
 *     beyond main, #include, format specifiers, anything
 *     not on the list above.
 *
 * Pipeline:
 *
 *   cc src.c -o out
 *     ├─ parse src.c → emit asm to /tmp/<basename>.cc.s
 *     ├─ spawn /bin/as <tmp>.s -o <tmp>.o
 *     ├─ spawn /bin/ld -o out <tmp>.o
 *     └─ remove intermediates (unless -S or -c)
 *
 *   cc -S src.c -o out.s   stops after emit
 *   cc -c src.c -o out.o   stops after /bin/as
 *
 * Codegen strategy: every string literal is materialised
 * inline in .text using the classic "bl past_str" trick.
 * The branch sets x30 (LR) to the byte right after the bl,
 * which is precisely the .ascii payload.  We then use x30
 * as the string pointer.  This avoids needing the linker
 * to support ADRP / ADR_PREL_PG_HI21 relocations (which
 * chapter 154's /bin/as does not emit).
 *
 * Per `printf("STR\n");` we emit:
 *
 *     bl   .LSjmpN
 *   .LSdataN:
 *     .ascii "STR\n"
 *     .balign 4
 *   .LSjmpN:
 *     mov   x1, x30           ; buf
 *     mov   x2, #LEN          ; count
 *     mov   x0, #1            ; fd = stdout
 *     mov   x8, #1            ; SYS_WRITE
 *     svc   #0
 *
 * Per `return N;`:
 *
 *     mov   x0, #N
 *     mov   x8, #2            ; SYS_EXIT
 *     svc   #0
 *
 * Fall-through from main exits with code 0.
 *
 * Driver returns 0 on success, 1 on any compile / assemble /
 * link error.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/stdio.h"

#define MAX_SRC      (256 * 1024)
#define MAX_ASM      (1024 * 1024)
#define MAX_TOK_LEN  4096
#define MAX_PATH     256

/* ──────────────────────────────────────────────────────────
 * Tiny string helpers (freestanding, no libc dep beyond
 * what userspace/libc/printf.h gives us).
 */
static int cc_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void cc_strcpy(char *d, const char *s)
{
    while ((*d++ = *s++) != 0) { }
}

static int cc_isdigit(int c) { return c >= '0' && c <= '9'; }
static int cc_isspace(int c) { return c == ' ' || c == '\t' ||
                                       c == '\n' || c == '\r'; }
static int cc_isalpha(int c) { return (c >= 'a' && c <= 'z') ||
                                       (c >= 'A' && c <= 'Z') ||
                                       c == '_'; }
static int cc_isalnum(int c) { return cc_isalpha(c) || cc_isdigit(c); }

/* memset/memcpy avoidance: never zero-init big structs at file
 * scope (the user memory note "freestanding-c-memset-trap" covers
 * the trap).  Our globals are .bss so they start zero implicitly. */
static char  g_src[MAX_SRC];
static int   g_src_n;
static int   g_src_pos;
static int   g_src_line;

static char  g_asm[MAX_ASM];
static int   g_asm_n;

static int   g_had_error;

/* ──────────────────────────────────────────────────────────
 * I/O helpers.
 */
static int read_file(const char *path, char *buf, int max)
{
    int fd = open(path, /*O_RDONLY*/ 0);
    if (fd < 0) {
        printf("cc: cannot open '%s'\n", path);
        return -1;
    }
    int total = 0;
    while (total < max) {
        long n = read(fd, buf + total, max - total);
        if (n <= 0) break;
        total += (int)n;
    }
    close(fd);
    if (total >= max) {
        printf("cc: input '%s' exceeds %d bytes\n", path, max);
        return -1;
    }
    buf[total] = '\0';
    return total;
}

static int write_file(const char *path, const char *buf, int n)
{
    /* O_WRONLY|O_CREAT|O_TRUNC.  open() in our libc is a 2-arg
     * shim — the kernel side handles the create/truncate flags
     * via the same syscall path /bin/as and /bin/ld use. */
    int fd = open(path, 0x241 /* O_WRONLY|O_CREAT|O_TRUNC */);
    if (fd < 0) {
        printf("cc: cannot create '%s'\n", path);
        return -1;
    }
    int total = 0;
    while (total < n) {
        long w = write(fd, buf + total, n - total);
        if (w <= 0) { close(fd); return -1; }
        total += (int)w;
    }
    close(fd);
    return 0;
}

/* asm buffer emit. */
static void emit_str(const char *s)
{
    while (*s) {
        if (g_asm_n >= MAX_ASM - 1) return;
        g_asm[g_asm_n++] = *s++;
    }
}

static void emit_int(long v)
{
    char buf[32];
    int n = 0;
    if (v < 0) { emit_str("-"); v = -v; }
    if (v == 0) { emit_str("0"); return; }
    char tmp[32]; int t = 0;
    while (v > 0) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0) buf[n++] = tmp[--t];
    buf[n] = '\0';
    emit_str(buf);
}

/* ──────────────────────────────────────────────────────────
 * Lexer.
 *
 * We tokenise on demand from g_src[g_src_pos..].  Tokens
 * carry kind + spelling + optional integer value.
 */
enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_INT,
    TK_STRING,
    TK_LPAREN, TK_RPAREN,
    TK_LBRACE, TK_RBRACE,
    TK_COMMA,
    TK_SEMI,
    TK_STAR,         /* for `char **argv` parsing */
    TK_KW_INT,
    TK_KW_VOID,
    TK_KW_CHAR,
    TK_KW_RETURN,
    TK_PLUS,
    TK_MINUS,
    TK_EQ,
};

static int   g_tok_kind;
static long  g_tok_int;
static char  g_tok_str[MAX_TOK_LEN];
static int   g_tok_str_n;
static int   g_tok_line;

static void cc_err(const char *msg)
{
    printf("cc: %s on line %d\n", msg, g_tok_line);
    g_had_error = 1;
}

static int consume_char(void)
{
    if (g_src_pos >= g_src_n) return 0;
    int c = (unsigned char)g_src[g_src_pos++];
    if (c == '\n') g_src_line++;
    return c;
}

static void skip_ws_and_comments(void)
{
    for (;;) {
        while (g_src_pos < g_src_n && cc_isspace(g_src[g_src_pos]))
            consume_char();
        if (g_src_pos + 1 >= g_src_n) return;
        if (g_src[g_src_pos] == '/' && g_src[g_src_pos + 1] == '/') {
            while (g_src_pos < g_src_n && g_src[g_src_pos] != '\n')
                consume_char();
            continue;
        }
        if (g_src[g_src_pos] == '/' && g_src[g_src_pos + 1] == '*') {
            consume_char(); consume_char();
            while (g_src_pos + 1 < g_src_n &&
                   !(g_src[g_src_pos] == '*' &&
                     g_src[g_src_pos + 1] == '/')) {
                consume_char();
            }
            if (g_src_pos + 1 < g_src_n) {
                consume_char(); consume_char();
            }
            continue;
        }
        return;
    }
}

static int kw_lookup(const char *s)
{
    if (cc_streq(s, "int"))    return TK_KW_INT;
    if (cc_streq(s, "void"))   return TK_KW_VOID;
    if (cc_streq(s, "char"))   return TK_KW_CHAR;
    if (cc_streq(s, "return")) return TK_KW_RETURN;
    return TK_IDENT;
}

static void lex(void)
{
    skip_ws_and_comments();
    g_tok_line = g_src_line;
    if (g_src_pos >= g_src_n) { g_tok_kind = TK_EOF; return; }

    int c = (unsigned char)g_src[g_src_pos];

    if (cc_isalpha(c)) {
        int n = 0;
        while (g_src_pos < g_src_n &&
               cc_isalnum((unsigned char)g_src[g_src_pos]) &&
               n < MAX_TOK_LEN - 1) {
            g_tok_str[n++] = g_src[g_src_pos];
            consume_char();
        }
        g_tok_str[n] = '\0';
        g_tok_str_n = n;
        g_tok_kind = kw_lookup(g_tok_str);
        return;
    }
    if (cc_isdigit(c)) {
        long v = 0;
        while (g_src_pos < g_src_n &&
               cc_isdigit((unsigned char)g_src[g_src_pos])) {
            v = v * 10 + (g_src[g_src_pos] - '0');
            consume_char();
        }
        g_tok_int = v;
        g_tok_kind = TK_INT;
        return;
    }
    if (c == '"') {
        consume_char();
        int n = 0;
        while (g_src_pos < g_src_n &&
               g_src[g_src_pos] != '"' &&
               n < MAX_TOK_LEN - 1) {
            if (g_src[g_src_pos] == '\\') {
                consume_char();
                int e = (unsigned char)g_src[g_src_pos];
                consume_char();
                switch (e) {
                    case 'n':  g_tok_str[n++] = '\n'; break;
                    case 't':  g_tok_str[n++] = '\t'; break;
                    case 'r':  g_tok_str[n++] = '\r'; break;
                    case '0':  g_tok_str[n++] = '\0'; break;
                    case '\\': g_tok_str[n++] = '\\'; break;
                    case '"':  g_tok_str[n++] = '"';  break;
                    default:   g_tok_str[n++] = (char)e; break;
                }
                continue;
            }
            g_tok_str[n++] = g_src[g_src_pos];
            consume_char();
        }
        if (g_src_pos < g_src_n && g_src[g_src_pos] == '"')
            consume_char();
        else {
            cc_err("unterminated string literal");
        }
        g_tok_str[n] = '\0';
        g_tok_str_n = n;
        g_tok_kind = TK_STRING;
        return;
    }

    consume_char();
    switch (c) {
        case '(': g_tok_kind = TK_LPAREN; return;
        case ')': g_tok_kind = TK_RPAREN; return;
        case '{': g_tok_kind = TK_LBRACE; return;
        case '}': g_tok_kind = TK_RBRACE; return;
        case ',': g_tok_kind = TK_COMMA;  return;
        case ';': g_tok_kind = TK_SEMI;   return;
        case '*': g_tok_kind = TK_STAR;   return;
        case '+': g_tok_kind = TK_PLUS;   return;
        case '-': g_tok_kind = TK_MINUS;  return;
        case '=': g_tok_kind = TK_EQ;     return;
    }
    cc_err("unexpected character");
    g_tok_kind = TK_EOF;
}

static void expect(int kind, const char *what)
{
    if (g_tok_kind != kind) {
        cc_err(what);
        return;
    }
    lex();
}

/* ──────────────────────────────────────────────────────────
 * Code emitters for individual statement patterns.
 */
static int g_label_counter;

/* ──────────────────────────────────────────────────────────
 * Chapter 159 additions: local variable table + expression
 * stack-machine codegen.
 *
 * Layout of the function frame (set up by prologue):
 *
 *      sp + 0     var[0]
 *      sp + 8     var[1]
 *      ...
 *      sp + 120   var[15]
 *      sp + 128   expr-push slot 0
 *      sp + 136   expr-push slot 1
 *      ...
 *      sp + 248   expr-push slot 15
 *
 * Total 256 bytes, reserved once per function with
 *      sub sp, sp, #256
 *
 * Push depth is tracked at compile time in g_expr_depth.
 * Push:   str x0, [sp, #(128 + depth*8)]; depth++
 * Pop:    depth--; ldr x1, [sp, #(128 + depth*8)]
 */
#define CC_MAX_VARS  16
#define CC_MAX_DEPTH 16

static char g_var_name[CC_MAX_VARS][MAX_TOK_LEN];
static int  g_var_count;
static int  g_expr_depth;
static int  g_max_expr_depth;   /* tracked for sanity; frame is fixed */

static int var_find(const char *name)
{
    for (int i = 0; i < g_var_count; i++) {
        if (cc_streq(g_var_name[i], name)) return i;
    }
    return -1;
}

static int var_declare(const char *name)
{
    if (var_find(name) >= 0) {
        cc_err("variable redeclared"); return -1;
    }
    if (g_var_count >= CC_MAX_VARS) {
        cc_err("too many locals (max 16)"); return -1;
    }
    int idx = g_var_count++;
    cc_strcpy(g_var_name[idx], name);
    return idx;
}

static void emit_load_var(int Rt, int var_idx)
{
    emit_str("    ldr  x"); emit_int(Rt);
    emit_str(", [sp, #"); emit_int(var_idx * 8);
    emit_str("]\n");
}

static void emit_store_var(int Rt, int var_idx)
{
    emit_str("    str  x"); emit_int(Rt);
    emit_str(", [sp, #"); emit_int(var_idx * 8);
    emit_str("]\n");
}

static void emit_push_x0(void)
{
    if (g_expr_depth >= CC_MAX_DEPTH) {
        cc_err("expression too deeply nested"); return;
    }
    emit_str("    str  x0, [sp, #");
    emit_int(128 + g_expr_depth * 8);
    emit_str("]\n");
    g_expr_depth++;
    if (g_expr_depth > g_max_expr_depth) g_max_expr_depth = g_expr_depth;
}

static void emit_pop_x1(void)
{
    if (g_expr_depth <= 0) {
        cc_err("internal: expr stack underflow"); return;
    }
    g_expr_depth--;
    emit_str("    ldr  x1, [sp, #");
    emit_int(128 + g_expr_depth * 8);
    emit_str("]\n");
}

/* Emit one .ascii line, properly escaping backslashes / quotes.
 * /bin/as supports `.ascii "..."` with the same escape rules
 * as gas (\n, \t, \", \\).  We re-escape what we lexed. */
static void emit_ascii_quoted(const char *s, int n)
{
    emit_str("    .ascii \"");
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') emit_str("\\\\");
        else if (c == '"') emit_str("\\\"");
        else if (c == '\n') emit_str("\\n");
        else if (c == '\t') emit_str("\\t");
        else if (c == '\r') emit_str("\\r");
        else if (c < 32 || c >= 127) {
            emit_str("\\");   /* /bin/as does not understand \\xHH,
                               * so we just drop weird bytes — the
                               * curated subset doesn't need them. */
        } else {
            char buf[2] = { (char)c, 0 };
            emit_str(buf);
        }
    }
    emit_str("\"\n");
}

/* Emit the `bl past_str / .ascii / .balign 4 / past_str:` pattern
 * followed by:
 *      mov x1, x30
 *      mov x2, #LEN
 *      mov x0, #FD
 *      mov x8, #1   ; SYS_WRITE
 *      svc #0
 */
static void emit_write_lit(int fd, const char *s, int n)
{
    int id = g_label_counter++;
    emit_str("    bl   .LSjmp"); emit_int(id); emit_str("\n");
    emit_str(".LSdata"); emit_int(id); emit_str(":\n");
    emit_ascii_quoted(s, n);
    emit_str("    .balign 4\n");
    emit_str(".LSjmp"); emit_int(id); emit_str(":\n");
    emit_str("    mov  x1, x30\n");
    emit_str("    mov  x2, #"); emit_int(n); emit_str("\n");
    emit_str("    mov  x0, #"); emit_int(fd); emit_str("\n");
    emit_str("    mov  x8, #1\n");      /* SYS_WRITE */
    emit_str("    svc  #0\n");
}

static void emit_exit(long code)
{
    emit_str("    mov  x0, #"); emit_int(code); emit_str("\n");
    emit_str("    mov  x8, #2\n");      /* SYS_EXIT */
    emit_str("    svc  #0\n");
}

/* Value-in-x0 variant — used when the exit code is the result of
 * a parsed expression. */
static void emit_exit_x0(void)
{
    emit_str("    mov  x8, #2\n");      /* SYS_EXIT */
    emit_str("    svc  #0\n");
}

/* ──────────────────────────────────────────────────────────
 * Expression parser + codegen.  Grammar (chapter 159):
 *
 *   expr    = primary ((`+` | `-`) primary)*
 *   primary = INT_LITERAL
 *           | IDENT
 *           | '(' expr ')'
 *
 * Result is always in x0.  Push/pop on the in-frame slot pile
 * (see g_expr_depth) for left-operand stashing.  No precedence
 * climbing needed yet — + and - have the same level.
 */
static void parse_expr(void);   /* forward */

static void parse_primary(void)
{
    if (g_tok_kind == TK_INT) {
        emit_str("    mov  x0, #"); emit_int(g_tok_int); emit_str("\n");
        lex();
        return;
    }
    if (g_tok_kind == TK_IDENT) {
        int idx = var_find(g_tok_str);
        if (idx < 0) {
            cc_err("undeclared identifier in expression");
        } else {
            emit_load_var(0, idx);
        }
        lex();
        return;
    }
    if (g_tok_kind == TK_LPAREN) {
        lex();
        parse_expr();
        expect(TK_RPAREN, "expected ')' in expression");
        return;
    }
    cc_err("expected expression");
    /* x0 left as garbage; subsequent compile-error abort. */
}

static void parse_expr(void)
{
    parse_primary();
    while (g_tok_kind == TK_PLUS || g_tok_kind == TK_MINUS) {
        int is_add = (g_tok_kind == TK_PLUS);
        lex();
        emit_push_x0();
        parse_primary();
        emit_pop_x1();
        /* x1 = left, x0 = right.  Result must go into x0. */
        if (is_add) emit_str("    add  x0, x1, x0\n");
        else        emit_str("    sub  x0, x1, x0\n");
    }
}

/* ──────────────────────────────────────────────────────────
 * Parser + codegen for the curated subset.
 */
static void parse_call_args_one_string(const char *what,
                                       char *out, int *out_n)
{
    expect(TK_LPAREN, "expected '('");
    if (g_tok_kind != TK_STRING) {
        cc_err("expected string literal");
        *out_n = 0; return;
    }
    int n = g_tok_str_n;
    if (n >= MAX_TOK_LEN) n = MAX_TOK_LEN - 1;
    for (int i = 0; i < n; i++) out[i] = g_tok_str[i];
    *out_n = n;
    lex();
    expect(TK_RPAREN, "expected ')'");
    (void)what;
}

static void parse_statement(void)
{
    /* int IDENT [= EXPR];   — declaration */
    if (g_tok_kind == TK_KW_INT) {
        lex();
        if (g_tok_kind != TK_IDENT) {
            cc_err("expected identifier after 'int'");
        } else {
            char name[MAX_TOK_LEN];
            cc_strcpy(name, g_tok_str);
            lex();
            int idx = var_declare(name);
            if (g_tok_kind == TK_EQ) {
                lex();
                parse_expr();
                if (idx >= 0) emit_store_var(0, idx);
            } else {
                /* Default-init to zero so reads-before-writes don't
                 * leak whatever was on the stack. */
                if (idx >= 0) {
                    emit_str("    mov  x0, #0\n");
                    emit_store_var(0, idx);
                }
            }
        }
        expect(TK_SEMI, "expected ';'");
        return;
    }
    if (g_tok_kind == TK_KW_RETURN) {
        lex();
        /* Accept either bare `;` (return 0) or an expression. */
        if (g_tok_kind == TK_SEMI) {
            emit_exit(0);
        } else {
            parse_expr();
            emit_exit_x0();
        }
        expect(TK_SEMI, "expected ';'");
        return;
    }
    if (g_tok_kind == TK_IDENT) {
        char name[MAX_TOK_LEN];
        cc_strcpy(name, g_tok_str);
        lex();

        char buf[MAX_TOK_LEN];
        int  len = 0;

        /* IDENT = EXPR;  — assignment to existing local */
        if (g_tok_kind == TK_EQ) {
            int idx = var_find(name);
            if (idx < 0) cc_err("assignment to undeclared identifier");
            lex();
            parse_expr();
            if (idx >= 0) emit_store_var(0, idx);
            expect(TK_SEMI, "expected ';'");
            return;
        }

        if (cc_streq(name, "printf")) {
            parse_call_args_one_string("printf", buf, &len);
            expect(TK_SEMI, "expected ';'");
            emit_write_lit(1, buf, len);
            return;
        }
        if (cc_streq(name, "puts")) {
            parse_call_args_one_string("puts", buf, &len);
            expect(TK_SEMI, "expected ';'");
            /* puts appends a newline */
            if (len < MAX_TOK_LEN - 1) buf[len++] = '\n';
            emit_write_lit(1, buf, len);
            return;
        }
        if (cc_streq(name, "exit")) {
            /* exit(EXPR);  — same effect as return EXPR; */
            expect(TK_LPAREN, "expected '('");
            parse_expr();
            expect(TK_RPAREN, "expected ')'");
            expect(TK_SEMI, "expected ';'");
            emit_exit_x0();
            return;
        }
        if (cc_streq(name, "write")) {
            /* write(FD, "STR", LEN); */
            expect(TK_LPAREN, "expected '('");
            long fd = 1;
            if (g_tok_kind == TK_INT) { fd = g_tok_int; lex(); }
            else cc_err("write fd must be integer literal");
            expect(TK_COMMA, "expected ','");
            if (g_tok_kind != TK_STRING) {
                cc_err("write buf must be string literal");
            } else {
                len = g_tok_str_n;
                if (len >= MAX_TOK_LEN) len = MAX_TOK_LEN - 1;
                for (int i = 0; i < len; i++) buf[i] = g_tok_str[i];
                lex();
            }
            expect(TK_COMMA, "expected ','");
            long n = len;
            if (g_tok_kind == TK_INT) { n = g_tok_int; lex(); }
            else cc_err("write len must be integer literal");
            expect(TK_RPAREN, "expected ')'");
            expect(TK_SEMI, "expected ';'");
            if (n > len) n = len;   /* clamp to actual buffer */
            emit_write_lit((int)fd, buf, (int)n);
            return;
        }
        cc_err("unknown function in statement");
        /* swallow until ';' to keep parsing */
        while (g_tok_kind != TK_SEMI && g_tok_kind != TK_EOF) lex();
        if (g_tok_kind == TK_SEMI) lex();
        return;
    }
    if (g_tok_kind == TK_SEMI) { lex(); return; }
    cc_err("expected statement");
    while (g_tok_kind != TK_SEMI && g_tok_kind != TK_EOF) lex();
    if (g_tok_kind == TK_SEMI) lex();
}

static void parse_main_signature(void)
{
    /* Already consumed `int main` and we're at `(` */
    expect(TK_LPAREN, "expected '('");
    if (g_tok_kind == TK_KW_VOID) { lex(); }
    else if (g_tok_kind == TK_KW_INT) {
        lex();
        /* int argc */
        if (g_tok_kind == TK_IDENT) lex();
        expect(TK_COMMA, "expected ',' after argc");
        /* char **argv  OR  char *argv[] (we accept char **) */
        expect(TK_KW_CHAR, "expected 'char'");
        expect(TK_STAR, "expected '*'");
        expect(TK_STAR, "expected '*'");
        if (g_tok_kind == TK_IDENT) lex();
    } /* else: () is accepted as ()-no-args */
    expect(TK_RPAREN, "expected ')'");
}

static void parse_program(void)
{
    /* Expect: int main(...) { stmts } */
    expect(TK_KW_INT, "program must start with 'int main'");
    if (g_tok_kind != TK_IDENT || !cc_streq(g_tok_str, "main")) {
        cc_err("only 'main' is supported"); return;
    }
    lex();
    parse_main_signature();
    expect(TK_LBRACE, "expected '{'");

    /* Emit prologue. */
    emit_str("/* generated by /bin/cc — chapter 159 */\n");
    emit_str(".text\n");
    emit_str(".global _user_start\n");
    emit_str("_user_start:\n");
    /* Reserve the fixed 256-byte frame: 16 locals + 16 expr-stack
     * slots.  See the layout comment near g_var_name. */
    emit_str("    sub  sp, sp, #256\n");

    while (g_tok_kind != TK_RBRACE && g_tok_kind != TK_EOF) {
        parse_statement();
        if (g_had_error) break;
    }
    expect(TK_RBRACE, "expected '}'");

    /* Implicit return 0 on fall-through. */
    emit_exit(0);
}

/* ──────────────────────────────────────────────────────────
 * Driver: orchestrate /bin/as and /bin/ld.
 */
static void path_for_tmp(const char *src, const char *suffix,
                         char *out, int max)
{
    /* "/tmp/" + basename(src) without extension + suffix */
    const char *base = src;
    for (const char *p = src; *p; p++) if (*p == '/') base = p + 1;
    int i = 0;
    const char *prefix = "/tmp/";
    while (*prefix && i < max - 1) out[i++] = *prefix++;
    while (*base && *base != '.' && i < max - 1) out[i++] = *base++;
    const char *q = suffix;
    while (*q && i < max - 1) out[i++] = *q++;
    out[i] = '\0';
}

static int run_tool(const char *path, const char *args)
{
    int pid = spawn(path, args);
    if (pid < 0) {
        printf("cc: spawn '%s' failed\n", path);
        return -1;
    }
    int code = 0;
    int r = waitpid(pid, &code, 0);
    if (r < 0) {
        printf("cc: waitpid on '%s' failed\n", path);
        return -1;
    }
    return code;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cc [-S|-c] SRC.c [-o OUT]\n");
        return 1;
    }

    const char *src_path = 0;
    const char *out_path = 0;
    int stop_after_emit = 0;
    int stop_after_as   = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (cc_streq(a, "-o") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (cc_streq(a, "-S")) {
            stop_after_emit = 1;
        } else if (cc_streq(a, "-c")) {
            stop_after_as = 1;
        } else if (a[0] == '-') {
            printf("cc: unknown option '%s'\n", a);
            return 1;
        } else {
            if (src_path) {
                printf("cc: only one source file accepted\n");
                return 1;
            }
            src_path = a;
        }
    }
    if (!src_path) {
        printf("cc: no input file\n");
        return 1;
    }
    if (!out_path) {
        out_path = "a.out";
    }

    /* 1) Read source. */
    g_src_n = read_file(src_path, g_src, sizeof(g_src) - 1);
    if (g_src_n < 0) return 1;
    g_src_line = 1;

    /* 2) Tokenise + parse + emit. */
    lex();
    parse_program();
    if (g_had_error) return 1;

    /* 3) Write asm to a temp file (or directly to -o if -S). */
    char asm_path[MAX_PATH];
    if (stop_after_emit) {
        cc_strcpy(asm_path, out_path);
    } else {
        path_for_tmp(src_path, ".cc.s", asm_path, sizeof(asm_path));
    }
    if (write_file(asm_path, g_asm, g_asm_n) != 0) return 1;
    printf("cc: emitted %s (%d bytes)\n", asm_path, g_asm_n);
    if (stop_after_emit) return 0;

    /* 4) Assemble. */
    char obj_path[MAX_PATH];
    if (stop_after_as) {
        cc_strcpy(obj_path, out_path);
    } else {
        path_for_tmp(src_path, ".cc.o", obj_path, sizeof(obj_path));
    }
    char as_args[512];
    int n = 0;
    const char *p1 = asm_path;
    while (*p1 && n < 510) as_args[n++] = *p1++;
    const char *p2 = " -o ";
    while (*p2 && n < 510) as_args[n++] = *p2++;
    const char *p3 = obj_path;
    while (*p3 && n < 510) as_args[n++] = *p3++;
    as_args[n] = '\0';
    int rc = run_tool("/bin/as", as_args);
    if (rc != 0) {
        printf("cc: /bin/as exited %d\n", rc);
        return 1;
    }
    if (stop_after_as) return 0;

    /* 5) Link.  Entry symbol is _user_start (our crt0 convention).
     *    Chapter 180 swapped the toy chapter-119 /bin/ld for the
     *    real GNU binutils ld.  GNU ld's default linker script
     *    places .text at 0x00400000, not the 0x1000100000 the
     *    kernel ELF loader expects, so we always pass `-T
     *    /bin/osdev.ld` — the in-guest copy of
     *    userspace/linker_user.ld (chapter 158 contract). */
    char ld_args[512];
    n = 0;
    const char *q1 = "-T /bin/osdev.ld -e _user_start -o ";
    while (*q1 && n < 510) ld_args[n++] = *q1++;
    const char *q2 = out_path;
    while (*q2 && n < 510) ld_args[n++] = *q2++;
    if (n < 510) ld_args[n++] = ' ';
    const char *q3 = obj_path;
    while (*q3 && n < 510) ld_args[n++] = *q3++;
    ld_args[n] = '\0';
    rc = run_tool("/bin/ld", ld_args);
    if (rc != 0) {
        printf("cc: /bin/ld exited %d\n", rc);
        return 1;
    }
    printf("cc: wrote %s\n", out_path);
    return 0;
}
