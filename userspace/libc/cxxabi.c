/* userspace/libc/cxxabi.c — chapter 186
 *
 * Minimal Itanium-C++-ABI stubs needed to LINK programs that have
 * static local C++ objects (cc1 / cc1plus / xgcc).  These end up
 * in libosdevc.a (the wrapper auto-pulls it into every link).
 *
 * SAFETY: cc1 in our cross-build is single-threaded.  These stubs
 * are NOT thread-safe; if the guest ever runs cc1 with threads the
 * guard logic must grow real atomic acquire/release.
 *
 * The four symbols below cover the calls gcc-14.2.0's C++
 * codegen emits for:
 *   - function-local static initialisation (`static T x = init();`)
 *     -> __cxa_guard_acquire / __cxa_guard_release / __cxa_guard_abort
 *   - file-scope object destructors (`namespace { Foo f; }`)
 *     -> __cxa_atexit, registered against __dso_handle
 */

typedef long long __guard;  /* matches generic libstdc++ cxxabi_tweaks.h */

/* The first byte of the guard is the "initialized" flag, the
 * second byte is "in progress" (recursive-init detection).  */

int __cxa_guard_acquire(__guard *g) {
    /* Already initialised — caller skips init. */
    if (*(volatile char *)g) return 0;
    /* Already in progress — recursive init = abort.  In the
     * single-threaded cc1 this should never happen. */
    if (((volatile char *)g)[1]) __builtin_trap();
    ((volatile char *)g)[1] = 1;  /* mark in-progress */
    return 1;                     /* caller proceeds with init */
}

void __cxa_guard_release(__guard *g) {
    ((volatile char *)g)[1] = 0;  /* clear in-progress */
    *(volatile char *)g = 1;      /* mark initialised */
}

void __cxa_guard_abort(__guard *g) {
    ((volatile char *)g)[1] = 0;  /* clear in-progress, leave init=0 */
}

/* __cxa_atexit: register a destructor to run at program exit.
 * cc1 effectively never exits cleanly in our usage (it does its
 * work then process exit; the kernel reaps the address space).
 * Recording nothing is safe: the destructors would only free
 * memory we're about to drop anyway. */
int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    (void)func; (void)arg; (void)dso;
    return 0;
}

/* __dso_handle is a magic symbol the C++ ABI uses to identify
 * "this shared object."  For a static executable any unique
 * address will do; the linker resolves &__dso_handle to it. */
void *__dso_handle = (void *)&__dso_handle;

/* ── operator new / operator delete ─────────────────────────────
 * Out-of-line so all TUs share a single definition, instead of
 * each TU emitting its own `_Znwm` comdat with malloc inlined
 * inside.  If two TUs disagree on which comdat copy of `_Znwm`
 * survives, the kept copy's inlined static malloc may be the
 * only emitted definition of `malloc` — ld discarding the loser
 * group also discards that malloc, breaking unrelated callers.
 * Funnelling through extern malloc/free here means there's
 * always one canonical malloc symbol (defined in cstring.c via
 * the __asm__("malloc") rename).  See chapter 186 notes.  */
typedef unsigned long __cxxabi_size_t;
extern void *malloc(__cxxabi_size_t);
extern void  free(void *);
extern void  abort(void) __attribute__((noreturn));

void *_Znwm(__cxxabi_size_t n) {            /* operator new(size_t) */
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
void *_Znam(__cxxabi_size_t n) {            /* operator new[](size_t) */
    return _Znwm(n);
}
void *_ZnwmRKSt9nothrow_t(__cxxabi_size_t n, const void *) {
    return malloc(n ? n : 1);
}
void *_ZnamRKSt9nothrow_t(__cxxabi_size_t n, const void *) {
    return malloc(n ? n : 1);
}
void _ZdlPv(void *p) { free(p); }                       /* operator delete(void*) */
void _ZdaPv(void *p) { free(p); }                       /* operator delete[](void*) */
void _ZdlPvm(void *p, __cxxabi_size_t) { free(p); }     /* operator delete(void*, size_t) */
void _ZdaPvm(void *p, __cxxabi_size_t) { free(p); }     /* operator delete[](void*, size_t) */
void _ZdlPvRKSt9nothrow_t(void *p, const void *) { free(p); }
void _ZdaPvRKSt9nothrow_t(void *p, const void *) { free(p); }
