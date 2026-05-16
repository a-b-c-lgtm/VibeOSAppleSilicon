/*
 * kernel/core/mmap_uapi.h — shared kernel/userspace mmap constants.
 *
 * Chapter 90 ships a deliberately-tiny subset of the POSIX mmap
 * interface.  Numbers picked to match Linux/glibc where it costs
 * nothing — that way userspace code can be ported without a
 * translation table.
 *
 * What chapter 90 supports:
 *   PROT_READ | PROT_WRITE          (PROT_EXEC ignored, treated as 0)
 *   MAP_PRIVATE
 *   MAP_ANONYMOUS                   (must be paired with MAP_PRIVATE)
 *   MAP_PRIVATE | (regular file fd) at PROT_READ                 only
 *
 * What chapter 90 does NOT support (returns -EINVAL):
 *   MAP_SHARED                       (no writeback path yet)
 *   MAP_FIXED                        (kernel always picks the VA)
 *   PROT_NONE                        (would need guard-page support)
 *   File-backed mmap with PROT_WRITE (would need COW on cached pages)
 *   Non-page-aligned offset
 *   len == 0
 */
#ifndef MMAP_UAPI_H
#define MMAP_UAPI_H

/* prot bits.  PROT_EXEC is accepted but ignored — chapter 90 maps
 * everything UXN=1 (no EL0 exec) because the only producer right
 * now is data, not code. */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* flags.  Numbers match Linux. */
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

/* Failure return value from mmap, matches POSIX MAP_FAILED. */
#define MAP_FAILED      ((void *)-1L)

#endif /* MMAP_UAPI_H */
