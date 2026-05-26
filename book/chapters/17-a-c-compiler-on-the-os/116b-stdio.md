# Chapter 116b — `FILE *`, fopen, fread, fwrite, fseek, fprintf

**Status:** Shipped. The buffered stdio layer that everything from
chapter 117 onward assumes is now in place. Substep of
[Chapter 116](116-libc-stdio-and-env.md); follows
[Chapter 116a](116a-errno.md).

## What this chapter ships

| File | Purpose |
| --- | --- |
| `userspace/libc/stdio.h` (new) | Header-only buffered FILE * layer: `FILE`, `fopen`, `fdopen`, `fclose`, `fread`, `fwrite`, `fgetc`, `fputc`, `fgets`, `fputs`, `ungetc`, `fflush`, `fseek`, `ftell`, `rewind`, `feof`, `ferror`, `clearerr`, `fileno`, `fprintf`, `vfprintf`, `setvbuf`, `stdin` / `stdout` / `stderr` |
| `userspace/libc/syscall.h` (modified) | New `SYS_LSEEK = 101` enum entry; new `lseek()` wrapper; `SEEK_SET` / `SEEK_CUR` / `SEEK_END` macros; `off_t` typedef |
| `kernel/core/syscall.h` (modified) | Added `SYS_LSEEK = 101` |
| `kernel/core/vfs.h` (modified) | Added `vfs_lseek` declaration and `#define ESPIPE 29` |
| `kernel/core/vfs.c` (modified) | Implemented `vfs_lseek` for FD_FILE, FD_OSFS2_FILE, FD_TMPFS_RW, FD_USERFS_FILE; returns `-ESPIPE` for pipe / socket / pty / console / ipc fds |
| `kernel/core/syscall.c` (modified) | Dispatch case for `SYS_LSEEK` |
| `userspace/stdiotest/stdiotest.c` (new) | 8-test smoke binary |
| `scripts/test_libc_stdio.py` (new) | Harness, 9 assertions |

## The shape of FILE

```c
typedef struct _IO_FILE {
    int             fd;
    int             mode;       /* _IONBF / _IOLBF / _IOFBF      */
    int             flags;      /* read/write/append/eof/err/... */
    int             dir;        /* _IO_DIR_READ or _IO_DIR_WRITE */
    int             ungot;      /* one-char pushback, -1 if empty */
    unsigned char  *buf;
    size_t          bufsize;
    size_t          bufpos;     /* next read OR write position   */
    size_t          bufend;     /* end of valid data (read dir)  */
    struct _IO_FILE *next;      /* open-FILE list link           */
} FILE;
```

A single 4 KiB buffer flips between **read direction** and **write
direction**. When the caller switches sides — say, after an
`fwrite` they call `fread` on the same `FILE *` — the layer
flushes the write buffer (if dirty) or discards the read prefetch
and `lseek`s the fd back by `bufend - bufpos` so the kernel offset
matches what the caller has logically consumed. POSIX C99 §7.21.5.2
mandates the user issue `fflush` or `fseek` between direction
switches; doing it transparently is friendlier and costs nothing.

Each `FILE *` lives on a singly-linked open-file list rooted at a
single static head pointer. `fflush(NULL)` walks the list and
drains every write-direction stream. The list is per-binary
(single-translation-unit pattern: every `.c` includes `stdio.h`
once, so each binary gets its own copy of the list head).

## Three buffering modes

| Mode | Used by | Behaviour |
| --- | --- | --- |
| `_IONBF` | `stderr` | Every `fputc` / `fwrite` calls `write()` immediately. Diagnostic output is never lost to a crashed buffer. |
| `_IOLBF` | `stdin`, `stdout` | Buffered up to 4 KiB; flushed on `'\n'` or when the buffer fills. The line-buffered flush makes interactive programs feel responsive. |
| `_IOFBF` | every `fopen()` result | Fully buffered: only the buffer-full or explicit-flush case writes through. This is the right default for bulk file copy. |

`setvbuf(f, NULL, _IONBF, 0)` switches a `FILE *` to unbuffered
after the fact and frees the old buffer.

## SYS_LSEEK and the kernel side

Before this chapter the only way to "seek" was to close-and-reopen
a file. The toolchain wants real seeks — ELF object emission
patches forward references after the section is written, and any future
compiler port's
own input-file reading needs `ungetc` (which I implement above the
fd layer) plus `fseek` for `#line` directives.

The new syscall is straightforward:

```c
long vfs_lseek(int fd, int64_t off, int whence)
```

Switch on `fd_entry.kind`:

- **`FD_FILE` (OSFS-1), `FD_OSFS2_FILE`, `FD_TMPFS_RW`** — direct
  manipulation of `e->offset`. SEEK_END uses `e->osfs_size`,
  `osfs2_size(ino)`, and `tmpfs_size_of(idx)` respectively.
- **`FD_USERFS_FILE`** — dispatched to `g_userfs_ops.lseek`, which
  today supports SEEK_SET / SEEK_CUR only (SEEK_END would need a
  new request type in the daemon protocol).
- **`FD_CONSOLE`, `FD_PIPE_*`, `FD_SOCKET*`, `FD_PTY_*`,
  `FD_SRV_*`** — return `-ESPIPE` per POSIX.

I added `#define ESPIPE 29` to `kernel/core/vfs.h` alongside the
other Linux-numbered errnos; it was previously absent because no
code path had needed it.

## How `fprintf` reuses the existing formatter

`printf.h` already implements the full format spec via the
`_fmt_sink` abstraction. Rather than duplicate that 300-line
formatter or fork it to take a `FILE *`, `vfprintf` renders into a
1 KiB stack buffer via `vsnprintf` (already in `printf.h`) and
feeds the result through `fwrite`. The whole apparatus is twelve
lines:

```c
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
```

The 1 KiB cap matches the old `printf.h` reality — that header
already wrote in batched 128-byte chunks, so long `printf`s have
never been atomic. Apps needing larger atomic writes should call
`fwrite` directly.

## Two things deliberately deferred

1. **Per-thread errno.** Today `__errno_value` is a single
   `.bss` slot inherited by every thread in the same address
   space. Two threads racing in stdio will clobber each other's
   errno. Fixing this needs the chapter-91 TPIDR_EL0 dance to
   move `__errno_value` into the TLS area, which I'll do in 116d
   when the convention flip happens anyway.

2. **printf retargeting.** `printf` still writes straight to fd 1
   via the legacy `printf.h` path; `fprintf(stdout, ...)` goes
   through the buffered layer. Mixing the two in one program
   produces interleaved output. Every existing app uses the bare
   `printf`; the migration to `fprintf(stdout, ...)` happens in
   116d alongside the convention flip so we only touch each app
   once.

## Test plan

`scripts/test_libc_stdio.py` boots the OS and runs
`/bin/stdiotest`, then asserts:

| Test | What it exercises |
| --- | --- |
| T1 | `fopen("/mnt/poem.txt","r")` + `fread` in 13-byte chunks until `feof`. Sums bytes and counts newlines. |
| T2 | `fopen("/data/stdio_out","w")` + 30 × 200-byte `fwrite`s. Crosses the 4096-byte buffer twice. `fclose` flushes. |
| T3 | Re-open + `fread`-back the whole pattern, byte-for-byte equality with the generator. |
| T4 | `fseek(SEEK_SET=100)`, `ftell()==100`, `fread` four bytes, verify they match `pattern[100..104]`. |
| T5 | `fseek(SEEK_END=0)`, `ftell()==6000`; `rewind()`, `ftell()==0`. |
| T6 | `fopen("/no/such/path","r")` returns NULL with `errno==ENOENT`. |
| T7 | Char-at-a-time round trip through `fputc` / `fgetc`. |
| T8 | `fprintf(stderr, ...)` reaches the serial output (visible to the harness). |

```
$ python3 scripts/test_libc_stdio.py
[chapter 116b] FILE * layer (fopen/fread/fwrite/fseek/...)
PASS: fread /mnt/poem.txt EOF + checksum
PASS: fwrite 30x200B to /data/stdio_out + fclose flush
PASS: fread back /data/stdio_out byte-equality
PASS: fseek+ftell SEEK_SET=100 lands on pattern byte
PASS: fseek SEEK_END returns filesize; rewind returns 0
PASS: fopen missing -> NULL with errno=ENOENT
PASS: fputc/fgetc round trip through /data/stdio_chars
PASS: fprintf(stderr, ...) reaches the serial output
PASS: binary printed ALL PASS marker

9 PASS / 0 FAIL
```

Regression sweep after this chapter (all green):
`test_libc_errno`, `test_userfs_echo`, `test_clipboard`,
`test_mount_ro`, `test_userfs_timeout`.

## Applied to

- **New regression binary:** `userspace/stdiotest/stdiotest.c` —
  the 8-test smoke binary. Run by `/bin/stdiotest`.
- **Existing app rewrites:** deferred to chapter 116d (the
  convention-flip chapter), where every `printf("...errno=%d",
  -fd)` site already needs touching for the `errno` migration.
  Rewriting `cat` / `wc` / `head` / `tail` / `notepad` to use
  `FILE *` in the same pass keeps churn to one chapter per app.

## Lessons learned

- **Header-only `FILE *` is fine for now.** The single-TU pattern
  printf and malloc already use means each binary gets its own
  stdin/stdout/stderr singletons. Real-libc semantics — one
  process-wide table shared across translation units — needs a
  proper `libc.a` static library, which is on the chapter 120
  roadmap.
- **Direction switching is the trap.** The first attempt didn't
  `lseek` back on read→write switch, which made T4's read-after-
  seek-after-discard return four bytes from the *prefetched*
  buffer instead of fresh ones from the new offset. The fix is
  the `_io_switch_dir` helper that always reconciles the kernel
  position with the buffer state.
- **Avoid forking the printf formatter.** Routing `vfprintf`
  through `vsnprintf` + `fwrite` is two orders of magnitude
  simpler than retargeting the `_fmt_sink` abstraction for every
  FILE. The 1 KiB cap matches the existing reality so nothing
  regresses.

## Next

[Chapter 116c](116c-env-arena.md) — owning `environ[]` arena so
toolchain processes can `setenv("PATH", ...)` without leaking
or clobbering the shell's table.
