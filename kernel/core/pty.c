/*
 * kernel/core/pty.c — pseudo-terminal implementation.  See pty.h
 * for the design.
 */

#include "pty.h"
#include "pipe.h"
#include "heap.h"
#include "thread.h"
#include <stddef.h>

/* SIGINT signum — must match userspace/libc/syscall.h.  Kept
 * local so pty.c doesn't have to drag in syscall.h. */
#define PTY_SIGINT 2

struct pty *pty_alloc(void)
{
    struct pty *p = (struct pty *)kmalloc(sizeof(struct pty));
    if (!p) return NULL;
    p->m2s    = pipe_alloc();
    p->s2m    = pipe_alloc();
    p->fg_pid = 0;
    p->refs   = 2;        /* master fd + slave fd */
    if (!p->m2s || !p->s2m) {
        /* OOM in either pipe — drop whichever we did get and
         * free the pty.  Each successful pipe_alloc starts at
         * r_refs=1, w_refs=1, so we drop both refs. */
        if (p->m2s) {
            pipe_unref(p->m2s, PIPE_REF_R);
            pipe_unref(p->m2s, PIPE_REF_W);
        }
        if (p->s2m) {
            pipe_unref(p->s2m, PIPE_REF_R);
            pipe_unref(p->s2m, PIPE_REF_W);
        }
        kfree(p);
        return NULL;
    }
    /* pipe_alloc starts each pipe with r_refs = w_refs = 1.
     * Those bootstrap refs map onto the master+slave fds:
     *   m2s.r_refs (1) is held by the slave fd (read of stdin).
     *   m2s.w_refs (1) is held by the master fd (write to stdin).
     *   s2m.r_refs (1) is held by the master fd (read of stdout).
     *   s2m.w_refs (1) is held by the slave fd (write to stdout).
     * pty_close_master / pty_close_slave drop their own halves
     * symmetrically. */
    return p;
}

/* ---------------- master end ---------------- */

long pty_master_read(struct pty *p, void *buf, size_t len)
{
    if (!p || !buf) return 0;
    if (len == 0)   return 0;
    /* Non-blocking by convention: if no data is queued, return
     * 0 immediately rather than blocking the gui_term render
     * loop.  EOF (slave closed) also surfaces as 0; gui_term
     * disambiguates via waitpid(child, ..., WNOHANG). */
    if (p->s2m->count == 0) return 0;
    return pipe_read(p->s2m, buf, len);
}

long pty_master_write(struct pty *p, const void *buf, size_t len)
{
    if (!p || !buf) return 0;
    const uint8_t *src = (const uint8_t *)buf;
    size_t accepted = 0;
    /* Walk byte-by-byte through the line discipline.  Anything
     * we eat (Ctrl-C → SIGINT to fg_pid) is silently dropped.
     * Anything else is forwarded to the slave's stdin pipe. */
    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == 0x03) {
            /* Ctrl-C.  Translate to SIGINT for the foreground
             * pid IF one has been registered; otherwise drop
             * silently (the shell itself doesn't want a literal
             * \x03 in its read buffer either). */
            if (p->fg_pid > 0)
                thread_signal_pid(p->fg_pid, PTY_SIGINT);
            accepted++;
            continue;
        }
        long w = pipe_write(p->m2s, &c, 1);
        if (w <= 0) {
            /* Slave gone or pipe write error.  Report what
             * we've accepted so far. */
            return accepted > 0 ? (long)accepted : w;
        }
        accepted++;
    }
    return (long)accepted;
}

/* ---------------- slave end ---------------- */

long pty_slave_read(struct pty *p, void *buf, size_t len)
{
    if (!p) return 0;
    return pipe_read(p->m2s, buf, len);
}

long pty_slave_write(struct pty *p, const void *buf, size_t len)
{
    if (!p) return 0;
    return pipe_write(p->s2m, buf, len);
}

/* ---------------- close paths ---------------- */

void pty_close_master(struct pty *p)
{
    if (!p) return;
    /* Master is a reader of s2m and a writer of m2s. */
    pipe_unref(p->s2m, PIPE_REF_R);
    pipe_unref(p->m2s, PIPE_REF_W);
    if (--p->refs == 0) kfree(p);
}

void pty_close_slave(struct pty *p)
{
    if (!p) return;
    /* Slave is a reader of m2s and a writer of s2m. */
    pipe_unref(p->m2s, PIPE_REF_R);
    pipe_unref(p->s2m, PIPE_REF_W);
    if (--p->refs == 0) kfree(p);
}
