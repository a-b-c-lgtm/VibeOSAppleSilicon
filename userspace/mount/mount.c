/* userspace/mount/mount.c — chapter 132: print the kernel's
 * mount table.
 *
 * Usage:
 *   mount                # one line per mount: <prefix>  [ro]
 *
 * Calls SYS_MOUNTS, which snapshots the kernel's g_mounts[]
 * array (vfs.c).  Each entry has a NUL-terminated prefix and a
 * flags word; we print "ro" for MOUNT_RO and nothing for
 * writable mounts.  Output is intentionally close to Linux
 * /proc/mounts so eyes can scan it the same way.
 *
 * Exits 0 on success, 1 on any kernel error.
 *
 * This is the user-visible payoff for chapter 132's mount-table
 * + fs_ops refactor: before the refactor, the only way to know
 * which filesystems existed was to read the kernel source.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

#define MAX_ENTRIES 16   /* matches MOUNT_MAX in kernel/core/vfs.h */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct mount_info table[MAX_ENTRIES];
    long n = mounts(table, MAX_ENTRIES);
    if (n < 0) {
        printf("mount: kernel returned %d\n", (int)n);
        return 1;
    }
    for (long i = 0; i < n; i++) {
        const char *ro = (table[i].flags & MOUNT_RO) ? "  [ro]" : "";
        printf("%s%s\n", table[i].prefix, ro);
    }
    return 0;
}
