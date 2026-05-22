/*
 * userspace/libc/clipboard.h — chapter 114 file-based client.
 *
 * The clipboard is now a userfs filesystem mounted at
 * /clipboard.  A single file, /clipboard/text, holds the
 * current payload.  Reading is `open + read + close`;
 * writing is `open(O_TRUNC) + write + close`.  No IPC
 * protocol, no MIME tag, no generation counter — the whole
 * point of the chapter-114 port is that `cat`, `echo`, and
 * the shell's `>` redirection just work on the clipboard
 * with no special handling.
 *
 * What this header still defines
 * ------------------------------
 *
 *   CLIP_MOUNT       "/clipboard"
 *   CLIP_TEXT_PATH   "/clipboard/text"
 *   CLIP_DATA_MAX    payload ceiling (32 KiB), enforced by
 *                    clipboardd so a misbehaving client can't
 *                    blow up the daemon
 *   clip_set         convenience: open(O_TRUNC) + write + close
 *   clip_get         convenience: open + read + close
 *   clip_clear       convenience: open(O_TRUNC) + close
 *
 * What this header no longer defines (chapter 108 -> 114)
 * -------------------------------------------------------
 *
 *   struct clip_msg, CLIP_OP_*, CLIP_SOCK_PATH,
 *   CLIP_FLAG_TRUNC, CLIP_ERR_*, CLIP_MIME_*, clip_open,
 *   clip_generation — all gone.  Callers that polled the
 *   generation to spot changes should stat /clipboard/text
 *   instead.
 */
#ifndef USER_CLIPBOARD_H
#define USER_CLIPBOARD_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"

#define CLIP_MOUNT      "/clipboard"
#define CLIP_TEXT_PATH  "/clipboard/text"

/* Payload ceiling.  Sized so a single read() can drain the
 * whole clipboard with a stack-local buffer of reasonable
 * size; clipboardd's static g_data is exactly this big and
 * will short-write at the limit. */
#define CLIP_DATA_MAX   32768

/* O_TRUNC value mirrored from kernel/core/vfs.h.  Kept local
 * so this header has no kernel-include dependency. */
#define _CLIP_O_WRONLY  1
#define _CLIP_O_CREAT   0100   /* 64 */
#define _CLIP_O_TRUNC   01000  /* 512 */

/* Replace the clipboard payload with `data[0..len]`.  Returns
 * the byte count written on success (always == len unless the
 * daemon truncated to CLIP_DATA_MAX), or a negative errno.
 *
 * len=0 with any data pointer is allowed and clears the
 * clipboard — same effect as clip_clear() but in one call. */
static inline int clip_set(const void *data, uint32_t len)
{
    int fd = open(CLIP_TEXT_PATH,
                  _CLIP_O_WRONLY | _CLIP_O_CREAT | _CLIP_O_TRUNC);
    if (fd < 0) return fd;
    long w = 0;
    if (len > 0 && data) {
        w = write(fd, data, len);
    }
    close(fd);
    if (w < 0) return (int)w;
    return (int)w;
}

/* Read the clipboard into `out[0..cap]`.  Writes the byte
 * count to *out_len (capped at cap).  Returns the byte count
 * on success (0 if empty) or a negative errno. */
static inline int clip_get(void *out, uint32_t cap, uint32_t *out_len)
{
    if (out_len) *out_len = 0;
    int fd = open(CLIP_TEXT_PATH, 0 /* O_RDONLY */);
    if (fd < 0) return fd;
    long n = read(fd, out, cap);
    close(fd);
    if (n < 0) return (int)n;
    if (out_len) *out_len = (uint32_t)n;
    return (int)n;
}

/* Wipe the clipboard.  Returns 0 on success or a negative
 * errno. */
static inline int clip_clear(void)
{
    int fd = open(CLIP_TEXT_PATH,
                  _CLIP_O_WRONLY | _CLIP_O_CREAT | _CLIP_O_TRUNC);
    if (fd < 0) return fd;
    close(fd);
    return 0;
}

#endif
