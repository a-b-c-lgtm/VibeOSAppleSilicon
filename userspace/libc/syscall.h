/*
 * userspace/libc/syscall.h — userspace-side syscall wrappers.
 *
 * Each wrapper is `static inline` so the application binary stays
 * a single object file with no external symbols beyond the
 * wrappers it actually uses.  The ABI matches kernel/core/syscall.h
 * exactly: x8 = number, x0..x5 = args, x0 = return.
 *
 * The inline asm uses register variables to pin x8 / x0 / x1 / x2
 * to the AAPCS-mandated places; "memory" clobber forces the
 * compiler to spill anything live across the SVC.
 */
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* Stable syscall numbers — must match kernel/core/syscall.h. */
enum {
    SYS_WRITE  = 1,
    SYS_EXIT   = 2,
    SYS_GETPID = 3,
    SYS_YIELD  = 4,
    SYS_OPEN   = 5,
    SYS_READ   = 6,
    SYS_CLOSE  = 7,
    SYS_SPAWN  = 8,
    SYS_WAIT   = 9,
    SYS_GETARGS= 10,
    SYS_SBRK   = 11,
    SYS_LISTDIR= 12,
    SYS_UPTIME_MS = 13,
    SYS_CHDIR  = 14,
    SYS_GETCWD = 15,
    SYS_GETENV = 16,
    SYS_SETENV = 17,
    SYS_UNSETENV = 18,
    SYS_GETENV_ALL = 19,
    SYS_SPAWN_REDIR = 20,
    SYS_SLEEP_MS = 21,
    SYS_PIPE     = 22,
    SYS_DUP2     = 23,
    SYS_SPAWN_PIPE = 24,
    SYS_UNLINK   = 25,
    SYS_TTY_RAW  = 26,
    SYS_KILL     = 27,
    SYS_SET_FG_PID = 28,

    /* Milestone 65 — fork + exec.  See kernel/core/syscall.h for
     * the full contract.  fork() returns the child pid in the
     * parent and 0 in the child; execv() never returns on success. */
    SYS_FORK     = 29,
    SYS_EXEC     = 30,

    /* Chapter 77 — Catchable signals.  See userspace/libc/signal.h
     * for the user-facing wrappers (signal / sigaction).  Issue
     * these manually only if you need the raw 3-arg shape. */
    SYS_SIGACTION = 31,
    SYS_SIGRETURN = 32,

    /* Chapter 78 — SIGCHLD + waitpid.  See waitpid() below for
     * the public wrapper; SYS_WAIT is preserved as the legacy
     * "any child, blocking" shape. */
    SYS_WAITPID  = 33,

    /* Chapter 79b — pseudo-terminal allocation. */
    SYS_OPENPTY  = 34,

    /* Chapter 82 — durability for OSFS-2 (no-op for other fds). */
    SYS_FSYNC    = 35,

    /* Chapter 85 — directory namespace.  See kernel/core/syscall.h
     * for the full contract. */
    SYS_MKDIR      = 36,
    SYS_LISTDIR_AT = 37,

    /* Milestone 40 — minimal in-kernel window manager. */
    SYS_GUI_CREATE_WINDOW  = 40,
    SYS_GUI_DESTROY_WINDOW = 41,
    SYS_GUI_PRESENT        = 42,
    SYS_GUI_FILL_RECT      = 43,
    SYS_GUI_DRAW_TEXT      = 44,
    SYS_GUI_FLUSH          = 45,
    SYS_GUI_POLL_EVENT     = 46,

    /* Milestone 47 — taskbar / always-on-top windows + window list. */
    SYS_GUI_CREATE_WINDOW_EX = 47,
    SYS_GUI_LIST_WINDOWS   = 48,
    SYS_GUI_RAISE_WINDOW   = 49,

    /* Milestone 50 — userspace desktop environment.  Lets the
     * desktop, taskbar, notify, etc. discover the actual
     * scanout size at runtime instead of hardcoding 1280x800. */
    SYS_GUI_GET_SCREEN_SIZE = 50,
    SYS_GUI_SET_MINIMIZED   = 51,

    /* Chapter 102 -- measure text in the kernel's default font. */
    SYS_GUI_MEASURE_TEXT    = 52,

    /* Chapter 108a -- userspace access to window pixel buffers.
     * See kernel/core/syscall.h for the contract; the userspace
     * wrappers are gui_window_fb / gui_window_damage /
     * gui_window_unmap below. */
    SYS_GUI_MAP_WINDOW      = 53,
    SYS_GUI_UNMAP_WINDOW    = 54,
    SYS_GUI_DAMAGE          = 55,

    /* Milestone 56 — sockets (active-open client side). */
    SYS_SOCKET_CONNECT  = 60,
    SYS_SOCKET_STATE    = 61,
    SYS_SOCKET_SHUTDOWN = 62,

    /* Milestone 57 — DNS resolver. */
    SYS_RESOLVE         = 63,

    /* Chapter 104 / M93 — sockets (passive-open server side).
     * listen creates a TCP_LISTEN slot bound to a port; accept
     * blocks until a fully-handshaken child is on the queue
     * and returns a fresh socket fd. */
    SYS_SOCKET_LISTEN   = 64,
    SYS_SOCKET_ACCEPT   = 65,

    /* Chapter 90 — mmap + page cache.  See kernel/core/syscall.h
     * for the full contract; mmap_uapi.h has the prot/flags
     * constants (mirrored below for userspace consumers). */
    SYS_MMAP    = 70,
    SYS_MUNMAP  = 71,

    /* Chapter 91 — userspace threads + futex.  See
     * kernel/core/syscall.h for the full contract; this
     * header's clone() / futex_wait() / futex_wake() wrappers
     * (further below) are the public API. */
    SYS_CLONE       = 72,
    SYS_FUTEX_WAIT  = 73,
    SYS_FUTEX_WAKE  = 74,

    /* Chapter 92 — clone with explicit CPU placement + getcpu.
     * The clone2() / getcpu() wrappers (further below) are the
     * public API; clone_cpu() / thread_spawn_on() in libc/thread.h
     * are the friendlier names callers actually reach for. */
    SYS_CLONE2      = 75,
    SYS_GETCPU      = 76,
    /* Chapter 93 — clone with extended argument struct + flags.
     * The clone3() wrapper below is the public API; the
     * thread_spawn_files() helper in libc/thread.h is the
     * friendlier name callers actually reach for. */
    SYS_CLONE3      = 77,

    /* Chapter 95 — wall-clock time via PL031 RTC.  See the
     * gettimeofday() / time() wrappers further below for the
     * public API. */
    SYS_GETTIMEOFDAY = 78,

    /* Chapter 96 — synthesise a square wave through the
     * virtio-sound stream.  See beep() wrapper further below. */
    SYS_BEEP        = 79,

    /* Chapter 100 — per-thread syscall tracer.  See trace_me()
     * wrapper further below; the kernel-side ring is exposed via
     * /proc/<pid>/trace. */
    SYS_TRACE_ME    = 80,

    /* Chapter 107 — named-IPC service bus.  See srv_bind /
     * srv_accept / srv_connect wrappers further below.  All
     * three accept ASCII "/srv/<name>" paths; messages are
     * length-prefixed datagrams capped at SRV_MSG_MAX (64 KiB).
     * read() / write() / close() on the returned fds use the
     * normal SYS_READ / SYS_WRITE / SYS_CLOSE wrappers. */
    SYS_SRV_BIND    = 81,
    SYS_SRV_ACCEPT  = 82,
    SYS_SRV_CONNECT = 83,

    /* Chapter 108d — userspace WSD foundation.  Single-
     * caller-wins claim on the active scanout framebuffer; the
     * caller's AS gets a RW user mapping of the FB's physical
     * pages, RW from EL0, non-executable, NOT inherited by
     * fork().  Idempotent for the holder; -EBUSY for other
     * callers; -EAGAIN if the FB isn't ready yet. */
    SYS_FB_MAP_SCANOUT = 84,
    SYS_WIN_FB_ALLOC   = 85,
    SYS_WIN_FB_MAP     = 86,
    SYS_WIN_FB_FREE    = 87,
    SYS_FB_PRESENT     = 88,

    /* chapter 108e -- userspace decorations + cursor.  wsd uses
     * these to poll the pointer state (sprite + hit-test),
     * reposition kernel "input shadow" windows when the user
     * drags a title bar, and inject a synthetic GUI_EVENT_CLOSE
     * when the user clicks the close button.  See the wrapper
     * comments below for the contract; the kernel implementations
     * live in kernel/core/wm.c. */
    SYS_POINTER_STATE    = 89,
    SYS_GUI_MOVE_WINDOW  = 90,
    SYS_GUI_DELIVER_EVENT = 91,

    /* chapter 108e -- toggle wsd-routed pointer-passthrough on
     * a kernel WM shadow.  When `on != 0`, the kernel WM's
     * pointer router skips this window entirely; wsd becomes
     * the sole authority on which window receives MOUSE_DOWN /
     * MOUSE_UP / MOUSE_MOVE for the shadow's pixels, and routes
     * them itself via gui_deliver_event.  Keyboard input still
     * flows via the kernel's focus tracker, which wsd keeps in
     * sync by calling gui_raise_window after each click-to-raise. */
    SYS_GUI_SET_INPUT_PASSTHROUGH = 92,
    /* chapter 108e -- in-place resize of an owned win_fb.
     * Owner-only.  See kernel/core/syscall.h for the full
     * contract; tl;dr: kernel allocs new pages, copies the
     * top-left of the old contents, uninstalls every existing
     * mapping (owner + mappers), frees old pages.  Owner and
     * mappers must re-call SYS_WIN_FB_MAP afterwards. */
    SYS_WIN_FB_RESIZE = 93,

    /* Chapter 112 — entropy source.  See getrandom() wrapper
     * further below for the public API.  Backed by virtio-rng +
     * ChaCha20 CSPRNG; do NOT use the bytes returned for TLS if
     * /proc/random/strong reads 0 (a future chapter exposes that
     * flag; today the only signal is the kernel boot log warning). */
    SYS_GETRANDOM     = 94,
};

/* Chapter 95 — POSIX-shaped wall-clock value.  Layout matches
 * kernel/core/syscall.h byte-for-byte; do not reorder.  The
 * 4-byte _pad keeps the total struct size a multiple of 8. */
struct timeval {
    int64_t  tv_sec;     /* seconds since 1970-01-01 UTC      */
    uint32_t tv_usec;    /* microseconds, 0..999_999          */
    uint32_t _pad;
};

/* Chapter 95 — POSIX-shaped time_t.  64-bit signed; Y2038-safe.
 * The kernel ABI is 64-bit too (see struct timeval) — even
 * though the underlying PL031 hardware register is only 32-bit,
 * the kernel promotes the value before exporting it. */
typedef int64_t time_t;

/* Chapter 93 — argument struct for clone3.  Layout matches
 * kernel/core/syscall.h byte-for-byte; do not reorder.  The
 * 4-byte _pad keeps the total size a multiple of 8. */
struct clone_args {
    uint64_t flags;
    uint64_t entry;
    uint64_t arg;
    uint64_t stack_top;
    uint64_t tls;
    int32_t  cpu_id;
    uint32_t _pad;
};

/* Chapter 93 — clone3 flag bits.  Only CLONE_FILES (bit 0) is
 * defined today.  Any other bit returns -EINVAL from the kernel. */
#define CLONE_FILES  0x01ULL

/* Chapter 90 mmap constants — must match kernel/core/mmap_uapi.h. */
#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_FAILED     ((void *)-1L)

/* Signal numbers (POSIX subset) — match kernel/core/thread.h.
 * Only SIGINT is currently delivered (by Ctrl-C in cooked mode);
 * the others are reserved for symmetry with POSIX userspace. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17    /* posted to the parent on child exit (chapter 78) */

/* waitpid options (matches POSIX where it overlaps). */
#define WNOHANG  1

static inline long _svc0(long n)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long _svc1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long _svc2(long n, long a, long b)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static inline long _svc3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static inline long _svc4(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

static inline long _svc5(long n, long a, long b, long c, long d,
                         long e)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                     : "memory");
    return x0;
}

static inline long _svc6(long n, long a, long b, long c, long d,
                         long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3),
                       "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

static inline long write(int fd, const void *buf, size_t len)
{
    return _svc3(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)len);
}

static inline int open(const char *name, int flags)
{
    return (int)_svc2(SYS_OPEN, (long)(uintptr_t)name, (long)flags);
}

static inline long read(int fd, void *buf, size_t len)
{
    return _svc3(SYS_READ, fd, (long)(uintptr_t)buf, (long)len);
}

static inline int close(int fd)
{
    return (int)_svc1(SYS_CLOSE, fd);
}

__attribute__((noreturn))
static inline void exit(int code)
{
    _svc1(SYS_EXIT, code);
    __builtin_unreachable();
}

static inline int getpid(void)
{
    return (int)_svc0(SYS_GETPID);
}

static inline void yield(void)
{
    _svc0(SYS_YIELD);
}

/* chapter 112b: cross-guarded with vendor/bearssl-shim/string.h so
 * TUs that include both syscall.h and bearssl.h (e.g. tls_socket.c,
 * httpsd.c, tlstest.c --handshake) don't get a duplicate-with-
 * different-linkage error from gcc.  The shim is non-static (it
 * forward-declares cstring.c's extern definition); ours is static
 * inline.  Whichever header is included first installs the symbol;
 * the other one's guard fires and skips. */
#ifndef OSDEV_STRLEN_PROVIDED
#define OSDEV_STRLEN_PROVIDED
static inline size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
#endif

static inline void puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

/* Print a signed decimal integer to stdout (no newline).  Tiny
 * formatter — useful for printing pids and errnos in demos. */
static inline void putd(long v)
{
    char  buf[24];
    int   i = 0;
    int   neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) buf[i++] = '-';
    /* Reverse in place. */
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char tmp = buf[a]; buf[a] = buf[b]; buf[b] = tmp;
    }
    write(1, buf, (size_t)i);
}

static inline int spawn(const char *path, const char *args)
{
    return (int)_svc2(SYS_SPAWN, (long)(uintptr_t)path,
                                  (long)(uintptr_t)args);
}

/* Milestone 65 — POSIX-style fork().
 *
 * On success returns 0 in the child and the child's pid in the
 * parent.  On failure returns -errno (in the parent only).
 *
 * The two return values come from the kernel scheduling the
 * child as a brand-new READY thread whose initial trap frame is
 * a clone of the parent's saved frame with x0 forced to 0; the
 * parent's eret restores its frame with x0 = child pid. */
static inline int fork(void)
{
    return (int)_svc0(SYS_FORK);
}

/* Milestone 65 — POSIX-style execv().
 *
 * Replaces the calling process's address space with the program
 * loaded from `path`.  `argv` is a NULL-terminated array of
 * NUL-terminated strings; argv[0] is conventionally the program
 * name.  On success this call does NOT return — control resumes
 * at the new program's entry point with a fresh user stack and
 * argc/argv visible to its main().  On failure the old AS is
 * preserved and -errno is returned (-ENOENT for missing path,
 * -EINVAL for a bad ELF, -ENOMEM for OOM, -EFAULT for bad argv).
 *
 * File descriptors survive exec by default — there is no
 * FD_CLOEXEC yet.  cwd, env, and the args buffer are reset to
 * match the new program (env survives; argv is rebuilt). */
static inline int execv(const char *path, char *const argv[])
{
    return (int)_svc2(SYS_EXEC, (long)(uintptr_t)path,
                                 (long)(uintptr_t)argv);
}

static inline int spawn_redir(const char *path, const char *args,
                              const char *stdin_path)
{
    return (int)_svc3(SYS_SPAWN_REDIR,
                      (long)(uintptr_t)path,
                      (long)(uintptr_t)args,
                      (long)(uintptr_t)stdin_path);
}

static inline int wait(int *code_out)
{
    return (int)_svc1(SYS_WAIT, (long)(uintptr_t)code_out);
}

/* Generalised reaper (chapter 78).
 *   pid > 0   : wait for that specific child only.
 *   pid <= 0  : wait for any child (same shape as wait()).
 *   options & WNOHANG : poll-only; returns 0 if a matching child
 *                       exists but hasn't exited yet.
 * Returns the reaped pid (and writes exit code via code_out if
 * non-NULL), 0 for the WNOHANG poll-no-exit case, or -1 if no
 * child matches the filter. */
static inline int waitpid(int pid, int *code_out, int options)
{
    return (int)_svc3(SYS_WAITPID,
                      (long)pid,
                      (long)(uintptr_t)code_out,
                      (long)options);
}

static inline long getargs(char *buf, size_t len)
{
    return _svc2(SYS_GETARGS, (long)(uintptr_t)buf, (long)len);
}

/* Adjust the program break by `inc` bytes (positive grows,
 * negative shrinks).  Returns the *previous* break, or (void *)-1
 * on failure (out of memory, or hit USER_HEAP_MAX).  Use this to
 * build malloc() / free() in user code. */
static inline void *sbrk(long inc)
{
    long r = _svc1(SYS_SBRK, inc);
    return (void *)(uintptr_t)r;
}

/* Chapter 90 \u2014 mmap a file or anonymous region.  See
 * mmap_uapi.h for the full contract.  Returns the mapped VA on
 * success or MAP_FAILED on error.  In chapter 90 `addr` is
 * ignored (no MAP_FIXED yet) and the kernel always picks a
 * fresh range from the per-AS mmap bump pointer. */
static inline void *mmap(void *addr, size_t len, int prot, int flags,
                         int fd, long offset)
{
    long r = _svc6(SYS_MMAP, (long)(uintptr_t)addr, (long)len,
                              (long)prot, (long)flags,
                              (long)fd, offset);
    if (r < 0 && r > -4096) return MAP_FAILED;     /* errno-shaped */
    return (void *)(uintptr_t)r;
}

/* Chapter 90 \u2014 unmap a range previously returned by mmap.
 * `addr` must be the exact start of an existing mmap; `len` is
 * currently ignored (whole vma is removed).  Returns 0 on
 * success or -errno on failure. */
static inline int munmap(void *addr, size_t len)
{
    return (int)_svc2(SYS_MUNMAP, (long)(uintptr_t)addr, (long)len);
}

/* ── Chapter 91 — userspace threads (clone) + futex ─────────────
 *
 * clone(entry, arg, stack_top, tls)
 *   Spawn a new thread sharing the calling thread's address
 *   space.  The new thread starts at `entry` with x0=arg,
 *   SP_EL0=stack_top, TPIDR_EL0=tls.  Returns the child's tid
 *   on success, or -errno (-EINVAL for a bogus entry / stack,
 *   -ENOMEM on OOM).  File descriptors are NOT shared — see
 *   the kernel banner in syscall.h.
 *
 * futex_wait(uaddr, expected)
 *   If *uaddr == expected, block until somebody calls
 *   futex_wake on the same address.  Otherwise return -EAGAIN
 *   (= -11) immediately.  Spurious wakes allowed; the caller
 *   must re-check the predicate after waking.  Returns 0 on
 *   normal wake, -EAGAIN on predicate mismatch, -EFAULT on a
 *   bogus pointer.  The futex word must be 32-bit aligned.
 *
 * futex_wake(uaddr, n)
 *   Wake threads blocked on `uaddr`.  Chapter 91 floor: `n`
 *   must be > 0; the kernel currently treats it as "wake all".
 *   Returns 1 (at least one wake attempted) or 0 (n was 0).
 *
 * Higher-level helpers (mutex, thread_spawn / thread_join,
 * atomic primitives) live in libc/thread.h. */

typedef void (*clone_entry_t)(void *arg);

static inline int clone(clone_entry_t entry, void *arg,
                        void *stack_top, void *tls)
{
    return (int)_svc4(SYS_CLONE,
                      (long)(uintptr_t)entry,
                      (long)(uintptr_t)arg,
                      (long)(uintptr_t)stack_top,
                      (long)(uintptr_t)tls);
}

static inline int futex_wait(volatile int *uaddr, int expected)
{
    return (int)_svc2(SYS_FUTEX_WAIT,
                      (long)(uintptr_t)uaddr,
                      (long)expected);
}

static inline int futex_wake(volatile int *uaddr, int n)
{
    return (int)_svc2(SYS_FUTEX_WAKE,
                      (long)(uintptr_t)uaddr,
                      (long)n);
}

/* ── Chapter 92 — clone with explicit CPU placement + getcpu ───
 *
 * clone2(entry, arg, stack_top, tls, cpu_id)
 *   Same as clone() but pins the new thread to absolute CPU
 *   `cpu_id`.  cpu_id == -1 inherits the calling CPU
 *   (identical behaviour to clone()).  cpu_id in
 *   [0, SMP_MAX_CPUS) pins to that CPU.  Out-of-range values
 *   return -EINVAL.  Threads do not migrate after creation;
 *   a clone2 child stays on cpu_id for its lifetime.
 *
 * getcpu()
 *   Return the CPU id the calling thread is currently running
 *   on.  For chapter-92 user threads (which are pinned to their
 *   home_cpu) the value is stable across the syscall return.
 *   Used by tests to verify clone2 placement actually happens.
 */

static inline int clone2(clone_entry_t entry, void *arg,
                         void *stack_top, void *tls, int cpu_id)
{
    return (int)_svc5(SYS_CLONE2,
                      (long)(uintptr_t)entry,
                      (long)(uintptr_t)arg,
                      (long)(uintptr_t)stack_top,
                      (long)(uintptr_t)tls,
                      (long)cpu_id);
}

static inline int getcpu(void)
{
    return (int)_svc0(SYS_GETCPU);
}

/* ── Chapter 93 — clone3: extended-args clone with flags ──────
 *
 * clone3(struct clone_args *a)
 *   POSIX-style clone with an extensible argument struct and
 *   per-clone "what to share" flags.  The pointer is a USER VA
 *   the kernel copies from before validating any field, so
 *   `a` must be readable for sizeof(*a) bytes for the call to
 *   succeed.  Returns the new tid on success, -EINVAL for any
 *   bad field (unknown flag bit, bogus entry / stack_top, bad
 *   cpu_id), -EFAULT for an unreadable struct, -ENOMEM on OOM.
 *
 *   Today the only flag bit defined is CLONE_FILES (bit 0).
 *   When set, the new thread shares the calling thread's
 *   fd_table by reference; both threads see the same fds at
 *   the same indices, and the table is freed only when the
 *   LAST referencing thread exits.  When clear, the new
 *   thread gets a fresh private fd_table (same as clone() /
 *   clone2()).
 *
 * The friendly wrapper `thread_spawn_files()` in libc/thread.h
 * is the recommended way to reach this for the common
 * "give me a worker thread that shares my fds" case. */
static inline int clone3(struct clone_args *a)
{
    return (int)_svc1(SYS_CLONE3, (long)(uintptr_t)a);
}

/* Read the idx-th directory entry into `name` (NUL-terminated, up
 * to `cap-1` bytes) and store the file size at *size_out.
 * Returns the length of the name written (excluding NUL), -2
 * (-ENOENT) when idx is past the last entry, or -14 (-EFAULT) on
 * a bad pointer.  Walk idx 0..N until you get a negative return. */
static inline long listdir(int idx, char *name, size_t cap, unsigned int *size_out)
{
    return _svc4(SYS_LISTDIR, (long)idx, (long)(uintptr_t)name,
                              (long)cap, (long)(uintptr_t)size_out);
}

/* Monotonic milliseconds since boot.  Never decreases.  Useful
 * for measuring elapsed time and building polling sleep loops on
 * top of yield(). */
static inline unsigned long uptime_ms(void)
{
    return (unsigned long)_svc0(SYS_UPTIME_MS);
}

/* Chapter 95 — wall-clock time.  Reads the kernel's PL031 RTC
 * snapshot + uptime delta into *tv.  Returns 0 on success or a
 * negative errno (-EFAULT for a bad pointer).
 *
 * Unlike uptime_ms(), the returned value depends on the RTC the
 * kernel saw at boot: on a system where the RTC was never found
 * tv->tv_sec will count from 0 (Unix epoch), which userspace can
 * detect by checking whether tv_sec is wildly in the past. */
static inline int gettimeofday(struct timeval *tv)
{
    return (int)_svc1(SYS_GETTIMEOFDAY, (long)(uintptr_t)tv);
}

/* Convenience wrapper: return wall-clock seconds, optionally
 * also storing them at *out.  Returns -1 on syscall failure
 * (rare — only -EFAULT, which userspace shouldn't hit when
 * passing a stack-local). */
static inline time_t time(time_t *out)
{
    struct timeval tv;
    if (gettimeofday(&tv) != 0)
        return (time_t)-1;
    if (out)
        *out = tv.tv_sec;
    return tv.tv_sec;
}

/* Sleep for at least `ms` milliseconds.  Granularity is one
 * scheduler tick (currently 100 ms); the call may return up to
 * one tick late. */
static inline int sleep_ms(unsigned long ms)
{
    return (int)_svc1(SYS_SLEEP_MS, (long)ms);
}

/* Chapter 96 — synthesise a square wave on the virtio-snd PCM
 * stream.  Blocks for approximately `duration_ms` while the
 * device consumes the samples.
 *
 *   freq_hz       :  20 .. 22_050 (clipped)
 *   duration_ms   :   1 .. 5_000  (clipped)
 *
 * Returns 0 on success, -ENODEV if the kernel never found a
 * virtio-sound device at boot, or -EIO on submission failure. */
static inline int beep(unsigned int freq_hz, unsigned int duration_ms)
{
    return (int)_svc2(SYS_BEEP, (long)freq_hz, (long)duration_ms);
}

/* Chapter 100 — enable per-thread syscall tracing on self.  After
 * this returns 0, every SVC issued by the calling thread is
 * recorded into a kernel-side ring (size STRACE_RING_CAP); read
 * the textual trace from `/proc/<getpid()>/trace`.  Idempotent;
 * no way to disable today (the ring lives until the thread
 * exits).  Returns 0 on success, -ENOMEM if the ring couldn't
 * be allocated.  See /bin/strace for the typical caller pattern
 * (fork → trace_me → execv). */
static inline int trace_me(void)
{
    return (int)_svc0(SYS_TRACE_ME);
}

/* Chapter 112 — fill `buf` with `len` bytes of cryptographically-
 * random data sourced from the kernel CSPRNG (which pulls its
 * seed from the virtio-rng device when present).
 *
 *   flags   reserved; must currently be 0 (-EINVAL otherwise)
 *   len     0 .. 1 MiB; larger requests should loop in userspace
 *
 * Returns the number of bytes written (== len on success) or a
 * negative errno on failure.  Never partial-fills: on success
 * exactly `len` bytes are present in `buf`. */
static inline long getrandom(void *buf, unsigned long len, unsigned int flags)
{
    return _svc3(SYS_GETRANDOM,
                 (long)(uintptr_t)buf,
                 (long)len,
                 (long)flags);
}

/* Allocate an anonymous pipe.  On success, fds[0] is the read
 * end and fds[1] is the write end; returns 0.  On failure
 * returns a negative errno (no fd table slots: -EMFILE; out of
 * memory: -ENOMEM; bad pointer: -EFAULT). */
static inline int pipe(int fds[2])
{
    return (int)_svc1(SYS_PIPE, (long)(uintptr_t)fds);
}

/* Duplicate `oldfd` onto `newfd`, closing newfd first if open.
 * Returns newfd on success, -EBADF on bad fds. */
static inline int dup2(int oldfd, int newfd)
{
    return (int)_svc2(SYS_DUP2, (long)oldfd, (long)newfd);
}

/* Spawn a child with optional stdin / stdout redirection from
 * the parent's fd table.  stdin_fd / stdout_fd of -1 mean "no
 * redirect — child gets default console for that slot."  The
 * parent retains its references; pipe refcounts are bumped on
 * the inherited side.  Returns the new tid or -errno. */
static inline int spawn_pipe(const char *path, const char *args,
                             int stdin_fd, int stdout_fd)
{
    return (int)_svc4(SYS_SPAWN_PIPE,
                      (long)(uintptr_t)path,
                      (long)(uintptr_t)args,
                      (long)stdin_fd,
                      (long)stdout_fd);
}

/* Remove a file from a writable filesystem.  Currently only
 * `/tmp/<name>` is supported.  Returns 0 on success or -errno.
 * Open fds referencing the unlinked file will start returning
 * -EBADF on subsequent read/write. */
static inline int unlink(const char *path)
{
    return (int)_svc1(SYS_UNLINK, (long)(uintptr_t)path);
}

/* Chapter 85 \u2014 directory namespace.
 *
 * mkdir(path) creates a single directory.  All parent components
 * must already exist (no "mkdir -p" semantics here \u2014 do that in
 * userspace if needed).  Currently `path` must start with /data/.
 * Returns 0 on success, -errno on failure. */
static inline int mkdir(const char *path)
{
    return (int)_svc1(SYS_MKDIR, (long)(uintptr_t)path);
}

/* Type tags returned in the `type_out` slot of listdir_at. */
#define LISTDIR_TYPE_FILE 1u
#define LISTDIR_TYPE_DIR  2u

/* listdir_at(dirpath, idx, name, cap, &size, &type) walks a
 * single directory by absolute path \u2014 unlike listdir() which
 * folds every mount into one linear list.  Returns the leaf-name
 * length (excluding NUL), -ENOENT past the last entry, or other
 * -errno on failure.  size_out / type_out may be NULL.  Pass
 * "/data" or "/data/" as `dirpath` to enumerate the writable
 * mount root; "/data/notes" to enumerate that subdirectory. */
static inline long listdir_at(const char *dirpath, int idx,
                              char *name, size_t cap,
                              unsigned int *size_out,
                              unsigned int *type_out)
{
    return _svc6(SYS_LISTDIR_AT,
                 (long)(uintptr_t)dirpath, (long)idx,
                 (long)(uintptr_t)name, (long)cap,
                 (long)(uintptr_t)size_out,
                 (long)(uintptr_t)type_out);
}

/* Toggle the calling thread's console-input mode.
 *   enable != 0 -> raw mode (per-byte, no echo, no buffering).
 *   enable == 0 -> cooked mode (line-buffered, local echo &
 *                  backspace handling).
 * Returns the previous mode (0 or 1).  Children spawned after
 * this call always start in cooked mode. */
static inline int tty_raw(int enable)
{
    return (int)_svc1(SYS_TTY_RAW, (long)enable);
}

/* Send a signal to a thread by pid.  Default action for every
 * signal is "terminate the target with code 128 + sig"; there
 * are no userspace handlers yet.  Returns 0 on success or
 * -errno (-EINVAL for bad sig, -ENOENT for unknown pid). */
static inline int kill(int pid, int sig)
{
    return (int)_svc2(SYS_KILL, (long)pid, (long)sig);
}

/* Designate `pid` as the foreground thread for the cooked-mode
 * console: a Ctrl-C (0x03) byte will be turned into a SIGINT
 * delivered to that pid.  Pass 0 to clear (Ctrl-C consumed
 * silently).  Always succeeds; returns the previous fg pid.
 *
 * Auto-routing (chapter 79b): if the calling thread's fd 0 is a
 * pty slave, this writes the pty's fg_pid field instead of the
 * global console fg_pid.  This means /bin/sh's existing
 * set_fg_pid(child) calls work transparently when sh is run
 * inside gui_term over a pty. */
static inline int set_fg_pid(int pid)
{
    return (int)_svc1(SYS_SET_FG_PID, (long)pid);
}

/* Allocate a pty (chapter 79b).  On success returns 0 and writes
 * the master fd into *master_out and the slave fd into
 * *slave_out.  The master is intended for the controlling app
 * (e.g. gui_term); the slave is meant to be dup2'd onto fds
 * 0/1/2 of a forked child shell.  Errors: -EFAULT, -EMFILE,
 * -ENOMEM. */
static inline int openpty(int *master_out, int *slave_out)
{
    return (int)_svc2(SYS_OPENPTY,
                      (long)(uintptr_t)master_out,
                      (long)(uintptr_t)slave_out);
}

/* Chapter 82 \u2014 force every dirty cache block backing `fd` to
 * disk before returning.  For OSFS-2 file fds this triggers a
 * synchronous flush of the kernel's write-back cache and only
 * returns once every virtio-blk write has acked.  For every
 * other fd kind (console, ramfs, OSFS-1, tmpfs, pipes, ptys,
 * sockets) it's a no-op that returns 0, so callers can fsync
 * defensively without special-casing the fd kind.
 * Errors: -EBADF (unopened fd), -EIO (a writeback failed; the
 * cache slot is left dirty so a retry can succeed). */
static inline int fsync(int fd)
{
    return (int)_svc1(SYS_FSYNC, fd);
}

/* Change the calling thread's current working directory.  Returns
 * 0 on success, -2 (-ENOENT) if the path isn't a recognized
 * directory, -22 (-EINVAL) if it's not absolute. */
static inline int chdir(const char *path)
{
    return (int)_svc1(SYS_CHDIR, (long)(uintptr_t)path);
}

/* Copy the calling thread's cwd (NUL-terminated) into `buf`.
 * Returns the number of bytes written including NUL, or -22
 * (-EINVAL) if `cap` is too small. */
static inline long getcwd(char *buf, size_t cap)
{
    return _svc2(SYS_GETCWD, (long)(uintptr_t)buf, (long)cap);
}

/* Look up `key` in the calling thread's env, copy the value
 * (NUL-terminated) into `buf`.  Returns bytes written including
 * NUL, -2 (-ENOENT) if not present. */
static inline long getenv(const char *key, char *buf, size_t cap)
{
    return _svc3(SYS_GETENV,
                 (long)(uintptr_t)key,
                 (long)(uintptr_t)buf,
                 (long)cap);
}

/* Set or replace KEY=VAL in the env.  Returns 0, -22 (EINVAL)
 * for empty key, -12 (ENOMEM) if the env block is full. */
static inline int setenv(const char *key, const char *val)
{
    return (int)_svc2(SYS_SETENV,
                      (long)(uintptr_t)key,
                      (long)(uintptr_t)val);
}

/* Remove KEY from env.  Returns 0 or -2 (ENOENT). */
static inline int unsetenv(const char *key)
{
    return (int)_svc1(SYS_UNSETENV, (long)(uintptr_t)key);
}

/* Copy the entire env blob (packed K=V\0K=V\0...\0) into buf.
 * Returns bytes written including the trailing extra NUL. */
static inline long getenv_all(char *buf, size_t cap)
{
    return _svc2(SYS_GETENV_ALL, (long)(uintptr_t)buf, (long)cap);
}

/* ── GUI (milestone 40 minimal window manager) ──────────────────
 *
 * All windows render into a kernel-side BGRA8 buffer; the WM owns
 * decorations (title bar + 1px border) and composites every
 * window onto the shared framebuffer.  The application coordinate
 * system is content-area relative (x=0,y=0 = first pixel below
 * the title bar / inside the border).
 *
 * Pixel format (for gui_present): packed B in low byte, then G,
 * then R, then ignored alpha.  A handy macro for callers: */
#define GUI_BGRA(R, G, B)  ((uint32_t)(((R) << 16) | ((G) << 8) | (B)))

#define GUI_EVENT_NONE        0
#define GUI_EVENT_KEY         1
#define GUI_EVENT_CLOSE       2
#define GUI_EVENT_MOUSE_MOVE  3   /* arg0=x, arg1=y (window-relative)    */
#define GUI_EVENT_MOUSE_DOWN  4   /* arg0=x, arg1=y, arg2=button bitmap  */
#define GUI_EVENT_MOUSE_UP    5   /* arg0=x, arg1=y, arg2=button bitmap  */
#define GUI_EVENT_RESIZE      6   /* arg0=new content w, arg1=new content h
                                   * — delivered to RESIZABLE windows
                                   * after the WM has already swapped
                                   * the pixel buffer.  Apps re-derive
                                   * their layout from arg0/arg1. */

#define GUI_BTN_LEFT          0x1u
#define GUI_BTN_RIGHT         0x2u
#define GUI_BTN_MIDDLE        0x4u

/* Extended GUI_EVENT_KEY codes for non-ASCII keys.  Apps that
 * compare arg0 to ASCII characters must mask with 0xFF (or use
 * the strict equality `arg0 == 0x1B` form) so that an arrow-key
 * event whose arg0 is GUI_KEY_LEFT (0x104) is not silently
 * mistaken for an ASCII byte of value 0x04. */
#define GUI_KEY_UP            0x101u
#define GUI_KEY_DOWN          0x102u
#define GUI_KEY_RIGHT         0x103u
#define GUI_KEY_LEFT          0x104u
#define GUI_KEY_HOME          0x105u
#define GUI_KEY_END           0x106u
#define GUI_KEY_PGUP          0x107u
#define GUI_KEY_PGDN          0x108u

/* Window flags (milestone 47, milestone 50, milestone 63). */
#define GUI_WIN_FLAG_NO_DECORATION   0x1u
#define GUI_WIN_FLAG_ALWAYS_ON_TOP   0x2u
#define GUI_WIN_FLAG_PIN_TO_BOTTOM   0x4u
#define GUI_WIN_FLAG_MINIMIZED       0x8u   /* read-only status bit */
#define GUI_WIN_FLAG_RESIZABLE       0x10u  /* milestone 63: user can drag the
                                              * grip in the bottom-right of
                                              * the title-bar-bordered rect.
                                              * Apps that opt in must handle
                                              * GUI_EVENT_RESIZE; ignoring it
                                              * leaves the old content top-
                                              * left-anchored in the new
                                              * buffer with gray padding. */
#define GUI_WIN_POS_AUTO             (-1)

struct gui_event {
    uint32_t type;
    int32_t  window_id;
    uint32_t arg0;     /* KEY: ASCII byte (0 if non-printable) */
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

/* Argument packs for the multi-parameter GUI syscalls.  The kernel
 * copies the struct out of user memory in a single copy_from_user.
 * Layout MUST stay identical to kernel/core/wm.c. */
struct gui_present_args {
    int32_t  id;
    uint32_t x, y, w, h;
    const void *src;       /* tightly packed BGRA, w*h*4 bytes */
};
struct gui_fill_rect_args {
    int32_t  id;
    uint32_t x, y, w, h;
    uint32_t bgra;
};
struct gui_draw_text_args {
    int32_t  id;
    uint32_t x, y;
    const char *s;
    uint32_t fg_bgra;
    uint32_t bg_bgra;
    int32_t  transparent;
};

static inline int gui_create_window(uint32_t w, uint32_t h, const char *title)
{
    return (int)_svc3(SYS_GUI_CREATE_WINDOW,
                      (long)w, (long)h, (long)(uintptr_t)title);
}

static inline int gui_destroy_window(int id)
{
    return (int)_svc1(SYS_GUI_DESTROY_WINDOW, (long)id);
}

static inline int gui_present(int id,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h,
                              const void *src_bgra)
{
    struct gui_present_args a = {
        .id = id, .x = x, .y = y, .w = w, .h = h, .src = src_bgra
    };
    return (int)_svc1(SYS_GUI_PRESENT, (long)(uintptr_t)&a);
}

static inline int gui_fill_rect(int id,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h,
                                uint32_t bgra)
{
    struct gui_fill_rect_args a = {
        .id = id, .x = x, .y = y, .w = w, .h = h, .bgra = bgra
    };
    return (int)_svc1(SYS_GUI_FILL_RECT, (long)(uintptr_t)&a);
}

static inline int gui_draw_text(int id,
                                uint32_t x, uint32_t y,
                                const char *s,
                                uint32_t fg_bgra, uint32_t bg_bgra,
                                int transparent)
{
    struct gui_draw_text_args a = {
        .id = id, .x = x, .y = y, .s = s,
        .fg_bgra = fg_bgra, .bg_bgra = bg_bgra,
        .transparent = transparent,
    };
    return (int)_svc1(SYS_GUI_DRAW_TEXT, (long)(uintptr_t)&a);
}

/* Chapter 102 -- measure a string's rendered width in pixels in
 * the kernel's default font (TTF DejaVu Sans @ 16 px today).
 * Stops at '\n'. Returns the pixel width as a non-negative int,
 * or 0 on EFAULT (the kernel returns -EFAULT but we don't expose
 * errno here; callers can detect bad input by checking that the
 * string they passed isn't empty). Used by apps that previously
 * assumed an 8-px glyph pitch -- carets, centred labels,
 * truncate-to-fit -- so they keep working with proportional
 * glyphs. */
static inline int gui_measure_text(const char *s)
{
    long r = _svc1(SYS_GUI_MEASURE_TEXT, (long)(uintptr_t)s);
    if (r < 0) return 0;
    return (int)r;
}

/* ── Chapter 108a: direct window-buffer access ─────────────── */

/* Descriptor returned by gui_window_fb().  `pixels` points at a
 * page-aligned BGRA framebuffer for the window; rows are
 * `stride` bytes apart (today always `w*4`); dimensions match
 * the window's content area.
 *
 * Chapter 108c added the `id` field so libgui's draw_* and
 * gui_window_dirty helpers can take a gui_fb* alone and still
 * issue the SYS_GUI_DAMAGE call without the app having to pass
 * the window id everywhere alongside it.  Pre-108c callers that
 * built a struct gui_fb by hand left `id` zero, which would
 * route damage to window 0; new callers should always obtain
 * the struct via gui_window_fb() (which fills it). */
struct gui_fb {
    uint8_t *pixels;
    uint32_t stride;
    uint32_t w, h;
    int32_t  id;
};

struct gui_map_window_args {
    int32_t  id;
    uint64_t va_out;        /* uint64_t * (cast to ptr) */
    uint64_t stride_out;    /* uint32_t * */
    uint64_t w_out;         /* uint32_t * */
    uint64_t h_out;         /* uint32_t * */
};

struct gui_damage_args {
    int32_t  id;
    uint32_t x, y, w, h;
};

/* Chapter 108a -- map this process's view onto a window's pixel
 * buffer.  On success returns 0 and populates *out with a
 * page-aligned BGRA framebuffer the app can draw into directly.
 * The mapping persists across multiple gui_poll_event /
 * gui_window_damage cycles -- callers typically map once at
 * startup and reuse the pointer forever.
 *
 * The window must NOT have been created with
 * GUI_WIN_FLAG_RESIZABLE (chapter 108a defers resize coherence).
 *
 * Returns -1 on error (the kernel return code is lost; callers
 * who need it can call the raw _svc1 form directly).  In
 * particular the app gets a hard "no" if it tries to map a
 * window it doesn't own or if pmem is exhausted. */
static inline int gui_window_fb(int id, struct gui_fb *out)
{
    if (!out) return -1;
    uint64_t va = 0;
    uint32_t stride = 0, ww = 0, hh = 0;
    struct gui_map_window_args a = {
        .id         = id,
        .va_out     = (uint64_t)(uintptr_t)&va,
        .stride_out = (uint64_t)(uintptr_t)&stride,
        .w_out      = (uint64_t)(uintptr_t)&ww,
        .h_out      = (uint64_t)(uintptr_t)&hh,
    };
    long r = _svc1(SYS_GUI_MAP_WINDOW, (long)(uintptr_t)&a);
    if (r < 0) return -1;
    out->pixels = (uint8_t *)(uintptr_t)va;
    out->stride = stride;
    out->w      = ww;
    out->h      = hh;
    out->id     = id;
    return 0;
}

/* Chapter 108a -- tell the WM that the rect (x,y,w,h) of the
 * mapped framebuffer has been written and needs to make it to
 * the screen.  Coordinates are window-content relative;
 * over-large rects are silently clipped to the window so a
 * blanket gui_window_damage(id, 0, 0, fb.w, fb.h) call after a
 * full repaint Just Works. */
static inline int gui_window_damage(int id,
                                    uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h)
{
    struct gui_damage_args a = {
        .id = id, .x = x, .y = y, .w = w, .h = h,
    };
    return (int)_svc1(SYS_GUI_DAMAGE, (long)(uintptr_t)&a);
}

/* Chapter 108a -- drop a previous gui_window_fb mapping.
 * Implicit on gui_destroy_window / process exit, so most apps
 * never need to call this; useful in tests that want to verify
 * the AS layer has actually released the VA range. */
static inline int gui_window_unmap(int id)
{
    return (int)_svc1(SYS_GUI_UNMAP_WINDOW, (long)id);
}

/* ── Chapter 108d: framebuffer mapping for WSD ──────── */

/* Layout matches struct fb_map_args_k in kernel/core/wsd_fb.c
 * byte-for-byte; do not reorder. */
struct fb_map_args {
    uint64_t va;        /* OUT: user VA where the FB is mapped     */
    uint32_t w;         /* OUT: scanout width  in pixels           */
    uint32_t h;         /* OUT: scanout height in pixels           */
    uint32_t stride;    /* OUT: bytes per scanline (= w*4)         */
    uint32_t size;      /* OUT: total bytes mapped, page-aligned   */
};

/* Map the active virtio-gpu scanout buffer's physical pages
 * into the caller's address space, RW from EL0, non-executable.
 * On success returns 0 and populates *out with the user VA,
 * dimensions, stride, and total mapped byte count.
 *
 * First-caller-wins: subsequent callers from other pids see
 * -EBUSY (= -16).  When the holding process exits the kernel
 * automatically releases the slot so a respawn can re-claim.
 * Idempotent for the holding pid.
 *
 * -EAGAIN (= -11) if the framebuffer isn't ready yet (very
 * early boot, or no virtio-gpu present).
 *
 * Intended for /bin/wsd (the window-server daemon).  Other
 * processes that try to call this will find -EBUSY once wsd
 * has claimed the FB, and -EAGAIN if they get in before
 * fb_init completes. */
static inline int fb_map_scanout(struct fb_map_args *out)
{
    if (!out) return -1;
    return (int)_svc1(SYS_FB_MAP_SCANOUT, (long)(uintptr_t)out);
}

/* ── Chapter 108d: per-window shareable framebuffer ── */

/* Layout matches struct win_fb_alloc_args_k in
 * kernel/core/win_fb.c byte-for-byte; do not reorder.  The
 * 4-byte _pad0 keeps `va` 8-aligned across both ILP32 and
 * LP64 hosts (the kernel is LP64; this matters only for
 * struct stability if any tool ever cross-compiles). */
struct win_fb_alloc_args {
    uint32_t w;         /* IN  */
    uint32_t h;         /* IN  */
    uint32_t id;        /* OUT: assigned win-fb id (monotonic) */
    uint32_t _pad0;
    uint64_t va;        /* OUT: caller-AS user VA              */
    uint32_t stride;    /* OUT: bytes per row (= w * 4)        */
    uint32_t size;      /* OUT: total mapped bytes (page-aligned) */
};

/* Layout matches struct win_fb_map_args_k. */
struct win_fb_map_args {
    uint32_t id;        /* IN  */
    uint32_t w;         /* OUT */
    uint32_t h;         /* OUT */
    uint32_t _pad0;
    uint64_t va;        /* OUT */
    uint32_t stride;    /* OUT */
    uint32_t size;      /* OUT */
};

/* Allocate a fresh shareable per-window framebuffer of
 * w*h*4 BGRA bytes (rounded up to a page).  Caller becomes
 * the owner and gets it mapped into its own AS.  On success
 * fills in {id, va, stride, size}.  -ENOMEM if no pages, no
 * mapping room or no table slot; -EINVAL on zero / out-of-
 * range dimensions.
 *
 * The id is the handle to pass to peers (typically: wsd
 * returns it to the client over /srv/wm).  Peers then call
 * win_fb_map(id, ...) to install the same physical pages
 * into their own AS at a fresh VA. */
static inline int win_fb_alloc(uint32_t w, uint32_t h,
                               struct win_fb_alloc_args *out)
{
    if (!out) return -1;
    out->w = w;
    out->h = h;
    return (int)_svc1(SYS_WIN_FB_ALLOC, (long)(uintptr_t)out);
}

/* Install a shareable per-window FB into the caller's AS.
 * `id` is the handle returned by the allocator (and passed
 * through the wm protocol).  On success fills in {va, w, h,
 * stride, size}.  -ENOENT if the id is unknown; -ENOSPC if
 * the per-FB mapping table is full.  Idempotent per AS:
 * calling twice from the same address space returns the
 * cached VA. */
static inline int win_fb_map(uint32_t id, struct win_fb_map_args *out)
{
    if (!out) return -1;
    out->id = id;
    return (int)_svc1(SYS_WIN_FB_MAP, (long)(uintptr_t)out);
}

/* Free a shareable per-window FB.  Owner-only; -EPERM
 * otherwise.  Uninstalls every mapper's user-VA range
 * before freeing the backing pages.  Returns -ENOENT if
 * the id is unknown. */
static inline int win_fb_free(uint32_t id)
{
    return (int)_svc1(SYS_WIN_FB_FREE, (long)id);
}

/* chapter 108e — resize an owned win_fb in place.  Owner-
 * only.  Allocates a fresh backing of new_w*new_h*4 bytes
 * (page-rounded), copies the top-left of the old contents
 * over, then uninstalls EVERY existing mapping (owner +
 * mappers) and frees the old pages.  Owner and mappers
 * must re-call win_fb_map afterwards to get a fresh VA
 * (their old VA is invalid; touching it translation-faults).
 *
 * Errors: -EINVAL (zero or oversized dims), -ENOENT (bad
 * id), -EPERM (not owner), -ENOMEM (alloc of new pages
 * failed).  On any error the FB is unchanged: old pages,
 * old mappings, old dims all intact. */
static inline int win_fb_resize(uint32_t id,
                                uint32_t new_w, uint32_t new_h)
{
    return (int)_svc3(SYS_WIN_FB_RESIZE,
                      (long)id, (long)new_w, (long)new_h);
}

/* Chapter 108d — userspace-driven GPU flush.  Tell
 * the kernel to submit a TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
 * for the given scanout rect.  Passing w=h=0 means "entire
 * scanout".  Used by wsd's compositor; legacy GUI apps don't
 * need it (their kernel-WM path is being retired). */
static inline int fb_present(uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h)
{
    return (int)_svc4(SYS_FB_PRESENT,
                      (long)x, (long)y, (long)w, (long)h);
}

static inline int gui_flush(int id)
{
    return (int)_svc1(SYS_GUI_FLUSH, (long)id);
}

static inline int gui_poll_event(struct gui_event *out)
{
    return (int)_svc1(SYS_GUI_POLL_EVENT, (long)(uintptr_t)out);
}

/* chapter 108e -- userspace decorations + cursor.  All three
 * are wsd-private in practice; nothing else in the tree links
 * against them.  Kept here (and not in a wsd-only header) so
 * any future userspace tool that wants the same hooks (e.g.
 * a debug overlay or screen recorder) can use them without a
 * separate kernel surface.
 *
 * pointer_state(): snapshot of the cursor.  Any out pointer
 *   may be NULL; (x,y) are in scanout pixel coords; buttons
 *   is the GUI_BTN_* bitmap.  Returns 0 / -EFAULT.
 *
 * gui_move_window(): relocate a kernel-WM window (typically a
 *   wsd "input shadow") to (x,y).  Used by wsd after dragging
 *   a title bar so body-click hit-testing lands in the right
 *   place.  Returns 0 / -EINVAL / -ENOENT.
 *
 * gui_deliver_event(): inject a gui_event into a kernel-WM
 *   window's event ring so the owner's next gui_poll_event
 *   returns it.  ev.window_id is overwritten by the kernel
 *   with `id`.  Used by wsd to deliver GUI_EVENT_CLOSE on a
 *   close-button click.  Returns 0 / -EINVAL / -ENOENT /
 *   -EFAULT / -ENOSPC. */
static inline int pointer_state(int32_t *out_x, int32_t *out_y,
                                uint32_t *out_btn)
{
    return (int)_svc3(SYS_POINTER_STATE,
                      (long)(uintptr_t)out_x,
                      (long)(uintptr_t)out_y,
                      (long)(uintptr_t)out_btn);
}

static inline int gui_move_window(int id, int32_t x, int32_t y)
{
    return (int)_svc3(SYS_GUI_MOVE_WINDOW,
                      (long)id, (long)x, (long)y);
}

static inline int gui_deliver_event(int id, const struct gui_event *ev)
{
    return (int)_svc2(SYS_GUI_DELIVER_EVENT,
                      (long)id, (long)(uintptr_t)ev);
}

/* chapter 108e -- turn on/off wsd-routed pointer passthrough on
 * a kernel WM shadow.  See SYS_GUI_SET_INPUT_PASSTHROUGH for the
 * routing semantics; on success the kernel pointer router stops
 * (or resumes) auto-delivering MOUSE events to this shadow.
 * Idempotent. */
static inline int gui_set_input_passthrough(int id, int on)
{
    return (int)_svc2(SYS_GUI_SET_INPUT_PASSTHROUGH,
                      (long)id, (long)on);
}

/* ─── Milestone 47: window list + raise + extended create ─── */

struct gui_create_window_ex_args {
    uint32_t w, h;
    const char *title;
    uint32_t flags;
    int32_t  x, y;
};

struct gui_window_info {
    int32_t  id;
    uint32_t flags;
    int32_t  x, y;
    uint32_t w, h;
    uint32_t z;
    int32_t  focused;
    uint64_t owner_pid;
    char     title[64];
};

static inline int gui_create_window_ex(uint32_t w, uint32_t h,
                                       const char *title,
                                       uint32_t flags,
                                       int32_t x, int32_t y)
{
    struct gui_create_window_ex_args a = {
        .w = w, .h = h, .title = title,
        .flags = flags, .x = x, .y = y,
    };
    return (int)_svc1(SYS_GUI_CREATE_WINDOW_EX, (long)(uintptr_t)&a);
}

static inline int gui_list_windows(struct gui_window_info *out, int max)
{
    return (int)_svc2(SYS_GUI_LIST_WINDOWS,
                      (long)(uintptr_t)out, (long)max);
}

static inline int gui_raise_window(int id)
{
    return (int)_svc1(SYS_GUI_RAISE_WINDOW, (long)id);
}

/* Returns 0 on success, writes the active scanout dimensions
 * (in pixels) into *out_w / *out_h.  Either pointer may be
 * NULL.  Returns -EINVAL if the GUI subsystem isn't ready. */
static inline int gui_get_screen_size(uint32_t *out_w, uint32_t *out_h)
{
    return (int)_svc2(SYS_GUI_GET_SCREEN_SIZE,
                      (long)(uintptr_t)out_w,
                      (long)(uintptr_t)out_h);
}

/* Hide / show a window without destroying it.  Minimized windows
 * are skipped by the compositor (so they don't appear on screen)
 * and by the WM's hit-test (so clicks fall through to whatever
 * is below).  The taskbar still lists the window so the user can
 * click it back into view.  on != 0 hides; on == 0 restores.
 * Restoring a window also raises it to the top and gives it
 * focus, matching what real desktops do. */
static inline int gui_set_minimized(int id, int on)
{
    return (int)_svc2(SYS_GUI_SET_MINIMIZED, (long)id, (long)on);
}

/* ----- Milestone 56: sockets ---------------------------------
 *
 * `socket_connect` opens a TCP connection to `ip4`:`port`,
 * waits for the three-way handshake to complete, and returns a
 * fd you can `read()` and `write()` and `close()` like any
 * other.  `ip4` is packed in network byte order: 10.0.2.2 is
 * `0x0A000202`.  `IP4(a,b,c,d)` is a convenience helper.
 *
 * `socket_shutdown` half-closes (sends FIN) but leaves the fd
 * usable for reads until you `close()` it — letting you drain
 * a server's response after sending your request.
 *
 * `socket_state` returns the kernel's tcp_state enum value.
 * Useful mostly for tests; production code should treat the
 * fd as opaque and rely on read returning 0 (EOF) instead.
 */
#define IP4(a,b,c,d) \
    ((uint32_t)((((uint32_t)(a)) << 24) | (((uint32_t)(b)) << 16) | \
                (((uint32_t)(c)) << 8)  |  ((uint32_t)(d))))

static inline int socket_connect(uint32_t ip4_be, uint16_t port)
{
    return (int)_svc2(SYS_SOCKET_CONNECT, (long)ip4_be, (long)port);
}
static inline int socket_state(int fd)
{
    return (int)_svc1(SYS_SOCKET_STATE, (long)fd);
}
static inline int socket_shutdown(int fd)
{
    return (int)_svc1(SYS_SOCKET_SHUTDOWN, (long)fd);
}

/* ----- Chapter 104: server-side sockets ----------------------
 *
 * `socket_listen(port, backlog)` returns a *listening* fd bound
 * to TCP port `port`.  The listening fd is not readable/
 * writable directly -- pass it to `socket_accept` to harvest
 * fully-handshaken peer connections.  `backlog` is currently
 * advisory (the kernel uses a fixed accept-queue capacity);
 * pick something modest like 4-8.
 *
 * `socket_accept(listen_fd, &peer_ip, &peer_port)` blocks
 * until the next peer finishes its 3-way handshake, then
 * returns a regular FD_SOCKET fd you can read/write/close.
 * Either out-pointer may be NULL if you don't care; the
 * peer IPv4 is delivered packed in network byte order (same
 * shape `socket_connect` takes).
 */
static inline int socket_listen(uint16_t port, int backlog)
{
    return (int)_svc2(SYS_SOCKET_LISTEN, (long)port, (long)backlog);
}
static inline int socket_accept(int listen_fd, uint32_t *peer_ip_be, uint16_t *peer_port)
{
    return (int)_svc3(SYS_SOCKET_ACCEPT,
                      (long)listen_fd,
                      (long)peer_ip_be,
                      (long)peer_port);
}

/* Resolve a hostname to a packed-BE IPv4 address (the same format
 * `socket_connect` expects).  Returns 0 on success, -errno on
 * failure.  The kernel does the actual UDP/53 round-trip; this
 * is just a thin wrapper.  Synchronous: blocks the caller for
 * up to ~3 seconds. */
static inline int resolve(const char *name, uint32_t *out_ip4_be)
{
    return (int)_svc2(SYS_RESOLVE, (long)name, (long)out_ip4_be);
}

/* ----- Chapter 107: named-IPC service bus -------------------
 *
 * A service binds a `/srv/<name>` path with `srv_bind`, then
 * loops over `srv_accept` to harvest one fd per connecting
 * client.  Clients dial a service via `srv_connect`, or
 * equivalently via `open("/srv/<name>", O_RDWR)` (the kernel
 * routes that case through the same path).
 *
 * Wire format: each `write` enqueues exactly one length-
 * prefixed datagram; each `read` returns exactly one.  Max
 * message size is 64 KiB.  If the caller's read buffer is
 * smaller than the next queued message, read returns
 * -EMSGSIZE and the message stays queued -- retry with a
 * bigger buffer.
 *
 * Lifetime: the conn dies when either end calls close().
 * The other end then sees read() return 0 (drain first),
 * write() return -EPIPE.  The listening fd outlives any
 * individual conn -- close it only when the service shuts
 * down for good.
 */
static inline int srv_bind(const char *path)
{
    return (int)_svc1(SYS_SRV_BIND, (long)path);
}
static inline int srv_accept(int listen_fd)
{
    return (int)_svc1(SYS_SRV_ACCEPT, (long)listen_fd);
}
static inline int srv_connect(const char *path)
{
    return (int)_svc1(SYS_SRV_CONNECT, (long)path);
}

#endif
