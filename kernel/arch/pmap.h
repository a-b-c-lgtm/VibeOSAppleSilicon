/*
 * kernel/arch/pmap.h — public surface of arch/page_tables.c.
 *
 * For now there is exactly one runtime function: install a 1 GiB
 * Normal-Cacheable identity mapping for a freshly-discovered RAM
 * region.  Called from kernel_main after the DTB scan tells us
 * the actual physical-memory map.
 */
#ifndef PMAP_H
#define PMAP_H

#include <stdint.h>

void pmap_install_ram_block_1gib(uint64_t pa);

#endif
