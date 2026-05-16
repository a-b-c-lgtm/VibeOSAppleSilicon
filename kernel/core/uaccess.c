/*
 * kernel/core/uaccess.c — implementation of the user/kernel
 * pointer-copy primitives declared in uaccess.h.
 *
 * Today the implementation is the bare minimum: bounds-check the
 * range against [USER_VA_BASE, USER_VA_END), then memcpy / strncpy
 * relying on the active TTBR0 to translate.  No fault recovery
 * yet — see uaccess.h for the explanation and the deferred plan.
 */

#include "uaccess.h"
#include "../arch/address_space.h"
#include "thread.h"

#define UACCESS_PAGE_SIZE 4096u

static int range_in_user(uint64_t uptr, size_t len)
{
    /* Length 0: still require uptr itself to be in range so we
     * don't accept a NULL "buffer" as legitimate. */
    if (uptr < USER_VA_BASE || uptr >= USER_VA_END)
        return 0;
    /* Catch integer overflow: uptr + len wraps around. */
    if (len > USER_VA_END)
        return 0;
    if (uptr + len > USER_VA_END)
        return 0;
    return 1;
}

int uaccess_check(uint64_t uptr, size_t len)
{
    return range_in_user(uptr, len) ? 0 : -EFAULT;
}

int copy_from_user(void *kdst, uint64_t uptr, size_t len)
{
    if (!range_in_user(uptr, len))
        return -EFAULT;
    /* Active TTBR0 IS the user's AS, so a plain memcpy through
     * the user VA goes through user mappings. */
    const uint8_t *src = (const uint8_t *)(uintptr_t)uptr;
    uint8_t       *dst = (uint8_t *)kdst;
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
    return 0;
}

int copy_to_user(uint64_t uptr, const void *ksrc, size_t len)
{
    if (!range_in_user(uptr, len))
        return -EFAULT;

    /* Chapter 75 \u2014 every page touched by the write must be
     * writable in the active AS.  Forked-but-unmodified user
     * pages are mapped RO + DESC_SW_COW, and AArch64 RO
     * permissions apply to EL1 too: a kernel memcpy through a
     * user VA into one of those pages would itself fault.  Walk
     * the destination range page-by-page and force COW resolution
     * on each shared page first.  Already-writable pages are a
     * no-op; genuinely-RO mappings (program text) cause -EFAULT. */
    if (len > 0) {
        struct thread *t = thread_current();
        if (t && t->as) {
            uint64_t first = uptr & ~(uint64_t)(UACCESS_PAGE_SIZE - 1);
            uint64_t last  = (uptr + len - 1) & ~(uint64_t)(UACCESS_PAGE_SIZE - 1);
            for (uint64_t p = first; p <= last; p += UACCESS_PAGE_SIZE) {
                if (address_space_make_writable(t->as, p) < 0)
                    return -EFAULT;
            }
        }
    }

    uint8_t       *dst = (uint8_t *)(uintptr_t)uptr;
    const uint8_t *src = (const uint8_t *)ksrc;
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
    return 0;
}

long ustrnlen(uint64_t uptr, size_t maxlen)
{
    /* The caller's `maxlen` is a *cap* on how far we'll walk, not
     * a requirement that all `maxlen` bytes be valid.  Cap it
     * further at the distance from `uptr` to USER_VA_END so a
     * stack-local buffer near the top of the user range
     * (e.g. sh's `char line[128]` a few bytes below USER_STACK_TOP)
     * isn't rejected just because uptr+maxlen overshoots. */
    if (uptr < USER_VA_BASE || uptr >= USER_VA_END)
        return -EFAULT;
    size_t avail = (size_t)(USER_VA_END - uptr);
    size_t cap   = maxlen < avail ? maxlen : avail;
    const char *p = (const char *)(uintptr_t)uptr;
    for (size_t i = 0; i < cap; i++) {
        if (p[i] == '\0')
            return (long)i;
    }
    /* Hit the cap without finding a NUL.  If we ran into the user
     * range edge, that's a faulty pointer; otherwise the source
     * is too long for the caller's buffer. */
    if (cap < maxlen)
        return -EFAULT;
    return -ENAMETOOLONG;
}

long copy_string_from_user(char *kdst, uint64_t uptr, size_t kdst_size)
{
    if (kdst_size == 0)
        return -ENAMETOOLONG;

    /* Find the source length first so we can fail fast on bad
     * pointers and on too-long strings without partially copying.
     * We only require the bytes actually walked to be in user
     * range; ustrnlen handles that. */
    long n = ustrnlen(uptr, kdst_size);
    if (n < 0)
        return n;

    /* n is the strlen excluding the NUL.  Copy n+1 bytes. */
    int rc = copy_from_user(kdst, uptr, (size_t)n + 1);
    if (rc < 0)
        return rc;
    return n;
}
