/*
 * kernel/core/fdt.h — minimal flat-device-tree walker.
 *
 * Just enough of libfdt's surface area to find the /memory node
 * and pull out the (base, size) pairs from its `reg` property.
 * No allocations.  Single-pass traversal, big-endian aware.
 */
#ifndef FDT_H
#define FDT_H

#include <stdint.h>
#include <stddef.h>

#define FDT_MAX_MEMORY_REGIONS 8

struct fdt_memory_region {
    uint64_t base;
    uint64_t size;
};

struct fdt_memory_map {
    size_t                   count;
    struct fdt_memory_region regions[FDT_MAX_MEMORY_REGIONS];
};

/* Returns 1 on success, 0 if the blob is not a valid DTB.  On
 * success, *total_size is populated with the DTB's totalsize for
 * later use (e.g. so the page allocator can avoid stamping on it). */
int fdt_validate(const void *blob, uint32_t *total_size);

/* Walk the structure block and fill `out` with every /memory node's
 * `reg` pairs.  Returns the number of regions found, or 0 on parse
 * failure.  Regions beyond FDT_MAX_MEMORY_REGIONS are silently
 * dropped. */
size_t fdt_read_memory(const void *blob, struct fdt_memory_map *out);

/* Read /psci/method into `out` (NUL-terminated, truncated to
 * cap-1).  Returns 1 on success, 0 if the node or property is
 * absent.  cap must be >= 4 to fit "smc" or "hvc". */
int fdt_read_psci_method(const void *blob, char *out, size_t cap);

/* Walk /cpus and fill `out_mpidrs` with each cpu node's `reg`
 * value (= MPIDR for that CPU's affinity).  Returns the number
 * of CPUs found.  Skips nodes whose `device_type` is not "cpu"
 * and nodes whose `status` is "disabled".  Caller passes max =
 * size of out_mpidrs[]. */
size_t fdt_read_cpus(const void *blob, uint64_t *out_mpidrs, size_t max);

/* Chapter 96 — find the first node whose `compatible` property
 * contains "arm,pl031" and return its first `reg` cell pair as
 * the MMIO base address in *base_out.  Returns 1 on success,
 * 0 if no such node is present.  We assume the parent's
 * #address-cells / #size-cells are 2/2 (the QEMU virt default
 * for non-/cpus nodes); on virt the PL031 reg is "0x0 0x9010000
 * 0x0 0x1000", so the first 64-bit cell is the base. */
int fdt_read_pl031(const void *blob, uint64_t *base_out);

#endif
