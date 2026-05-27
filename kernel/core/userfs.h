/*
 * kernel/core/userfs.h — userspace filesystem servers (chapter 140).
 *
 * A userfs mount delegates every fs_ops method to a userspace
 * daemon over a pair of anonymous pipes carrying a simplified
 * 9P-shaped RPC.  The kernel half lives in this file and
 * userfs.c; the userspace half lives in userspace/libfs/.
 *
 * The wire format is a fixed 32-byte struct p9_msg header
 * followed by op-specific variable payload bytes (paths for
 * lookups, data for read/write, names for listdir).
 *
 * Per-mount concurrency is serialised: each userfs_channel has
 * its own spinlock and at most one request is in flight at a
 * time.  This keeps the protocol stateless (no tag-keyed reply
 * demultiplexer in v1) at the cost of pipelining; chapter 140's
 * step-6 hardening pass can add a per-channel wait queue keyed
 * by tag once we have a workload that needs it.
 *
 * Channels are created via SYS_MOUNT (which also allocates the
 * two pipes and registers a fresh entry in the VFS mount table)
 * and torn down via SYS_UMOUNT.  Closing a mount that still has
 * open file descriptors against it returns -EBUSY_VFS; closing a
 * mount whose daemon has died is the normal cleanup path.
 */
#ifndef USERFS_H
#define USERFS_H

#include <stdint.h>
#include <stddef.h>
#include "../arch/spinlock.h"
#include "vfs.h"

/* Forward decl. */
struct pipe;

/* 9P-shaped operation codes.  Request side: low byte values.
 * Reply side: same op with P9_REPLY bit ORed in.  The reply's
 * `status` field carries 0 on success or -errno on failure. */
#define P9_OP_OPEN    1u
#define P9_OP_READ    2u
#define P9_OP_WRITE   3u
#define P9_OP_CLOSE   4u
#define P9_OP_LISTDIR 5u
#define P9_OP_UNLINK  6u
#define P9_OP_MKDIR   7u
#define P9_OP_IS_DIR  8u
#define P9_OP_LOAD    9u
#define P9_REPLY      0x80000000u

/* On-wire message header.  Fixed 32 bytes; little-endian.
 * Variable-length payload (path / data / name) follows
 * immediately, of `length` bytes. */
struct p9_msg {
    uint32_t op;        /* P9_OP_* or P9_OP_* | P9_REPLY */
    uint32_t tag;       /* request id, echoed by reply */
    uint32_t handle;    /* per-mount handle; 0 for path-keyed ops */
    uint32_t flags;     /* open flags / listdir type / is_dir result */
    uint64_t offset;    /* read / write offset */
    uint32_t length;    /* payload bytes that follow this header */
    int32_t  status;    /* 0 in requests; 0 or -errno in replies */
};

/* Per-mount channel.  Owned by the entry in the VFS mount table
 * via the cookie pointer.  Freed by sys_umount once no open fds
 * reference it.
 *
 * `in_flight` serialises concurrent userfs_call invocations
 * onto a single channel.  Earlier revisions used a spinlock_t
 * here, but holding a busy-spin lock across the blocking pipe
 * read for the daemon's reply starved the daemon (the spinning
 * second caller's CPU never picked up the daemon, and the
 * busy-spin disables our cooperative scheduling).  The wait-
 * queue form (block on the address of `in_flight`, woken by
 * thread_wake_blocked once the holder clears it) lets the
 * scheduler run the daemon while contenders sleep cheaply. */
struct userfs_channel {
    struct pipe       *req_pipe;     /* kernel writes here; daemon reads */
    struct pipe       *rsp_pipe;     /* daemon writes here; kernel reads */
    int                owner_pid;    /* serving daemon's pid; 0 = unset */
    int                in_flight;    /* 1 = an RPC is mid-round-trip */
    uint32_t           next_tag;     /* monotonic per channel */
    int                alive;        /* set to 0 on EPIPE / umount */
    int                open_fds;     /* number of FD_USERFS_FILE entries */
    const char        *prefix;       /* duplicate of mount->prefix; logging */
};

/* Cap on the payload bytes a single op may carry.  Matches the
 * libfs daemon side; both halves must agree.  Sized to one
 * pipe-buffer for v1 so reads/writes don't block on partial
 * pipe drains. */
#define P9_MAX_PAYLOAD 2048u

/* Allocate + initialise a fresh channel.  Returns NULL on OOM.
 * Both pipes are heap-allocated and their refcounts begin at 1
 * for the kernel-internal end; the daemon-side fds are created
 * by sys_mount, which bumps the other side's refcount. */
struct userfs_channel *userfs_channel_create(const char *prefix,
                                             int owner_pid);

/* Drop the kernel-internal references on the channel's pipes
 * and free the channel struct itself.  Safe to call only once
 * `open_fds` has dropped to zero and the mount-table entry has
 * been removed. */
void userfs_channel_destroy(struct userfs_channel *c);

/* Mount-table-facing vtable.  cookie is a struct userfs_channel *. */
extern const struct fs_ops g_userfs_ops;

/* Write the request half of a p9_msg to the channel and read the
 * reply.  Returns 0 on success and copies the reply header into
 * *rsp_out plus up to `cap` payload bytes into rsp_payload.  On
 * a protocol error or dead channel returns a negative errno.
 * `rsp_payload` may be NULL only if cap is 0; the function will
 * drain any payload the daemon sent regardless. */
int userfs_call(struct userfs_channel *c,
                uint32_t op, uint32_t handle, uint32_t flags,
                uint64_t offset,
                const void *req_payload, uint32_t req_len,
                struct p9_msg *rsp_out,
                void *rsp_payload, uint32_t cap);

#endif /* USERFS_H */
