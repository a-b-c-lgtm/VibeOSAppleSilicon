/*
 * userspace/libc/font_proto.h — chapter 115 wire protocol for
 * the userspace font server.
 *
 * Shared between three callers:
 *
 *   - userspace/fontd/fontd.c        — service side; binds
 *                                       /srv/font and answers
 *                                       requests.
 *   - kernel/core/wm_font.c          — kernel-side WM client;
 *                                       holds one long-lived
 *                                       conn so wm_draw_text /
 *                                       wm_measure_text get
 *                                       glyph metrics + alpha
 *                                       bitmaps without owning
 *                                       a rasteriser.
 *   - userspace/libgui/text.h        — future client lib for
 *                                       chapter 116 (apps that
 *                                       have mapped their own
 *                                       pixel buffer per ch108a
 *                                       and want to draw text
 *                                       without a syscall).
 *
 * Wire shape: one chapter-107 IPC datagram = one request OR
 * one reply.  Header is fixed-size; bitmap reply piggybacks
 * the alpha bytes immediately after the header (one byte per
 * pixel, `bmp_w * bmp_h` bytes total).
 *
 * The protocol is request/reply, ONE outstanding per conn.
 * Clients are expected to serialise their own use of a conn
 * (the WM-side client does this with a coarse mutex; future
 * libgui callers can either do the same or open multiple
 * conns).
 *
 * The bitmap mode is "inline bytes in the reply" rather than
 * the "shared mmap'd glyph page" sketch in the book's chapter
 * stub.  Inline-bytes is the simpler v1: glyphs are ~hundreds
 * of bytes each, the protocol cap is 64 KiB, and we already
 * pay one round-trip per cache miss anyway.  Mmap'd glyph
 * pages are an optimisation; defer to a later chapter if
 * profiling ever shows they matter.
 */

#ifndef LIBC_FONT_PROTO_H
#define LIBC_FONT_PROTO_H

#include <stdint.h>

/* Bound /srv/<name> for the font daemon.  Stable across all
 * clients; if we ever ship per-user font servers we'll add a
 * suffix here. */
#define FONT_SOCK_PATH      "/srv/font"

/* Default font id.  Clients that don't care about font
 * selection just pass FONT_ID_DEFAULT and get DejaVu Sans
 * @ the default pixel size.  Allocated dynamically per
 * FONT_OP_OPEN; reserved low ids never appear from OPEN. */
#define FONT_ID_DEFAULT     1u

/* Default pixel size.  Matches what the chapter-102 kernel
 * rasteriser shipped, so apps that don't pass a size get
 * identical output to before the move. */
#define FONT_SIZE_DEFAULT   16u

/* Op codes.  Stable wire numbers — adding a new op gets a
 * new number, never reuses an old one. */
enum font_op {
    FONT_OP_GLYPH    = 1,   /* request: rasterise glyph; reply: metrics + alpha bytes */
    FONT_OP_METRICS  = 2,   /* request: just metrics; reply: metrics, no bitmap     */
    FONT_OP_OPEN     = 3,   /* future: map a font-name to a font_id (v1: hardcoded) */
    FONT_OP_HEALTH   = 4,   /* request: are you alive?; reply: bytes processed       */
    FONT_OP_ERR      = 99,  /* reply only: status field carries the negative errno   */
};

/* Status codes carried in the reply's `status` field.
 * Always negative when an error; 0 on success. */
#define FONT_OK             0
#define FONT_ERR_NOFONT     (-1)
#define FONT_ERR_NOGLYPH    (-2)
#define FONT_ERR_OOM        (-3)
#define FONT_ERR_PROTO      (-4)
#define FONT_ERR_NOSIZE     (-5)

/* Fixed-size message header.  The same struct is used in
 * both directions — request fills op/font_id/codepoint/
 * size_px; reply fills status + the metric fields + the
 * bmp_w/bmp_h size of the trailing payload.
 *
 * Sized at 32 bytes so a string of 50 small-bitmap glyphs
 * still fits in one chapter-107 datagram (50 * (32 + ~200)
 * = ~12 KiB; cap is 64 KiB). */
struct font_msg {
    uint16_t op;            /* enum font_op                         */
    uint16_t flags;         /* reserved; 0 today                    */
    uint32_t font_id;       /* FONT_ID_DEFAULT or a value from OPEN */
    uint32_t codepoint;     /* unicode scalar value                 */
    uint16_t size_px;       /* requested pixel size                 */
    int16_t  status;        /* reply only: FONT_OK or negative      */

    /* Metric fields — populated on FONT_OP_GLYPH / METRICS reply. */
    int16_t  left_bearing;  /* px from pen origin to bitmap left    */
    int16_t  top_bearing;   /* px from baseline up to bitmap top    */
    uint16_t advance;       /* px to advance pen after this glyph   */

    /* Bitmap dims — populated on FONT_OP_GLYPH reply.  Zero
     * for whitespace and unknown glyphs (the reply still
     * carries a valid `advance` so layout works). */
    uint16_t bmp_w;
    uint16_t bmp_h;
    uint16_t _pad;          /* keep struct a multiple of 8 bytes    */
};

/* Bytes after the header on a FONT_OP_GLYPH reply.  Caller
 * must check that the IPC `read` returned at least
 * sizeof(struct font_msg) + bmp_w * bmp_h bytes. */
#define FONT_REPLY_BMP_BYTES(msg)   ((uint32_t)(msg).bmp_w * (uint32_t)(msg).bmp_h)

#endif /* LIBC_FONT_PROTO_H */
