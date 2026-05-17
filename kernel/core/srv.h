/*
 * kernel/core/srv.h — chapter 107 named-IPC service bus.
 *
 * A `/srv/<name>` endpoint is the cross-process equivalent of
 * a TCP listening socket: a long-running service `bind`s a
 * path, clients `connect` to that path, the kernel hands the
 * service one fresh fd per connection.  The wire format is
 * length-prefixed datagrams (not a byte stream): every
 * `write` enqueues exactly one message, every `read` returns
 * exactly one message — or -EMSGSIZE if the caller's buffer is
 * too small to hold the next message.
 *
 * Why a new module instead of reusing pipe.c?  Pipes are
 * unidirectional byte streams with no framing; we need
 * bidirectional message-shaped delivery so apps don't have
 * to invent length prefixes themselves.  And the listening
 * side is a registry keyed by path, which pipes don't have.
 *
 * Lifetime / refcounting follows the pipe.c shape:
 *   - `struct srv_listen` is allocated by srv_bind and freed
 *     when the listener fd is closed (refcount hits zero).
 *     Pending connections that hadn't yet been accepted are
 *     dropped (their conn objects are unrefed from the listen
 *     side; the client side returns -EPIPE on its next op).
 *   - `struct srv_conn` is allocated by srv_connect.  Both
 *     endpoints hold one refcount; close on either side drops
 *     a ref, last close kfree's.
 *
 * Blocking discipline mirrors pipe.c:
 *   - srv_accept blocks on the listen object via
 *     thread_block_on(listen); srv_connect wakes via
 *     thread_wake_blocked(listen).
 *   - srv_read blocks on the conn (peer-write side) when the
 *     queue is empty; srv_write wakes via
 *     thread_wake_blocked(conn).
 *   - srv_write blocks on the conn (own-write side) when the
 *     queue is full; srv_read wakes the conn after draining.
 *   - All blocking loops check thread_current()->sig_pending
 *     and bail with -EINTR, so Ctrl-C aborts a stuck service.
 *
 * Caps:
 *   - SRV_MAX_LISTENERS — total registered services. 16 is
 *     plenty for v1 (clipboard, mixer, future TLS proxy,
 *     filesystem servers).
 *   - SRV_NAME_MAX — including the "/srv/" prefix.
 *   - SRV_MSG_MAX — per-message length cap. 64 KiB matches
 *     the chapter spec.
 *   - SRV_QUEUE_MSGS — how many in-flight messages a single
 *     direction can hold before write blocks.
 *   - SRV_PENDING_MAX — backlog of un-accepted connections.
 */

#ifndef SRV_H
#define SRV_H

#include <stdint.h>
#include <stddef.h>

#define SRV_NAME_MAX      64u
#define SRV_MAX_LISTENERS 16u
#define SRV_MSG_MAX       (64u * 1024u)
#define SRV_QUEUE_MSGS    8u
#define SRV_PENDING_MAX   4u

/* POSIX-shaped errnos introduced by this module.  Mirrors what
 * pipe.c keeps local; promoted to the header so the syscall
 * dispatcher (which also returns -EPIPE / -EMSGSIZE) can share
 * the numbers without duplicating them. */
#ifndef EPIPE
#define EPIPE     32
#endif
#ifndef EMSGSIZE
#define EMSGSIZE  90
#endif

/* One in-flight message in either direction of a connected
 * fd.  Allocated on enqueue, freed on dequeue.  Keeping
 * messages on the heap (not in a fixed ring) avoids paying
 * the SRV_MSG_MAX cost for every slot — the per-direction
 * cap is messages, not bytes. */
struct srv_msg {
    struct srv_msg *next;
    uint32_t        len;
    /* len bytes follow inline via flexible array — alloc as
     * sizeof(struct srv_msg) + len. */
    uint8_t         data[];
};

/* One direction of a connected fd.  Datagrams arrive at the
 * tail and depart from the head; `count` tracks how many
 * are queued so srv_write can apply backpressure (block at
 * SRV_QUEUE_MSGS).  No need for a separate "is anyone
 * waiting" flag — thread_wake_blocked on the conn pointer
 * is cheap whether or not anyone was listening. */
struct srv_queue {
    struct srv_msg *head;
    struct srv_msg *tail;
    uint32_t        count;
};

/* A connected fd.  Two queues — client-to-service and
 * service-to-client — plus a refcount and a closed flag.
 *
 * `peer_closed` is set when either side closes; the other
 * side's next read returns 0 (EOF) after draining whatever
 * was already queued, and its next write returns -EPIPE.
 * Refcount goes to zero only when both fds have been
 * closed; the last side kfree's the conn and all queued
 * messages.
 *
 * The "which end am I" question is answered by which queue
 * a given fd reads from / writes to.  FD_SRV_CONN slots
 * allocated by srv_connect (the client side) read from
 * `s2c` and write to `c2s`; slots allocated by srv_accept
 * (the service side) do the reverse.  The fd kind alone
 * isn't enough to tell them apart, so each fd carries an
 * `is_service_end` bit (see vfs.h fd_entry.srv_is_service). */
struct srv_conn {
    struct srv_queue c2s;          /* client  -> service */
    struct srv_queue s2c;          /* service -> client  */
    int              client_open;  /* nonzero while client fd live  */
    int              service_open; /* nonzero while service fd live */
    uint32_t         peer_pid_c;   /* client pid at connect time   */
    uint32_t         peer_pid_s;   /* service pid at accept time   */
};

/* A bound /srv/<name>.  Owns a queue of pending connections
 * that haven't been accepted yet (clients block in connect
 * until the service drains them).  `name` includes the
 * leading "/srv/" prefix exactly as it was bound, so
 * srv_connect path matching is a single strcmp. */
struct srv_pending {
    struct srv_pending *next;
    struct srv_conn    *conn;
};

struct srv_listen {
    char                name[SRV_NAME_MAX];
    int                 in_use;
    int                 refs;       /* listener fd count (0 = free)  */
    uint32_t            owner_pid;  /* pid that bound this name      */
    struct srv_pending *pending_head;
    struct srv_pending *pending_tail;
    uint32_t            pending_count;
};

/* Bind `/srv/<name>` to the calling thread.  `name` MUST
 * begin with "/srv/" and have at least one non-empty
 * component after it.  Returns a pointer to the listen
 * object (refcount = 1) on success, or NULL with *err set
 * to a negative errno on failure.
 *
 *   -EINVAL   bad path shape
 *   -EADDRINUSE  another service already owns this name
 *   -ENOMEM   table full or OOM */
struct srv_listen *srv_bind(const char *path, int *err);

/* Drop a reference on the listen object.  Called from
 * vfs_close when the listener fd goes away.  When the last
 * ref drops, any pending un-accepted conns get their
 * service side marked closed (so the client's next op
 * returns -EPIPE), then the slot is freed. */
void srv_unref_listen(struct srv_listen *ls);

/* Block until a pending connection is available, then
 * return its conn pointer (with service_open = 1).  On
 * -EINTR returns NULL with *err = -EINTR. */
struct srv_conn *srv_accept(struct srv_listen *ls, int *err);

/* Connect to `/srv/<name>`.  Allocates a fresh conn,
 * queues it on the listen object's pending queue, wakes
 * the listener, and blocks until the service accepts.
 * Returns the conn pointer on success (client_open = 1).
 *
 *   -ENOENT   no such service bound
 *   -ENOMEM   pending queue full or OOM
 *   -EINTR    caller got a signal
 *   -EPIPE    service vanished mid-handshake */
struct srv_conn *srv_connect(const char *path, int *err);

/* Drop a reference from one end of a connected conn.
 * `is_service_end` distinguishes the two FDs that share
 * the conn.  Wakes the peer so a blocked read returns 0
 * or a blocked write returns -EPIPE.  Last close kfree's
 * the conn and drains any queued messages. */
void srv_unref_conn(struct srv_conn *c, int is_service_end);

/* Read one datagram into `buf`.  `is_service_end` picks
 * which queue we read from.  Returns the message length
 * on success, 0 on EOF (peer closed and queue drained),
 * -EMSGSIZE if the caller's buffer is too small (the
 * message is NOT consumed in that case), -EINTR on
 * signal, or -EBADF if the conn is already torn down. */
long srv_read(struct srv_conn *c, int is_service_end, void *buf, size_t len);

/* Write one datagram from `buf`.  Length-prefixed inside
 * the conn — each call is exactly one message.  Returns
 * the message length on success, -EMSGSIZE if `len`
 * exceeds SRV_MSG_MAX, -EPIPE if the peer has gone away,
 * -EINTR on signal, -ENOMEM if the per-direction queue is
 * at SRV_QUEUE_MSGS and we got woken without space (we
 * loop until either space appears or peer closes; this
 * branch is for OOM allocating the message). */
long srv_write(struct srv_conn *c, int is_service_end, const void *buf, size_t len);

/* List the currently bound services into `names_out` (one
 * name per element, each up to SRV_NAME_MAX bytes
 * including NUL).  Returns the number written, capped by
 * `cap`.  Used by /bin/srvls and (later) by procfs. */
int srv_list(char (*names_out)[SRV_NAME_MAX], int cap);

#endif /* SRV_H */
