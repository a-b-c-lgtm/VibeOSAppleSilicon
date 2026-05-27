/*
 * userspace/hangfs/hangfs.c — chapter 146 deadline-test daemon.
 *
 * The smallest possible "broken" userfs daemon: it mounts
 * `/hang/` and then never services its request pipe.  Any
 * client that opens a file under /hang/ will sit in
 * `userfs_call` until the kernel's 5 s per-request deadline
 * fires, at which point the call returns -ETIMEDOUT_VFS
 * (= -110) and the channel is marked dead.
 *
 * This is the test fixture for `scripts/test_userfs_timeout.py`.
 * It is deliberately built as a real `/bin/hangfs` binary so
 * the test harness can spawn it the same way it spawns any
 * other daemon — no kernel-internal toggle required.
 *
 * The daemon never replies, so once the deadline fires the
 * mount is permanently broken; the test does not try to use
 * /hang again after the first timeout.  In production this
 * is the desired failure mode: a wedged daemon should not
 * silently corrupt or hide its breakage.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int fds[2];
    long r = mount_kernel("/hang", fds, 0);
    if (r < 0) {
        printf("hangfs: mount_kernel -> %ld\n", r);
        return 1;
    }
    printf("hangfs: mounted /hang (mount_id=%ld), going to sleep\n", r);

    /* Sleep forever in 1 s chunks.  We do NOT close `fds` and
     * we do NOT call userfs_serve — that's the whole point.
     * Any client that opens /hang/<anything> will block on
     * the reply pipe; the kernel's per-request deadline is
     * what unblocks them. */
    for (;;) {
        sleep_ms(1000);
    }
    return 0;
}
