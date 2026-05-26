/* userspace/libc/env.h — POSIX environ + arena (chapter 116c).
 *
 * Header-only, single-translation-unit pattern (same as printf.h
 * / malloc.h / stdio.h).  Include from the one `.c` of each
 * binary; each binary gets its own `environ[]`, arena, and
 * cached view of the kernel-side env block.
 *
 * What this header gives you (the POSIX surface every UNIX
 * program written since 1990 expects):
 *
 *   extern char **environ;
 *   char *getenv(const char *name);
 *   int   setenv(const char *name, const char *value, int overwrite);
 *   int   unsetenv(const char *name);
 *   int   putenv(char *str);          // takes ownership of `str`
 *   int   clearenv(void);
 *
 *   `getenv` returns a pointer that stays valid until the next
 *   setenv/unsetenv/clearenv against the *same* key, exactly per
 *   POSIX §8.2.  No more "copy into a caller buffer" dance.
 *
 * What sits behind these calls:
 *
 *   - A 16 KiB byte arena (`g_env_arena`) packed full of
 *     NUL-terminated "KEY=VAL" entries.
 *   - A `char *g_envv[ENV_MAX_ENTRIES + 1]` array of pointers
 *     into the arena, NULL-terminated; `environ` aliases its
 *     base.
 *   - Lazy init on first env.h call: pull the whole env blob
 *     from the kernel via `__sys_getenv_all`, then split into
 *     arena entries.
 *   - Every mutation (setenv / unsetenv / putenv / clearenv)
 *     writes through to the kernel via `__sys_setenv` /
 *     `__sys_unsetenv` so child processes inherit the change.
 *     If you only mutate via env.h, the cache and the kernel
 *     stay in lockstep.  Bypassing env.h (calling `__sys_setenv`
 *     directly) WILL desync the cache; the workaround is to call
 *     `clearenv()` and re-init.
 *
 * Why a userspace arena at all when the kernel already stores
 * env?  Because the POSIX getenv signature returns `char *`, and
 * that pointer has to remain stable across calls.  The kernel
 * copies into caller buffers; we can't return a kernel pointer
 * across the syscall boundary.  So we cache in userspace and
 * write through.
 *
 * Concurrency: not safe across threads.  The chapter 116 plan
 * defers thread-safe env to 116d (along with per-thread errno).
 */
#ifndef USER_ENV_H
#define USER_ENV_H

#include <stdint.h>
#include <stddef.h>

#include "syscall.h"
#include "errno.h"
#include "malloc.h"

#ifndef ENV_ARENA_SIZE
#define ENV_ARENA_SIZE   (16 * 1024)
#endif

#ifndef ENV_MAX_ENTRIES
#define ENV_MAX_ENTRIES  256
#endif

/* The arena and entry array.  Backing storage for `environ`. */
static char  g_env_arena[ENV_ARENA_SIZE];
static size_t g_env_arena_used;
static char *g_env_envv[ENV_MAX_ENTRIES + 1];
static int   g_env_count;
static int   g_env_inited;

/* `environ` is the canonical POSIX exported symbol.
 *
 * Strong global, guarded by OSDEV_LIBC_NO_GLOBAL_DEFS (chapter
 * 130a) for the Doom shim path that explicitly suppresses the
 * per-TU def.  Multi-TU vendor builds (binutils ld: ~150 .o
 * files all carrying <stdlib.h>) take the same path — their
 * CFLAGS sets -DOSDEV_LIBC_NO_GLOBAL_DEFS and cstring.o
 * provides a single weak `environ` that satisfies any vendor
 * extern reference. */
#ifndef OSDEV_LIBC_NO_GLOBAL_DEFS
char **environ = g_env_envv;
#endif

/* --- internal helpers ----------------------------------------------- */

static inline size_t _env_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int _env_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

/* Find the index in g_env_envv of the entry whose key matches
 * `name` (must not contain '=').  Returns -1 if not present. */
static inline int _env_find(const char *name)
{
    size_t nlen = _env_strlen(name);
    for (int i = 0; i < g_env_count; i++) {
        const char *e = g_env_envv[i];
        if (!e) continue;
        if (_env_strncmp(e, name, nlen) == 0 && e[nlen] == '=') {
            return i;
        }
    }
    return -1;
}

/* Append a fresh "KEY=VAL\0" record into the arena and return a
 * pointer to it.  Returns NULL on arena overflow. */
static inline char *_env_arena_add(const char *name, const char *val)
{
    size_t nlen = _env_strlen(name);
    size_t vlen = _env_strlen(val);
    size_t need = nlen + 1 + vlen + 1;     /* "name=val\0" */
    if (g_env_arena_used + need > ENV_ARENA_SIZE) return (char *)0;

    char *p = g_env_arena + g_env_arena_used;
    for (size_t i = 0; i < nlen; i++) p[i] = name[i];
    p[nlen] = '=';
    for (size_t i = 0; i < vlen; i++) p[nlen + 1 + i] = val[i];
    p[nlen + 1 + vlen] = '\0';
    g_env_arena_used += need;
    return p;
}

/* Append a raw "KEY=VAL" string (must already have an '='); used
 * by putenv and by the lazy-init splitter.  Returns a pointer into
 * the arena, or NULL on overflow. */
static inline char *_env_arena_add_raw(const char *s)
{
    size_t len = _env_strlen(s);
    size_t need = len + 1;
    if (g_env_arena_used + need > ENV_ARENA_SIZE) return (char *)0;
    char *p = g_env_arena + g_env_arena_used;
    for (size_t i = 0; i < len; i++) p[i] = s[i];
    p[len] = '\0';
    g_env_arena_used += need;
    return p;
}

/* Lazy init: walk the kernel's packed env blob and slice it into
 * the arena + envv.  Idempotent; cheap to call.
 *
 * Reads the kernel blob DIRECTLY into g_env_arena (no intermediate
 * stack buffer) — chapter 132f.  Earlier versions of this code
 * declared `char tmp[ENV_ARENA_SIZE]` on the stack and copied
 * through it, which put a 16 KiB allocation on the user stack
 * (USER_STACK_PAGES is 16 → 64 KiB total).  For xgcc, whose first
 * libc call is `getenv("GCC_EXEC_PREFIX")` from deep inside
 * `process_command()` with several MiB of GCC vendor frames
 * already on the stack, the resulting `sub sp, sp, #0x4000`
 * silently overwrote argv strings at the top of the stack page.
 * Writing into the static arena directly side-steps that.  */
static inline void _env_init(void)
{
    if (g_env_inited) return;
    g_env_inited = 1;

    long n = __sys_getenv_all(g_env_arena, ENV_ARENA_SIZE);
    if (n <= 0) {
        g_env_envv[0] = (char *)0;
        return;
    }

    /* The kernel writes "K=V\0K=V\0...\0\0" (final extra NUL is
     * the end-of-blob marker).  Treat its bytes [0, n) as the
     * packed payload, drop the trailing extra NUL, then walk
     * entries by stepping past each inner '\0'.  Pointers go
     * directly into the arena; no copy needed.  */
    size_t total = (size_t)n;
    if (total > 0 && g_env_arena[total - 1] == '\0') total--;
    g_env_arena_used = total;

    size_t cursor = 0;
    while (cursor < total && g_env_count < ENV_MAX_ENTRIES) {
        char *entry = g_env_arena + cursor;
        size_t elen = _env_strlen(entry);
        if (elen == 0) { cursor++; continue; }
        g_env_envv[g_env_count++] = entry;
        cursor += elen + 1;
    }
    g_env_envv[g_env_count] = (char *)0;
}

/* --- public API ----------------------------------------------------- */

static inline char *getenv(const char *name)
{
    if (!name || *name == '\0') return (char *)0;
    _env_init();
    int i = _env_find(name);
    if (i < 0) return (char *)0;
    char *e = g_env_envv[i];
    while (*e && *e != '=') e++;
    if (*e == '=') return e + 1;
    return (char *)0;
}

static inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || *name == '\0' || !value) {
        errno = EINVAL;
        return -1;
    }
    /* POSIX: name may not contain '='. */
    for (const char *p = name; *p; p++) {
        if (*p == '=') { errno = EINVAL; return -1; }
    }
    _env_init();

    int idx = _env_find(name);
    if (idx >= 0 && !overwrite) return 0;

    char *entry = _env_arena_add(name, value);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }

    if (idx >= 0) {
        g_env_envv[idx] = entry;
    } else {
        if (g_env_count >= ENV_MAX_ENTRIES) {
            errno = ENOMEM;
            return -1;
        }
        g_env_envv[g_env_count++] = entry;
        g_env_envv[g_env_count] = (char *)0;
    }

    /* Write through to the kernel so children inherit. */
    int rc = __sys_setenv(name, value);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return 0;
}

static inline int unsetenv(const char *name)
{
    if (!name || *name == '\0') { errno = EINVAL; return -1; }
    for (const char *p = name; *p; p++) {
        if (*p == '=') { errno = EINVAL; return -1; }
    }
    _env_init();
    int idx = _env_find(name);
    if (idx >= 0) {
        for (int j = idx; j < g_env_count - 1; j++)
            g_env_envv[j] = g_env_envv[j + 1];
        g_env_count--;
        g_env_envv[g_env_count] = (char *)0;
    }
    int rc = __sys_unsetenv(name);
    /* -ENOENT from the kernel is fine -- POSIX says unsetenv on
     * a missing name is success. */
    if (rc < 0 && rc != -2) {
        errno = -rc;
        return -1;
    }
    return 0;
}

/* POSIX putenv: takes ownership of `str` (caller may NOT free it
 * or modify it).  We copy into the arena to match real-libc
 * behaviour of stability across re-entry; the original `str` is
 * forgotten.  The "ownership" word in the spec exists so callers
 * don't pass stack-allocated strings expecting libc to copy them
 * -- we copy anyway, but the contract still holds. */
static inline int putenv(char *str)
{
    if (!str) { errno = EINVAL; return -1; }
    /* Find the '=' delimiter; reject strings without it. */
    char *eq = (char *)0;
    for (char *p = str; *p; p++) {
        if (*p == '=') { eq = p; break; }
    }
    if (!eq || eq == str) { errno = EINVAL; return -1; }

    /* Split for the kernel write-through. */
    *eq = '\0';
    const char *key = str;
    const char *val = eq + 1;
    int rc = setenv(key, val, 1);
    *eq = '=';
    return rc;
}

static inline int clearenv(void)
{
    _env_init();
    /* Walk a copy of the current keys so unsetenv()'s in-place
     * compaction doesn't invalidate our iteration. */
    char names[ENV_MAX_ENTRIES][64];
    int n = 0;
    for (int i = 0; i < g_env_count && n < ENV_MAX_ENTRIES; i++) {
        const char *e = g_env_envv[i];
        if (!e) continue;
        size_t k = 0;
        while (e[k] && e[k] != '=' && k < 63) {
            names[n][k] = e[k];
            k++;
        }
        names[n][k] = '\0';
        n++;
    }
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (unsetenv(names[i]) != 0) rc = -1;
    }
    /* Reset arena bookkeeping so future setenvs reclaim space. */
    g_env_arena_used = 0;
    g_env_count = 0;
    g_env_envv[0] = (char *)0;
    return rc;
}

#endif /* USER_ENV_H */
