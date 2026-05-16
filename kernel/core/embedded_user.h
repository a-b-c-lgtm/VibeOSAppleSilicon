/*
 * kernel/core/embedded_user.h — handles to user binaries baked
 * into the kernel image at link time.
 *
 * The Makefile uses `objcopy -I binary` to wrap each user .bin into
 * an .o exposing three symbols of the form:
 *
 *     _binary_<filename>_bin_start
 *     _binary_<filename>_bin_end
 *     _binary_<filename>_bin_size      (an absolute symbol, NOT a uint64_t)
 *
 * In C we declare them as char arrays and take their addresses;
 * the value of `_size` is the *address* of the symbol (which the
 * linker sets equal to the size in bytes), so we cast it through
 * a uintptr_t.
 *
 * Adding a new user binary means: (1) add it to USER_PROGRAMS in
 * the Makefile, (2) declare its handles below, (3) instantiate it
 * with embedded_user_image_for() at the call site.
 */
#ifndef EMBEDDED_USER_H
#define EMBEDDED_USER_H

#include <stddef.h>
#include <stdint.h>

extern char _binary_hello_elf_bin_start[];
extern char _binary_hello_elf_bin_end[];
extern char _binary_hello_elf_bin_size[];

extern char _binary_cat_elf_bin_start[];
extern char _binary_cat_elf_bin_end[];
extern char _binary_cat_elf_bin_size[];

static inline const uint8_t *embedded_hello_data(void) {
    return (const uint8_t *)_binary_hello_elf_bin_start;
}
static inline size_t embedded_hello_size(void) {
    return (size_t)((uintptr_t)_binary_hello_elf_bin_end -
                    (uintptr_t)_binary_hello_elf_bin_start);
}

static inline const uint8_t *embedded_cat_data(void) {
    return (const uint8_t *)_binary_cat_elf_bin_start;
}
static inline size_t embedded_cat_size(void) {
    return (size_t)((uintptr_t)_binary_cat_elf_bin_end -
                    (uintptr_t)_binary_cat_elf_bin_start);
}

#endif
