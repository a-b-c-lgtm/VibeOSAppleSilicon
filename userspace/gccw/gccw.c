/*
 * userspace/gccw/gccw.c — chapter 186 /bin/gcc front-end shim.
 *
 * Ships as /bin/gcc on the guest.  All it does is:
 *
 *   1.  Prepend `-B/bin/` to argv so the xgcc driver finds
 *       cc1, as, and ld at /bin/cc1, /bin/as, /bin/ld.
 *
 *       Without this, the xgcc binary (built on the host with
 *       --prefix=$HOST/build/toolchain) would look for cc1 at
 *       the absolute path it was configured with, which does
 *       not exist on the guest.
 *
 *   2.  Prepend `-isystem /bin` so cc1 finds the libc headers
 *       (stdio.h, string.h, etc.) shipped alongside the binaries
 *       on the OSFS image.  -B does NOT extend cpp's include
 *       search path -- chapter 187's `-B` vs `-L` lesson, but
 *       for the preprocessor side.  Added in chapter 189.
 *
 *   3.  execv("/bin/xgcc", new_argv).  The real driver does the
 *       rest -- argument parsing, spec processing, spawning
 *       cc1/as/ld.
 *
 * Why a separate binary and not a shell script:  the kernel
 * doesn't grok shebang lines (chapter 186's "ship-to-guest
 * blockers" memo).  A 1 KiB C wrapper is the simplest way to
 * inject a fixed argv prefix without bloating xgcc itself.
 *
 * Why not just configure xgcc with --libexecdir=/bin/:  GCC's
 * exec lookup still wraps the path in `gcc/<target>/<version>/`
 * subdirs, which OSFS-1 (flat namespace) can't represent.  The
 * -B prefix bypasses that nesting -- gcc tries the literal
 * prefix + tool name as the first lookup.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"

#define MAX_ARGS 256

static int gccw_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

int main(int argc, char **argv)
{
    if (argc + 4 > MAX_ARGS) {
        printf("gcc: too many arguments (max %d)\n", MAX_ARGS - 4);
        return 1;
    }

    /* Build a new argv that injects "-B/bin/" and "-isystem /bin"
     * right after argv[0].  Placing them first means user-supplied
     * -B / -isystem flags appended later take priority for any
     * tool or header the user wants to override. */
    static char *new_argv[MAX_ARGS];
    int n = 0;
    new_argv[n++] = (char *)"/bin/xgcc";
    new_argv[n++] = (char *)"-B/bin/";
    new_argv[n++] = (char *)"-isystem";
    new_argv[n++] = (char *)"/bin";
    for (int i = 1; i < argc; i++) {
        new_argv[n++] = argv[i];
    }
    new_argv[n] = (void *)0;

    (void)gccw_strlen;  /* unused, kept for future flag parsing */

    int rc = execv("/bin/xgcc", new_argv);
    /* execv only returns on failure. */
    printf("gcc: execv /bin/xgcc failed (rc=%d)\n", rc);
    return 1;
}
