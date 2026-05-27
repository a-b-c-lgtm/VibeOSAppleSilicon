/* userspace/rm/rm.c — POSIX-ish file remover.
 *
 * Removes each named path via SYS_UNLINK (libc wrapper).  Supports
 * a single flag, `-f`, which silences "no such file" errors — that
 * is what /bin/make's standard `rm -f $(OUTPUT)` clean recipes
 * need.  Multi-letter flag clusters like `-rf` are accepted but
 * only the `f` bit means anything today; no directory recursion
 * exists in the kernel yet, so any other letter is silently
 * ignored to keep host-authored Makefile recipes portable.
 *
 * Exit status:
 *   0 — every named file was removed (or absent under -f).
 *   1 — at least one file could not be removed, or no operand
 *       was supplied without -f.
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"

int main(int argc, char **argv)
{
    int force      = 0;
    int any_err    = 0;
    int any_target = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] == '\0') continue;

        if (a[0] == '-' && a[1] != '\0') {
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'f') force = 1;
            }
            continue;
        }

        any_target = 1;
        if (unlink(a) < 0) {
            if (force && errno == ENOENT) continue;
            printf("rm: cannot remove '%s': %s\n", a, strerror(errno));
            any_err = 1;
        }
    }

    if (!any_target && !force) {
        printf("rm: missing operand\n");
        return 1;
    }
    return any_err ? 1 : 0;
}
