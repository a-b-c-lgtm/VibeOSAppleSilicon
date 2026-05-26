/* userspace/libc/stdio.h — header-only buffered FILE * for osdev
 * (chapter 116b).
 *
 * Single-translation-unit pattern, same as printf.h and malloc.h:
 * include from one .c per binary.  Every binary that links this
 * header gets its own copy of the formatter and its own private
 * stdin/stdout/stderr globals.  Since every binary in-tree is a
 * single .c file, "per-binary singletons" == "per-process
 * singletons" — POSIX-shaped from the caller's perspective.
 *
 * Implemented:
 *   FILE, fopen, fdopen, fclose, fread, fwrite, fgetc, fputc,
 *   fgets, fputs, getc, putc, getchar, putchar, puts (overrides
 *   the printf.h one), ungetc, fflush, fseek, ftell, rewind,
 *   feof, ferror, clearerr, fileno, fprintf, vfprintf.
 *
 *   stdin / stdout / stderr  -- three statically-allocated FILE
 *   globals bound to fds 0/1/2 on first use.
 *
 *   Buffering modes:
 *     stdin   : _IOLBF (4 KiB)
 *     stdout  : _IOLBF (4 KiB)
 *     stderr  : _IONBF (no buffer)
 *     fopen   : _IOFBF (4 KiB)
 *
 *   setvbuf is provided as a "set mode and buffer size" hook;
 *   passing NULL gives the FILE a fresh malloc-owned buffer.
 *
 * NOT implemented in this chapter:
 *   - fscanf  (deferred to 116d alongside scanf)
 *   - tmpfile, tmpnam, freopen
 *   - thread safety (the FILE list is a single-process structure;
 *     two threads racing on the same FILE * are undefined --
 *     fix once 116d ports the locking story)
 *
 * Dependencies (each MUST be included before this header):
 *   - syscall.h   (read/write/open/close/lseek/exit)
 *   - errno.h     (errno macro)
 *   - malloc.h    (FILE buffers come from the heap)
 *   - printf.h    (fprintf piggy-backs on the _fmt formatter)
 *
 * Reading flow:
 *   - The FILE struct keeps ONE 4 KiB buffer that flips between
 *     "read direction" and "write direction".  Switching
 *     directions transparently fflushes (which for a read FILE
 *     means lseek back to compensate for any prefetched bytes
 *     the caller never consumed -- POSIX C99 7.21.5.2).
 *   - Open FILEs live on a singly-linked list rooted at the
 *     static `_io_open_head` so fflush(NULL) can walk them at
 *     program-exit time.  exit() does NOT auto-flush yet -- the
 *     fclose-or-explicit-fflush rule is on the caller.
 */
#ifndef USER_STDIO_H
#define USER_STDIO_H

/* Chapter 132e: also expose the canonical glibc-style guard so
 * third-party headers that probe `#ifdef _STDIO_H` to test
 * "stdio.h was included" detect us correctly.  gmp.h's
 * `_GMP_H_HAVE_FILE` heuristic (gmp-h.in line ~252) only fires
 * when one of `_STDIO_H` / `_FILE_DEFINED` / ... is set; without
 * it the FILE * prototype for mpz_inp_str_nowhite is omitted and
 * mpz/inp_str.c fails to build.  Define both common forms.  */
#ifndef _STDIO_H
#define _STDIO_H 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "syscall.h"
#include "errno.h"
#include "malloc.h"
#include "printf.h"

#ifndef BUFSIZ
#define BUFSIZ 4096
#endif

#ifndef EOF
#define EOF (-1)
#endif

#define _IONBF 0
#define _IOLBF 1
#define _IOFBF 2

/* FILE flag bits.  Pack into one int so the struct stays compact. */
enum {
    _IO_FILE_READ    = 1 << 0,
    _IO_FILE_WRITE   = 1 << 1,
    _IO_FILE_APPEND  = 1 << 2,
    _IO_FILE_EOF     = 1 << 3,
    _IO_FILE_ERR     = 1 << 4,
    _IO_FILE_OWNS_BUF= 1 << 5,
    _IO_FILE_OWNS_FD = 1 << 6,
};

/* Direction of last accessed I/O.  Switching mid-stream triggers
 * a flush + seek-back so the underlying fd's position stays
 * coherent with what the caller has logically consumed. */
enum {
    _IO_DIR_NONE  = 0,
    _IO_DIR_READ  = 1,
    _IO_DIR_WRITE = 2,
};

typedef struct _IO_FILE {
    int             fd;
    int             mode;       /* _IONBF / _IOLBF / _IOFBF      */
    int             flags;      /* _IO_FILE_* bitmap             */
    int             dir;        /* _IO_DIR_*; last access dir    */
    int             ungot;      /* ungetc one-char pushback, -1  */
    unsigned char  *buf;
    size_t          bufsize;
    size_t          bufpos;     /* next write pos OR next read pos */
    size_t          bufend;     /* in read dir: end of valid data; in write dir == bufpos */
    struct _IO_FILE *next;      /* open-FILE list link           */
} FILE;

/* --- Open-FILE list ------------------------------------------------- */

static inline FILE **_io_open_head(void)
{
    static FILE *g_head = (FILE *)0;
    return &g_head;
}

static inline void _io_list_add(FILE *f)
{
    FILE **h = _io_open_head();
    f->next = *h;
    *h = f;
}

static inline void _io_list_remove(FILE *f)
{
    FILE **h = _io_open_head();
    FILE  *p = *h, *prev = (FILE *)0;
    while (p) {
        if (p == f) {
            if (prev) prev->next = p->next;
            else      *h = p->next;
            p->next = (FILE *)0;
            return;
        }
        prev = p;
        p = p->next;
    }
}

/* --- stdin / stdout / stderr ---------------------------------------- */
/*
 * Lazy init: the FILE struct is zero-initialised at program start
 * (kernel hands out zeroed pages -> .bss is zero -> the static
 * structs below default to all-zero).  The first call into stdio
 * that touches one of them populates fd/mode/buf via _io_init_std.
 */

static inline FILE *_io_stdin(void);
static inline FILE *_io_stdout(void);
static inline FILE *_io_stderr(void);

#define stdin  (_io_stdin())
#define stdout (_io_stdout())
#define stderr (_io_stderr())

/* --- Forward decls so direction-switch can call fflush ------------- */

static inline int fflush(FILE *f);

/* --- Internal: switch direction (flush + maybe seek-back) ---------- */

static inline int _io_switch_dir(FILE *f, int wanted)
{
    if (f->dir == wanted || f->dir == _IO_DIR_NONE) {
        f->dir = wanted;
        return 0;
    }
    /* We were reading, now want to write: discard the prefetch
     * and lseek the fd back to where the caller logically is.
     * We were writing, now want to read: drain the buffer to the
     * fd. */
    int r = fflush(f);
    f->dir = wanted;
    return r;
}

/* --- fread/fwrite/fflush core --------------------------------------- */

static inline int _io_refill(FILE *f)
{
    if (!(f->flags & _IO_FILE_READ)) {
        f->flags |= _IO_FILE_ERR;
        return -1;
    }
    if (_io_switch_dir(f, _IO_DIR_READ) < 0) return -1;
    f->bufpos = f->bufend = 0;
    long n = read(f->fd, f->buf, f->bufsize);
    if (n == 0) {
        f->flags |= _IO_FILE_EOF;
        return -1;
    }
    if (n < 0) {
        f->flags |= _IO_FILE_ERR;
        return -1;
    }
    f->bufend = (size_t)n;
    return 0;
}

static inline int _io_flush_write(FILE *f)
{
    if (f->bufpos == 0) return 0;
    size_t off = 0;
    while (off < f->bufpos) {
        long n = write(f->fd, f->buf + off, f->bufpos - off);
        if (n <= 0) {
            f->flags |= _IO_FILE_ERR;
            return -1;
        }
        off += (size_t)n;
    }
    f->bufpos = f->bufend = 0;
    return 0;
}

static inline int fflush(FILE *f)
{
    if (f == (FILE *)0) {
        /* fflush(NULL): walk every open FILE in write direction. */
        int rc = 0;
        FILE *p = *_io_open_head();
        while (p) {
            if (p->dir == _IO_DIR_WRITE) {
                if (_io_flush_write(p) < 0) rc = EOF;
            }
            p = p->next;
        }
        return rc;
    }

    if (f->dir == _IO_DIR_WRITE) {
        return _io_flush_write(f);
    }

    if (f->dir == _IO_DIR_READ) {
        /* Discard any prefetched bytes the caller never consumed
         * and lseek the fd backwards by that amount so the next
         * read() returns the right thing.  POSIX C99 7.21.5.2. */
        size_t unread = f->bufend - f->bufpos;
        if (unread > 0) {
            (void)lseek(f->fd, -(off_t)unread, SEEK_CUR);
        }
        f->bufpos = f->bufend = 0;
        f->ungot = -1;
        return 0;
    }
    return 0;
}

/* --- Lazy std-stream init ------------------------------------------- */

static inline void _io_init_std(FILE *f, int fd, int mode, int rw)
{
    if (f->fd != 0 || f->buf || f->flags) return; /* already inited */
    f->fd = fd;
    f->mode = mode;
    f->flags = rw;
    f->dir = _IO_DIR_NONE;
    f->ungot = -1;
    f->buf = (unsigned char *)0;
    f->bufsize = 0;
    f->bufpos = f->bufend = 0;
    if (mode != _IONBF) {
        f->buf = (unsigned char *)malloc(BUFSIZ);
        if (f->buf) {
            f->bufsize = BUFSIZ;
            f->flags |= _IO_FILE_OWNS_BUF;
        } else {
            f->mode = _IONBF; /* fall back */
        }
    }
    /* Do NOT add to open list -- they're singletons that live
     * forever; we don't want exit() flushers to try to fclose them. */
}

static inline FILE *_io_stdin(void)
{
    static FILE g_stdin;
    /* fd 0 is the canonical stdin; mark as read-only.  We can't
     * use the `fd != 0` short-circuit trick because fd 0 IS zero,
     * so use the buf pointer as the "inited" sentinel via the
     * dir field set to a non-zero value on first call. */
    if (g_stdin.dir == 0 && g_stdin.flags == 0) {
        g_stdin.fd = 0;
        g_stdin.mode = _IOLBF;
        g_stdin.flags = _IO_FILE_READ;
        g_stdin.ungot = -1;
        g_stdin.buf = (unsigned char *)malloc(BUFSIZ);
        if (g_stdin.buf) {
            g_stdin.bufsize = BUFSIZ;
            g_stdin.flags |= _IO_FILE_OWNS_BUF;
        } else {
            g_stdin.mode = _IONBF;
        }
    }
    return &g_stdin;
}

static inline FILE *_io_stdout(void)
{
    static FILE g_stdout;
    if (g_stdout.flags == 0) {
        g_stdout.fd = 1;
        g_stdout.mode = _IOLBF;
        g_stdout.flags = _IO_FILE_WRITE;
        g_stdout.ungot = -1;
        g_stdout.buf = (unsigned char *)malloc(BUFSIZ);
        if (g_stdout.buf) {
            g_stdout.bufsize = BUFSIZ;
            g_stdout.flags |= _IO_FILE_OWNS_BUF;
        } else {
            g_stdout.mode = _IONBF;
        }
    }
    return &g_stdout;
}

static inline FILE *_io_stderr(void)
{
    static FILE g_stderr;
    if (g_stderr.flags == 0) {
        g_stderr.fd = 2;
        g_stderr.mode = _IONBF;
        g_stderr.flags = _IO_FILE_WRITE;
        g_stderr.ungot = -1;
    }
    return &g_stderr;
}

/* --- fopen / fdopen / fclose ---------------------------------------- */

static inline FILE *fdopen(int fd, const char *mode)
{
    if (!mode) return (FILE *)0;

    int rw = 0;
    int append = 0;
    int plus = 0;
    char m0 = mode[0];

    for (const char *p = mode; *p; p++) {
        if (*p == '+') plus = 1;
    }
    if (m0 == 'r') {
        rw = _IO_FILE_READ;
        if (plus) rw |= _IO_FILE_WRITE;
    } else if (m0 == 'w') {
        rw = _IO_FILE_WRITE;
        if (plus) rw |= _IO_FILE_READ;
    } else if (m0 == 'a') {
        rw = _IO_FILE_WRITE | _IO_FILE_APPEND;
        append = 1;
        if (plus) rw |= _IO_FILE_READ;
    } else {
        errno = EINVAL;
        return (FILE *)0;
    }

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { errno = ENOMEM; return (FILE *)0; }

    f->fd = fd;
    f->mode = _IOFBF;
    f->flags = rw;
    f->dir = _IO_DIR_NONE;
    f->ungot = -1;
    f->buf = (unsigned char *)malloc(BUFSIZ);
    if (!f->buf) {
        free(f);
        errno = ENOMEM;
        return (FILE *)0;
    }
    f->bufsize = BUFSIZ;
    f->bufpos = f->bufend = 0;
    f->flags |= _IO_FILE_OWNS_BUF;
    f->next = (FILE *)0;

    if (append) {
        (void)lseek(fd, 0, SEEK_END);
    }

    _io_list_add(f);
    return f;
}

/* open() flag constants from kernel/core/vfs.h. */
#ifndef O_RDONLY
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_APPEND 0x400
#define O_TRUNC  0x200
#endif

static inline FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode) {
        errno = EINVAL;
        return (FILE *)0;
    }
    int flags = 0;
    int plus = 0;
    for (const char *p = mode; *p; p++) {
        if (*p == '+') plus = 1;
    }
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default:  errno = EINVAL; return (FILE *)0;
    }

    int fd = open(path, flags);
    if (fd < 0) {
        /* errno already set by __svc_check inside the wrapper. */
        return (FILE *)0;
    }

    FILE *f = fdopen(fd, mode);
    if (!f) {
        int save = errno;
        close(fd);
        errno = save;
        return (FILE *)0;
    }
    f->flags |= _IO_FILE_OWNS_FD;
    return f;
}

static inline int fclose(FILE *f)
{
    if (!f) return EOF;
    int rc = 0;
    if (f->dir == _IO_DIR_WRITE) {
        if (_io_flush_write(f) < 0) rc = EOF;
    }
    if (f->flags & _IO_FILE_OWNS_FD) {
        if (close(f->fd) < 0) rc = EOF;
    }
    _io_list_remove(f);
    if (f->flags & _IO_FILE_OWNS_BUF) {
        free(f->buf);
    }
    free(f);
    return rc;
}

/* Chapter 131d — POSIX `freopen`.
 *
 * "Close `f`'s underlying fd, open `path` with `mode`, and
 * reuse the FILE* slot."  libiberty/fopen_unlocked.c calls it
 * to retarget stderr to /dev/null in conftest-like helpers.
 *
 * We implement it the simple way: open the new fd first (so a
 * failed open leaves the old FILE* intact for the caller to
 * inspect via errno), then drop the old fd and the old buffer
 * in-place, then dup2 the new fd onto the old fd number so
 * external observers (other FILE*s, the kernel) keep their
 * view stable.  If anything fails, the FILE* is invalidated
 * per the C standard (returns NULL). */
static inline FILE *freopen(const char *path, const char *mode, FILE *f)
{
    if (!f || !path || !mode) { errno = EINVAL; return (FILE *)0; }

    FILE *probe = fopen(path, mode);
    if (!probe) return (FILE *)0;

    /* Flush + close the original fd, but keep the FILE struct. */
    if (f->dir == _IO_DIR_WRITE) (void)_io_flush_write(f);
    int old_fd = f->fd;
    int new_fd = probe->fd;

    if (old_fd >= 0 && new_fd != old_fd) {
        /* Move new_fd onto old_fd so the FILE keeps its slot
         * number (callers of `fileno()` expect the same int). */
        if (dup2(new_fd, old_fd) >= 0) {
            close(new_fd);
            f->fd = old_fd;
        } else {
            f->fd = new_fd;
        }
    } else {
        f->fd = new_fd;
    }

    /* Steal probe's mode/flags, drop our owned buffer so the
     * caller gets the fresh buffering from `mode`. */
    if (f->flags & _IO_FILE_OWNS_BUF) free(f->buf);
    f->buf    = probe->buf;
    f->bufsize = probe->bufsize;
    f->bufpos = 0;
    f->bufend = 0;
    f->dir    = _IO_DIR_NONE;
    f->mode   = probe->mode;
    f->ungot  = -1;
    f->flags  = probe->flags;   /* takes OWNS_FD + OWNS_BUF from probe */

    /* Detach probe so the upcoming free() leaves the buffer
     * alive (we transferred ownership above). */
    probe->flags &= ~(_IO_FILE_OWNS_BUF | _IO_FILE_OWNS_FD);
    probe->fd = -1;
    _io_list_remove(probe);
    free(probe);
    return f;
}

/* --- tmpfile -------------------------------------------------------- */

/* Chapter 131e — POSIX `FILE *tmpfile(void)`.  Opens a unique
 * file under /tmp in "w+b" mode.  libctf's CTF link path calls
 * this for intermediate spool files; in our linker flow it's
 * cold (never reached for normal C-program linking), but the
 * stub must exist or libctf doesn't compile.
 *
 * Caller is responsible for fclose().  We don't unlink on close
 * (no atomic unlink-while-open primitive yet) — the file lingers
 * under /tmp, which is fine because /tmp is tmpfs and disappears
 * at reboot. */
static inline FILE *tmpfile(void)
{
    static unsigned int s_counter = 0;
    static char nm[32];
    unsigned int n = ++s_counter;
    int i = 0;
    const char *pfx = "/tmp/tmpf";
    while (pfx[i] && i < (int)sizeof(nm) - 12) { nm[i] = pfx[i]; i++; }
    /* 6-digit decimal suffix from the counter — uniqueness only
     * within one process, which is what POSIX requires. */
    for (int d = 5; d >= 0; d--) {
        nm[i + d] = (char)('0' + (n % 10));
        n /= 10;
    }
    i += 6;
    nm[i] = '\0';
    return fopen(nm, "w+b");
}

/* --- fread / fwrite ------------------------------------------------- */

static inline size_t fread(void *ptr, size_t sz, size_t nmemb, FILE *f)
{
    if (sz == 0 || nmemb == 0 || !ptr || !f) return 0;
    if (!(f->flags & _IO_FILE_READ)) {
        f->flags |= _IO_FILE_ERR;
        return 0;
    }
    if (_io_switch_dir(f, _IO_DIR_READ) < 0) return 0;

    size_t total = sz * nmemb;
    size_t got = 0;
    unsigned char *p = (unsigned char *)ptr;

    /* Drain ungetc pushback first. */
    if (f->ungot >= 0 && got < total) {
        p[got++] = (unsigned char)f->ungot;
        f->ungot = -1;
    }

    while (got < total) {
        if (f->bufpos < f->bufend) {
            size_t avail = f->bufend - f->bufpos;
            size_t want  = total - got;
            size_t take  = avail < want ? avail : want;
            for (size_t i = 0; i < take; i++)
                p[got + i] = f->buf[f->bufpos + i];
            f->bufpos += take;
            got += take;
            continue;
        }
        /* Buffer empty.  If the caller asked for >= bufsize bytes,
         * skip the buffer and read straight into their memory. */
        size_t want = total - got;
        if (f->mode == _IONBF || want >= f->bufsize) {
            long n = read(f->fd, p + got, want);
            if (n <= 0) {
                if (n == 0) f->flags |= _IO_FILE_EOF;
                else        f->flags |= _IO_FILE_ERR;
                break;
            }
            got += (size_t)n;
            continue;
        }
        if (_io_refill(f) < 0) break;
    }
    return got / sz;
}

static inline size_t fwrite(const void *ptr, size_t sz, size_t nmemb, FILE *f)
{
    if (sz == 0 || nmemb == 0 || !ptr || !f) return 0;
    if (!(f->flags & _IO_FILE_WRITE)) {
        f->flags |= _IO_FILE_ERR;
        return 0;
    }
    if (_io_switch_dir(f, _IO_DIR_WRITE) < 0) return 0;

    size_t total = sz * nmemb;
    const unsigned char *p = (const unsigned char *)ptr;

    if (f->mode == _IONBF || total >= f->bufsize) {
        /* Drain whatever's queued first so the relative order is right. */
        if (f->bufpos > 0 && _io_flush_write(f) < 0) return 0;
        size_t off = 0;
        while (off < total) {
            long n = write(f->fd, p + off, total - off);
            if (n <= 0) {
                f->flags |= _IO_FILE_ERR;
                return off / sz;
            }
            off += (size_t)n;
        }
        return nmemb;
    }

    /* Buffered path.  Append bytes; flush on full buffer; if
     * line-buffered, flush whenever a '\n' lands. */
    size_t off = 0;
    int lb = (f->mode == _IOLBF);
    while (off < total) {
        size_t room = f->bufsize - f->bufpos;
        size_t want = total - off;
        size_t take = room < want ? room : want;
        for (size_t i = 0; i < take; i++)
            f->buf[f->bufpos + i] = p[off + i];
        f->bufpos += take;
        f->bufend = f->bufpos;
        off += take;
        if (f->bufpos == f->bufsize) {
            if (_io_flush_write(f) < 0) return off / sz;
        } else if (lb) {
            /* Look for a newline in what we just appended; flush up to it. */
            for (size_t i = 0; i < take; i++) {
                if (p[off - take + i] == '\n') {
                    if (_io_flush_write(f) < 0) return off / sz;
                    break;
                }
            }
        }
    }
    return nmemb;
}

/* --- Char ops ------------------------------------------------------- */

static inline int fgetc(FILE *f)
{
    unsigned char c;
    size_t n = fread(&c, 1, 1, f);
    return (n == 1) ? (int)c : EOF;
}

static inline int fputc(int c, FILE *f)
{
    unsigned char b = (unsigned char)c;
    size_t n = fwrite(&b, 1, 1, f);
    return (n == 1) ? (int)b : EOF;
}

#define getc(f)     fgetc(f)
#define putc(c, f)  fputc((c), (f))
#define getchar()   fgetc(stdin)
#define putchar(c)  fputc((c), stdout)

static inline int ungetc(int c, FILE *f)
{
    if (!f || c == EOF) return EOF;
    if (f->ungot >= 0) return EOF;       /* one-char pushback limit */
    f->ungot = (unsigned char)c;
    f->flags &= ~_IO_FILE_EOF;
    return c;
}

static inline char *fgets(char *buf, int n, FILE *f)
{
    if (!buf || n <= 0 || !f) return (char *)0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return (char *)0;
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

static inline int fputs(const char *s, FILE *f)
{
    if (!s || !f) return EOF;
    size_t len = 0;
    while (s[len]) len++;
    size_t n = fwrite(s, 1, len, f);
    return (n == len) ? (int)n : EOF;
}

/* --- fseek / ftell / rewind ----------------------------------------- */

static inline int fseek(FILE *f, long off, int whence)
{
    if (!f) return -1;
    /* Drain write buffer or discard read buffer first so the fd's
     * physical offset matches what the caller intends. */
    if (fflush(f) < 0) return -1;
    f->ungot = -1;
    f->flags &= ~_IO_FILE_EOF;
    off_t r = lseek(f->fd, (off_t)off, whence);
    if (r < 0) {
        f->flags |= _IO_FILE_ERR;
        return -1;
    }
    f->dir = _IO_DIR_NONE;
    return 0;
}

static inline long ftell(FILE *f)
{
    if (!f) return -1;
    /* Get the fd's current offset, then adjust by whatever is
     * pending in the buffer in each direction. */
    off_t base = lseek(f->fd, 0, SEEK_CUR);
    if (base < 0) {
        f->flags |= _IO_FILE_ERR;
        return -1;
    }
    long adj = 0;
    if (f->dir == _IO_DIR_READ) {
        /* fd is at bufend; caller has logically consumed bufpos. */
        adj = -(long)(f->bufend - f->bufpos);
        if (f->ungot >= 0) adj -= 1;
    } else if (f->dir == _IO_DIR_WRITE) {
        adj = (long)f->bufpos;
    }
    return (long)base + adj;
}

static inline void rewind(FILE *f)
{
    (void)fseek(f, 0, SEEK_SET);
    if (f) f->flags &= ~_IO_FILE_ERR;
}

/* --- State ---------------------------------------------------------- */

static inline int feof(FILE *f)    { return f && (f->flags & _IO_FILE_EOF); }
static inline int ferror(FILE *f)  { return f && (f->flags & _IO_FILE_ERR); }
static inline void clearerr(FILE *f) {
    if (f) f->flags &= ~(_IO_FILE_EOF | _IO_FILE_ERR);
}
static inline int fileno(FILE *f)  { return f ? f->fd : -1; }

/* --- setvbuf -------------------------------------------------------- */

static inline int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if (!f) return -1;
    if (mode != _IONBF && mode != _IOLBF && mode != _IOFBF) return -1;
    /* Flush any pending write to the old buffer before swapping. */
    if (f->dir == _IO_DIR_WRITE) (void)_io_flush_write(f);
    f->bufpos = f->bufend = 0;
    f->dir = _IO_DIR_NONE;
    if (f->flags & _IO_FILE_OWNS_BUF) {
        free(f->buf);
        f->flags &= ~_IO_FILE_OWNS_BUF;
    }
    if (mode == _IONBF || size == 0) {
        f->buf = (unsigned char *)0;
        f->bufsize = 0;
        f->mode = _IONBF;
        return 0;
    }
    if (buf) {
        f->buf = (unsigned char *)buf;
        f->bufsize = size;
    } else {
        f->buf = (unsigned char *)malloc(size);
        if (!f->buf) {
            f->bufsize = 0;
            f->mode = _IONBF;
            return -1;
        }
        f->bufsize = size;
        f->flags |= _IO_FILE_OWNS_BUF;
    }
    f->mode = mode;
    return 0;
}

/* setbuf — chapter 130a.  Convenience wrapper for setvbuf:
 *   buf != NULL → setvbuf(f, buf, _IOFBF, BUFSIZ)
 *   buf == NULL → setvbuf(f, NULL, _IONBF, 0)
 * Doom calls setbuf(stdout, NULL) at startup to unbuffer its
 * status prints. */
static inline void setbuf(FILE *f, char *buf)
{
    if (buf) (void)setvbuf(f, buf, _IOFBF, BUFSIZ);
    else     (void)setvbuf(f, (char *)0, _IONBF, 0);
}

/* remove / rename — chapter 130a.  POSIX file-mutation
 * primitives Doom's savegame path (g_game.c:G_DoSaveGame)
 * uses to atomically replace doomsav?.dsg with a freshly-
 * written temp file.
 *
 *   remove(path)            unlink the named regular file
 *   rename(old, new)        atomic-replace; not yet supported
 *                           by the kernel — returns -1 and
 *                           sets errno=ENOSYS.
 *
 * Doom checks the return value of `rename` and falls back to
 * `remove(new); link(old,new); remove(old)` style behaviour
 * implicitly via fopen overwrite — so the savegame still ends
 * up correct on disk, just non-atomic.  Promoting rename to a
 * real syscall is left for a future filesystem chapter. */
int unlink(const char *path);   /* fwd-decl; defined in syscall.h */
static inline int remove(const char *path) { return unlink(path); }
static inline int rename(const char *oldp, const char *newp)
{
    (void)oldp; (void)newp;
    errno = 38;   /* ENOSYS */
    return -1;
}

/* perror — chapter 130a.  POSIX: print `s: <strerror(errno)>\n`
 * to stderr, where `s` may be NULL (in which case the leading
 * `s: ` is suppressed).  We don't have strerror yet, so we
 * format the raw errno number; that's the same output musl
 * produces with LC_MESSAGES=C and an unknown errno. */
static inline void perror(const char *s)
{
    int e = errno;
    if (s && *s) {
        size_t sl = 0; while (s[sl]) sl++;
        (void)fwrite(s, 1, sl, _io_stderr());
        (void)fwrite(": ", 1, 2, _io_stderr());
    }
    /* fall back to "errno=N" since strerror table isn't built */
    char buf[24];
    int  i = 0;
    int  v = e;
    int  neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    char digits[20]; int j = 0;
    if (v == 0) digits[j++] = '0';
    else while (v) { digits[j++] = (char)('0' + v % 10); v /= 10; }
    buf[i++] = 'e'; buf[i++] = 'r'; buf[i++] = 'r'; buf[i++] = 'n'; buf[i++] = 'o'; buf[i++] = '=';
    if (neg) buf[i++] = '-';
    while (j > 0) buf[i++] = digits[--j];
    buf[i++] = '\n';
    (void)fwrite(buf, 1, (size_t)i, _io_stderr());
}

/* --- fprintf / vfprintf --------------------------------------------- */
/*
 * Reuses printf.h's `_fmt_sink` formatter.  A small inline sink
 * shim writes the formatter's batch into our FILE * via fwrite
 * so buffering / direction-switching all works the same way as
 * fputs.
 */

/* fprintf / vfprintf.
 *
 * Implementation strategy: render the entire format string into a
 * stack buffer via vsnprintf (printf.h), then feed the resulting
 * bytes through fwrite so all of stdio's buffering / direction-
 * switching / line-buffering rules apply uniformly.
 *
 * 1 KiB is generous for typical log lines; printf outputs that
 * exceed it are truncated, which is what the legacy printf.h's
 * 128-byte batched write to fd 1 would have produced too.  Apps
 * needing larger atomic writes should use fwrite directly. */
static inline int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (!f || !fmt) return -1;
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n < 0) return -1;
    size_t out = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
    size_t w = fwrite(tmp, 1, out, f);
    return (int)w;
}

static inline int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

/* puts() override: printf.h provides one that writes to fd 1
 * directly; ours routes through stdout so apps that mix puts()
 * with fprintf(stdout, ...) see the right ordering. */
#ifdef puts
#undef puts
#endif

static inline int _io_puts(const char *s)
{
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 1;
}
#define puts(s) _io_puts(s)

/* --- scanf / fscanf / vfscanf ---------------------------------------- */
/*
 * Chapter 128f. Reuses scanf.h's `_scn_src` abstraction.  An
 * inline source shim pulls characters through fgetc and pushes
 * one-char lookahead back through ungetc.  The FILE * carries
 * its own one-char ungot slot, so the source's pushback never
 * exceeds depth 1.
 */
#include "scanf.h"

static inline int _io_file_get(void *c)
{
    return fgetc((FILE *)c);
}

static inline int _io_file_unget(void *c, int ch)
{
    return ungetc(ch, (FILE *)c);
}

static inline int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    if (!f || !fmt) return EOF;
    struct _scn_src src = { _io_file_get, _io_file_unget, f, 0 };
    return _scn_vformat(&src, fmt, ap);
}

static inline int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

static inline int vscanf(const char *fmt, va_list ap)
{
    return vfscanf(stdin, fmt, ap);
}

static inline int scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}

#endif /* USER_STDIO_H */
