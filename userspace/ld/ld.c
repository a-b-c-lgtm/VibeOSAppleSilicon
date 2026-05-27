/*
 * userspace/ld/ld.c — minimal AArch64 ELF64 linker for our OS.
 *
 * Consumes ET_REL relocatable objects produced by /bin/as
 * (chapter 154), merges their sections, resolves symbols
 * across inputs, applies a small reloc set, and writes a
 * single ET_EXEC ELF64-LSB file the kernel ELF loader
 * (kernel/core/elf.c) can mmap and run.
 *
 * Usage:  ld -o out.elf [-e entry] file1.o [file2.o ...]
 *
 * Defaults:
 *   -e entry        defaults to "_user_start" (matches our
 *                   crt0 convention) if present, else "_start".
 *   USER_LOAD_ADDR  fixed at 0x1000100000 to match
 *                   userspace/linker_user.ld.
 *
 * Output layout (mirrors what aarch64-elf-ld produces for us):
 *
 *   +-------------------+ vaddr USER_LOAD_ADDR
 *   |   Elf64_Ehdr      |
 *   |   2 x Elf64_Phdr  |  (PT_LOAD R+X for .text/.rodata
 *   |                   |   and PT_LOAD R+W for .data; .bss
 *   |                   |   rides as memsz>filesz on it)
 *   +-------------------+ aligned to 4K
 *   |   .text           |  PROGBITS, R+X
 *   |   .rodata         |  PROGBITS, R
 *   +-------------------+ aligned to 4K
 *   |   .data           |  PROGBITS, R+W
 *   |   .bss            |  NOBITS, contributes only memsz
 *   +-------------------+
 *
 * Supported relocations:
 *   R_AARCH64_ABS64     — 64-bit absolute, used by `.quad sym`
 *   R_AARCH64_CALL26    — bl <sym>
 *   R_AARCH64_JUMP26    — b  <sym>
 *
 * Out of scope (deferred to chapter 157 / 123 if a real
 * compiler demands them): adrp+add pairs, GOT, PLT, TLS,
 * shared libraries, archive scanning.  /bin/ld takes only
 * loose .o files; /bin/ar (in this same chapter) produces
 * archives but ld doesn't consume them yet.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/sys/stat.h"
#include "../libc/elf_write.h"

/* ---------- mem*: avoid the freestanding-memset trap. ---------- */
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

/* Tiny realloc on top of malloc.h's free-list allocator.  See
 * chapter-118 /bin/as for the same hook. */
static void *ld_realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    size_t old_total = *((size_t *)((char *)p - UALLOC_HDR_SIZE));
    size_t old_payload = old_total - UALLOC_HDR_SIZE;
    if (n <= old_payload) return p;
    void *np = malloc(n);
    if (!np) return (void *)0;
    char *src = (char *)p; char *dst = (char *)np;
    for (size_t i = 0; i < old_payload; i++) dst[i] = src[i];
    free(p);
    return np;
}

void *ew_realloc(void *p, size_t n) { return ld_realloc(p, n); }

/* ---------- Tiny string utils. ---------- */

static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------- Input object representation. ---------- */

#define LD_MAX_INPUTS  16
#define LD_MAX_SYMS    2048

#define LD_S_TEXT   0
#define LD_S_RODATA 1
#define LD_S_DATA   2
#define LD_S_BSS    3
#define LD_S_COUNT  4

/* For each input file, the byte range each of its four
 * named sections occupies (after concatenation into the
 * output's combined sections).  Used both for layout and to
 * resolve symbol VAs and apply relocations. */
typedef struct {
    const char    *path;        /* filename for diagnostics */
    uint8_t       *data;        /* whole file contents */
    size_t         size;
    ew_ehdr64_t   *ehdr;
    ew_shdr64_t   *shdrs;       /* points into data */
    int            shnum;
    int            sh_text;     /* shdr index, -1 if absent */
    int            sh_rodata;
    int            sh_data;
    int            sh_bss;
    int            sh_symtab;
    int            sh_strtab;
    int            sh_relatext;
    /* Offsets where this input's sections were placed inside
     * the output's merged section payload (file-relative
     * within that section, NOT yet shifted to the segment
     * base).  Filled in during layout. */
    uint64_t       out_off[LD_S_COUNT];
    uint64_t       out_size[LD_S_COUNT];
} ld_input_t;

static ld_input_t g_in[LD_MAX_INPUTS];
static int g_ninputs = 0;

/* Global symbol table (merged across all inputs).  Only
 * STB_GLOBAL symbols live here.  Locals are resolved
 * per-input using the input's own symtab. */
typedef struct {
    char       name[64];
    int        defined;          /* 1 if some input defines this */
    int        owner_input;      /* which g_in[] slot defines it */
    int        owner_sec;        /* LD_S_TEXT / LD_S_RODATA / ... */
    uint64_t   value;            /* sym.st_value (offset within
                                    its input section) */
} ld_gsym_t;

static ld_gsym_t g_gsyms[LD_MAX_SYMS];
static int g_ngsyms = 0;

/* ---------- Convenience: read a file fully into malloc'd buffer. ---------- */

static int read_whole_file(const char *path, uint8_t **buf, size_t *out_sz)
{
    int fd = open(path, 0 /* O_RDONLY */);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t sz = (size_t)st.st_size;
    uint8_t *p = (uint8_t *)malloc(sz ? sz : 1);
    if (!p) { close(fd); return -1; }
    size_t got = 0;
    while (got < sz) {
        long n = read(fd, p + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != sz) { free(p); return -1; }
    *buf = p;
    *out_sz = sz;
    return 0;
}

/* ---------- ELF reader. ---------- */

static int load_input(ld_input_t *in, const char *path)
{
    if (read_whole_file(path, &in->data, &in->size) < 0) {
        printf("ld: cannot read %s: errno=%d\n", path, errno);
        return -1;
    }
    in->path = path;
    if (in->size < sizeof(ew_ehdr64_t)) {
        printf("ld: %s: too small to be ELF\n", path); return -1;
    }
    in->ehdr = (ew_ehdr64_t *)in->data;
    if (!(in->ehdr->e_ident[0] == 0x7F &&
          in->ehdr->e_ident[1] == 'E' &&
          in->ehdr->e_ident[2] == 'L' &&
          in->ehdr->e_ident[3] == 'F')) {
        printf("ld: %s: not an ELF file\n", path); return -1;
    }
    if (in->ehdr->e_ident[4] != ELFCLASS64 ||
        in->ehdr->e_ident[5] != ELFDATA2LSB ||
        in->ehdr->e_type    != ET_REL      ||
        in->ehdr->e_machine != EM_AARCH64) {
        printf("ld: %s: not an AArch64 ELF64-LSB relocatable\n",
               path);
        return -1;
    }
    in->shdrs = (ew_shdr64_t *)(in->data + in->ehdr->e_shoff);
    in->shnum = in->ehdr->e_shnum;
    in->sh_text = in->sh_rodata = in->sh_data = in->sh_bss = -1;
    in->sh_symtab = in->sh_strtab = in->sh_relatext = -1;
    /* Find .shstrtab to name sections. */
    if (in->ehdr->e_shstrndx == 0 ||
        in->ehdr->e_shstrndx >= in->shnum) {
        printf("ld: %s: bad e_shstrndx\n", path); return -1;
    }
    const char *shstr = (const char *)(in->data +
        in->shdrs[in->ehdr->e_shstrndx].sh_offset);
    for (int i = 0; i < in->shnum; i++) {
        const char *nm = shstr + in->shdrs[i].sh_name;
        if (s_eq(nm, ".text"))      in->sh_text    = i;
        else if (s_eq(nm, ".rodata")) in->sh_rodata = i;
        else if (s_eq(nm, ".data"))   in->sh_data   = i;
        else if (s_eq(nm, ".bss"))    in->sh_bss    = i;
        else if (s_eq(nm, ".symtab")) in->sh_symtab = i;
        else if (s_eq(nm, ".strtab")) in->sh_strtab = i;
        else if (s_eq(nm, ".rela.text")) in->sh_relatext = i;
    }
    if (in->sh_symtab < 0 || in->sh_strtab < 0) {
        printf("ld: %s: missing .symtab or .strtab\n", path);
        return -1;
    }
    return 0;
}

/* ---------- Symbol table merge. ---------- */

static int copy_name(char *dst, size_t dstcap, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < dstcap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return (int)i;
}

static int gsym_find(const char *name)
{
    for (int i = 0; i < g_ngsyms; i++)
        if (s_eq(g_gsyms[i].name, name)) return i;
    return -1;
}

static int gsym_add_or_update(const char *name, int input_idx,
                              int sec_kind, uint64_t value,
                              int is_defined)
{
    int idx = gsym_find(name);
    if (idx < 0) {
        if (g_ngsyms >= LD_MAX_SYMS) {
            printf("ld: too many global symbols\n");
            return -1;
        }
        idx = g_ngsyms++;
        copy_name(g_gsyms[idx].name, sizeof(g_gsyms[idx].name), name);
        g_gsyms[idx].defined = 0;
    }
    if (is_defined) {
        if (g_gsyms[idx].defined) {
            printf("ld: multiple definitions of '%s'\n", name);
            return -1;
        }
        g_gsyms[idx].defined = 1;
        g_gsyms[idx].owner_input = input_idx;
        g_gsyms[idx].owner_sec = sec_kind;
        g_gsyms[idx].value = value;
    }
    return idx;
}

/* Map an input shndx to one of our LD_S_* kinds (or -1
 * for any "other" section we don't care about). */
static int sec_kind_for_input_shndx(const ld_input_t *in, int shndx)
{
    if (shndx == in->sh_text)   return LD_S_TEXT;
    if (shndx == in->sh_rodata) return LD_S_RODATA;
    if (shndx == in->sh_data)   return LD_S_DATA;
    if (shndx == in->sh_bss)    return LD_S_BSS;
    return -1;
}

static int merge_input_symbols(int input_idx)
{
    ld_input_t *in = &g_in[input_idx];
    ew_sym64_t *syms = (ew_sym64_t *)(in->data +
        in->shdrs[in->sh_symtab].sh_offset);
    size_t nsyms = (size_t)in->shdrs[in->sh_symtab].sh_size /
                   sizeof(ew_sym64_t);
    const char *strtab = (const char *)(in->data +
        in->shdrs[in->sh_strtab].sh_offset);
    for (size_t i = 0; i < nsyms; i++) {
        ew_sym64_t *s = &syms[i];
        unsigned bind = (unsigned)(s->st_info >> 4);
        if (bind != STB_GLOBAL) continue;
        const char *name = strtab + s->st_name;
        if (!name[0]) continue;
        int sec_kind = -1;
        int defined = 0;
        if (s->st_shndx > 0 && s->st_shndx < in->shnum) {
            sec_kind = sec_kind_for_input_shndx(in, s->st_shndx);
            if (sec_kind >= 0) defined = 1;
        }
        if (gsym_add_or_update(name, input_idx, sec_kind,
                                s->st_value, defined) < 0)
            return -1;
    }
    return 0;
}

/* ---------- Layout. ---------- */

static uint64_t g_sec_va[LD_S_COUNT];
static uint64_t g_sec_size[LD_S_COUNT];
static uint64_t g_sec_foff[LD_S_COUNT];   /* file offset; .bss has none */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    if (a == 0) return v;
    return (v + a - 1) & ~(a - 1);
}

#define LD_LOAD_BASE       0x1000100000ULL
#define LD_TEXT_SEG_ALIGN  0x1000ULL
#define LD_DATA_SEG_ALIGN  0x1000ULL

static int layout_sections(void)
{
    /* For each section kind, sum its input contributions to get
     * the merged size; also fill in out_off / out_size per
     * input so we can later place data and resolve symbols. */
    for (int k = 0; k < LD_S_COUNT; k++) g_sec_size[k] = 0;
    for (int i = 0; i < g_ninputs; i++) {
        ld_input_t *in = &g_in[i];
        for (int k = 0; k < LD_S_COUNT; k++) {
            int shndx = -1;
            if (k == LD_S_TEXT)   shndx = in->sh_text;
            if (k == LD_S_RODATA) shndx = in->sh_rodata;
            if (k == LD_S_DATA)   shndx = in->sh_data;
            if (k == LD_S_BSS)    shndx = in->sh_bss;
            if (shndx < 0) { in->out_off[k] = 0;
                              in->out_size[k] = 0; continue; }
            uint64_t algn = in->shdrs[shndx].sh_addralign;
            if (!algn) algn = 1;
            g_sec_size[k] = align_up(g_sec_size[k], algn);
            in->out_off[k] = g_sec_size[k];
            in->out_size[k] = in->shdrs[shndx].sh_size;
            g_sec_size[k] += in->out_size[k];
        }
    }
    /* Output layout: ehdr + 2 phdrs at the very front, then
     * .text + .rodata follow within the first PT_LOAD, then
     * .data + .bss in the second.  Compute file offsets and
     * VAs for each merged section. */
    uint64_t ehdr_sz = sizeof(ew_ehdr64_t) + 2 * 56 /* phdr */;
    /* First PT_LOAD VA starts at LOAD_BASE; the headers are
     * "free" within that page because text segment is
     * page-aligned. */
    uint64_t va  = LD_LOAD_BASE;
    uint64_t off = 0;
    /* Skip past ehdr+phdrs. */
    off = ehdr_sz;
    /* Align .text to 4K — kernel ELF loader maps PT_LOADs at
     * page granularity. */
    off = align_up(off, LD_TEXT_SEG_ALIGN);
    g_sec_foff[LD_S_TEXT] = off;
    g_sec_va  [LD_S_TEXT] = va + off;
    off += g_sec_size[LD_S_TEXT];
    /* .rodata follows .text in the same segment. */
    off = align_up(off, 8);
    g_sec_foff[LD_S_RODATA] = off;
    g_sec_va  [LD_S_RODATA] = va + off;
    off += g_sec_size[LD_S_RODATA];
    /* .data goes in its own PT_LOAD so we can mark it RW.
     * Align both VA and file offset to a page so the kernel
     * loader maps it cleanly. */
    off = align_up(off, LD_DATA_SEG_ALIGN);
    /* Keep VA stride matching file stride for now (no gap). */
    g_sec_foff[LD_S_DATA] = off;
    g_sec_va  [LD_S_DATA] = va + off;
    off += g_sec_size[LD_S_DATA];
    /* .bss does NOT consume file bytes. */
    g_sec_foff[LD_S_BSS] = 0;
    g_sec_va  [LD_S_BSS] = g_sec_va[LD_S_DATA] +
                           g_sec_size[LD_S_DATA];
    return 0;
}

static uint64_t input_sec_va(const ld_input_t *in, int sec_kind)
{
    return g_sec_va[sec_kind] + in->out_off[sec_kind];
}

/* Resolve a symbol from an input's own .symtab into a final
 * output VA.  Locals are resolved directly; globals defer to
 * the merged g_gsyms table. */
static int resolve_input_sym(const ld_input_t *in, int sym_idx,
                              uint64_t *out_va)
{
    ew_sym64_t *syms = (ew_sym64_t *)(in->data +
        in->shdrs[in->sh_symtab].sh_offset);
    const char *strtab = (const char *)(in->data +
        in->shdrs[in->sh_strtab].sh_offset);
    ew_sym64_t *s = &syms[sym_idx];
    unsigned bind = (unsigned)(s->st_info >> 4);
    unsigned type = (unsigned)(s->st_info & 0xF);
    if (type == STT_SECTION) {
        /* st_shndx is a section in the input; map to LD_S_*. */
        int k = sec_kind_for_input_shndx(in, s->st_shndx);
        if (k < 0) return -1;
        *out_va = input_sec_va(in, k);
        return 0;
    }
    if (bind == STB_GLOBAL) {
        const char *name = strtab + s->st_name;
        int gi = gsym_find(name);
        if (gi < 0 || !g_gsyms[gi].defined) {
            printf("ld: undefined reference to '%s' in %s\n",
                   name, in->path);
            return -1;
        }
        const ld_input_t *def = &g_in[g_gsyms[gi].owner_input];
        *out_va = input_sec_va(def, g_gsyms[gi].owner_sec) +
                  g_gsyms[gi].value;
        return 0;
    }
    /* Local symbol — resolve relative to its defining
     * section within THIS input. */
    int k = sec_kind_for_input_shndx(in, s->st_shndx);
    if (k < 0) {
        const char *name = strtab + s->st_name;
        printf("ld: local symbol '%s' in %s lives in an "
               "unknown section\n", name, in->path);
        return -1;
    }
    *out_va = input_sec_va(in, k) + s->st_value;
    return 0;
}

/* ---------- Output assembly. ---------- */

static uint8_t *g_out;
static size_t   g_out_size;

static int alloc_output(void)
{
    /* Total file size = max file-offset + payload size of the
     * last file-resident section. */
    size_t total = (size_t)(g_sec_foff[LD_S_DATA] +
                            g_sec_size[LD_S_DATA]);
    if (total < (size_t)(g_sec_foff[LD_S_RODATA] +
                          g_sec_size[LD_S_RODATA]))
        total = (size_t)(g_sec_foff[LD_S_RODATA] +
                          g_sec_size[LD_S_RODATA]);
    if (total < (size_t)(g_sec_foff[LD_S_TEXT] +
                          g_sec_size[LD_S_TEXT]))
        total = (size_t)(g_sec_foff[LD_S_TEXT] +
                          g_sec_size[LD_S_TEXT]);
    g_out_size = total;
    g_out = (uint8_t *)malloc(total ? total : 1);
    if (!g_out) return -1;
    for (size_t i = 0; i < total; i++) g_out[i] = 0;
    return 0;
}

static void copy_input_sections(void)
{
    for (int i = 0; i < g_ninputs; i++) {
        ld_input_t *in = &g_in[i];
        for (int k = 0; k < LD_S_COUNT; k++) {
            if (k == LD_S_BSS) continue;
            int shndx = -1;
            if (k == LD_S_TEXT)   shndx = in->sh_text;
            if (k == LD_S_RODATA) shndx = in->sh_rodata;
            if (k == LD_S_DATA)   shndx = in->sh_data;
            if (shndx < 0) continue;
            ew_shdr64_t *sh = &in->shdrs[shndx];
            if (sh->sh_type == SHT_NOBITS) continue;
            uint64_t dst = g_sec_foff[k] + in->out_off[k];
            uint8_t *src = in->data + sh->sh_offset;
            for (uint64_t j = 0; j < sh->sh_size; j++)
                g_out[dst + j] = src[j];
        }
    }
}

/* Apply a single AArch64 relocation. */
static int apply_one_reloc(const ld_input_t *in,
                           uint64_t target_file_off,
                           uint64_t target_va,
                           uint64_t sym_va,
                           uint32_t rtype,
                           int64_t  addend)
{
    if (rtype == R_AARCH64_ABS64) {
        uint64_t v = sym_va + (uint64_t)addend;
        for (int b = 0; b < 8; b++)
            g_out[target_file_off + b] = (uint8_t)(v >> (b * 8));
        return 0;
    }
    if (rtype == R_AARCH64_CALL26 || rtype == R_AARCH64_JUMP26) {
        int64_t delta = (int64_t)(sym_va + (uint64_t)addend) -
                        (int64_t)target_va;
        if (delta & 0x3) {
            printf("ld: branch target not 4-aligned in %s\n",
                   in->path); return -1;
        }
        int64_t imm26 = delta / 4;
        if (imm26 < -(1 << 25) || imm26 >= (1 << 25)) {
            printf("ld: branch out of range in %s\n", in->path);
            return -1;
        }
        uint8_t *bp = g_out + target_file_off;
        uint32_t insn = (uint32_t)bp[0] | ((uint32_t)bp[1] << 8) |
                        ((uint32_t)bp[2] << 16) |
                        ((uint32_t)bp[3] << 24);
        insn = (insn & 0xFC000000u) |
               ((uint32_t)imm26 & 0x03FFFFFFu);
        bp[0] = (uint8_t)(insn & 0xFF);
        bp[1] = (uint8_t)((insn >> 8) & 0xFF);
        bp[2] = (uint8_t)((insn >> 16) & 0xFF);
        bp[3] = (uint8_t)((insn >> 24) & 0xFF);
        return 0;
    }
    printf("ld: unsupported reloc type %u in %s\n",
           rtype, in->path);
    return -1;
}

static int apply_input_relocs(int input_idx)
{
    ld_input_t *in = &g_in[input_idx];
    if (in->sh_relatext < 0) return 0;
    ew_rela64_t *r = (ew_rela64_t *)(in->data +
        in->shdrs[in->sh_relatext].sh_offset);
    size_t n = (size_t)in->shdrs[in->sh_relatext].sh_size /
               sizeof(ew_rela64_t);
    for (size_t i = 0; i < n; i++) {
        uint32_t sym_idx = (uint32_t)(r[i].r_info >> 32);
        uint32_t rtype   = (uint32_t)(r[i].r_info & 0xFFFFFFFF);
        uint64_t sym_va  = 0;
        if (resolve_input_sym(in, (int)sym_idx, &sym_va) < 0)
            return -1;
        uint64_t in_text_off = in->out_off[LD_S_TEXT];
        uint64_t tgt_off_in_text = r[i].r_offset;
        uint64_t target_file_off = g_sec_foff[LD_S_TEXT] +
                                   in_text_off + tgt_off_in_text;
        uint64_t target_va = g_sec_va[LD_S_TEXT] +
                             in_text_off + tgt_off_in_text;
        if (apply_one_reloc(in, target_file_off, target_va,
                            sym_va, rtype, r[i].r_addend) < 0)
            return -1;
    }
    return 0;
}

/* ---------- Final ELF write. ---------- */

#pragma pack(push, 1)
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} ld_phdr_t;
#pragma pack(pop)

#define PT_LOAD  1
#define PF_X     1
#define PF_W     2
#define PF_R     4

static int write_output(const char *out_path, uint64_t entry_va)
{
    /* Ehdr at offset 0. */
    ew_ehdr64_t ehdr;
    ew_ehdr_init(&ehdr, ET_EXEC);
    ehdr.e_entry = entry_va;
    ehdr.e_phoff = sizeof(ew_ehdr64_t);
    ehdr.e_phnum = 2;
    ehdr.e_phentsize = sizeof(ld_phdr_t);
    ehdr.e_shoff = 0;
    ehdr.e_shentsize = 0;
    ehdr.e_shnum = 0;
    ehdr.e_shstrndx = 0;
    for (size_t b = 0; b < sizeof(ehdr); b++)
        g_out[b] = ((uint8_t *)&ehdr)[b];
    /* Two phdrs. */
    ld_phdr_t ph_text;
    ph_text.p_type   = PT_LOAD;
    ph_text.p_flags  = PF_R | PF_X;
    ph_text.p_offset = 0;
    ph_text.p_vaddr  = LD_LOAD_BASE;
    ph_text.p_paddr  = LD_LOAD_BASE;
    ph_text.p_filesz = g_sec_foff[LD_S_RODATA] +
                       g_sec_size[LD_S_RODATA];
    ph_text.p_memsz  = ph_text.p_filesz;
    ph_text.p_align  = LD_TEXT_SEG_ALIGN;
    ld_phdr_t ph_data;
    ph_data.p_type   = PT_LOAD;
    ph_data.p_flags  = PF_R | PF_W;
    ph_data.p_offset = g_sec_foff[LD_S_DATA];
    ph_data.p_vaddr  = g_sec_va[LD_S_DATA];
    ph_data.p_paddr  = ph_data.p_vaddr;
    ph_data.p_filesz = g_sec_size[LD_S_DATA];
    ph_data.p_memsz  = g_sec_size[LD_S_DATA] +
                       g_sec_size[LD_S_BSS];
    ph_data.p_align  = LD_DATA_SEG_ALIGN;
    uint64_t poff = sizeof(ew_ehdr64_t);
    for (size_t b = 0; b < sizeof(ph_text); b++)
        g_out[poff + b] = ((uint8_t *)&ph_text)[b];
    poff += sizeof(ph_text);
    for (size_t b = 0; b < sizeof(ph_data); b++)
        g_out[poff + b] = ((uint8_t *)&ph_data)[b];
    /* Open + write. */
    int fd = open(out_path, 0101 /* O_WRONLY|O_CREAT */);
    if (fd < 0) {
        printf("ld: cannot open %s: errno=%d\n", out_path, errno);
        return -1;
    }
    long w = write(fd, g_out, g_out_size);
    close(fd);
    if (w < 0 || (size_t)w != g_out_size) {
        printf("ld: short write to %s (%ld of %u)\n",
               out_path, w, (unsigned)g_out_size);
        return -1;
    }
    printf("ld: wrote %s (%u bytes, entry=0x%x)\n", out_path,
           (unsigned)g_out_size, (unsigned)(uint32_t)entry_va);
    return 0;
}

/* ---------- Entry. ---------- */

int main(int argc, char **argv)
{
    const char *out_path = "a.out";
    const char *entry_name = "_user_start";  /* default */
    int entry_explicit = 0;

    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (s_eq(a, "-o") && i + 1 < argc) {
            out_path = argv[i + 1]; i += 2; continue;
        }
        if (s_eq(a, "-e") && i + 1 < argc) {
            entry_name = argv[i + 1]; entry_explicit = 1; i += 2;
            continue;
        }
        if (a[0] == '-') {
            printf("ld: unknown option %s\n", a); return 1;
        }
        if (g_ninputs >= LD_MAX_INPUTS) {
            printf("ld: too many input files\n"); return 1;
        }
        if (load_input(&g_in[g_ninputs], a) < 0) return 1;
        g_ninputs++;
        i++;
    }
    if (g_ninputs == 0) {
        printf("ld: no input files\n"); return 1;
    }
    for (int j = 0; j < g_ninputs; j++)
        if (merge_input_symbols(j) < 0) return 1;
    if (layout_sections() < 0) return 1;
    if (alloc_output() < 0) {
        printf("ld: OOM\n"); return 1;
    }
    copy_input_sections();
    for (int j = 0; j < g_ninputs; j++)
        if (apply_input_relocs(j) < 0) return 1;
    /* Pick entry. */
    int gi = gsym_find(entry_name);
    if (gi < 0 || !g_gsyms[gi].defined) {
        if (!entry_explicit) {
            /* Try _start as a fallback. */
            entry_name = "_start";
            gi = gsym_find(entry_name);
        }
    }
    if (gi < 0 || !g_gsyms[gi].defined) {
        printf("ld: entry symbol '%s' not defined\n", entry_name);
        return 1;
    }
    uint64_t entry_va = input_sec_va(&g_in[g_gsyms[gi].owner_input],
                                     g_gsyms[gi].owner_sec) +
                        g_gsyms[gi].value;
    if (write_output(out_path, entry_va) < 0) return 1;
    return 0;
}
