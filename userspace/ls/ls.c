/* userspace/ls/ls.c — list files in the FS namespace.
 *
 * Two modes, kept for backward compatibility with the rest of
 * the shell:
 *
 *   ls            — flat dump of every file the kernel's
 *                   SYS_LISTDIR fold knows about (every mount,
 *                   every depth).  Useful for debugging.
 *
 *   ls /path      — POSIX-shaped directory listing:
 *                     • `stat(path)` to confirm it's a directory
 *                       (file paths print just the one file).
 *                     • `opendir` + `readdir` to walk children.
 *                     • `<DIR>` marker for d_type==DT_DIR,
 *                       byte size for d_type==DT_REG.
 *
 * Chapter 153 port — the previous implementation used
 * SYS_LISTDIR_AT directly with prefix-filtering and a
 * dir-dedup buffer.  All of that is now subsumed by the
 * `opendir`/`readdir` wrapper in libc/dirent.h, which means
 * `ls /data/notes/` works automatically (the kernel handles
 * subdirectory enumeration; we just iterate the cursor).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/sys/stat.h"
#include "../libc/dirent.h"
#include "../libc/errno.h"

#define NAME_CAP 128

static size_t s_copy_normalised(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    /* Strip trailing '/' unless the whole path is just "/" so that
     * "/data/" and "/data" produce identical output.  opendir()
     * also strips, but we need the stripped form for our own
     * display logic too. */
    if (i > 1 && dst[i - 1] == '/') { dst[i - 1] = '\0'; i--; }
    return i;
}

static void s_render_dir_entry(const char *parent, size_t plen,
                               const struct dirent *de)
{
    char display[NAME_CAP];
    size_t di = 0;
    /* parent + '/' + de->d_name, with an extra trailing '/'
     * when de is a directory itself.  Skip the joining '/'
     * when parent is already "/". */
    int parent_is_root = (plen == 1 && parent[0] == '/');
    if (!parent_is_root) {
        for (size_t k = 0; k < plen && di + 3 < sizeof(display); k++)
            display[di++] = parent[k];
    }
    display[di++] = '/';
    for (size_t k = 0; de->d_name[k] && di + 2 < sizeof(display); k++)
        display[di++] = de->d_name[k];
    if (de->d_type == DT_DIR) display[di++] = '/';
    display[di] = '\0';

    if (de->d_type == DT_DIR)
        printf("  %8s  %s\n", "<DIR>", display);
    else
        printf("  %8u  %s\n", de->d_size, display);
}

int main(int argc, char **argv)
{
    /* Bare `ls`: preserve the historical flat-dump that has
     * been the dev/debug interface since the early listdir chapter.  This
     * uses the kernel's SYS_LISTDIR which folds every mount
     * into a single linear cursor — opendir/readdir is the
     * wrong tool for "show me everything everywhere". */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        char         name[NAME_CAP];
        unsigned int size = 0;
        int          listed = 0;
        for (int idx = 0;; idx++) {
            long n = listdir(idx, name, sizeof(name), &size);
            if (n < 0) break;
            printf("  %8u  %s\n", size, name);
            listed++;
        }
        if (listed == 0) printf("ls: no files\n");
        return 0;
    }

    /* `ls /path` — POSIX path. */
    char path[NAME_CAP];
    size_t plen = s_copy_normalised(path, sizeof(path), argv[1]);

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("ls: %s: ", path);
        if (errno == 2) printf("no such file or directory\n");
        else if (errno == 13) printf("permission denied\n");
        else printf("errno=%d\n", errno);
        return 1;
    }

    /* If it's a regular file, print just that one entry. */
    if (S_ISREG(st.st_mode)) {
        printf("  %8u  %s\n", (unsigned)st.st_size, path);
        return 0;
    }

    /* Otherwise enumerate.  We trust S_ISDIR / any-other-type
     * to land in opendir, which will return NULL for sockets /
     * fifos / etc. with a meaningful errno. */
    DIR *d = opendir(path);
    if (!d) {
        printf("ls: %s: cannot open directory (errno=%d)\n", path, errno);
        return 1;
    }
    int listed = 0;
    struct dirent *de;
    while ((de = readdir(d)) != (struct dirent *)0) {
        s_render_dir_entry(path, plen, de);
        listed++;
    }
    closedir(d);
    if (listed == 0) printf("ls: %s: directory is empty\n", path);
    return 0;
}
