/* userspace/libc/dirent.h -- POSIX opendir / readdir / closedir.
 *
 * Chapter 117 of the book.  Thin wrapper over the existing
 * SYS_LISTDIR_AT primitive (chapter 85): each `DIR *` carries
 * the directory path plus an `int idx` cursor; `readdir` calls
 * `listdir_at(dirpath, idx, ...)` and bumps idx by one each
 * call, returning NULL when the kernel reports past-end.
 *
 * Header-only, single-TU.  No global state.  Each DIR owns a
 * single `struct dirent` whose memory is reused on each
 * `readdir` call (POSIX explicitly allows this -- callers that
 * want to keep the value across the next readdir must copy it).
 *
 * Usage:
 *     #include "libc/dirent.h"
 *
 *     DIR *d = opendir("/data");
 *     if (!d) { perror("opendir"); return 1; }
 *     struct dirent *de;
 *     while ((de = readdir(d)) != NULL) {
 *         printf("%s  type=%u  size=%u\n",
 *                de->d_name, de->d_type, de->d_size);
 *     }
 *     closedir(d);
 */
#ifndef USER_DIRENT_H
#define USER_DIRENT_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "errno.h"

/* Chapter 132f — gcc-tree code (libcpp/system.h) poisons the
 * unprefixed `malloc` / `free` identifiers, so the static-inline
 * opendir/closedir below can't call them directly even from a
 * TU that never executes the inlines.  Use the cstring.c extern
 * wrappers (chapter 131e) whose C-level identifiers carry the
 * `__cstring_` prefix; their __asm__-rename redirects the
 * emitted symbol back to the real malloc/free at link time. */
extern void *__cstring_malloc(size_t) __asm__("malloc");
extern void  __cstring_free(void *) __asm__("free");

#ifndef DIRENT_NAME_MAX
#define DIRENT_NAME_MAX 256
#endif

/* d_type values mirror the LISTDIR_TYPE_* tags from syscall.h
 * so apps don't have to know about both. */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_REG     1   /* matches LISTDIR_TYPE_FILE */
#define DT_DIR     2   /* matches LISTDIR_TYPE_DIR  */
#endif

struct dirent {
    uint32_t d_size;                  /* bytes; 0 for dirs */
    uint32_t d_type;                  /* DT_REG / DT_DIR */
    char     d_name[DIRENT_NAME_MAX];
};

typedef struct DIR {
    char          path[256];
    int           idx;
    struct dirent ent;
} DIR;

static inline DIR *opendir(const char *path)
{
    if (!path || !*path) { errno = 22 /*EINVAL*/; return (DIR *)0; }
    DIR *d = (DIR *)__cstring_malloc(sizeof(DIR));
    if (!d) { errno = 12 /*ENOMEM*/; return (DIR *)0; }
    size_t i = 0;
    while (path[i] && i + 1 < sizeof(d->path)) { d->path[i] = path[i]; i++; }
    d->path[i] = '\0';
    /* Strip trailing slash for the listdir_at call so "/data/"
     * and "/data" behave identically. */
    if (i > 1 && d->path[i - 1] == '/') d->path[i - 1] = '\0';
    d->idx = 0;
    return d;
}

static inline struct dirent *readdir(DIR *d)
{
    if (!d) { errno = 22 /*EINVAL*/; return (struct dirent *)0; }
    uint32_t size = 0, type = 0;
    long n = listdir_at(d->path, d->idx, d->ent.d_name,
                        sizeof(d->ent.d_name), &size, &type);
    if (n < 0) {
        /* End-of-directory is not an error per POSIX -- callers
         * test (readdir == NULL) for both EOF and real failures,
         * disambiguating via errno (set to 0 here). */
        errno = 0;
        return (struct dirent *)0;
    }
    d->ent.d_size = size;
    d->ent.d_type = type;
    d->idx++;
    return &d->ent;
}

static inline int closedir(DIR *d)
{
    if (!d) { errno = 22 /*EINVAL*/; return -1; }
    __cstring_free(d);
    return 0;
}

static inline void rewinddir(DIR *d)
{
    if (d) d->idx = 0;
}

#endif /* USER_DIRENT_H */
