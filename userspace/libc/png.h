/*
 * userspace/libc/png.h — header-only PNG decoder.
 *
 * Chapter 98 deliverable, extended in chapter 99 to cover the
 * other common colour types real-world PNGs use:
 *
 *   - colour type 0 (grayscale)        at bit-depths 1, 2, 4, 8
 *   - colour type 2 (RGB)              at bit-depth  8
 *   - colour type 3 (palette / index)  at bit-depths 1, 2, 4, 8
 *   - colour type 4 (gray + alpha)     at bit-depth  8
 *   - colour type 6 (RGBA)             at bit-depth  8
 *   - no interlacing (Adam7 not implemented)
 *   - filters: None, Sub, Up, Average, Paeth (all 5 spec'd)
 *   - chunks: IHDR, PLTE, tRNS, IDAT (one or more), IEND;
 *     everything else (gAMA, sRGB, pHYs, iCCP, ...) is ignored
 *
 * The tRNS chunk is honoured for types 0, 2, 3 (single-key
 * transparency for gray/RGB, per-entry alpha for palettes).
 *
 * Output is a heap-allocated tightly-packed BGRA byte array
 * (B, G, R, A repeated w*h times) so it can be handed straight
 * to gui_present without a colour-channel re-shuffle.  RGB
 * sources get an alpha of 0xFF on every pixel.
 *
 * The decoder is self-contained: a 256-byte state machine for
 * the bit reader, a ~120-line RFC 1951 inflate, the PNG chunk
 * walk, and the per-row filter undo.  Total ~600 lines of
 * header.  No external dependencies beyond malloc and printf
 * (used only for error reporting).
 *
 * Memory model: png_decode allocates ONE BGRA buffer of size
 * w*h*4 bytes.  The caller frees with png_free (which is
 * literally `free()` — exposed as a name so the API surface
 * documents the ownership transfer).  All scratch buffers
 * (zlib payload assembly, raw pixel rows) are freed before
 * return.
 *
 * Failure modes: returns -1 with a one-line printf to stdout
 * (prefixed `[png] `).  We do NOT longjmp or otherwise unwind
 * partially-decoded state — every alloc that escapes a check
 * is freed on every error path.  Set PNG_QUIET to 1 before
 * the include to suppress the per-failure printf.
 */
#ifndef USER_PNG_H
#define USER_PNG_H

#include <stdint.h>
#include <stddef.h>

#include "malloc.h"
#include "printf.h"

#ifndef PNG_QUIET
#define PNG_QUIET 0
#endif

#if PNG_QUIET
  #define PNG_LOG(...) do { } while (0)
#else
  #define PNG_LOG(...) printf(__VA_ARGS__)
#endif

/* ---- public API ---- */

/* Decode `src[0 .. src_len)` as a PNG.  On success returns 0,
 * stores the decoded BGRA buffer in `*out_bgra` (caller frees
 * with png_free), and stores width/height in `*out_w`/`*out_h`.
 * On failure returns -1 and leaves `*out_bgra` NULL.
 *
 * Output BGRA layout: pixel (x, y) lives at byte offset
 *   (y * w + x) * 4
 * with bytes B, G, R, A in increasing address order. */
static int  png_decode(const uint8_t *src, size_t src_len,
                       uint8_t **out_bgra, int *out_w, int *out_h);

/* Free a BGRA buffer returned by png_decode. */
static inline void png_free(uint8_t *bgra) { if (bgra) free(bgra); }

/* ============================================================
 *   Internals — bit reader for the deflate stream
 * ============================================================ */

struct png_br {
    const uint8_t *p;     /* next byte to consume */
    const uint8_t *end;   /* one past the last byte */
    uint32_t bitbuf;      /* lowest `nbits` bits hold pending input */
    int      nbits;
};

static inline int png_br_need(struct png_br *br, int n)
{
    while (br->nbits < n) {
        if (br->p >= br->end) return -1;
        br->bitbuf |= (uint32_t)(*br->p++) << br->nbits;
        br->nbits += 8;
    }
    return 0;
}

static inline uint32_t png_br_get(struct png_br *br, int n)
{
    /* Caller must have ensured nbits >= n via png_br_need. */
    uint32_t v = br->bitbuf & ((n == 32) ? 0xffffffffu : ((1u << n) - 1u));
    br->bitbuf >>= n;
    br->nbits  -= n;
    return v;
}

static inline void png_br_align_byte(struct png_br *br)
{
    int drop = br->nbits & 7;
    br->bitbuf >>= drop;
    br->nbits  -= drop;
}

/* ============================================================
 *   Internals — Huffman table for inflate
 * ============================================================ */

/* Per RFC 1951 §3.2.7: max code length 15 bits, max symbols
 * 288 (literal/length alphabet).  We use a flat lookup table
 * scheme: for each code length L, a sorted list of (code, sym)
 * pairs.  Decoding walks from L=1 upward, peeking L bits at a
 * time. */
#define PNG_MAX_CODE_LEN  15
#define PNG_MAX_SYMS      288

struct png_huff {
    /* counts[L] = number of codes of length L (0 .. 15). */
    uint16_t counts[PNG_MAX_CODE_LEN + 1];
    /* symbols sorted by (length, symbol value). */
    uint16_t syms[PNG_MAX_SYMS];
};

static int png_huff_build(struct png_huff *h,
                          const uint8_t *lens, int nsyms)
{
    int i, l;
    /* Zero counts. */
    for (l = 0; l <= PNG_MAX_CODE_LEN; l++) h->counts[l] = 0;
    for (i = 0; i < nsyms; i++) {
        if (lens[i] > PNG_MAX_CODE_LEN) {
            PNG_LOG("[png] huff: oversized code length %u\n", lens[i]);
            return -1;
        }
        h->counts[lens[i]]++;
    }
    /* Compute offsets[L] = first index in syms[] for length L. */
    uint16_t offsets[PNG_MAX_CODE_LEN + 2];
    offsets[0] = 0; offsets[1] = 0;
    for (l = 1; l <= PNG_MAX_CODE_LEN; l++)
        offsets[l + 1] = offsets[l] + h->counts[l];
    /* Place each symbol into syms[] at offsets[len], post-incrementing. */
    uint16_t cursor[PNG_MAX_CODE_LEN + 2];
    for (l = 0; l <= PNG_MAX_CODE_LEN + 1; l++) cursor[l] = offsets[l];
    for (i = 0; i < nsyms; i++) {
        int len = lens[i];
        if (len > 0)
            h->syms[cursor[len]++] = (uint16_t)i;
    }
    return 0;
}

/* Decode one symbol from the bit stream using table h.  Returns
 * the symbol on success, -1 on stream error. */
static int png_huff_decode(struct png_huff *h, struct png_br *br)
{
    /* Walk lengths 1..15 building up the canonical code value
     * one bit at a time.  At each length L the codes are
     * contiguous; we know their starting code (`first`) and the
     * starting symbol-array index (`base`).  If our peeked code
     * is in range we're done; otherwise scale up to the next
     * length and shift in another bit. */
    int code = 0;
    int first = 0;
    int base = 0;
    for (int l = 1; l <= PNG_MAX_CODE_LEN; l++) {
        if (png_br_need(br, 1) < 0) return -1;
        code = (code << 1) | (int)png_br_get(br, 1);
        int cnt = h->counts[l];
        if (code - first < cnt)
            return h->syms[base + (code - first)];
        base  += cnt;
        first  = (first + cnt) << 1;
    }
    PNG_LOG("[png] huff: oversized code\n");
    return -1;
}

/* ============================================================
 *   Internals — RFC 1951 inflate
 *
 *   `out` is a growing byte buffer; the caller knows the final
 *   size (height * (width * bytes_per_pixel + 1)) and pre-
 *   allocates exactly that many bytes.  The deflate stream
 *   should produce that exact count or fewer.
 * ============================================================ */

/* RFC 1951 §3.2.5 length / distance code base + extra-bits
 * tables.  Indexed by symbol - 257 (for length) or symbol - 0
 * (for distance). */
static const uint16_t png_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23,
    27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
    195, 227, 258
};
static const uint8_t png_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t png_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97,
    129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
    3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t png_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* Code-length-code-length permutation (RFC 1951 §3.2.7). */
static const uint8_t png_cl_perm[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Inflate a stored-block (BTYPE=00). */
static int png_inflate_stored(struct png_br *br,
                              uint8_t *out, size_t out_cap, size_t *opos)
{
    png_br_align_byte(br);
    if (br->p + 4 > br->end) {
        PNG_LOG("[png] stored: truncated header\n");
        return -1;
    }
    uint16_t len  = (uint16_t)(br->p[0] | (br->p[1] << 8));
    uint16_t nlen = (uint16_t)(br->p[2] | (br->p[3] << 8));
    br->p += 4;
    br->bitbuf = 0;  br->nbits = 0;
    if ((uint16_t)~len != nlen) {
        PNG_LOG("[png] stored: len/nlen mismatch\n");
        return -1;
    }
    if (br->p + len > br->end) {
        PNG_LOG("[png] stored: truncated payload\n");
        return -1;
    }
    if (*opos + len > out_cap) {
        PNG_LOG("[png] stored: output overrun (%u + %u > %u)\n",
                (unsigned)*opos, (unsigned)len, (unsigned)out_cap);
        return -1;
    }
    for (uint16_t i = 0; i < len; i++) out[(*opos)++] = br->p[i];
    br->p += len;
    return 0;
}

/* Inflate one dynamic or fixed huffman block, given pre-built
 * literal and distance tables. */
static int png_inflate_huff_block(struct png_br *br,
                                  struct png_huff *lit,
                                  struct png_huff *dist,
                                  uint8_t *out, size_t out_cap,
                                  size_t *opos)
{
    for (;;) {
        int sym = png_huff_decode(lit, br);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*opos >= out_cap) {
                PNG_LOG("[png] huff: literal overruns out (%u)\n",
                        (unsigned)out_cap);
                return -1;
            }
            out[(*opos)++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return 0;          /* end of block */
        sym -= 257;
        if (sym >= 29) {
            PNG_LOG("[png] huff: bad length symbol %d\n", sym + 257);
            return -1;
        }
        if (png_br_need(br, png_len_extra[sym]) < 0) return -1;
        uint32_t len_v = png_len_base[sym]
                       + png_br_get(br, png_len_extra[sym]);

        int dsym = png_huff_decode(dist, br);
        if (dsym < 0) return -1;
        if (dsym >= 30) {
            PNG_LOG("[png] huff: bad distance symbol %d\n", dsym);
            return -1;
        }
        if (png_br_need(br, png_dist_extra[dsym]) < 0) return -1;
        uint32_t dist_v = png_dist_base[dsym]
                        + png_br_get(br, png_dist_extra[dsym]);

        if (dist_v == 0 || dist_v > *opos) {
            PNG_LOG("[png] huff: bad backref dist=%u opos=%u\n",
                    (unsigned)dist_v, (unsigned)*opos);
            return -1;
        }
        if (*opos + len_v > out_cap) {
            PNG_LOG("[png] huff: backref overruns out\n");
            return -1;
        }
        /* Byte-by-byte copy, NOT memcpy: backrefs may overlap
         * (RLE-style: dist=1, len=N copies the previous byte N
         * times).  memcpy semantics are undefined for that. */
        for (uint32_t i = 0; i < len_v; i++) {
            out[*opos] = out[*opos - dist_v];
            (*opos)++;
        }
    }
}

/* Build the fixed (BTYPE=01) literal and distance trees. */
static void png_inflate_fixed_tables(struct png_huff *lit,
                                     struct png_huff *dist)
{
    uint8_t lens[288];
    for (int i = 0; i < 144; i++) lens[i] = 8;
    for (int i = 144; i < 256; i++) lens[i] = 9;
    for (int i = 256; i < 280; i++) lens[i] = 7;
    for (int i = 280; i < 288; i++) lens[i] = 8;
    png_huff_build(lit, lens, 288);
    uint8_t dlens[30];
    for (int i = 0; i < 30; i++) dlens[i] = 5;
    png_huff_build(dist, dlens, 30);
}

/* Build the dynamic (BTYPE=10) tables from the bit stream. */
static int png_inflate_dynamic_tables(struct png_br *br,
                                      struct png_huff *lit,
                                      struct png_huff *dist)
{
    if (png_br_need(br, 14) < 0) return -1;
    int hlit  = (int)png_br_get(br, 5) + 257;   /* 257..286 */
    int hdist = (int)png_br_get(br, 5) + 1;     /* 1..32    */
    int hclen = (int)png_br_get(br, 4) + 4;     /* 4..19    */
    if (hlit > 286 || hdist > 30) {
        PNG_LOG("[png] dyn: hlit=%d hdist=%d out of range\n", hlit, hdist);
        return -1;
    }

    /* Step 1: read code-length-code lengths (3 bits each). */
    uint8_t cl_lens[19] = { 0 };
    for (int i = 0; i < hclen; i++) {
        if (png_br_need(br, 3) < 0) return -1;
        cl_lens[png_cl_perm[i]] = (uint8_t)png_br_get(br, 3);
    }
    struct png_huff cl;
    if (png_huff_build(&cl, cl_lens, 19) < 0) return -1;

    /* Step 2: decode `hlit + hdist` lengths using cl.  Symbols
     * 0..15 are literal lengths; 16 = "repeat last 3..6 times";
     * 17 = "0 for 3..10 times"; 18 = "0 for 11..138 times". */
    uint8_t all_lens[286 + 30];
    int total = hlit + hdist;
    int i = 0;
    while (i < total) {
        int sym = png_huff_decode(&cl, br);
        if (sym < 0) return -1;
        if (sym < 16) {
            all_lens[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) {
                PNG_LOG("[png] dyn: rep-last with no last\n");
                return -1;
            }
            if (png_br_need(br, 2) < 0) return -1;
            int rep = (int)png_br_get(br, 2) + 3;
            if (i + rep > total) {
                PNG_LOG("[png] dyn: rep-last overruns\n");
                return -1;
            }
            uint8_t prev = all_lens[i - 1];
            for (int k = 0; k < rep; k++) all_lens[i++] = prev;
        } else if (sym == 17) {
            if (png_br_need(br, 3) < 0) return -1;
            int rep = (int)png_br_get(br, 3) + 3;
            if (i + rep > total) return -1;
            for (int k = 0; k < rep; k++) all_lens[i++] = 0;
        } else if (sym == 18) {
            if (png_br_need(br, 7) < 0) return -1;
            int rep = (int)png_br_get(br, 7) + 11;
            if (i + rep > total) return -1;
            for (int k = 0; k < rep; k++) all_lens[i++] = 0;
        } else {
            PNG_LOG("[png] dyn: bad meta sym %d\n", sym);
            return -1;
        }
    }

    if (png_huff_build(lit,  all_lens,        hlit)  < 0) return -1;
    if (png_huff_build(dist, all_lens + hlit, hdist) < 0) return -1;
    return 0;
}

/* Top-level: inflate the deflate stream `src[0..src_len)` into
 * `out`, expecting EXACTLY `out_cap` bytes (which is what the
 * PNG decoder always passes).  Returns 0 on success. */
static int png_inflate(const uint8_t *src, size_t src_len,
                       uint8_t *out, size_t out_cap)
{
    struct png_br br = { src, src + src_len, 0, 0 };
    size_t opos = 0;

    for (;;) {
        if (png_br_need(&br, 3) < 0) return -1;
        int bfinal = (int)png_br_get(&br, 1);
        int btype  = (int)png_br_get(&br, 2);
        if (btype == 0) {
            if (png_inflate_stored(&br, out, out_cap, &opos) < 0) return -1;
        } else if (btype == 1) {
            struct png_huff lit, dist;
            png_inflate_fixed_tables(&lit, &dist);
            if (png_inflate_huff_block(&br, &lit, &dist, out, out_cap, &opos) < 0)
                return -1;
        } else if (btype == 2) {
            struct png_huff lit, dist;
            if (png_inflate_dynamic_tables(&br, &lit, &dist) < 0) return -1;
            if (png_inflate_huff_block(&br, &lit, &dist, out, out_cap, &opos) < 0)
                return -1;
        } else {
            PNG_LOG("[png] inflate: reserved BTYPE=11\n");
            return -1;
        }
        if (bfinal) break;
    }
    if (opos != out_cap) {
        PNG_LOG("[png] inflate: short stream (got %u of %u)\n",
                (unsigned)opos, (unsigned)out_cap);
        return -1;
    }
    return 0;
}

/* ============================================================
 *   Internals — PNG framing (signature + chunk walk)
 * ============================================================ */

static const uint8_t png_signature[8] = {
    137, 80, 78, 71, 13, 10, 26, 10
};

static inline uint32_t png_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ============================================================
 *   Internals — per-row filter undo (RFC 2083 §6.3)
 * ============================================================ */

static inline uint8_t png_paeth(int a, int b, int c)
{
    int p  = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc)             return (uint8_t)b;
    return (uint8_t)c;
}

/* Extract sample `x` from a packed row at the given bit depth.
 * Samples are packed MSB-first within each byte (PNG spec
 * §2.3 / §7.2).  Valid bit_depth values: 1, 2, 4, 8. */
static inline uint8_t png_unpack_sample(const uint8_t *row,
                                        uint32_t x, int bit_depth)
{
    if (bit_depth == 8) return row[x];
    int spb       = 8 / bit_depth;            /* samples per byte */
    uint32_t byte = x / (uint32_t)spb;
    int sub       = (int)(x - byte * (uint32_t)spb);
    int shift     = 8 - bit_depth * (sub + 1);
    int mask      = (1 << bit_depth) - 1;
    return (uint8_t)((row[byte] >> shift) & mask);
}

/* For grayscale at sub-8-bit depths, scale the raw sample to
 * the full 8-bit display range (PNG spec §13.13).
 *   1-bit: 0→ 0x00, 1→ 0xFF                  (gain 0xFF)
 *   2-bit: sample * 0x55                       (gain 0x55)
 *   4-bit: sample * 0x11                       (gain 0x11)
 *   8-bit: sample (unchanged)                                  */
static inline uint8_t png_scale_gray(uint8_t sample, int bit_depth)
{
    if (bit_depth == 8) return sample;
    if (bit_depth == 4) return (uint8_t)(sample * 0x11);
    if (bit_depth == 2) return (uint8_t)(sample * 0x55);
    if (bit_depth == 1) return sample ? 0xFF : 0x00;
    return sample;
}

/* Undo the filter on row `cur` (of `row_bytes` data bytes,
 * preceded by 1 filter-type byte).  `bpp` is bytes-per-pixel.
 * `prev` is the previous row's data (or NULL on row 0). */
static int png_unfilter_row(uint8_t filt,
                            uint8_t *cur,
                            const uint8_t *prev,
                            int row_bytes, int bpp)
{
    int x;
    switch (filt) {
    case 0:  /* None */
        return 0;
    case 1:  /* Sub: cur[x] += cur[x-bpp] */
        for (x = bpp; x < row_bytes; x++) cur[x] += cur[x - bpp];
        return 0;
    case 2:  /* Up: cur[x] += prev[x] */
        if (prev) for (x = 0; x < row_bytes; x++) cur[x] += prev[x];
        return 0;
    case 3:  /* Average: cur[x] += (cur[x-bpp] + prev[x]) / 2 */
        for (x = 0; x < row_bytes; x++) {
            int a = (x >= bpp)        ? cur[x - bpp] : 0;
            int b = prev              ? prev[x]      : 0;
            cur[x] += (uint8_t)((a + b) >> 1);
        }
        return 0;
    case 4:  /* Paeth */
        for (x = 0; x < row_bytes; x++) {
            int a = (x >= bpp)              ? cur[x - bpp]      : 0;
            int b = prev                    ? prev[x]           : 0;
            int c = (x >= bpp && prev)      ? prev[x - bpp]     : 0;
            cur[x] += png_paeth(a, b, c);
        }
        return 0;
    default:
        PNG_LOG("[png] unfilter: bad filter byte %u\n", filt);
        return -1;
    }
}

/* ============================================================
 *   Public API: png_decode
 * ============================================================ */

static int png_decode(const uint8_t *src, size_t src_len,
                      uint8_t **out_bgra, int *out_w, int *out_h)
{
    *out_bgra = 0;
    *out_w = 0; *out_h = 0;

    if (src_len < 8 + 25) {       /* signature + IHDR (12+13) */
        PNG_LOG("[png] file too short (%u bytes)\n", (unsigned)src_len);
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        if (src[i] != png_signature[i]) {
            PNG_LOG("[png] bad signature at byte %d\n", i);
            return -1;
        }
    }

    /* ---- chunk walker ---- */
    const uint8_t *p   = src + 8;
    const uint8_t *end = src + src_len;

    /* IHDR must be the first chunk. */
    if (p + 8 > end) return -1;
    uint32_t ihdr_len = png_read_be32(p);
    if (ihdr_len != 13 || p[4] != 'I' || p[5] != 'H' ||
        p[6] != 'D' || p[7] != 'R') {
        PNG_LOG("[png] first chunk is not IHDR\n");
        return -1;
    }
    if (p + 8 + 13 + 4 > end) return -1;
    uint32_t w        = png_read_be32(p + 8);
    uint32_t h        = png_read_be32(p + 12);
    uint8_t  bit_depth = p[16];
    uint8_t  color_type = p[17];
    uint8_t  compress   = p[18];
    uint8_t  filter_m   = p[19];
    uint8_t  interlace  = p[20];
    p += 8 + 13 + 4;            /* len + tag + body + crc */

    /* Validate colour type. */
    if (color_type != 0 && color_type != 2 && color_type != 3 &&
        color_type != 4 && color_type != 6) {
        PNG_LOG("[png] unsupported color type %u\n", color_type);
        return -1;
    }
    /* Validate bit depth (varies by colour type per PNG spec table 11.1). */
    {
        int ok = 0;
        if (color_type == 0) ok = (bit_depth == 1 || bit_depth == 2 ||
                                   bit_depth == 4 || bit_depth == 8);
        else if (color_type == 3) ok = (bit_depth == 1 || bit_depth == 2 ||
                                        bit_depth == 4 || bit_depth == 8);
        else /* 2, 4, 6 */     ok = (bit_depth == 8);
        if (!ok) {
            PNG_LOG("[png] unsupported bit depth %u for color type %u\n",
                    bit_depth, color_type);
            return -1;
        }
    }
    if (compress != 0 || filter_m != 0) {
        PNG_LOG("[png] unsupported compression/filter method\n");
        return -1;
    }
    if (interlace != 0) {
        PNG_LOG("[png] interlaced PNGs not supported\n");
        return -1;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        PNG_LOG("[png] implausible dimensions %ux%u\n",
                (unsigned)w, (unsigned)h);
        return -1;
    }

    /* Channels per pixel for each colour type. */
    int channels;
    switch (color_type) {
      case 0: channels = 1; break;  /* grey */
      case 2: channels = 3; break;  /* RGB  */
      case 3: channels = 1; break;  /* idx  */
      case 4: channels = 2; break;  /* GA   */
      case 6: channels = 4; break;  /* RGBA */
      default: return -1;           /* unreachable (validated above) */
    }
    int bits_per_pixel = channels * bit_depth;
    /* For the filter pass, PNG spec defines `bpp` as bits-per-
     * pixel rounded UP to the nearest whole byte (§9.2).  Thus
     * sub-byte depths (1/2/4) all get bpp_filter == 1. */
    int bpp_filter = (bits_per_pixel + 7) / 8;
    if (bpp_filter < 1) bpp_filter = 1;
    /* Bytes of pixel data per row, also rounded up.  Sub-byte
     * rows may have unused trailing bits in the last byte; the
     * decoder reads only the first `w` samples and ignores them. */
    int row_data = (int)(((uint64_t)w * (uint32_t)bits_per_pixel + 7) / 8);
    int row_with_filter = row_data + 1;

    /* ---- accumulate IDAT bodies into one contiguous buffer ---- */
    /* zlib stream: 2 bytes header, deflate payload, 4 bytes adler.
     * We'll skip the header and trailer at decode time.
     *
     * While we're walking we also pick up PLTE and tRNS so we
     * don't have to scan the file a third time.  PNG spec
     * requires PLTE to precede the first IDAT and tRNS to
     * precede the first IDAT as well — we don't enforce that
     * ordering, just record whatever we see. */
    size_t idat_total = 0;
    const uint8_t *plte_p = 0; uint32_t plte_n = 0;
    const uint8_t *trns_p = 0; uint32_t trns_n = 0;

    /* First pass: total IDAT size + locate PLTE / tRNS. */
    {
        const uint8_t *q = p;
        while (q + 8 <= end) {
            uint32_t cl = png_read_be32(q);
            const uint8_t *tag = q + 4;
            if (q + 8 + cl + 4 > end) break;
            if (tag[0] == 'I' && tag[1] == 'D' &&
                tag[2] == 'A' && tag[3] == 'T') {
                idat_total += cl;
            } else if (tag[0] == 'P' && tag[1] == 'L' &&
                       tag[2] == 'T' && tag[3] == 'E') {
                plte_p = q + 8;
                plte_n = cl;
            } else if (tag[0] == 't' && tag[1] == 'R' &&
                       tag[2] == 'N' && tag[3] == 'S') {
                trns_p = q + 8;
                trns_n = cl;
            } else if (tag[0] == 'I' && tag[1] == 'E' &&
                       tag[2] == 'N' && tag[3] == 'D') {
                break;
            }
            q += 8 + cl + 4;
        }
    }
    if (idat_total < 6) {
        PNG_LOG("[png] no IDAT or implausibly tiny (%u bytes)\n",
                (unsigned)idat_total);
        return -1;
    }

    /* Palette images REQUIRE a valid PLTE. */
    uint32_t palette_entries = 0;
    if (color_type == 3) {
        if (!plte_p || plte_n == 0 || plte_n % 3 != 0) {
            PNG_LOG("[png] palette image missing or malformed PLTE\n");
            return -1;
        }
        palette_entries = plte_n / 3;
        if (palette_entries > 256) {
            PNG_LOG("[png] PLTE has too many entries: %u\n",
                    (unsigned)palette_entries);
            return -1;
        }
    }

    /* Decode tRNS by colour type (single-key for 0/2, per-index
     * alpha array for 3).  We capture the raw sample(s) without
     * scaling — the comparison happens against the raw (filtered)
     * sample bytes during the row conversion pass. */
    uint16_t trns_gray   = 0;    int trns_gray_present = 0;
    uint16_t trns_r = 0, trns_g = 0, trns_b = 0;
    int      trns_rgb_present = 0;
    uint8_t  trns_alpha[256];
    uint32_t trns_alpha_n = 0;
    for (int i = 0; i < 256; i++) trns_alpha[i] = 0xFF;
    if (trns_p && trns_n) {
        if (color_type == 0 && trns_n >= 2) {
            trns_gray = (uint16_t)((trns_p[0] << 8) | trns_p[1]);
            trns_gray_present = 1;
        } else if (color_type == 2 && trns_n >= 6) {
            trns_r = (uint16_t)((trns_p[0] << 8) | trns_p[1]);
            trns_g = (uint16_t)((trns_p[2] << 8) | trns_p[3]);
            trns_b = (uint16_t)((trns_p[4] << 8) | trns_p[5]);
            trns_rgb_present = 1;
        } else if (color_type == 3) {
            uint32_t n = trns_n > 256 ? 256 : trns_n;
            for (uint32_t i = 0; i < n; i++) trns_alpha[i] = trns_p[i];
            trns_alpha_n = n;
        }
    }
    uint8_t *zbuf = (uint8_t *)malloc(idat_total);
    if (!zbuf) {
        PNG_LOG("[png] OOM for zbuf %u bytes\n", (unsigned)idat_total);
        return -1;
    }
    /* Second pass: copy IDAT bodies out. */
    {
        size_t off = 0;
        const uint8_t *q = p;
        while (q + 8 <= end) {
            uint32_t cl = png_read_be32(q);
            const uint8_t *tag = q + 4;
            if (q + 8 + cl + 4 > end) break;
            if (tag[0] == 'I' && tag[1] == 'D' &&
                tag[2] == 'A' && tag[3] == 'T') {
                for (uint32_t i = 0; i < cl; i++) zbuf[off++] = q[8 + i];
            } else if (tag[0] == 'I' && tag[1] == 'E' &&
                       tag[2] == 'N' && tag[3] == 'D') {
                break;
            }
            q += 8 + cl + 4;
        }
    }

    /* ---- strip zlib header (2 bytes), ignore adler trailer ---- */
    if (idat_total < 6) { free(zbuf); return -1; }
    const uint8_t *defl_p = zbuf + 2;
    size_t          defl_n = idat_total - 2 - 4;
    /* Sanity: zlib's CMF/CINFO must be 0x78 (deflate, 32 KiB
     * window) for the stuff Pillow / libpng emit; we don't
     * actually USE the value, but log if it's weird. */
    if (zbuf[0] != 0x78 && !PNG_QUIET) {
        printf("[png] note: unusual CMF=0x%02x (proceeding anyway)\n",
               zbuf[0]);
    }

    /* ---- inflate into a row-with-filter buffer ---- */
    size_t raw_cap = (size_t)row_with_filter * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_cap);
    if (!raw) {
        PNG_LOG("[png] OOM for raw %u bytes\n", (unsigned)raw_cap);
        free(zbuf);
        return -1;
    }
    int rc = png_inflate(defl_p, defl_n, raw, raw_cap);
    free(zbuf);
    if (rc < 0) {
        free(raw);
        return -1;
    }

    /* ---- unfilter rows in place + convert to BGRA ---- */
    uint8_t *bgra = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!bgra) {
        PNG_LOG("[png] OOM for bgra %ux%u\n", (unsigned)w, (unsigned)h);
        free(raw);
        return -1;
    }

    /* Holding buffer for "previous row's data" (without filter byte). */
    uint8_t *prev_row_data = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * row_with_filter;
        uint8_t  filt = row[0];
        uint8_t *data = row + 1;
        if (png_unfilter_row(filt, data, prev_row_data,
                             row_data, bpp_filter) < 0) {
            free(raw); free(bgra);
            return -1;
        }
        /* Convert this row to BGRA.  The dispatch is per colour
         * type; sub-byte depths (types 0 and 3 only) take an
         * extra unpack step before the channel shuffle. */
        uint8_t *dst = bgra + (size_t)y * (size_t)w * 4;
        switch (color_type) {
        case 6:  /* RGBA, 8-bit */
            for (uint32_t x = 0; x < w; x++) {
                uint8_t r = data[x * 4 + 0];
                uint8_t g = data[x * 4 + 1];
                uint8_t b = data[x * 4 + 2];
                uint8_t a = data[x * 4 + 3];
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = a;
            }
            break;
        case 2:  /* RGB, 8-bit (with optional single-key tRNS) */
            for (uint32_t x = 0; x < w; x++) {
                uint8_t r = data[x * 3 + 0];
                uint8_t g = data[x * 3 + 1];
                uint8_t b = data[x * 3 + 2];
                uint8_t a = 0xFF;
                if (trns_rgb_present &&
                    r == (uint8_t)trns_r &&
                    g == (uint8_t)trns_g &&
                    b == (uint8_t)trns_b) a = 0;
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = a;
            }
            break;
        case 4:  /* grayscale + alpha, 8-bit */
            for (uint32_t x = 0; x < w; x++) {
                uint8_t v = data[x * 2 + 0];
                uint8_t a = data[x * 2 + 1];
                dst[x * 4 + 0] = v;
                dst[x * 4 + 1] = v;
                dst[x * 4 + 2] = v;
                dst[x * 4 + 3] = a;
            }
            break;
        case 0:  /* grayscale, 1/2/4/8-bit (with optional tRNS) */
            for (uint32_t x = 0; x < w; x++) {
                uint8_t sample = png_unpack_sample(data, x, bit_depth);
                uint8_t a = 0xFF;
                if (trns_gray_present && sample == (uint8_t)trns_gray)
                    a = 0;
                uint8_t v = png_scale_gray(sample, bit_depth);
                dst[x * 4 + 0] = v;
                dst[x * 4 + 1] = v;
                dst[x * 4 + 2] = v;
                dst[x * 4 + 3] = a;
            }
            break;
        case 3:  /* palette, 1/2/4/8-bit (optional per-entry alpha) */
            for (uint32_t x = 0; x < w; x++) {
                uint8_t idx = png_unpack_sample(data, x, bit_depth);
                if ((uint32_t)idx >= palette_entries) {
                    /* Spec violation: index out of range.  Clamp
                     * rather than fail \u2014 some real-world PNGs
                     * have trailing junk indices and most viewers
                     * just paint them as the last palette entry. */
                    idx = (uint8_t)(palette_entries - 1);
                }
                uint8_t r = plte_p[idx * 3 + 0];
                uint8_t g = plte_p[idx * 3 + 1];
                uint8_t b = plte_p[idx * 3 + 2];
                uint8_t a = ((uint32_t)idx < trns_alpha_n)
                          ? trns_alpha[idx] : 0xFF;
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = a;
            }
            break;
        }
        prev_row_data = data;
    }

    free(raw);
    *out_bgra = bgra;
    *out_w = (int)w;
    *out_h = (int)h;
    return 0;
}

#endif /* USER_PNG_H */
