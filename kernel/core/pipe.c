/*
 * kernel/core/pipe.c — anonymous in-memory pipe implementation.
 *
 * See pipe.h for design.  The ring buffer uses head/tail/count
 * (rather than head/tail with a "empty vs full" sentinel) so we
 * can use the full PIPE_BUF_SIZE without losing a byte.
 */

#include "pipe.h"
#include "heap.h"
#include "thread.h"
#include "vfs.h"
#include "serial.h"
#include <stddef.h>

#define EPIPE  32   /* matches typical POSIX EPIPE for "no readers" */
#define EINTR_PIPE 4    /* mirror of vfs.h EINTR; kept local so pipe.c
                         * doesn't have to drag in vfs.h's full set */

struct pipe *pipe_alloc(void)
{
    struct pipe *p = (struct pipe *)kmalloc(sizeof(struct pipe));
    if (!p) return NULL;
    p->head   = 0;
    p->tail   = 0;
    p->count  = 0;
    p->r_refs = 1;
    p->w_refs = 1;
    return p;
}

void pipe_unref(struct pipe *p, enum pipe_ref which)
{
    if (!p) return;
    if (which == PIPE_REF_R) {
        if (p->r_refs > 0) p->r_refs--;
        /* No more readers — wake any blocked writers so they
         * notice and bail with -EPIPE. */
        if (p->r_refs == 0) thread_wake_blocked(p);
    } else {
        if (p->w_refs > 0) p->w_refs--;
        /* No more writers — wake any blocked readers so they
         * notice empty + no writers and return EOF (0). */
        if (p->w_refs == 0) thread_wake_blocked(p);
    }
    if (p->r_refs == 0 && p->w_refs == 0)
        kfree(p);
}

long pipe_read(struct pipe *p, void *buf, size_t len)
{
    if (!p || !buf) return -EINVAL_VFS;
    if (len == 0) return 0;

    uint8_t *dst = (uint8_t *)buf;

    while (p->count == 0) {
        if (p->w_refs == 0) return 0;   /* EOF — no more writers */
        thread_block_on(p);
        /* Chapter 79b — thread_signal_pid wakes blocked threads.
         * If we got woken by a signal (rather than by a writer),
         * bail out with -EINTR so the dispatcher's pre-eret
         * sig_pending check fires.  This is what lets gui_term's
         * Ctrl-C interrupt a shell sitting in read(). */
        struct thread *me = thread_current();
        if (me && me->sig_pending) return -EINTR_PIPE;
    }

    /* Drain up to min(len, count) bytes. */
    size_t n = (len < p->count) ? len : p->count;
    for (size_t i = 0; i < n; i++) {
        dst[i] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count -= (uint32_t)n;

    /* If we drained any bytes, a blocked writer might now have
     * room.  Wake everyone blocked on this pipe; the writers
     * will retry pipe_write and any (extant) readers will see
     * count == 0 again and re-block. */
    thread_wake_blocked(p);

    return (long)n;
}

long pipe_write(struct pipe *p, const void *buf, size_t len)
{
    if (!p || !buf) return -EINVAL_VFS;
    if (len == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;

    while (written < len) {
        if (p->r_refs == 0) {
            /* No more readers.  POSIX raises SIGPIPE; we just
             * return -EPIPE.  If we already wrote some bytes,
             * report them. */
            return written > 0 ? (long)written : -EPIPE;
        }
        if (p->count == PIPE_BUF_SIZE) {
            thread_block_on(p);
            continue;
        }
        size_t can = PIPE_BUF_SIZE - p->count;
        size_t want = len - written;
        if (want > can) want = can;
        for (size_t i = 0; i < want; i++) {
            p->buf[p->tail] = src[written + i];
            p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        }
        p->count += (uint32_t)want;
        written += want;
        /* Wake any blocked reader. */
        thread_wake_blocked(p);
    }
    return (long)written;
}
