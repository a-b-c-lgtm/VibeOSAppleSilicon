/*
 * kernel/core/pty.h — minimal pseudo-terminal: two pipes plus a
 * foreground-pid field.
 *
 * Chapter 79: gui_term spawns a real /bin/sh and routes
 * keystrokes through a pty rather than synthesising "one
 * command per Enter" itself.  A pty here is the smallest thing
 * that earns the name:
 *
 *   - master end (held by gui_term):
 *       read  : drains the slave-to-master ring (non-blocking
 *               by convention; returns 0 if no data is ready).
 *       write : enqueues bytes to the master-to-slave ring,
 *               first scanning each byte for a control sequence.
 *               byte 0x03 (Ctrl-C) AND fg_pid > 0 → deliver
 *               SIGINT to fg_pid and DROP the byte.
 *
 *   - slave end (held by the shell as fd 0/1/2):
 *       read  : drains the master-to-slave ring (blocking,
 *               normal pipe semantics).
 *       write : enqueues bytes to the slave-to-master ring.
 *
 *   - fg_pid: the pid that should receive SIGINT when Ctrl-C
 *     arrives at the master.  Updated by sys_set_fg_pid() when
 *     the calling thread's fd 0 is a pty slave (auto-routed
 *     so that /bin/sh works unmodified in both serial-console
 *     and gui_term contexts).
 *
 * No SIGTSTP / SIGSTOP / SIGCONT yet — those land with chapter
 * 79's job-control work and will be added to the master's
 * line-discipline scanner at the same time.
 *
 * Refcounting: each fd that points at the pty (master_r,
 * master_w, slave_r, slave_w) is an independent reference on
 * the underlying pipes; the pty struct itself is freed only
 * once both pipes have dropped to zero r_refs + w_refs.  In
 * practice the master holds 1 r_ref + 1 w_ref on each pipe and
 * the slave holds 1 r_ref + 1 w_ref on each pipe — closing all
 * four fds frees the pty.
 */

#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include <stddef.h>

struct pipe;

struct pty {
    struct pipe *m2s;     /* master-to-slave (shell stdin) */
    struct pipe *s2m;     /* slave-to-master (shell stdout) */
    int          fg_pid;  /* pid Ctrl-C signals; 0 = none */
    int          refs;    /* outstanding fd references; 2 at alloc
                           * (master + slave); each pty_close_*
                           * decrements; pty kfree'd at zero. */
};

/* Allocate a pty with both pipes initialised.  Returns NULL on
 * OOM.  The caller is responsible for installing fds and
 * dropping the pty's initial pipe references via pty_close_*. */
struct pty *pty_alloc(void);

/* Read from the master end (drains s2m).  Non-blocking by
 * convention: returns 0 if no data is available, otherwise the
 * number of bytes copied (up to len).  Returns 0 (EOF) only when
 * the slave has been closed AND the ring is empty. */
long pty_master_read(struct pty *p, void *buf, size_t len);

/* Write to the master end (enqueues to m2s after scanning for
 * control bytes).  Returns the number of bytes accepted (which
 * may be less than len if Ctrl-C bytes were eaten as signals). */
long pty_master_write(struct pty *p, const void *buf, size_t len);

/* Read from the slave end (drains m2s).  Blocking, matches
 * pipe_read semantics. */
long pty_slave_read(struct pty *p, void *buf, size_t len);

/* Write from the slave end (enqueues to s2m).  Blocking. */
long pty_slave_write(struct pty *p, const void *buf, size_t len);

/* Drop the master end's references on both pipes.  Call once
 * when the master fd table entries are released. */
void pty_close_master(struct pty *p);

/* Drop the slave end's references on both pipes.  Call once
 * when the slave fd table entries are released. */
void pty_close_slave(struct pty *p);

#endif /* PTY_H */
