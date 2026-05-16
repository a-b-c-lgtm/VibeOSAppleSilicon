/*
 * kernel/core/heap.h — kernel heap public API.
 *
 * Three-function interface:
 *
 *   kheap_init()          one-time setup; called from kernel_main
 *                         after gic_init/timer_init.
 *   kmalloc(size)         allocate `size` bytes, 16-byte aligned.
 *                         Returns NULL on OOM.
 *   kfree(ptr)            release a previously kmalloc'd block.
 *                         Idempotent on NULL.
 *
 * Plus two diagnostics that the milestone-3 demo uses to verify
 * coalescing actually works:
 *
 *   kheap_used()          sum of payload bytes currently allocated.
 *   kheap_block_count()   number of blocks (free + used) on the
 *                         implicit list; coalescing should drive
 *                         this back to a small number after frees.
 */

#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

void   kheap_init(uint64_t base, size_t size);
void  *kmalloc(size_t size);
void   kfree(void *ptr);
size_t kheap_used(void);
size_t kheap_block_count(void);

#endif /* HEAP_H */
