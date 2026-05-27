/*
 * kernel/core/srv.c — chapter 112 named-IPC service bus.
 *
 * See srv.h for the public contract and design notes.
 *
 * Implementation shape: the registry is a fixed-size array of
 * SRV_MAX_LISTENERS slots (compile-time bound matches the
 * chapter-spec "16 services is plenty for v1").  Lookup,
 * bind, and unbind walk the array linearly.  At 16 slots that
 * is one cache line of work, which is far cheaper than the
 * hash-table machinery a tree would buy us.
 *
 * Concurrency: cooperative scheduler (single CPU runqueue,
 * yield-only preemption today), so we don't need locks around
 * the listener table beyond the implicit "I'm not yielding
 * inside a critical section" discipline that the rest of the
 * kernel already follows.  The blocking primitives
 * (thread_block_on / thread_wake_blocked) handle the only
 * cross-thread coordination we care about — handoff from
 * srv_connect to srv_accept and from srv_write to srv_read.
 */

#include "srv.h"
#include "vfs.h"
#include "heap.h"
#include "thread.h"
#include "serial.h"
#include "uaccess.h"
#include <stddef.h>

/* EPIPE / EMSGSIZE come from srv.h.  ENOMEM/EAGAIN are local
 * aliases kept here for clarity even though the only ENOMEM
 * paths today route through ENOMEM_VFS from vfs.h. */
#define EAGAIN     11   /* unused today but reserved    */

/* ---- internal helpers ------------------------------------- */

static struct srv_listen g_listeners[SRV_MAX_LISTENERS];

static int strn_eq(const char *a, const char *b, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    /* Both ran past `cap` with all-equal — treat as equal. */
    return 1;
}

static size_t str_len_cap(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static void str_copy_cap(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Look up a listener by exact path match.  Returns NULL if
 * no such name is bound (or it's a freed slot). */
static struct srv_listen *find_listener(const char *path)
{
    for (uint32_t i = 0; i < SRV_MAX_LISTENERS; i++) {
        struct srv_listen *ls = &g_listeners[i];
        if (!ls->in_use) continue;
        if (strn_eq(ls->name, path, SRV_NAME_MAX)) return ls;
    }
    return NULL;
}

/* "/srv/" + at least one byte, no NULs above SRV_NAME_MAX,
 * no slashes inside the service name (so "/srv/foo/bar" is
 * rejected — keeps the registry flat). */
static int validate_srv_path(const char *path)
{
    if (!path) return 0;
    if (path[0] != '/' || path[1] != 's' || path[2] != 'r' ||
        path[3] != 'v' || path[4] != '/') return 0;
    if (path[5] == '\0') return 0;
    for (size_t i = 5; i < SRV_NAME_MAX; i++) {
        char c = path[i];
        if (c == '\0') return 1;
        if (c == '/')  return 0;
    }
    /* No NUL inside SRV_NAME_MAX. */
    return 0;
}

/* ---- queue helpers ---------------------------------------- */

static void queue_init(struct srv_queue *q)
{
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

static void queue_push(struct srv_queue *q, struct srv_msg *m)
{
    m->next = NULL;
    if (q->tail) q->tail->next = m;
    else         q->head       = m;
    q->tail = m;
    q->count++;
}

static struct srv_msg *queue_pop(struct srv_queue *q)
{
    struct srv_msg *m = q->head;
    if (!m) return NULL;
    q->head = m->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    return m;
}

static void queue_drain(struct srv_queue *q)
{
    struct srv_msg *m = q->head;
    while (m) {
        struct srv_msg *next = m->next;
        kfree(m);
        m = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
}

/* ---- public API ------------------------------------------- */

struct srv_listen *srv_bind(const char *path, int *err)
{
    if (err) *err = 0;
    if (!validate_srv_path(path)) {
        if (err) *err = -EINVAL_VFS;
        return NULL;
    }
    if (find_listener(path)) {
        if (err) *err = -EADDRINUSE;
        return NULL;
    }
    for (uint32_t i = 0; i < SRV_MAX_LISTENERS; i++) {
        struct srv_listen *ls = &g_listeners[i];
        if (ls->in_use) continue;
        /* Take the slot. */
        ls->in_use = 1;
        ls->refs   = 1;
        str_copy_cap(ls->name, path, SRV_NAME_MAX);
        struct thread *me = thread_current();
        ls->owner_pid = me ? (uint32_t)me->id : 0;
        ls->pending_head = NULL;
        ls->pending_tail = NULL;
        ls->pending_count = 0;
        return ls;
    }
    if (err) *err = -ENOMEM_VFS;
    return NULL;
}

void srv_unref_listen(struct srv_listen *ls)
{
    if (!ls) return;
    if (ls->refs > 0) ls->refs--;
    if (ls->refs > 0) return;

    /* Last listener fd gone.  Reject every still-pending
     * client by marking its conn's service side closed;
     * the client's next read returns 0, its next write
     * returns -EPIPE, and once it closes, the conn frees
     * itself.  We also drop the pending node. */
    struct srv_pending *p = ls->pending_head;
    while (p) {
        struct srv_pending *next = p->next;
        if (p->conn) {
            p->conn->service_open = 0;
            /* Wake the client thread blocked in srv_connect. */
            thread_wake_blocked(ls);
            thread_wake_blocked(p->conn);
        }
        kfree(p);
        p = next;
    }
    ls->pending_head = ls->pending_tail = NULL;
    ls->pending_count = 0;
    ls->in_use = 0;
    ls->name[0] = '\0';
}

struct srv_conn *srv_accept(struct srv_listen *ls, int *err)
{
    if (err) *err = 0;
    if (!ls || !ls->in_use) { if (err) *err = -EBADF; return NULL; }

    for (;;) {
        if (ls->pending_head) {
            struct srv_pending *p = ls->pending_head;
            ls->pending_head = p->next;
            if (!ls->pending_head) ls->pending_tail = NULL;
            ls->pending_count--;
            struct srv_conn *c = p->conn;
            kfree(p);
            if (!c) continue;
            /* Mark service side live, stamp our pid, wake the
             * waiting client so srv_connect can return. */
            c->service_open = 1;
            struct thread *me = thread_current();
            c->peer_pid_s = me ? (uint32_t)me->id : 0;
            thread_wake_blocked(c);
            return c;
        }
        /* Empty pending queue — block on the listen object. */
        thread_block_on(ls);
        struct thread *me = thread_current();
        if (me && me->sig_pending) {
            if (err) *err = -EINTR;
            return NULL;
        }
        /* Loop and re-check.  If srv_unref_listen ran, ls is
         * no longer in_use; bail. */
        if (!ls->in_use) {
            if (err) *err = -EBADF;
            return NULL;
        }
    }
}

struct srv_conn *srv_connect(const char *path, int *err)
{
    if (err) *err = 0;
    if (!validate_srv_path(path)) {
        if (err) *err = -EINVAL_VFS;
        return NULL;
    }
    struct srv_listen *ls = find_listener(path);
    if (!ls) { if (err) *err = -ENOENT_VFS; return NULL; }

    if (ls->pending_count >= SRV_PENDING_MAX) {
        /* Backlog full.  We could block here instead, but
         * for v1 the caller gets -ENOMEM and can retry —
         * mirrors the way TCP listen overflow surfaces. */
        if (err) *err = -ENOMEM_VFS;
        return NULL;
    }

    struct srv_conn *c = (struct srv_conn *)kmalloc(sizeof(*c));
    if (!c) { if (err) *err = -ENOMEM_VFS; return NULL; }
    queue_init(&c->c2s);
    queue_init(&c->s2c);
    c->client_open  = 1;
    c->service_open = 0;          /* set by srv_accept */
    struct thread *me = thread_current();
    c->peer_pid_c = me ? (uint32_t)me->id : 0;
    c->peer_pid_s = 0;

    struct srv_pending *p = (struct srv_pending *)kmalloc(sizeof(*p));
    if (!p) {
        kfree(c);
        if (err) *err = -ENOMEM_VFS;
        return NULL;
    }
    p->next = NULL;
    p->conn = c;

    /* Enqueue on the listener's pending queue and wake one
     * waiter.  thread_wake_blocked wakes every blocked
     * thread on the token; that's fine — the first to run
     * grabs the head, the rest re-block. */
    if (ls->pending_tail) ls->pending_tail->next = p;
    else                  ls->pending_head       = p;
    ls->pending_tail = p;
    ls->pending_count++;
    thread_wake_blocked(ls);

    /* Block until accept stamps service_open = 1.  If the
     * service vanishes (srv_unref_listen flips
     * service_open back to 0 from un-accepted), bail with
     * -EPIPE. */
    while (!c->service_open) {
        thread_block_on(c);
        struct thread *me2 = thread_current();
        if (me2 && me2->sig_pending) {
            /* Caller interrupted.  We can't easily yank the
             * pending node now (it might already have been
             * accepted in the window between wake and us
             * running again).  Conservative: detach the
             * client side; if service hasn't accepted yet,
             * the service's accept loop will see the conn
             * already client-closed and discard it.  If it
             * has accepted, the service will see EOF on next
             * read. */
            c->client_open = 0;
            if (!c->service_open) {
                /* Try to find and remove the pending node.
                 * O(n) walk over a queue of at most
                 * SRV_PENDING_MAX entries. */
                struct srv_pending *prev = NULL;
                struct srv_pending *cur  = ls->pending_head;
                while (cur) {
                    if (cur->conn == c) {
                        if (prev) prev->next = cur->next;
                        else      ls->pending_head = cur->next;
                        if (ls->pending_tail == cur)
                            ls->pending_tail = prev;
                        ls->pending_count--;
                        kfree(cur);
                        kfree(c);
                        if (err) *err = -EINTR;
                        return NULL;
                    }
                    prev = cur;
                    cur = cur->next;
                }
                /* Conn already dequeued by accept but hasn't
                 * flipped service_open yet?  Very tight
                 * race — fall through and let the service
                 * see the half-closed conn on its first
                 * read. */
            }
            if (err) *err = -EINTR;
            return NULL;
        }
        /* If the listener went away mid-connect, surface
         * that as -EPIPE — handshake half-completed. */
        if (!ls->in_use && !c->service_open) {
            c->client_open = 0;
            kfree(c);
            if (err) *err = -EPIPE;
            return NULL;
        }
    }
    return c;
}

void srv_unref_conn(struct srv_conn *c, int is_service_end)
{
    if (!c) return;
    if (is_service_end) c->service_open = 0;
    else                c->client_open  = 0;

    /* Wake any blocked peer so they observe the closed
     * flag and return 0 (read) or -EPIPE (write). */
    thread_wake_blocked(c);

    if (!c->client_open && !c->service_open) {
        queue_drain(&c->c2s);
        queue_drain(&c->s2c);
        kfree(c);
    }
}

long srv_read(struct srv_conn *c, int is_service_end, void *buf, size_t len)
{
    if (!c) return -EBADF;
    if (!buf && len > 0) return -EINVAL_VFS;

    struct srv_queue *q = is_service_end ? &c->c2s : &c->s2c;
    int *peer_open = is_service_end ? &c->client_open : &c->service_open;

    for (;;) {
        if (q->count > 0) {
            struct srv_msg *m = q->head;
            if (m->len > len) {
                /* Caller's buffer too small.  Per spec, do
                 * NOT consume the message — let the caller
                 * retry with a bigger buffer.  Return the
                 * negative error so they can also peek the
                 * needed length via a subsequent attempt
                 * with a 0-length read returning EMSGSIZE. */
                return -EMSGSIZE;
            }
            (void)queue_pop(q);
            /* Chapter 117 — copy through copy_to_user
             * so the destination's pages get pre-faulted (lazy
             * anon mmap) and COW-broken before the bytes land.
             * The previous byte-by-byte memcpy panicked at EL1
             * whenever `buf` happened to point at an unmapped
             * or COW-shared page (common for wsd worker thread
             * stacks and forked gui_term post-fork reads). */
            long n = (long)m->len;
            int rc = copy_to_user((uint64_t)(uintptr_t)buf,
                                  m->data, m->len);
            kfree(m);
            /* Drained one slot — wake a blocked writer. */
            thread_wake_blocked(c);
            if (rc < 0) return -EFAULT;
            return n;
        }
        if (!*peer_open) return 0;   /* EOF */
        thread_block_on(c);
        struct thread *me = thread_current();
        if (me && me->sig_pending) return -EINTR;
    }
}

long srv_write(struct srv_conn *c, int is_service_end, const void *buf, size_t len)
{
    if (!c) return -EBADF;
    if (!buf && len > 0) return -EINVAL_VFS;
    if (len > SRV_MSG_MAX) return -EMSGSIZE;

    struct srv_queue *q = is_service_end ? &c->s2c : &c->c2s;
    int *peer_open = is_service_end ? &c->client_open : &c->service_open;

    for (;;) {
        if (!*peer_open) return -EPIPE;
        if (q->count < SRV_QUEUE_MSGS) {
            struct srv_msg *m = (struct srv_msg *)
                kmalloc(sizeof(struct srv_msg) + len);
            if (!m) return -ENOMEM_VFS;
            m->next = NULL;
            m->len  = (uint32_t)len;
            /* `buf` here is a KERNEL pointer: sys_write for
             * FD_SRV_CONN already copy_from_user'd the user
             * payload into a kheap buffer and is calling us
             * with that kheap pointer.  A plain byte-by-byte
             * copy is the right shape (no user-mode page faults
             * possible). */
            const uint8_t *src = (const uint8_t *)buf;
            for (uint32_t i = 0; i < (uint32_t)len; i++) m->data[i] = src[i];
            queue_push(q, m);
            /* Wake a blocked reader. */
            thread_wake_blocked(c);
            return (long)len;
        }
        /* Per-direction queue full.  Block until reader
         * drains a slot. */
        thread_block_on(c);
        struct thread *me = thread_current();
        if (me && me->sig_pending) return -EINTR;
    }
}

int srv_list(char (*names_out)[SRV_NAME_MAX], int cap)
{
    if (!names_out || cap <= 0) return 0;
    int n = 0;
    for (uint32_t i = 0; i < SRV_MAX_LISTENERS && n < cap; i++) {
        if (!g_listeners[i].in_use) continue;
        size_t L = str_len_cap(g_listeners[i].name, SRV_NAME_MAX - 1);
        for (size_t j = 0; j < L; j++) names_out[n][j] = g_listeners[i].name[j];
        names_out[n][L] = '\0';
        n++;
    }
    return n;
}
