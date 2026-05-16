/*
 * kernel/core/uaccess.h — safe user/kernel pointer copies.
 *
 * Until now syscalls treated user-supplied pointers as raw kernel
 * pointers and dereferenced them directly.  That works while the
 * caller's address space is the active TTBR0, but it has two real
 * problems:
 *
 *   1. PRIVILEGE ESCALATION.  A user can pass an address that
 *      points AT KERNEL MEMORY.  Slot 8 (the heap) is now AP=00
 *      so EL0 can't touch it directly — but the kernel runs at
 *      EL1 with full privileges, so when the kernel dereferences
 *      a user-supplied pointer the access SUCCEEDS regardless.
 *      Result: a malicious user can have the kernel read any
 *      kernel address into a buffer the user controls (sys_read
 *      with buf = kernel address) or write to it (sys_write with
 *      buf = kernel address).
 *
 *   2. DENIAL OF SERVICE.  A user passes a wild pointer outside
 *      its mapped range.  The kernel takes a Data Abort From
 *      Same EL, which our current handler treats as fatal and
 *      halts the whole machine.
 *
 * The fix for (1) is a bounds check: every user pointer used by
 * the kernel must lie entirely inside [USER_VA_BASE, USER_VA_END).
 * That single check rules out *all* kernel addresses (which live
 * in slots 0..8, far below user range at slot 64).
 *
 * The fix for (2) is harder — we'd need either ARMv8.1 PAN/UAO
 * support or a `do_safely / on_fault` fixup mechanism that lets
 * the kernel recover from EL1 data aborts inside copy_from_user.
 * For now we accept the DoS: a userspace passing wild pointers
 * inside its own VA range can panic the kernel.  This file is
 * structured so that adding fixup tables later is a localized
 * change to copy_from_user / copy_to_user.
 *
 * All functions in this header return 0 on success and a negative
 * errno on failure.  -EFAULT is returned when bounds check fails.
 */

#ifndef UACCESS_H
#define UACCESS_H

#include <stddef.h>
#include <stdint.h>

#ifndef EFAULT
#define EFAULT 14
#endif

#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

/* True if [uptr, uptr+len) lies entirely inside the user VA range
 * defined by address_space.h.  Length-zero requests are accepted
 * iff uptr itself is in range.  Catches integer overflow in
 * uptr+len.  Does NOT verify that the range is mapped — only that
 * it cannot possibly point at kernel memory. */
int uaccess_check(uint64_t uptr, size_t len);

/* Bounded NUL-terminated user-string check.  Returns the string
 * length (excluding NUL) on success, -EFAULT if the range is
 * outside user VA, -ENAMETOOLONG if no NUL is found within the
 * first `maxlen` bytes after uptr.  This walks user memory so it
 * has the same DoS caveat as copy_from_user — caller must pass a
 * sane maxlen. */
long ustrnlen(uint64_t uptr, size_t maxlen);

/* Copy `len` bytes FROM the user pointer to a kernel buffer.
 * Returns 0 on success, -EFAULT on failure.  Pre-conditions:
 * `kdst` is a kernel pointer with at least `len` writable bytes;
 * `uptr` is the user-supplied address. */
int copy_from_user(void *kdst, uint64_t uptr, size_t len);

/* Copy `len` bytes from a kernel buffer TO the user pointer.
 * Returns 0 on success, -EFAULT on failure.  Pre-conditions:
 * `ksrc` is a kernel pointer with at least `len` readable bytes;
 * `uptr` is the user-supplied address. */
int copy_to_user(uint64_t uptr, const void *ksrc, size_t len);

/* Copy a NUL-terminated string from `uptr` into `kdst` (size
 * `kdst_size`).  On success returns the string length (excluding
 * NUL) and `kdst` is NUL-terminated.  Returns -EFAULT if the user
 * range is bad, -ENAMETOOLONG if the source string is >= kdst_size
 * bytes (in which case `kdst` is left in an unspecified state).
 * Caller must ensure kdst_size >= 1. */
long copy_string_from_user(char *kdst, uint64_t uptr, size_t kdst_size);

#endif /* UACCESS_H */
