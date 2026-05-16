/* userspace/sync/sync.c — chapter 82 fsync demo / utility.
 *
 * `sync` opens /data/.sync (creating it on first run) and calls
 * fsync(fd) on it.  Because the kernel-side fsync flushes the
 * WHOLE OSFS-2 write-back cache (see kernel/core/osfs2_cache.c),
 * this single fsync is enough to make every previous write
 * durable.  Under-the-hood semantics:
 *
 *   1. open(/data/.sync, O_CREAT|O_WRONLY)  — gets an OSFS-2 fd.
 *   2. fsync(fd)                            — drains the cache.
 *   3. close(fd)                            — releases the fd.
 *
 * The .sync file is an artefact, not a feature \u2014 any OSFS-2 fd
 * works.  We use a known name so multiple `sync` invocations don't
 * accumulate junk in /data/.
 *
 * Exit code: 0 on success, 1 on any failure.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"

/* O_WRONLY (1) | O_CREAT (0100=64) = 65. */
#define OPEN_WRITE_CREATE 65

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int fd = open("/data/.sync", OPEN_WRITE_CREATE);
    if (fd < 0) {
        printf("sync: open /data/.sync failed (%d)\n", fd);
        return 1;
    }

    int rc = fsync(fd);
    close(fd);
    if (rc != 0) {
        printf("sync: fsync failed (%d)\n", rc);
        return 1;
    }
    return 0;
}
