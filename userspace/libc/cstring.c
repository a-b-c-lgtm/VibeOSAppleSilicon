/* userspace/libc/cstring.c — chapter 112a.
 *
 * Tiny extern-symbol implementations of the libc functions that
 * BearSSL's archive references but our freestanding userspace
 * has historically avoided needing.  Previous binaries got away
 * with the static `memcpy`/`memset`/`memmove` shims in
 * libc/freestanding.h because every translation unit that needed
 * them included that header.  An external .a archive can't —
 * BearSSL's .o files were compiled without our headers, so the
 * link line for any binary that pulls libbearssl.a needs an
 * EXTERN definition somewhere.
 *
 * Five functions cover everything BearSSL uses out of <string.h>
 * (verified by `grep -rho 'mem[a-z]*\|str[a-z]*' vendor/bearssl/`);
 * `time(NULL)` is called in exactly one place
 * (`vendor/bearssl/src/x509/x509_minimal.c`).  We stub `time` to
 * return 0 here so the link succeeds even when something pulls
 * x509_minimal in.  Real cert-expiry validation will set the
 * reference time explicitly via `br_x509_minimal_set_time`
 * (chapter 112c).
 *
 * NOTE: these are NOT constant-time and NOT optimised.  They are
 * the slowest-possible byte-at-a-time implementations, kept tiny
 * because BearSSL's own constant-time primitives don't rely on
 * any property of these stubs other than correctness.  If
 * profiling ever shows AES-GCM bottlenecked on memcpy we can
 * replace them with word-at-a-time versions.
 */

/* This .c file defines EXTERN strlen and time.  The
 * corresponding `static inline` copies in syscall.h are
 * guarded by these macros; setting them up here before any
 * indirect include of syscall.h (e.g. via "malloc.h" below)
 * keeps the build clean. */
#define OSDEV_STRLEN_PROVIDED
#define OSDEV_TIME_PROVIDED

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* strdup — chapter 130a (Doom port).
 *
 * Several DoomGeneric translation units call strdup to copy
 * command-line strings (-iwad path, -file path), so we ship
 * one extern definition here rather than inline it in every
 * TU.  Uses the chapter 131e extern allocator below (same
 * heap as the binutils ld vendor archives), NOT malloc.h's
 * static-inline allocator (which would give a separate
 * per-TU heap and split strdup'd memory from the rest). */
extern void *malloc(size_t);
char *strdup(const char *s)
{
    if (!s) return (char *)0;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) return (char *)0;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return d;
}

/* time() stub.  BearSSL's x509_minimal calls time(NULL) only as a
 * default when no validation-time has been pre-set.  Returning 0
 * means "epoch", which makes every certificate look not-yet-valid;
 * callers that actually want validation MUST call
 * br_x509_minimal_set_time(ctx, days_since_epoch, seconds_in_day)
 * before starting a handshake.  Chapter 112c provides a helper
 * built on top of SYS_GETTIMEOFDAY. */
typedef long time_t;
time_t time(time_t *out)
{
    if (out) *out = 0;
    return 0;
}

/* chapter 112b: br_prng_seeder_system() stub.
 *
 * BearSSL's ssl_engine.c calls this from br_ssl_engine_init_rand
 * to find an OS-provided entropy source.  The real implementation
 * lives in vendor/bearssl/src/rand/sysrng.c -- which we deliberately
 * exclude from the build (see Makefile BEARSSL_SRCS) because it
 * probes /dev/urandom, getentropy, and CryptGenRandom, none of
 * which exist in our freestanding userspace.
 *
 * Returning NULL ("no seeder available") is the documented BearSSL
 * contract for "the caller MUST call br_ssl_engine_inject_entropy
 * before the first reset()".  tls_socket.c and httpsd.c both do
 * exactly that, pulling 64 bytes from SYS_GETRANDOM (chapter 112's
 * kernel CSPRNG, seeded from /dev/urandom on the host via the
 * virtio-rng device in the QEMU command line).
 *
 * The signature must match exactly -- br_prng_seeder is a function
 * pointer typedef'd in bearssl_rand.h.  We declare its return type
 * locally as a void(*)() to avoid pulling bearssl_rand.h into
 * cstring.c; the linker only cares about the symbol name. */
typedef int (*br_prng_seeder_fn)(void **ctx);  /* approximate */
br_prng_seeder_fn br_prng_seeder_system(const char **name)
{
    if (name) *name = "none";
    return 0;
}

/* ── Chapter 128c — __assert_fail ───────────────────────────────
 *
 * Implementation behind the assert() macro in
 * userspace/libc/assert.h.  Writes a fixed-shape diagnostic to
 * fd 2 and then calls _Exit-like syscall machinery directly --
 * we can't include signal.h here (cstring.c is a leaf TU that
 * BearSSL pulls in) without dragging the whole signal trampoline
 * setup into binaries that don't otherwise need it.
 *
 * Diagnostic shape (matches glibc / musl conventions closely
 * enough that test scripts grepping for it can use the same
 * regex):
 *
 *     <file>:<line>: <func>: Assertion `<expr>' failed.
 *
 * Termination: SYS_EXIT with code 134 (== 128 + SIGABRT).  This
 * is what a "real" abort() would produce after the kernel's
 * default action; calling exit() directly here skips the
 * signal-raise / handler / sigreturn dance, which is the right
 * thing for an assertion failure -- we want to die NOW, not
 * give a user handler the chance to swallow the failure.
 */

/* Inline svc trampolines.  We re-emit the asm here rather than
 * include syscall.h because cstring.c must compile in
 * environments (e.g. linked beside BearSSL's archive) where
 * including the full syscall header would drag in unrelated
 * declarations. */
#ifndef SYS_WRITE
# define SYS_WRITE 1
#endif
#ifndef SYS_EXIT
# define SYS_EXIT  2
#endif

static inline long __cstring_svc1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long __cstring_svc3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void __assert_write(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    __cstring_svc3(SYS_WRITE, 2, (long)(uintptr_t)s, (long)n);
}

static void __assert_write_int(long v)
{
    char buf[24];
    int  i = 0;
    int  neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) buf[i++] = '-';
    /* reverse */
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    __cstring_svc3(SYS_WRITE, 2, (long)(uintptr_t)buf, (long)i);
}

__attribute__((noreturn))
void __assert_fail(const char *expr,
                   const char *file,
                   int line,
                   const char *func)
{
    __assert_write(file ? file : "?");
    __assert_write(":");
    __assert_write_int(line);
    __assert_write(": ");
    __assert_write(func ? func : "?");
    __assert_write(": Assertion `");
    __assert_write(expr ? expr : "?");
    __assert_write("' failed.\n");
    __cstring_svc1(SYS_EXIT, 134);   /* 128 + SIGABRT */
    /* Unreachable. */
    for (;;) { }
}

/* ── Chapter 131e — extern wrappers for vendor archives ─────────
 *
 * binutils ld pulls in libiberty's strdup.c / vasprintf.c /
 * objalloc.c etc.  Each of those carries its own
 * `extern void *malloc(size_t);`, `extern void free(void *);`,
 * etc.  Our libc puts those as `static inline` in malloc.h /
 * signal.h, which gives every-other-TU its own private copy and
 * NEVER emits an external symbol, so the libiberty TUs resolve
 * to "undefined reference to malloc" at link time.
 *
 * Fix: provide strong extern symbols here.  This file does NOT
 * include malloc.h — we'd get a duplicate-symbol error at
 * assembly time between malloc.h's static-inline malloc (emitted
 * as a file-local symbol "malloc") and our `__asm__("malloc")`
 * extern wrapper.  Instead this TU carries its own self-
 * contained allocator (the same shape as malloc.h's, ~20 lines)
 * so the extern malloc owns its heap end-to-end.
 *
 * The two heaps in the same process — malloc.h's per-TU static
 * inline, and cstring.o's extern — never share a free()'d
 * pointer because no in-tree app mixes the two paths: vendor
 * archives always call extern malloc/free; our own apps always
 * inline the static malloc/free.  strdup above sits on cstring's
 * side via the forward-declared extern malloc, so a vendor TU
 * that strdup's a string and free's it later stays on the same
 * heap throughout.
 *
 * Other functions (strcmp, abort, exit, …) are simple wrappers
 * that re-implement the body locally rather than forward to the
 * static-inline copies, again to avoid the asm-name collision. */

/* ── Self-contained allocator ──────────────────────────────── */
struct _extern_blk { size_t size; struct _extern_blk *next; };
#define _EXTERN_HDR  ((size_t)sizeof(size_t))
#define _EXTERN_ALIGN ((size_t)16)
#define _EXTERN_GROW (16u * 1024u)
static struct _extern_blk *_extern_free_head = (struct _extern_blk *)0;

#ifndef SYS_SBRK
# define SYS_SBRK 11
#endif

static void *_extern_sbrk(long delta)
{
    return (void *)(uintptr_t)__cstring_svc1(SYS_SBRK, delta);
}

static size_t _extern_round(size_t want)
{
    size_t total = want + _EXTERN_HDR;
    if (total < 16 + _EXTERN_HDR) total = 16 + _EXTERN_HDR;
    return (total + _EXTERN_ALIGN - 1) & ~(_EXTERN_ALIGN - 1);
}

static int _extern_grow(size_t need)
{
    size_t chunk = need < _EXTERN_GROW ? _EXTERN_GROW : need;
    chunk = (chunk + _EXTERN_ALIGN - 1) & ~(_EXTERN_ALIGN - 1);
    void *p = _extern_sbrk((long)chunk);
    if ((long)(uintptr_t)p < 0) return -1;
    struct _extern_blk *b = (struct _extern_blk *)p;
    b->size = chunk;
    b->next = _extern_free_head;
    _extern_free_head = b;
    return 0;
}

void *__cstring_malloc(size_t want) __asm__("malloc");
void *__cstring_malloc(size_t want)
{
    if (want == 0) return (void *)0;
    size_t need = _extern_round(want);
    for (int attempt = 0; attempt < 2; attempt++) {
        struct _extern_blk **pp = &_extern_free_head;
        while (*pp) {
            struct _extern_blk *b = *pp;
            if (b->size >= need) {
                size_t leftover = b->size - need;
                if (leftover >= 32) {
                    struct _extern_blk *tail =
                        (struct _extern_blk *)((char *)b + need);
                    tail->size = leftover;
                    tail->next = b->next;
                    b->size    = need;
                    *pp = tail;
                } else {
                    *pp = b->next;
                }
                return (char *)b + _EXTERN_HDR;
            }
            pp = &b->next;
        }
        if (_extern_grow(need) != 0) return (void *)0;
    }
    return (void *)0;
}

void __cstring_free(void *p) __asm__("free");
void __cstring_free(void *p)
{
    if (!p) return;
    if ((uintptr_t)p < 0x10000u) return;
    struct _extern_blk *b = (struct _extern_blk *)((char *)p - _EXTERN_HDR);
    struct _extern_blk **pp = &_extern_free_head;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    if (b->next && (char *)b + b->size == (char *)b->next) {
        b->size += b->next->size;
        b->next  = b->next->next;
    }
}

void *__cstring_calloc(size_t n, size_t sz) __asm__("calloc");
void *__cstring_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (n != 0 && total / n != sz) return (void *)0;
    void *p = __cstring_malloc(total);
    if (!p) return p;
    unsigned char *q = (unsigned char *)p;
    for (size_t i = 0; i < total; i++) q[i] = 0;
    return p;
}

void *__cstring_realloc(void *p, size_t n) __asm__("realloc");
void *__cstring_realloc(void *p, size_t n)
{
    if (!p) return __cstring_malloc(n);
    if (n == 0) { __cstring_free(p); return (void *)0; }
    struct _extern_blk *b = (struct _extern_blk *)((char *)p - _EXTERN_HDR);
    size_t old_payload = b->size - _EXTERN_HDR;
    size_t copy = (old_payload < n) ? old_payload : n;
    void *q = __cstring_malloc(n);
    if (!q) return (void *)0;
    const unsigned char *s = (const unsigned char *)p;
    unsigned char       *d = (unsigned char *)q;
    for (size_t i = 0; i < copy; i++) d[i] = s[i];
    __cstring_free(p);
    return q;
}

/* ── abort / exit ─────────────────────────────────────────── */
__attribute__((noreturn)) void __cstring_abort(void) __asm__("abort");
__attribute__((noreturn)) void __cstring_abort(void)
{
    /* Diagnostic: surface abort() with the call-site LR.  Until
     * chapter 132f the cstring allocator silently mis-numbered
     * SYS_SBRK as SYS_GETARGS and abort() in operator new fired
     * with no clue — this one-line write makes the call site
     * obvious from `addr2line` on the LR value. */
    static const char msg[] = "*** abort() from LR=0x";
    __cstring_svc3(SYS_WRITE, 2, (long)(uintptr_t)msg, (long)(sizeof msg - 1));
    {
        unsigned long lr;
        __asm__ volatile("mov %0, x30" : "=r"(lr));
        char buf[18];
        int  i = 0;
        for (int shift = 60; shift >= 0; shift -= 4) {
            unsigned nyb = (unsigned)((lr >> shift) & 0xF);
            buf[i++] = (char)(nyb < 10 ? '0' + nyb : 'a' + nyb - 10);
        }
        buf[i++] = '\n';
        __cstring_svc3(SYS_WRITE, 2, (long)(uintptr_t)buf, (long)i);
    }
    __cstring_svc1(SYS_EXIT, 134); /* 128 + SIGABRT */
    for (;;) { }
}

__attribute__((noreturn)) void __cstring_exit(int code) __asm__("exit");
__attribute__((noreturn)) void __cstring_exit(int code)
{
    __cstring_svc1(SYS_EXIT, code);
    for (;;) { }
}

/* ── String functions ─────────────────────────────────────── */
int __cstring_strcmp(const char *a, const char *b) __asm__("strcmp");
int __cstring_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int __cstring_strncmp(const char *a, const char *b, size_t n) __asm__("strncmp");
int __cstring_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *__cstring_strcpy(char *d, const char *s) __asm__("strcpy");
char *__cstring_strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

char *__cstring_strncpy(char *d, const char *s, size_t n) __asm__("strncpy");
char *__cstring_strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && (*d++ = *s++)) { n--; }
    while (n--) *d++ = '\0';
    return r;
}

char *__cstring_strcat(char *d, const char *s) __asm__("strcat");
char *__cstring_strcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) { }
    return r;
}

char *__cstring_strchr(const char *s, int c) __asm__("strchr");
char *__cstring_strchr(const char *s, int c)
{
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return (c == 0) ? (char *)s : (char *)0;
}

char *__cstring_strrchr(const char *s, int c) __asm__("strrchr");
char *__cstring_strrchr(const char *s, int c)
{
    const char *last = (c == 0) ? s : (const char *)0;
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) last = s;
    return (char *)last;
}

char *__cstring_strstr(const char *h, const char *n) __asm__("strstr");
char *__cstring_strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return (char *)0;
}

/* ── Weak `environ` for vendor builds ─────────────────────────
 * The binutils ld build compiles vendor TUs with
 * -DOSDEV_LIBC_NO_GLOBAL_DEFS so env.h does not emit a per-TU
 * `environ` (avoids ~150 multi-def errors).  ld's lexsup.c
 * still references `extern char **environ;`, so we provide one
 * weak slot here.  Apps that pull in env.h normally get the
 * strong def from env.h and override this. */
__attribute__((weak)) char **environ = (char **)0;

/* ── Chapter 132e — extern POSIX I/O syscalls ─────────────────
 * Cross-built autoconfs (gmp, mpfr, mpc, gcc) compile conftest
 * snippets that reference `open`, `close`, `read`, `write`,
 * `lseek` without including any libc header.  Without an extern
 * definition the link fails, so autoconf records "compiler
 * doesn't work" and aborts.  The static inlines in syscall.h
 * still resolve first for code that #includes it; the wrappers
 * below are pulled from libosdevc.a only when something
 * references the bare symbol.
 *
 * Syscall numbers (chapter 116 enum): WRITE=1, OPEN=5, READ=6,
 * CLOSE=7, LSEEK=101.  These match userspace/libc/syscall.h. */
#define SYS_OPEN_NR   5
#define SYS_READ_NR   6
#define SYS_CLOSE_NR  7
#define SYS_LSEEK_NR  101

static inline long __cstring_svc2(long n, long a, long b)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

int __cstring_open(const char *name, int flags, ...) __asm__("open");
int __cstring_open(const char *name, int flags, ...)
{
    return (int)__cstring_svc2(SYS_OPEN_NR,
                               (long)(uintptr_t)name,
                               (long)flags);
}

int __cstring_close(int fd) __asm__("close");
int __cstring_close(int fd)
{
    return (int)__cstring_svc1(SYS_CLOSE_NR, (long)fd);
}

long __cstring_read(int fd, void *buf, size_t len) __asm__("read");
long __cstring_read(int fd, void *buf, size_t len)
{
    return __cstring_svc3(SYS_READ_NR, (long)fd,
                          (long)(uintptr_t)buf, (long)len);
}

long __cstring_write(int fd, const void *buf, size_t len) __asm__("write");
long __cstring_write(int fd, const void *buf, size_t len)
{
    return __cstring_svc3(SYS_WRITE, (long)fd,
                          (long)(uintptr_t)buf, (long)len);
}

long __cstring_lseek(int fd, long off, int whence) __asm__("lseek");
long __cstring_lseek(int fd, long off, int whence)
{
    return __cstring_svc3(SYS_LSEEK_NR, (long)fd,
                          (long)off, (long)whence);
}

/* ---------------------------------------------------------------
 * Chapter 132f — soft-float TFmode (binary128 / `long double` on
 * aarch64) helper trap-stubs.
 *
 * Aarch64's default `long double` is 128-bit IEEE binary128.  GCC
 * lowers TFmode arithmetic to libgcc soft-float helpers
 * (`__multf3`, `__divtf3`, `__gttf2`, `__trunctfdf2`, ...).  Our
 * userspace/libc/libgcc.h shim ships only the integer helpers
 * (popcount/clz/ctz/SSP); TF support would be ~24 functions of
 * IEEE 128-bit emulation, several hundred lines of bit-fiddling.
 *
 * We don't need that for what xgcc actually does.  The traps
 * exist so the *link* succeeds — specifically, gcc's own
 * configure runs an MPC link-probe that statically links
 * `libmpc.a(set_ld.o)`, which references the full TF helper set
 * even though the probe only calls `mpc_init2`/`cosh`/`pow`/etc.
 * (none of which need long double).  Without these stubs the
 * probe ld-errs with ~30 "undefined reference to __XXXtf2"
 * lines and gcc's configure aborts.
 *
 * If GCC's frontend ever actually *evaluates* a `long double`
 * constant at compile time, the stub triggers abort().  We've
 * documented this in chapter 132f as a known limitation: cross-
 * compiling C code that uses `long double` constants will fail
 * at compile time inside our xgcc.  Real binary128 emulation can
 * land later as its own chapter — see `vendor/gcc-14.2.0/libgcc/
 * soft-fp/` for the canonical implementation, which is what we'd
 * port if/when the limitation actually bites a target program.
 *
 * Symbol surface drawn from
 * `vendor/gcc-14.2.0/libgcc/config/aarch64/sfp-machine.h` and
 * the IEEE-binary128 entries in `libgcc/Makefile.in`.  All bodies
 * are identical: print a one-line marker and abort.  The
 * signatures use `long` for arg/return to satisfy the linker
 * without dragging in a real `__float128` typedef (which would
 * require `__FLT128_MAX__` machinery).  Aarch64 ABI passes TF
 * values in v0/v1 vector regs anyway, so the trap doesn't read
 * them — it just aborts on entry. */

__attribute__((noreturn))
static void __cstring_tf_trap(const char *name)
{
    static const char head[] = "*** soft-float TFmode helper '";
    static const char tail[] = "' called -- not implemented "
                               "(chapter 132f); aborting ***\n";
    (void)__cstring_write(2, head, sizeof(head) - 1);
    {
        const char *p = name;
        long n = 0;
        while (p[n]) n++;
        (void)__cstring_write(2, name, (size_t)n);
    }
    (void)__cstring_write(2, tail, sizeof(tail) - 1);
    __cstring_abort();
}

#define _TF_TRAP(sym)                                            \
    long sym(long a, long b, long c, long d);                    \
    long sym(long a, long b, long c, long d) {                   \
        (void)a; (void)b; (void)c; (void)d;                      \
        __cstring_tf_trap(#sym);                                 \
    }

/* Arithmetic (binary128) */
_TF_TRAP(__addtf3)
_TF_TRAP(__subtf3)
_TF_TRAP(__multf3)
_TF_TRAP(__divtf3)
_TF_TRAP(__negtf2)

/* Comparison (binary128) */
_TF_TRAP(__eqtf2)
_TF_TRAP(__netf2)
_TF_TRAP(__lttf2)
_TF_TRAP(__letf2)
_TF_TRAP(__gttf2)
_TF_TRAP(__getf2)
_TF_TRAP(__unordtf2)

/* Conversions: smaller -> TF */
_TF_TRAP(__extendsftf2)
_TF_TRAP(__extenddftf2)
_TF_TRAP(__floatsitf)
_TF_TRAP(__floatditf)
_TF_TRAP(__floatunsitf)
_TF_TRAP(__floatunditf)

/* Conversions: TF -> smaller / int */
_TF_TRAP(__trunctfsf2)
_TF_TRAP(__trunctfdf2)
_TF_TRAP(__fixtfsi)
_TF_TRAP(__fixtfdi)
_TF_TRAP(__fixunstfsi)
_TF_TRAP(__fixunstfdi)

#undef _TF_TRAP
