/*
 * kernel/core/pipe.h — anonymous in-memory pipes.
 *
 * A pipe is a fixed-size byte ring buffer (PIPE_BUF_SIZE) with
 * two fd kinds attached: FD_PIPE_R and FD_PIPE_W.  Each fd
 * holds a reference; closing the last reader (r_refs == 0)
 * causes the next pipe_write to fail with -EPIPE; closing the
 * last writer (w_refs == 0) causes pipe_read to return 0
 * (EOF) once the buffer drains.
 *
 * Blocking semantics:
 *   - pipe_read with empty buffer + writers present blocks
 *     the caller via thread_sleep_until (uses the same
 *     THREAD_SLEEPING infrastructure as sleep_ms).
 *   - pipe_write with full buffer + readers present blocks
 *     similarly.
 *   - When a reader produces space (drains some bytes), it
 *     wakes any writer waiting on this pipe.
 *   - When a writer adds bytes, it wakes any reader.
 *
 * No SMP locks (single CPU); IRQs are masked through the
 * read/write critical regions just enough to keep the wake
 * walks consistent.
 */

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>

#define PIPE_BUF_SIZE 4096u

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;       /* next byte to read */
    uint32_t tail;       /* next byte to write */
    uint32_t count;      /* bytes currently in the ring */
    int      r_refs;     /* number of FD_PIPE_R fds pointing here */
    int      w_refs;     /* number of FD_PIPE_W fds pointing here */
};

/* Allocate a pipe with both refcounts initialised to 1.  The
 * caller (sys_pipe) installs the matching fds.  Returns NULL on
 * OOM. */
struct pipe *pipe_alloc(void);

/* Drop a reference of the named kind (FD_PIPE_R or FD_PIPE_W).
 * Wakes any threads blocked on the now-impossible operation
 * (readers when no writers remain; writers when no readers
 * remain).  Frees the pipe when both refcounts reach zero. */
enum pipe_ref { PIPE_REF_R, PIPE_REF_W };
void pipe_unref(struct pipe *p, enum pipe_ref which);

/* Read up to `len` bytes from `p` into `buf`.  Blocks if empty
 * and writers exist; returns 0 (EOF) if empty and no writers
 * remain.  Returns bytes read, or a negative errno. */
long pipe_read(struct pipe *p, void *buf, size_t len);

/* Write up to `len` bytes from `buf` to `p`.  Blocks if full
 * and readers exist; returns -EPIPE if no readers remain.
 * Returns bytes written, or a negative errno.  Short writes
 * are possible if the buffer fills mid-write — the caller
 * should loop. */
long pipe_write(struct pipe *p, const void *buf, size_t len);

#endif /* PIPE_H */
