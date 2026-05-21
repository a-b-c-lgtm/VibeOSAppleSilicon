/* vendor/bearssl-shim/string.h — chapter 112a.
 *
 * Minimal freestanding shim for BearSSL's `#include <string.h>` in
 * `vendor/bearssl/src/inner.h`.  The aarch64-elf cross toolchain
 * does not ship newlib headers, so `<string.h>` is absent.  We
 * provide just the five symbols BearSSL actually uses out of
 * <string.h>; implementations live in userspace/libc/cstring.c
 * and are linked into every binary that pulls libbearssl.a.
 *
 * The size_t typedef is normally provided by <stddef.h>, which is
 * one of the freestanding-required headers and IS shipped by GCC.
 * We include it here so callers don't need an extra `#include`.
 */

#ifndef BEARSSL_SHIM_STRING_H
#define BEARSSL_SHIM_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* chapter 112b: cross-guarded with userspace/libc/syscall.h's
 * static-inline strlen.  Whichever header is included first wins;
 * the other one's guard fires and skips, so TUs that include both
 * (tls_socket.c, httpsd.c, tlstest.c) build cleanly. */
#ifndef OSDEV_STRLEN_PROVIDED
#define OSDEV_STRLEN_PROVIDED
size_t strlen(const char *s);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BEARSSL_SHIM_STRING_H */
