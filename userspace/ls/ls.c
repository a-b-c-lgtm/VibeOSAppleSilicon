/* userspace/ls/ls.c — list files in the FS namespace.
 *
 * Walks SYS_LISTDIR from idx=0 until it gets back -ENOENT, and
 * prints one line per entry.  Output format:
 *
 *      8 bytes  /motd
 *    113 bytes  /mnt/hello.txt
 *      <DIR>    /mnt/
 *
 * Usage:
 *   ls            — full flat dump of every file in the
 *                   namespace (every mount, every depth).
 *                   Useful for debugging.
 *   ls /          — direct children of root.  Files at the
 *                   top level (e.g. /motd) appear by name;
 *                   anything under /mnt/, /data/, /tmp/ is
 *                   collapsed into a single "<DIR>  /mnt/"
 *                   line per mount point.
 *   ls /data/     — direct children of /data/.  Files appear
 *                   directly; deeper subdirectories collapse
 *                   into "<DIR>  /data/sub/" lines.
 *   ls /data      — same as `ls /data/`; the trailing slash
 *                   is implied so `ls /dataX` does NOT match.
 *
 * The kernel's SYS_LISTDIR returns one big flat list spanning
 * every mount.  Both prefix-filter and the directory collapse
 * happen here in userspace; the kernel API stays a simple
 * "give me the next leaf".
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

#define NAME_CAP   128

/* Dedup buffer for synthesized directory entries.  Each name is
 * stored NUL-terminated back-to-back.  Sized to comfortably hold
 * the current set of mount points (/mnt/, /data/, /tmp/) plus
 * room for future per-app subdirs under /data/. */
#define DIRDEDUP_CAP 1024
static char dir_dedup[DIRDEDUP_CAP];
static int  dir_dedup_used = 0;

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

/* Return 1 if `name` was newly added; 0 if already present. */
static int dir_seen(const char *name)
{
    int i = 0;
    while (i < dir_dedup_used) {
        const char *cand = &dir_dedup[i];
        if (str_eq(cand, name)) return 0;
        while (dir_dedup[i]) i++;
        i++;                  /* skip NUL */
    }
    /* Append. */
    int j = 0;
    while (name[j] && dir_dedup_used + j + 1 < DIRDEDUP_CAP) {
        dir_dedup[dir_dedup_used + j] = name[j];
        j++;
    }
    dir_dedup[dir_dedup_used + j] = '\0';
    dir_dedup_used += j + 1;
    return 1;
}

int main(int argc, char **argv)
{
    /* Optional path-prefix.  We normalise argv[1] into prefix_buf
     * with a guaranteed trailing '/' so `ls /data` and `ls /data/`
     * behave identically and `ls /dataX` does NOT spuriously match
     * /data*.  When a prefix is present, ls switches to "direct
     * children only" mode (see header comment). */
    char   prefix_buf[NAME_CAP];
    size_t prefix_len = 0;
    int    have_prefix = 0;

    if (argc >= 2 && argv[1] && argv[1][0]) {
        size_t i = 0;
        while (argv[1][i] && i + 2 < sizeof(prefix_buf)) {
            prefix_buf[i] = argv[1][i];
            i++;
        }
        if (i > 0 && prefix_buf[i - 1] != '/')
            prefix_buf[i++] = '/';
        prefix_buf[i] = '\0';
        prefix_len = i;
        have_prefix = 1;       /* `/` counts as a real prefix now;
                                * we use it to decide between flat
                                * and directory-aware mode. */
    }

    /* Chapter 85 \u2014 if the prefix is anywhere under /data/, switch
     * to listdir_at mode so we get type tags (file vs dir) for
     * every leaf and don't have to rely on the kernel's flat
     * listdir collapsing nested children we have no way of
     * recursing into.  This is also the *only* way to enumerate
     * subdirectories like /data/notes/ \u2014 the flat listdir only
     * walks the OSFS-2 root. */
    int use_at = 0;
    if (have_prefix) {
        const char *p = prefix_buf;
        if (p[0] == '/' && p[1] == 'd' && p[2] == 'a' && p[3] == 't' &&
            p[4] == 'a' && (p[5] == '/' || p[5] == '\0'))
            use_at = 1;
        /* Chapter 99 — /proc is also per-path enumeration only
         * (it doesn't appear in the flat namespace), so route it
         * the same way as /data. */
        else if (p[0] == '/' && p[1] == 'p' && p[2] == 'r' &&
                 p[3] == 'o' && p[4] == 'c' &&
                 (p[5] == '/' || p[5] == '\0'))
            use_at = 1;
    }

    char         name[NAME_CAP];
    unsigned int size = 0;
    unsigned int type = 0;
    int          listed = 0;

    if (use_at) {
        /* Strip trailing '/' for the listdir_at call so "/data/"
         * becomes "/data" \u2014 the kernel happily accepts either
         * but rejects extra slashes mid-path. */
        char dirpath[NAME_CAP];
        size_t i = 0;
        while (i < prefix_len && i + 1 < sizeof(dirpath)) {
            dirpath[i] = prefix_buf[i];
            i++;
        }
        if (i > 1 && dirpath[i - 1] == '/') i--;
        dirpath[i] = '\0';

        for (int idx = 0;; idx++) {
            long n = listdir_at(dirpath, idx, name, sizeof(name),
                                &size, &type);
            if (n < 0) break;
            if (type == LISTDIR_TYPE_DIR) {
                /* Render "<DIR>  /data/sub/". */
                char display[NAME_CAP];
                size_t di = 0;
                for (size_t k = 0; k < prefix_len && di + 2 < sizeof(display); k++)
                    display[di++] = prefix_buf[k];
                for (size_t k = 0; name[k] && di + 2 < sizeof(display); k++)
                    display[di++] = name[k];
                display[di++] = '/';
                display[di]   = '\0';
                printf("  %8s  %s\n", "<DIR>", display);
            } else {
                char display[NAME_CAP];
                size_t di = 0;
                for (size_t k = 0; k < prefix_len && di + 1 < sizeof(display); k++)
                    display[di++] = prefix_buf[k];
                for (size_t k = 0; name[k] && di + 1 < sizeof(display); k++)
                    display[di++] = name[k];
                display[di] = '\0';
                printf("  %8u  %s\n", size, display);
            }
            listed++;
        }
        if (listed == 0) printf("ls: no files in %s\n", prefix_buf);
        return 0;
    }

    for (int idx = 0;; idx++) {
        long n = listdir(idx, name, sizeof(name), &size);
        if (n < 0) break;     /* end of namespace OR error */

        if (!have_prefix) {
            /* No filter — preserve the historical "dump
             * everything" behaviour bare `ls` has had since
             * milestone 20. */
            printf("  %8u  %s\n", size, name);
            listed++;
            continue;
        }

        /* Prefix filter. */
        int match = 1;
        for (size_t k = 0; k < prefix_len; k++) {
            if (name[k] != prefix_buf[k]) { match = 0; break; }
        }
        if (!match) continue;

        /* Direct-child check.  If the tail (everything after the
         * prefix) contains a '/', the entry lives in a deeper
         * subdirectory.  Synthesize a single "<DIR>" line for
         * the immediate subdir and dedupe. */
        const char *tail = name + prefix_len;
        size_t      slash = 0;
        while (tail[slash] && tail[slash] != '/') slash++;

        if (tail[slash] == '/') {
            /* Build the synthesized directory name:
             * prefix + first_segment + '/'. */
            char dir[NAME_CAP];
            size_t di = 0;
            for (size_t k = 0; k < prefix_len && di + 2 < sizeof(dir); k++)
                dir[di++] = prefix_buf[k];
            for (size_t k = 0; k < slash && di + 2 < sizeof(dir); k++)
                dir[di++] = tail[k];
            dir[di++] = '/';
            dir[di]   = '\0';
            if (dir_seen(dir)) {
                printf("  %8s  %s\n", "<DIR>", dir);
                listed++;
            }
        } else {
            /* Direct file under the prefix. */
            printf("  %8u  %s\n", size, name);
            listed++;
        }
    }

    if (listed == 0) {
        if (have_prefix) printf("ls: no files in %s\n", prefix_buf);
        else             printf("ls: no files\n");
    }
    return 0;
}
