/* userspace/libc/elf_write.h — minimal ELF64 writer.
 *
 * Header-only.  Used by chapter 154's /bin/as, chapter 155's
 * /bin/ld, and chapter 156's runtime tooling.
 *
 * We only emit AArch64 ELF64-LSB.  No 32-bit, no big-endian,
 * no other architecture.  The reader contract (book INDEX.md)
 * is AArch64-only.
 *
 * Two output shapes are supported via the same low-level
 * helpers: relocatable (.o files: ET_REL with sections but no
 * program headers) and executable (ET_EXEC with program
 * headers).  Chapter 154 only uses the relocatable shape; 119
 * grows the executable shape on top of the same byte-pushers.
 */
#ifndef LIBC_ELF_WRITE_H
#define LIBC_ELF_WRITE_H

#include <stdint.h>
#include <stddef.h>

/* ---------- ELF64 on-disk types (subset). ---------- */

#define ELF_MAGIC       0x464C457FU  /* 0x7F 'E' 'L' 'F' little-endian */
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1

#define ET_REL          1
#define ET_EXEC         2
#define EM_AARCH64      183

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8

#define SHF_WRITE       (1u << 0)
#define SHF_ALLOC       (1u << 1)
#define SHF_EXECINSTR   (1u << 2)

#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_SECTION     3

#define ELF64_ST_INFO(b,t)   (((b) << 4) | ((t) & 0xF))
#define ELF64_R_INFO(s,t)    ((((uint64_t)(s)) << 32) | (uint64_t)((uint32_t)(t)))

/* AArch64 relocation type numbers we emit. */
#define R_AARCH64_NONE                  0
#define R_AARCH64_ABS64                 257
#define R_AARCH64_CALL26                283
#define R_AARCH64_JUMP26                282
#define R_AARCH64_ADR_PREL_PG_HI21      275
#define R_AARCH64_ADD_ABS_LO12_NC       277

#pragma pack(push, 1)

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} ew_ehdr64_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} ew_shdr64_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} ew_sym64_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} ew_rela64_t;

#pragma pack(pop)

/* ---------- Growable byte buffer. ---------- */

typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
} ew_buf_t;

static inline void ew_buf_init(ew_buf_t *b) { b->p = 0; b->n = 0; b->cap = 0; }

/* Caller-provided allocator hook: ew_realloc(ptr, new_size) must
 * behave like realloc(); freed when caller is done.  We declare
 * the prototype here and let the host TU provide it (typically
 * just `realloc` from the chapter-116c malloc.h, or a sandbox
 * arena in tests). */
void *ew_realloc(void *p, size_t n);

static inline int ew_grow(ew_buf_t *b, size_t need)
{
    if (b->n + need <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->n + need) nc *= 2;
    void *np = ew_realloc(b->p, nc);
    if (!np) return -1;
    b->p = (uint8_t *)np;
    b->cap = nc;
    return 0;
}

static inline int ew_emit(ew_buf_t *b, const void *src, size_t n)
{
    if (ew_grow(b, n) < 0) return -1;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) b->p[b->n + i] = s[i];
    b->n += n;
    return 0;
}

static inline int ew_emit_u32(ew_buf_t *b, uint32_t v)
{
    uint8_t buf[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF),
    };
    return ew_emit(b, buf, 4);
}

static inline int ew_emit_u64(ew_buf_t *b, uint64_t v)
{
    if (ew_emit_u32(b, (uint32_t)v) < 0) return -1;
    return ew_emit_u32(b, (uint32_t)(v >> 32));
}

static inline int ew_pad_to(ew_buf_t *b, size_t align)
{
    while (b->n % align) {
        uint8_t z = 0;
        if (ew_emit(b, &z, 1) < 0) return -1;
    }
    return 0;
}

/* ---------- ELF header for AArch64 ELF64-LSB. ---------- */

static inline void ew_ehdr_init(ew_ehdr64_t *e, uint16_t type)
{
    for (int i = 0; i < 16; i++) e->e_ident[i] = 0;
    e->e_ident[0] = 0x7F;
    e->e_ident[1] = 'E';
    e->e_ident[2] = 'L';
    e->e_ident[3] = 'F';
    e->e_ident[4] = ELFCLASS64;
    e->e_ident[5] = ELFDATA2LSB;
    e->e_ident[6] = EV_CURRENT;
    /* e_ident[7..15] = 0 (OSABI = SYSV, padding) */
    e->e_type = type;
    e->e_machine = EM_AARCH64;
    e->e_version = EV_CURRENT;
    e->e_entry = 0;
    e->e_phoff = 0;
    e->e_shoff = 0;
    e->e_flags = 0;
    e->e_ehsize = sizeof(ew_ehdr64_t);
    e->e_phentsize = 0;
    e->e_phnum = 0;
    e->e_shentsize = sizeof(ew_shdr64_t);
    e->e_shnum = 0;
    e->e_shstrndx = 0;
}

#endif /* LIBC_ELF_WRITE_H */
