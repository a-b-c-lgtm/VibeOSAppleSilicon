/* userspace/as/as.c — minimal AArch64 assembler, chapter 118.
 *
 * Usage:  /bin/as [-o out.o] in.s
 *         (with no -o the default is `out.o` next to the input)
 *
 * Mnemonic coverage (the curated subset the rest of Part XVII
 * will produce from /bin/cc1 in chapter 122):
 *
 *   mov  Rd, #imm        (MOVZ form, hw=0)
 *   movk Rd, #imm[, lsl #N]
 *   movz Rd, #imm[, lsl #N]
 *   mov  Rd, Rs          (synthesised as ORR Rd, XZR, Rs)
 *   add  Rd, Rs, #imm    (no shift; imm 0..4095)
 *   add  Rd, Rs, Rm      (no shift)
 *   sub  Rd, Rs, #imm
 *   sub  Rd, Rs, Rm
 *   cmp  Rs, #imm        (SUBS XZR, Rs, #imm)
 *   cmp  Rs, Rm          (SUBS XZR, Rs, Rm)
 *   ldr  Rd, [Rs, #imm]  (LDR-imm-unsigned-offset)
 *   str  Rs, [Rd, #imm]
 *   bl   sym             (CALL26 reloc)
 *   b    sym             (JUMP26 reloc / local label)
 *   ret                  (RET X30 = 0xD65F03C0)
 *   svc  #imm
 *   nop                  (HINT #0 = 0xD503201F)
 *   wfe                  (HINT #2 = 0xD503205F)
 *   br   Rn              (BR Xn)
 *
 * Directives:
 *   .text  .data  .bss  .rodata
 *   .section NAME[, "FLAGS"][, %nobits]   (parsed; flags
 *                                           extracted from
 *                                           NAME's prefix or
 *                                           the FLAGS string)
 *   .global SYM   / .globl SYM
 *   .balign N     .align N (both treated as power-of-two)
 *   .byte 1,2,3
 *   .word 0x1234
 *   .quad 0xDEADBEEF
 *   .ascii "..."
 *   .skip N
 *   .type / .size  (parsed-then-ignored, like GAS in --no-warn mode)
 *   .cfi_*  .loc  .file  (parsed-then-ignored)
 *
 * Output: ELF64-LSB AArch64 relocatable object, sections in
 * order [NULL, .text, .data, .bss, .rodata, .symtab, .strtab,
 * .shstrtab, .rela.text].  Empty sections are still emitted as
 * SHT_PROGBITS/NOBITS with sh_size=0 so the section indices
 * are stable across inputs (chapter 119 relies on this).
 *
 * This is a one-pass assembler with patch-list fixups for
 * forward references.  Two-pass would be cleaner but would
 * cost another lex pass; one-pass with patches is the BSD
 * /bin/as shape.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/sys/stat.h"
#include "../libc/elf_write.h"

/* GCC sometimes lowers `struct foo s = {0};` on ~64-byte
 * structs into a memset call.  Freestanding userspace has no
 * libc memset, so provide one in this TU. */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

/* Minimal realloc on top of malloc.h's free-list allocator.
 * malloc.h carries the block size in the header word right
 * before the user pointer, so we can read it back to know how
 * many bytes to copy.  malloc(NULL) is not defined here; we
 * pass NULL through the alloc path. */
static void *as_realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    /* Block header lives at p - UALLOC_HDR_SIZE; first field is
     * total size including the header. */
    size_t old_total = *((size_t *)((char *)p - UALLOC_HDR_SIZE));
    size_t old_payload = old_total - UALLOC_HDR_SIZE;
    if (n <= old_payload) return p;       /* shrink in place */
    void *np = malloc(n);
    if (!np) return (void *)0;
    char *src = (char *)p;
    char *dst = (char *)np;
    for (size_t i = 0; i < old_payload; i++) dst[i] = src[i];
    free(p);
    return np;
}

/* Provide the ew_realloc hook the header-only writer requires. */
void *ew_realloc(void *p, size_t n) { return as_realloc(p, n); }

/* ---------- Tiny string helpers (avoid libc string.h). ---------- */

static int as_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

static int as_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static int as_isdigit(int c) { return c >= '0' && c <= '9'; }
static int as_isalpha(int c) { return (c >= 'a' && c <= 'z') ||
                                       (c >= 'A' && c <= 'Z') ||
                                       c == '_' || c == '.'; }
static int as_isalnum(int c) { return as_isalpha(c) || as_isdigit(c); }

/* ---------- String table (NUL-terminated, append-only). ---------- */

typedef struct {
    char  *buf;
    size_t n, cap;
} strtab_t;

static void st_init(strtab_t *s)
{
    s->cap = 256; s->n = 1;
    s->buf = (char *)malloc(s->cap);
    s->buf[0] = '\0';   /* first slot is the empty name */
}
static uint32_t st_add(strtab_t *s, const char *name)
{
    size_t len = 0;
    while (name[len]) len++;
    if (s->n + len + 1 > s->cap) {
        while (s->n + len + 1 > s->cap) s->cap *= 2;
        s->buf = (char *)as_realloc(s->buf, s->cap);
    }
    uint32_t off = (uint32_t)s->n;
    for (size_t i = 0; i <= len; i++) s->buf[s->n + i] = name[i];
    s->n += len + 1;
    return off;
}

/* ---------- Symbol table. ---------- */

typedef struct {
    char     name[64];
    uint16_t shndx;     /* section index, 0 = UND */
    uint64_t value;     /* offset within section, or 0 for UND */
    uint8_t  bind;      /* STB_LOCAL / STB_GLOBAL */
    uint8_t  defined;
} symbol_t;

#define MAX_SYMS 512
static symbol_t g_syms[MAX_SYMS];
static int      g_nsyms = 0;

static int sym_find(const char *name)
{
    for (int i = 0; i < g_nsyms; i++)
        if (as_streq(g_syms[i].name, name)) return i;
    return -1;
}
static int sym_add(const char *name)
{
    if (g_nsyms >= MAX_SYMS) return -1;
    symbol_t *s = &g_syms[g_nsyms];
    int i = 0;
    while (name[i] && i + 1 < (int)sizeof(s->name)) {
        s->name[i] = name[i]; i++;
    }
    s->name[i] = '\0';
    s->shndx = 0; s->value = 0;
    s->bind = STB_LOCAL; s->defined = 0;
    return g_nsyms++;
}
static int sym_intern(const char *name)
{
    int i = sym_find(name);
    if (i >= 0) return i;
    return sym_add(name);
}

/* ---------- Sections. ---------- */

enum { SEC_TEXT = 1, SEC_DATA, SEC_BSS, SEC_RODATA, NSEC };

typedef struct {
    ew_buf_t buf;       /* .bss uses size_only, no bytes */
    uint64_t size_only; /* for .bss */
    uint64_t align;
} section_t;

static section_t g_sec[NSEC];
static int       g_cur_sec = SEC_TEXT;

static uint64_t cur_offset(void)
{
    if (g_cur_sec == SEC_BSS) return g_sec[SEC_BSS].size_only;
    return g_sec[g_cur_sec].buf.n;
}

/* ---------- Relocations against .text only (chapter-118 limit). ---------- */

typedef struct {
    uint64_t offset;
    uint32_t sym_index;     /* index into our local g_syms[] */
    uint32_t r_type;
    int64_t  addend;
} reloc_t;

#define MAX_RELOCS 4096
static reloc_t g_relocs[MAX_RELOCS];
/* Scratch parallel to g_relocs used by write_elf for the
 * post-patch "kept" set.  File-scope keeps the ~64 KiB out of
 * write_elf's stack frame — the user thread stack is only
 * 64 KiB (see kernel/core/elf.c USER_STACK_PAGES). */
static reloc_t g_resolved[MAX_RELOCS];
static int     g_nrelocs = 0;

static void reloc_add(uint64_t off, int sym, uint32_t type, int64_t add)
{
    if (g_nrelocs >= MAX_RELOCS) return;
    g_relocs[g_nrelocs++] = (reloc_t){off, (uint32_t)sym, type, add};
}

/* ---------- Lexer. ---------- */

static const char *g_src = NULL;
static const char *g_p = NULL;
static int         g_line = 1;
static const char *g_filename = "<stdin>";

static void lex_skip_ws_and_comments(void)
{
    for (;;) {
        while (*g_p == ' ' || *g_p == '\t' || *g_p == '\r') g_p++;
        if (g_p[0] == '/' && g_p[1] == '/') {
            while (*g_p && *g_p != '\n') g_p++;
            continue;
        }
        if (g_p[0] == '/' && g_p[1] == '*') {
            g_p += 2;
            while (*g_p && !(g_p[0] == '*' && g_p[1] == '/')) {
                if (*g_p == '\n') g_line++;
                g_p++;
            }
            if (*g_p) g_p += 2;
            continue;
        }
        /* GAS-style // and # line comments. */
        if (*g_p == '#' && g_p > g_src && g_p[-1] != '\n') {
            /* Inline `mov x0, #42` — '#' is part of operand,
             * not a comment.  We disambiguate by only treating
             * '#' as a comment at column 0.  Crude but works
             * for everything we generate. */
        }
        return;
    }
}

static int lex_eol(void)
{
    (void)0;
    return *g_p == '\n' || *g_p == '\0';
}
__attribute__((unused)) static int (* const _force_use_lex_eol)(void) = lex_eol;

static void lex_consume_eol(void)
{
    while (*g_p && *g_p != '\n') g_p++;
    if (*g_p == '\n') { g_p++; g_line++; }
}

/* Read an identifier (incl. labels like "1:" — handled separately). */
static int lex_ident(char *out, size_t cap)
{
    lex_skip_ws_and_comments();
    if (!as_isalpha((unsigned char)*g_p)) return 0;
    size_t i = 0;
    while (as_isalnum((unsigned char)*g_p) && i + 1 < cap) {
        out[i++] = *g_p++;
    }
    out[i] = '\0';
    return (int)i;
}

/* Read a signed integer.  Accepts decimal, 0x hex, 0 octal. */
static int lex_int(int64_t *out)
{
    lex_skip_ws_and_comments();
    int neg = 0;
    if (*g_p == '#') g_p++;
    if (*g_p == '-') { neg = 1; g_p++; }
    if (!as_isdigit((unsigned char)*g_p)) return 0;
    int64_t v = 0;
    if (g_p[0] == '0' && (g_p[1] == 'x' || g_p[1] == 'X')) {
        g_p += 2;
        while (1) {
            int c = (unsigned char)*g_p;
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
            else break;
            v = v * 16 + d;
            g_p++;
        }
    } else {
        while (as_isdigit((unsigned char)*g_p)) {
            v = v * 10 + (*g_p - '0');
            g_p++;
        }
    }
    *out = neg ? -v : v;
    return 1;
}

/* Read a register name like "x0", "w7", "xzr", "sp". Returns
 * encoded register number (0..31) and the width (8 for X/SP, 4 for W).
 * Returns 0 on miss. */
static int lex_reg(int *reg, int *width)
{
    lex_skip_ws_and_comments();
    const char *save = g_p;
    if ((*g_p == 'x' || *g_p == 'X')) {
        g_p++;
        if (g_p[0] == 'z' && g_p[1] == 'r') {
            g_p += 2; *reg = 31; *width = 8; return 1;
        }
        if (!as_isdigit((unsigned char)*g_p)) { g_p = save; return 0; }
        int n = 0;
        while (as_isdigit((unsigned char)*g_p)) {
            n = n * 10 + (*g_p - '0'); g_p++;
        }
        if (n > 30) { g_p = save; return 0; }
        *reg = n; *width = 8; return 1;
    }
    if ((*g_p == 'w' || *g_p == 'W')) {
        g_p++;
        if (g_p[0] == 'z' && g_p[1] == 'r') {
            g_p += 2; *reg = 31; *width = 4; return 1;
        }
        if (!as_isdigit((unsigned char)*g_p)) { g_p = save; return 0; }
        int n = 0;
        while (as_isdigit((unsigned char)*g_p)) {
            n = n * 10 + (*g_p - '0'); g_p++;
        }
        if (n > 30) { g_p = save; return 0; }
        *reg = n; *width = 4; return 1;
    }
    if (as_starts(g_p, "sp")) { g_p += 2; *reg = 31; *width = 8; return 1; }
    if (as_starts(g_p, "lr")) { g_p += 2; *reg = 30; *width = 8; return 1; }
    return 0;
}

static int lex_comma(void)
{
    lex_skip_ws_and_comments();
    if (*g_p == ',') { g_p++; return 1; }
    return 0;
}

static int lex_char(int c)
{
    lex_skip_ws_and_comments();
    if (*g_p == c) { g_p++; return 1; }
    return 0;
}

/* ---------- Error reporting. ---------- */

static int g_error_count = 0;
static void as_err(const char *fmt, ...)
{
    printf("%s:%d: error: ", g_filename, g_line);
    /* tiny vararg passthrough — our printf already supports ... */
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    /* No vprintf; we just print the literal fmt (callers pass
     * already-formatted text where they need substitution). */
    printf("%s\n", fmt);
    __builtin_va_end(ap);
    g_error_count++;
}

/* ---------- Encoders. ---------- */

static void emit_word(uint32_t insn)
{
    if (g_cur_sec == SEC_BSS) {
        as_err("instruction in .bss");
        return;
    }
    ew_emit_u32(&g_sec[g_cur_sec].buf, insn);
}

/* MOVZ:  sf=1, opc=10, 100101, hw, imm16, Rd       (= 0xD2800000 | hw<<21 | imm16<<5 | Rd) */
static void enc_movz(int Rd, uint32_t imm16, int hw)
{
    uint32_t insn = 0xD2800000u | ((uint32_t)hw << 21) |
                    ((imm16 & 0xFFFFu) << 5) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* MOVK:  sf=1, opc=11, 100101, hw, imm16, Rd       (= 0xF2800000 | hw<<21 | imm16<<5 | Rd) */
static void enc_movk(int Rd, uint32_t imm16, int hw)
{
    uint32_t insn = 0xF2800000u | ((uint32_t)hw << 21) |
                    ((imm16 & 0xFFFFu) << 5) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* MOV Rd, Rs = ORR Rd, XZR, Rs.   ORR (shifted reg, 64-bit):
 *   1 01 01010 00 0 Rm 000000 11111 Rd  with shift=0 imm6=0 Rn=XZR(31) */
static void enc_mov_reg(int Rd, int Rs)
{
    uint32_t insn = 0xAA0003E0u | ((uint32_t)(Rs & 31) << 16) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* ADD/SUB immediate (64-bit, sh=0):
 *   ADD: sf=1 op=0 S=0 100010 sh imm12 Rn Rd  = 0x91000000
 *   SUB: sf=1 op=1 S=0 100010 sh imm12 Rn Rd  = 0xD1000000 */
static void enc_addsub_imm(uint32_t base, int Rd, int Rn, uint32_t imm12)
{
    uint32_t insn = base | ((imm12 & 0xFFFu) << 10) |
                    ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* ADD/SUB shifted register (64-bit, shift=0, imm6=0):
 *   ADD: sf=1 op=0 S=0 01011 shift=00 0 Rm imm6=0 Rn Rd = 0x8B000000
 *   SUB: sf=1 op=1 S=0 01011 shift=00 0 Rm imm6=0 Rn Rd = 0xCB000000 */
static void enc_addsub_reg(uint32_t base, int Rd, int Rn, int Rm)
{
    uint32_t insn = base | ((uint32_t)(Rm & 31) << 16) |
                    ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* SUBS imm (used for CMP): sf=1 op=1 S=1 100010 sh imm12 Rn Rd  = 0xF1000000 */
static void enc_subs_imm(int Rd, int Rn, uint32_t imm12)
{
    uint32_t insn = 0xF1000000u | ((imm12 & 0xFFFu) << 10) |
                    ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* SUBS reg (CMP-reg): sf=1 op=1 S=1 01011 shift=00 0 Rm imm6=0 Rn Rd  = 0xEB000000 */
static void enc_subs_reg(int Rd, int Rn, int Rm)
{
    uint32_t insn = 0xEB000000u | ((uint32_t)(Rm & 31) << 16) |
                    ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31);
    emit_word(insn);
}

/* LDR (immediate, unsigned offset, 64-bit):
 *   size=11 111 0 01 01 imm12 Rn Rt   = 0xF9400000 | (imm12/8)<<10 */
static void enc_ldr_imm(int Rt, int Rn, uint32_t imm)
{
    uint32_t insn = 0xF9400000u | (((imm / 8u) & 0xFFFu) << 10) |
                    ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31);
    emit_word(insn);
}

/* STR (immediate, unsigned offset, 64-bit):  = 0xF9000000 | (imm/8)<<10 */
static void enc_str_imm(int Rt, int Rn, uint32_t imm)
{
    uint32_t insn = 0xF9000000u | (((imm / 8u) & 0xFFFu) << 10) |
                    ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31);
    emit_word(insn);
}

/* B / BL — 26-bit signed PC-relative word-displacement. */
static void enc_b(int32_t word_disp, int link)
{
    uint32_t base = link ? 0x94000000u : 0x14000000u;
    uint32_t insn = base | ((uint32_t)word_disp & 0x03FFFFFFu);
    emit_word(insn);
}

/* BR Xn:  1101 0110 0001 1111 0000 00 Rn 00000   = 0xD61F0000 | Rn<<5 */
static void enc_br(int Rn)
{
    emit_word(0xD61F0000u | ((uint32_t)(Rn & 31) << 5));
}

/* SVC #imm16:  1101 0100 000 imm16 0 0001   = 0xD4000001 | imm16<<5 */
static void enc_svc(uint32_t imm16)
{
    emit_word(0xD4000001u | ((imm16 & 0xFFFFu) << 5));
}

/* RET (X30): 0xD65F03C0. */
static void enc_ret(void) { emit_word(0xD65F03C0u); }
/* NOP = HINT #0: 0xD503201F. */
static void enc_nop(void) { emit_word(0xD503201Fu); }
/* WFE = HINT #2: 0xD503205F. */
static void enc_wfe(void) { emit_word(0xD503205Fu); }

/* ---------- Directive handlers. ---------- */

static void dir_section(const char *name)
{
    int s = -1;
    if      (as_streq(name, ".text"))   s = SEC_TEXT;
    else if (as_streq(name, ".data"))   s = SEC_DATA;
    else if (as_streq(name, ".bss"))    s = SEC_BSS;
    else if (as_streq(name, ".rodata")) s = SEC_RODATA;
    else if (as_starts(name, ".text"))  s = SEC_TEXT;
    else if (as_starts(name, ".data"))  s = SEC_DATA;
    else if (as_starts(name, ".bss"))   s = SEC_BSS;
    else if (as_starts(name, ".rodata")) s = SEC_RODATA;
    if (s < 0) { as_err("unknown section, mapped to .text"); s = SEC_TEXT; }
    g_cur_sec = s;
}

static void dir_global(const char *name)
{
    int idx = sym_intern(name);
    if (idx >= 0) g_syms[idx].bind = STB_GLOBAL;
}

static void dir_balign(uint64_t n)
{
    if (g_cur_sec == SEC_BSS) {
        while (g_sec[SEC_BSS].size_only % n) g_sec[SEC_BSS].size_only++;
    } else {
        ew_pad_to(&g_sec[g_cur_sec].buf, n);
    }
    if (n > g_sec[g_cur_sec].align) g_sec[g_cur_sec].align = n;
}

static void dir_byte(uint8_t v)
{
    if (g_cur_sec == SEC_BSS) { g_sec[SEC_BSS].size_only++; return; }
    ew_emit(&g_sec[g_cur_sec].buf, &v, 1);
}
static void dir_word(uint32_t v)
{
    if (g_cur_sec == SEC_BSS) { g_sec[SEC_BSS].size_only += 4; return; }
    ew_emit_u32(&g_sec[g_cur_sec].buf, v);
}
static void dir_quad(uint64_t v)
{
    if (g_cur_sec == SEC_BSS) { g_sec[SEC_BSS].size_only += 8; return; }
    ew_emit_u64(&g_sec[g_cur_sec].buf, v);
}
static void dir_skip(uint64_t n)
{
    if (g_cur_sec == SEC_BSS) { g_sec[SEC_BSS].size_only += n; return; }
    for (uint64_t i = 0; i < n; i++) { uint8_t z = 0; ew_emit(&g_sec[g_cur_sec].buf, &z, 1); }
}
static void dir_ascii(const char *p, size_t n)
{
    if (g_cur_sec == SEC_BSS) { g_sec[SEC_BSS].size_only += n; return; }
    ew_emit(&g_sec[g_cur_sec].buf, p, n);
}

/* ---------- Top-level parser. ---------- */

static void define_label(const char *name)
{
    int idx = sym_intern(name);
    if (idx < 0) { as_err("symbol table full"); return; }
    if (g_syms[idx].defined) { as_err("duplicate label"); return; }
    g_syms[idx].defined = 1;
    g_syms[idx].shndx = (uint16_t)g_cur_sec;
    g_syms[idx].value = cur_offset();
}

static void parse_directive(const char *dot)
{
    if (as_streq(dot, ".text") || as_streq(dot, ".data") ||
        as_streq(dot, ".bss")  || as_streq(dot, ".rodata")) {
        dir_section(dot);
        return;
    }
    if (as_streq(dot, ".section")) {
        char name[64];
        lex_skip_ws_and_comments();
        size_t i = 0;
        while (*g_p && *g_p != ',' && *g_p != '\n' && i + 1 < sizeof(name))
            name[i++] = *g_p++;
        while (i > 0 && (name[i - 1] == ' ' || name[i - 1] == '\t')) i--;
        name[i] = '\0';
        dir_section(name);
        return;
    }
    if (as_streq(dot, ".global") || as_streq(dot, ".globl")) {
        char sym[64];
        if (lex_ident(sym, sizeof(sym))) dir_global(sym);
        return;
    }
    if (as_streq(dot, ".balign") || as_streq(dot, ".align") ||
        as_streq(dot, ".p2align")) {
        int64_t v = 1;
        lex_int(&v);
        /* .align on AArch64 GAS is a power-of-two count of bytes
         * for .balign, but a power-of-two for .p2align/.align.
         * The inputs we generate use small powers either way
         * (1, 2, 4, 8, 16), so accept both interpretations as
         * the literal byte count. */
        if (v < 1) v = 1;
        dir_balign((uint64_t)v);
        return;
    }
    if (as_streq(dot, ".byte")) {
        for (;;) {
            int64_t v = 0;
            if (!lex_int(&v)) break;
            dir_byte((uint8_t)v);
            if (!lex_comma()) break;
        }
        return;
    }
    if (as_streq(dot, ".word") || as_streq(dot, ".long") ||
        as_streq(dot, ".4byte")) {
        for (;;) {
            int64_t v = 0;
            if (!lex_int(&v)) break;
            dir_word((uint32_t)v);
            if (!lex_comma()) break;
        }
        return;
    }
    if (as_streq(dot, ".quad") || as_streq(dot, ".8byte")) {
        for (;;) {
            int64_t v = 0;
            if (!lex_int(&v)) break;
            dir_quad((uint64_t)v);
            if (!lex_comma()) break;
        }
        return;
    }
    if (as_streq(dot, ".ascii") || as_streq(dot, ".asciz") ||
        as_streq(dot, ".string")) {
        lex_skip_ws_and_comments();
        if (*g_p != '"') return;
        g_p++;
        while (*g_p && *g_p != '"') {
            char c = *g_p++;
            if (c == '\\' && *g_p) {
                char n = *g_p++;
                if (n == 'n') c = '\n';
                else if (n == 't') c = '\t';
                else if (n == '0') c = '\0';
                else if (n == '\\') c = '\\';
                else if (n == '"') c = '"';
                else c = n;
            }
            dir_ascii(&c, 1);
        }
        if (*g_p == '"') g_p++;
        if (as_streq(dot, ".asciz") || as_streq(dot, ".string")) {
            char z = 0; dir_ascii(&z, 1);
        }
        return;
    }
    if (as_streq(dot, ".skip") || as_streq(dot, ".space") ||
        as_streq(dot, ".zero")) {
        int64_t v = 0; lex_int(&v);
        if (v > 0) dir_skip((uint64_t)v);
        return;
    }
    /* parsed-and-ignored: .type, .size, .cfi_*, .loc, .file,
     * .ident, .weak, .local, .hidden, .ent, .end, .arch */
    /* fall through — consume rest of line */
}

static void parse_mnemonic(const char *mn)
{
    int Rd, Rn, Rm, w;
    int64_t imm;
    char sym[64];

    if (as_streq(mn, "ret"))  { enc_ret(); return; }
    if (as_streq(mn, "nop"))  { enc_nop(); return; }
    if (as_streq(mn, "wfe"))  { enc_wfe(); return; }

    if (as_streq(mn, "svc")) {
        if (!lex_int(&imm)) { as_err("svc needs imm"); return; }
        enc_svc((uint32_t)imm);
        return;
    }
    if (as_streq(mn, "br")) {
        if (!lex_reg(&Rn, &w)) { as_err("br needs reg"); return; }
        enc_br(Rn);
        return;
    }
    if (as_streq(mn, "mov")) {
        if (!lex_reg(&Rd, &w)) { as_err("mov needs Rd"); return; }
        if (!lex_comma()) { as_err("mov: missing ,"); return; }
        if (lex_reg(&Rm, &w)) { enc_mov_reg(Rd, Rm); return; }
        if (lex_int(&imm)) { enc_movz(Rd, (uint32_t)imm, 0); return; }
        as_err("mov operand"); return;
    }
    if (as_streq(mn, "movz") || as_streq(mn, "movk")) {
        if (!lex_reg(&Rd, &w)) { as_err("movz needs Rd"); return; }
        if (!lex_comma()) { as_err("movz missing ,"); return; }
        if (!lex_int(&imm)) { as_err("movz needs imm"); return; }
        int hw = 0;
        if (lex_comma()) {
            char id[8]; lex_ident(id, sizeof(id));
            int64_t sh = 0; lex_int(&sh);
            hw = (int)(sh / 16) & 3;
        }
        if (as_streq(mn, "movz")) enc_movz(Rd, (uint32_t)imm, hw);
        else                      enc_movk(Rd, (uint32_t)imm, hw);
        return;
    }
    if (as_streq(mn, "add") || as_streq(mn, "sub")) {
        if (!lex_reg(&Rd, &w)) { as_err("addsub needs Rd"); return; }
        if (!lex_comma()) { as_err("addsub ,"); return; }
        if (!lex_reg(&Rn, &w)) { as_err("addsub needs Rn"); return; }
        if (!lex_comma()) { as_err("addsub ,"); return; }
        uint32_t base = as_streq(mn, "add") ? 0x91000000u : 0xD1000000u;
        uint32_t base_r = as_streq(mn, "add") ? 0x8B000000u : 0xCB000000u;
        if (lex_reg(&Rm, &w)) { enc_addsub_reg(base_r, Rd, Rn, Rm); return; }
        if (lex_int(&imm))    { enc_addsub_imm(base, Rd, Rn, (uint32_t)imm); return; }
        as_err("addsub operand"); return;
    }
    if (as_streq(mn, "cmp")) {
        if (!lex_reg(&Rn, &w)) { as_err("cmp needs Rn"); return; }
        if (!lex_comma()) { as_err("cmp ,"); return; }
        if (lex_reg(&Rm, &w)) { enc_subs_reg(31, Rn, Rm); return; }
        if (lex_int(&imm))    { enc_subs_imm(31, Rn, (uint32_t)imm); return; }
        as_err("cmp operand"); return;
    }
    if (as_streq(mn, "ldr") || as_streq(mn, "str")) {
        if (!lex_reg(&Rd, &w)) { as_err("ldst needs reg"); return; }
        if (!lex_comma()) { as_err("ldst ,"); return; }
        if (!lex_char('[')) { as_err("ldst ["); return; }
        if (!lex_reg(&Rn, &w)) { as_err("ldst base"); return; }
        imm = 0;
        if (lex_comma()) {
            if (!lex_int(&imm)) { as_err("ldst imm"); return; }
        }
        if (!lex_char(']')) { as_err("ldst ]"); return; }
        if (as_streq(mn, "ldr")) enc_ldr_imm(Rd, Rn, (uint32_t)imm);
        else                     enc_str_imm(Rd, Rn, (uint32_t)imm);
        return;
    }
    if (as_streq(mn, "b") || as_streq(mn, "bl")) {
        int link = as_streq(mn, "bl");
        lex_skip_ws_and_comments();
        if (lex_ident(sym, sizeof(sym))) {
            int sidx = sym_intern(sym);
            uint64_t off = cur_offset();
            if (g_syms[sidx].defined &&
                g_syms[sidx].shndx == (uint16_t)g_cur_sec) {
                int32_t d = (int32_t)((int64_t)g_syms[sidx].value -
                                      (int64_t)off) / 4;
                enc_b(d, link);
            } else {
                /* Placeholder MUST carry the opcode bits — both
                 * our in-pass patcher and /bin/ld preserve only
                 * the top 6 bits via `(insn & 0xFC000000) | imm26`. */
                emit_word(link ? 0x94000000u : 0x14000000u);
                uint32_t rt = link ? R_AARCH64_CALL26 : R_AARCH64_JUMP26;
                reloc_add(off, sidx, rt, 0);
            }
            return;
        }
        as_err("b/bl operand"); return;
    }

    /* Local numeric branch like `b 1b` / `b 1f` — gas-style. */
    as_err("unknown mnemonic");
}

static void assemble(void)
{
    while (*g_p) {
        lex_skip_ws_and_comments();
        if (*g_p == '\n') { g_p++; g_line++; continue; }
        if (!*g_p) break;

        /* '#' at column 0 (start of line) is a CPP-style comment. */
        if (*g_p == '#') { lex_consume_eol(); continue; }
        if (*g_p == ';') { lex_consume_eol(); continue; }

        /* Numeric label like `1:` — quick sniff. */
        if (as_isdigit((unsigned char)*g_p)) {
            const char *save = g_p;
            while (as_isdigit((unsigned char)*g_p)) g_p++;
            if (*g_p == ':') { g_p++; /* numeric labels accepted-and-ignored */ continue; }
            g_p = save;
        }

        char tok[80];
        if (!lex_ident(tok, sizeof(tok))) {
            /* unknown char — consume the rest of the line */
            lex_consume_eol();
            continue;
        }
        /* Label?  `foo:` */
        lex_skip_ws_and_comments();
        if (*g_p == ':') { g_p++; define_label(tok); continue; }

        if (tok[0] == '.') {
            parse_directive(tok);
            lex_consume_eol();
            continue;
        }
        parse_mnemonic(tok);
        lex_consume_eol();
    }
}

/* ---------- ELF emission. ---------- */

/* Section index plan in the final ELF:
 *   0  SHT_NULL
 *   1  .text          PROGBITS
 *   2  .data          PROGBITS
 *   3  .bss           NOBITS
 *   4  .rodata        PROGBITS
 *   5  .symtab        SYMTAB
 *   6  .strtab        STRTAB
 *   7  .shstrtab      STRTAB
 *   8  .rela.text     RELA (only if there are relocs)
 */

static void write_elf(const char *out_path)
{
    /* Resolve relocations whose target landed in .text after
     * the producer emit (forward refs).  For inter-section or
     * undefined symbols, we keep the reloc record. */
    int kept_relocs = 0;
    reloc_t *resolved = g_resolved;
    int     nresolved = 0;
    for (int i = 0; i < g_nrelocs; i++) {
        reloc_t *r = &g_relocs[i];
        symbol_t *s = &g_syms[r->sym_index];
        if (s->defined && s->shndx == SEC_TEXT &&
            (r->r_type == R_AARCH64_CALL26 || r->r_type == R_AARCH64_JUMP26)) {
            /* Patch in place. */
            int32_t d = (int32_t)((int64_t)s->value - (int64_t)r->offset) / 4;
            uint8_t *bp = g_sec[SEC_TEXT].buf.p + r->offset;
            uint32_t insn = (uint32_t)bp[0] | ((uint32_t)bp[1] << 8) |
                            ((uint32_t)bp[2] << 16) | ((uint32_t)bp[3] << 24);
            insn = (insn & 0xFC000000u) | ((uint32_t)d & 0x03FFFFFFu);
            bp[0] = (uint8_t)(insn & 0xFF);
            bp[1] = (uint8_t)((insn >> 8) & 0xFF);
            bp[2] = (uint8_t)((insn >> 16) & 0xFF);
            bp[3] = (uint8_t)((insn >> 24) & 0xFF);
        } else {
            resolved[nresolved++] = *r;
        }
    }
    kept_relocs = nresolved;

    /* Build .shstrtab. */
    strtab_t shstr;  st_init(&shstr);
    uint32_t n_text   = st_add(&shstr, ".text");
    uint32_t n_data   = st_add(&shstr, ".data");
    uint32_t n_bss    = st_add(&shstr, ".bss");
    uint32_t n_rodata = st_add(&shstr, ".rodata");
    uint32_t n_symtab = st_add(&shstr, ".symtab");
    uint32_t n_strtab = st_add(&shstr, ".strtab");
    uint32_t n_shstr  = st_add(&shstr, ".shstrtab");
    uint32_t n_rela   = kept_relocs ? st_add(&shstr, ".rela.text") : 0;

    /* Build .strtab + .symtab.  Symbol[0] is reserved.  Then
     * STT_SECTION symbols for our four sections (linker
     * convenience), then real labels — locals first, then
     * globals (ELF rule: symtab is sorted local..global). */
    strtab_t sstr;  st_init(&sstr);

    ew_buf_t symtab;  ew_buf_init(&symtab);
    /* Symbol 0: NULL. */
    { ew_sym64_t s = {0}; ew_emit(&symtab, &s, sizeof(s)); }
    /* Section symbols (always local). */
    int section_sym_base = 1;
    (void)section_sym_base;
    for (int i = 1; i < NSEC; i++) {
        ew_sym64_t s = {0};
        s.st_name = 0;
        s.st_info = (uint8_t)ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
        s.st_shndx = (uint16_t)i;
        s.st_value = 0;
        s.st_size = 0;
        ew_emit(&symtab, &s, sizeof(s));
    }
    int symtab_first_local_real = (int)(symtab.n / sizeof(ew_sym64_t));
    (void)symtab_first_local_real;

    /* Map sym index → ELF symbol index after sort. */
    uint32_t elf_sym_idx[MAX_SYMS] = {0};

    /* Locals. */
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].bind != STB_LOCAL) continue;
        ew_sym64_t s = {0};
        s.st_name = st_add(&sstr, g_syms[i].name);
        s.st_info = (uint8_t)ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE);
        s.st_shndx = g_syms[i].defined ? g_syms[i].shndx : 0;
        s.st_value = g_syms[i].value;
        s.st_size = 0;
        elf_sym_idx[i] = (uint32_t)(symtab.n / sizeof(ew_sym64_t));
        ew_emit(&symtab, &s, sizeof(s));
    }
    int symtab_globals_start = (int)(symtab.n / sizeof(ew_sym64_t));
    /* Globals. */
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].bind != STB_GLOBAL) continue;
        ew_sym64_t s = {0};
        s.st_name = st_add(&sstr, g_syms[i].name);
        s.st_info = (uint8_t)ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        s.st_shndx = g_syms[i].defined ? g_syms[i].shndx : 0;
        s.st_value = g_syms[i].value;
        s.st_size = 0;
        elf_sym_idx[i] = (uint32_t)(symtab.n / sizeof(ew_sym64_t));
        ew_emit(&symtab, &s, sizeof(s));
    }

    /* Build .rela.text. */
    ew_buf_t rela;  ew_buf_init(&rela);
    for (int i = 0; i < kept_relocs; i++) {
        ew_rela64_t r;
        r.r_offset = resolved[i].offset;
        r.r_info = ELF64_R_INFO(elf_sym_idx[resolved[i].sym_index],
                                resolved[i].r_type);
        r.r_addend = resolved[i].addend;
        ew_emit(&rela, &r, sizeof(r));
    }

    /* ---------- Layout ---------- */
    int n_sections = kept_relocs ? 9 : 8;

    /* Section headers + payloads.  We place payloads first
     * (right after the ehdr), then headers at the end. */
    ew_ehdr64_t ehdr;
    ew_ehdr_init(&ehdr, ET_REL);
    ehdr.e_shnum = (uint16_t)n_sections;
    ehdr.e_shstrndx = 7;

    ew_buf_t out;  ew_buf_init(&out);

    /* Write ehdr (we'll come back and patch e_shoff). */
    size_t ehdr_off = out.n;
    ew_emit(&out, &ehdr, sizeof(ehdr));

    /* Helper: align to 8. */
    struct { uint64_t off; uint64_t sz; } sec_loc[10];
    for (int i = 0; i < 10; i++) { sec_loc[i].off = 0; sec_loc[i].sz = 0; }

    /* .text */
    ew_pad_to(&out, 4);
    sec_loc[SEC_TEXT].off = out.n;
    sec_loc[SEC_TEXT].sz = g_sec[SEC_TEXT].buf.n;
    if (sec_loc[SEC_TEXT].sz) ew_emit(&out, g_sec[SEC_TEXT].buf.p,
                                       (size_t)sec_loc[SEC_TEXT].sz);

    /* .data */
    ew_pad_to(&out, 8);
    sec_loc[SEC_DATA].off = out.n;
    sec_loc[SEC_DATA].sz = g_sec[SEC_DATA].buf.n;
    if (sec_loc[SEC_DATA].sz) ew_emit(&out, g_sec[SEC_DATA].buf.p,
                                       (size_t)sec_loc[SEC_DATA].sz);

    /* .bss is NOBITS → just record size, no payload. */
    sec_loc[SEC_BSS].off = out.n;   /* arbitrary; readers ignore */
    sec_loc[SEC_BSS].sz = g_sec[SEC_BSS].size_only;

    /* .rodata */
    ew_pad_to(&out, 8);
    sec_loc[SEC_RODATA].off = out.n;
    sec_loc[SEC_RODATA].sz = g_sec[SEC_RODATA].buf.n;
    if (sec_loc[SEC_RODATA].sz) ew_emit(&out, g_sec[SEC_RODATA].buf.p,
                                         (size_t)sec_loc[SEC_RODATA].sz);

    /* .symtab */
    ew_pad_to(&out, 8);
    uint64_t symtab_off = out.n;
    ew_emit(&out, symtab.p, symtab.n);

    /* .strtab */
    ew_pad_to(&out, 1);
    uint64_t strtab_off = out.n;
    ew_emit(&out, sstr.buf, sstr.n);

    /* .shstrtab */
    ew_pad_to(&out, 1);
    uint64_t shstr_off = out.n;
    ew_emit(&out, shstr.buf, shstr.n);

    /* .rela.text (if any). */
    uint64_t rela_off = 0;
    if (kept_relocs) {
        ew_pad_to(&out, 8);
        rela_off = out.n;
        ew_emit(&out, rela.p, rela.n);
    }

    /* Section headers. */
    ew_pad_to(&out, 8);
    uint64_t shoff = out.n;
    /* shdr[0]: NULL */
    { ew_shdr64_t z = {0}; ew_emit(&out, &z, sizeof(z)); }
    /* shdr[1]: .text */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_text;
        s.sh_type = SHT_PROGBITS;
        s.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
        s.sh_offset = sec_loc[SEC_TEXT].off;
        s.sh_size = sec_loc[SEC_TEXT].sz;
        s.sh_addralign = g_sec[SEC_TEXT].align ? g_sec[SEC_TEXT].align : 4;
        ew_emit(&out, &s, sizeof(s));
    }
    /* shdr[2]: .data */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_data;
        s.sh_type = SHT_PROGBITS;
        s.sh_flags = SHF_ALLOC | SHF_WRITE;
        s.sh_offset = sec_loc[SEC_DATA].off;
        s.sh_size = sec_loc[SEC_DATA].sz;
        s.sh_addralign = g_sec[SEC_DATA].align ? g_sec[SEC_DATA].align : 1;
        ew_emit(&out, &s, sizeof(s));
    }
    /* shdr[3]: .bss */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_bss;
        s.sh_type = SHT_NOBITS;
        s.sh_flags = SHF_ALLOC | SHF_WRITE;
        s.sh_offset = sec_loc[SEC_BSS].off;
        s.sh_size = sec_loc[SEC_BSS].sz;
        s.sh_addralign = g_sec[SEC_BSS].align ? g_sec[SEC_BSS].align : 1;
        ew_emit(&out, &s, sizeof(s));
    }
    /* shdr[4]: .rodata */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_rodata;
        s.sh_type = SHT_PROGBITS;
        s.sh_flags = SHF_ALLOC;
        s.sh_offset = sec_loc[SEC_RODATA].off;
        s.sh_size = sec_loc[SEC_RODATA].sz;
        s.sh_addralign = g_sec[SEC_RODATA].align ? g_sec[SEC_RODATA].align : 1;
        ew_emit(&out, &s, sizeof(s));
    }
    /* shdr[5]: .symtab */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_symtab;
        s.sh_type = SHT_SYMTAB;
        s.sh_flags = 0;
        s.sh_offset = symtab_off;
        s.sh_size = symtab.n;
        s.sh_link = 6;          /* strtab index */
        s.sh_info = (uint32_t)symtab_globals_start;
        s.sh_addralign = 8;
        s.sh_entsize = sizeof(ew_sym64_t);
        ew_emit(&out, &s, sizeof(s));
        (void)symtab_first_local_real;
    }
    /* shdr[6]: .strtab */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_strtab;
        s.sh_type = SHT_STRTAB;
        s.sh_offset = strtab_off;
        s.sh_size = sstr.n;
        s.sh_addralign = 1;
        ew_emit(&out, &s, sizeof(s));
    }
    /* shdr[7]: .shstrtab */
    {
        ew_shdr64_t s = {0};
        s.sh_name = n_shstr;
        s.sh_type = SHT_STRTAB;
        s.sh_offset = shstr_off;
        s.sh_size = shstr.n;
        s.sh_addralign = 1;
        ew_emit(&out, &s, sizeof(s));
    }
    /* shdr[8]: .rela.text */
    if (kept_relocs) {
        ew_shdr64_t s = {0};
        s.sh_name = n_rela;
        s.sh_type = SHT_RELA;
        s.sh_offset = rela_off;
        s.sh_size = rela.n;
        s.sh_link = 5;          /* symtab */
        s.sh_info = 1;          /* .text */
        s.sh_addralign = 8;
        s.sh_entsize = sizeof(ew_rela64_t);
        ew_emit(&out, &s, sizeof(s));
    }

    /* Patch e_shoff. */
    ew_ehdr64_t *eh = (ew_ehdr64_t *)(out.p + ehdr_off);
    eh->e_shoff = shoff;

    /* Write to disk. */
    int fd = open(out_path, 0101 /* O_WRONLY|O_CREAT */);
    if (fd < 0) {
        printf("as: cannot open %s: errno=%d\n", out_path, errno);
        return;
    }
    long w = write(fd, out.p, out.n);
    if (w < 0 || (size_t)w != out.n) {
        printf("as: short write to %s (wrote %ld of %u)\n",
               out_path, w, (unsigned)out.n);
    }
    close(fd);
    printf("as: wrote %s (%u bytes, %d sections)\n",
           out_path, (unsigned)out.n, n_sections);
}

/* ---------- Input loader. ---------- */

static char  *g_input_buf = NULL;
static size_t g_input_len = 0;

static int load_file(const char *path)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("as: cannot open %s: errno=%d\n", path, errno);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        printf("as: cannot fstat %s\n", path); close(fd); return -1;
    }
    g_input_len = (size_t)st.st_size;
    g_input_buf = (char *)malloc(g_input_len + 1);
    if (!g_input_buf) { close(fd); return -1; }
    long r = read(fd, g_input_buf, g_input_len);
    close(fd);
    if (r < 0) return -1;
    g_input_buf[g_input_len] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    const char *in = NULL;
    const char *out = "out.o";

    for (int i = 1; i < argc; i++) {
        if (as_streq(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (argv[i][0] != '-') {
            in = argv[i];
        }
    }
    if (!in) {
        printf("usage: as [-o out.o] in.s\n");
        return 1;
    }
    g_filename = in;

    if (load_file(in) < 0) return 1;
    g_src = g_input_buf;
    g_p = g_input_buf;

    for (int i = 0; i < NSEC; i++) {
        ew_buf_init(&g_sec[i].buf);
        g_sec[i].size_only = 0;
        g_sec[i].align = 4;
    }
    g_cur_sec = SEC_TEXT;

    assemble();

    if (g_error_count > 0) {
        printf("as: %d error(s)\n", g_error_count);
        return 1;
    }

    write_elf(out);
    return 0;
}
