/*
 * kernel/core/fdt.c — minimal flat-device-tree walker.
 *
 * Implements the slice of libfdt we need for early bring-up:
 *
 *   - validate the FDT header magic and bounds
 *   - walk the structure block, decoding BEGIN_NODE / END_NODE /
 *     PROP / NOP / END tokens
 *   - find every node whose name begins with "memory" or "memory@"
 *     and read its `reg` property using the parent's
 *     #address-cells / #size-cells (defaulting to 2/2 as on the
 *     QEMU virt machine)
 *
 * Everything is big-endian on disk; we convert to host-order with
 * be32_to_cpu / be64_to_cpu.  The blob is treated as immutable and
 * read directly from its loaded address — no copies, no allocations.
 */

#include "fdt.h"
#include <stdint.h>
#include <stddef.h>

/* DTB magic and structure-block tokens (all big-endian on disk). */
#define FDT_MAGIC           0xd00dfeedu
#define FDT_BEGIN_NODE      0x00000001u
#define FDT_END_NODE        0x00000002u
#define FDT_PROP            0x00000003u
#define FDT_NOP             0x00000004u
#define FDT_END             0x00000009u

/* On-disk header layout. */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static uint32_t be32_to_cpu(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static int str_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int fdt_validate(const void *blob, uint32_t *total_size)
{
    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (be32_to_cpu(h->magic) != FDT_MAGIC)
        return 0;
    if (total_size)
        *total_size = be32_to_cpu(h->totalsize);
    return 1;
}

/* Walk one PROP cell and return a pointer past it.  *name_out points
 * into the strings block; *data_out points at the property bytes
 * (still big-endian if numeric); *len_out is the byte length. */
static const uint8_t *parse_prop(const uint8_t *cursor,
                                 const char *strings,
                                 const char **name_out,
                                 const uint8_t **data_out,
                                 uint32_t *len_out)
{
    uint32_t len     = read_be32(cursor); cursor += 4;
    uint32_t nameoff = read_be32(cursor); cursor += 4;
    *name_out = strings + nameoff;
    *data_out = cursor;
    *len_out  = len;
    cursor += len;
    /* Pad to 4-byte alignment. */
    while ((uintptr_t)cursor & 3u) cursor++;
    return cursor;
}

size_t fdt_read_memory(const void *blob, struct fdt_memory_map *out)
{
    out->count = 0;

    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (be32_to_cpu(h->magic) != FDT_MAGIC)
        return 0;

    const uint8_t *base    = (const uint8_t *)blob;
    const uint8_t *strings = base + be32_to_cpu(h->off_dt_strings);
    const uint8_t *cursor  = base + be32_to_cpu(h->off_dt_struct);
    const uint8_t *end     = cursor + be32_to_cpu(h->size_dt_struct);

    /* Track #address-cells / #size-cells along the path.  Stack
     * depth on the QEMU virt DTB is shallow (a few levels at most);
     * 16 is plenty.  We avoid `= { 2 }` style initialisers because
     * GCC lowers those to memset, which the freestanding kernel
     * does not link. */
    uint32_t addr_cells_stack[16];
    uint32_t size_cells_stack[16];
    for (int i = 0; i < 16; i++) {
        addr_cells_stack[i] = 2;   /* spec default */
        size_cells_stack[i] = 1;   /* spec default; updated from root */
    }
    int depth = 0;
    int in_memory_node = 0;

    while (cursor < end) {
        uint32_t token = read_be32(cursor); cursor += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)cursor;
            /* Skip name + null terminator. */
            while (*cursor) cursor++;
            cursor++;
            while ((uintptr_t)cursor & 3u) cursor++;

            /* Inherit cell counts from parent. */
            if (depth + 1 < (int)(sizeof(addr_cells_stack) / sizeof(uint32_t))) {
                addr_cells_stack[depth + 1] = addr_cells_stack[depth];
                size_cells_stack[depth + 1] = size_cells_stack[depth];
            }
            depth++;

            /* Match "memory" or "memory@..." (a memory node may also
             * be at deeper levels in theory, but on virt it's always
             * a direct child of root).  Note depth counting: root
             * is at depth 1 because we increment on its BEGIN_NODE,
             * so its children — including /memory — sit at depth 2. */
            in_memory_node = 0;
            if (depth == 2 &&
                (str_eq(name, "memory") || str_starts_with(name, "memory@")))
                in_memory_node = 1;
            break;
        }

        case FDT_END_NODE:
            in_memory_node = 0;
            if (depth > 0) depth--;
            break;

        case FDT_PROP: {
            const char    *pname;
            const uint8_t *pdata;
            uint32_t       plen;
            cursor = parse_prop(cursor, (const char *)strings,
                                &pname, &pdata, &plen);

            if (depth == 1 || depth == 0) {
                /* Pick up #address-cells / #size-cells from the
                 * parent of any node we will descend into.  We
                 * write them into the *current* depth slot since
                 * BEGIN_NODE will inherit from there. */
                if (str_eq(pname, "#address-cells") && plen >= 4)
                    addr_cells_stack[depth] = read_be32(pdata);
                else if (str_eq(pname, "#size-cells") && plen >= 4)
                    size_cells_stack[depth] = read_be32(pdata);
            }

            if (in_memory_node && str_eq(pname, "reg")) {
                uint32_t ac = addr_cells_stack[depth - 1];
                uint32_t sc = size_cells_stack[depth - 1];
                uint32_t entry_bytes = (ac + sc) * 4;
                if (entry_bytes > 0) {
                    uint32_t entries = plen / entry_bytes;
                    for (uint32_t i = 0; i < entries; i++) {
                        const uint8_t *e = pdata + i * entry_bytes;
                        uint64_t addr = 0, size = 0;
                        for (uint32_t j = 0; j < ac; j++)
                            addr = (addr << 32) | read_be32(e + j * 4);
                        for (uint32_t j = 0; j < sc; j++)
                            size = (size << 32) | read_be32(e + (ac + j) * 4);
                        if (out->count < FDT_MAX_MEMORY_REGIONS) {
                            out->regions[out->count].base = addr;
                            out->regions[out->count].size = size;
                            out->count++;
                        }
                    }
                }
            }
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return out->count;

        default:
            /* Malformed blob — bail. */
            return out->count;
        }
    }
    return out->count;
}

/* ------------------------------------------------------------------
 * Chapter 87 additions: PSCI conduit + cpu enumeration.
 *
 * Both walkers reuse the same token-cursor pattern as
 * fdt_read_memory above; the only differences are which node names
 * we match and which properties we extract.  Could be folded into
 * a single generic walker but the duplication is small (~80 lines
 * total) and keeps each function readable in isolation.
 * ------------------------------------------------------------------ */

/* Match "psci", "psci@..." or "psci-...".  Returns 1 if `name`
 * is the PSCI node. */
static int name_is_psci(const char *name)
{
    return name[0] == 'p' && name[1] == 's' && name[2] == 'c' &&
           name[3] == 'i' && (name[4] == 0 || name[4] == '@' ||
                              name[4] == '-');
}

int fdt_read_psci_method(const void *blob, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = 0;

    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (be32_to_cpu(h->magic) != FDT_MAGIC)
        return 0;

    const uint8_t *base    = (const uint8_t *)blob;
    const uint8_t *strings = base + be32_to_cpu(h->off_dt_strings);
    const uint8_t *cursor  = base + be32_to_cpu(h->off_dt_struct);
    const uint8_t *end     = cursor + be32_to_cpu(h->size_dt_struct);

    int depth     = 0;
    int in_psci   = 0;
    int found     = 0;

    while (cursor < end) {
        uint32_t token = read_be32(cursor); cursor += 4;
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)cursor;
            while (*cursor) cursor++;
            cursor++;
            while ((uintptr_t)cursor & 3u) cursor++;
            depth++;
            /* /psci is a direct child of root.  Root sits at depth
             * 1 (we increment on its BEGIN_NODE), so /psci is at
             * depth 2. */
            if (depth == 2 && name_is_psci(name))
                in_psci = 1;
            break;
        }
        case FDT_END_NODE:
            if (in_psci && depth == 2) in_psci = 0;
            if (depth > 0) depth--;
            break;
        case FDT_PROP: {
            const char    *pname;
            const uint8_t *pdata;
            uint32_t       plen;
            cursor = parse_prop(cursor, (const char *)strings,
                                &pname, &pdata, &plen);
            if (in_psci && str_eq(pname, "method") && plen > 0) {
                size_t n = plen < cap ? plen : cap - 1;
                for (size_t i = 0; i < n; i++) out[i] = (char)pdata[i];
                out[n] = 0;
                found = 1;
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return found;
        default:
            return found;
        }
    }
    return found;
}

size_t fdt_read_cpus(const void *blob, uint64_t *out_mpidrs, size_t max)
{
    if (!out_mpidrs || max == 0)
        return 0;

    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (be32_to_cpu(h->magic) != FDT_MAGIC)
        return 0;

    const uint8_t *base    = (const uint8_t *)blob;
    const uint8_t *strings = base + be32_to_cpu(h->off_dt_strings);
    const uint8_t *cursor  = base + be32_to_cpu(h->off_dt_struct);
    const uint8_t *end     = cursor + be32_to_cpu(h->size_dt_struct);

    /* /cpus declares its own #address-cells / #size-cells.  On
     * QEMU virt these are 1/0 (one 32-bit "reg" cell per cpu, no
     * size).  Default per spec: 2/1.  We track the value of the
     * /cpus node specifically. */
    uint32_t cpus_addr_cells = 2;
    uint32_t cpus_size_cells = 1;  /* unused but tracked for completeness */
    (void)cpus_size_cells;

    int      depth          = 0;
    int      in_cpus        = 0;       /* depth == 2 inside /cpus     */
    int      in_cpu_node    = 0;       /* depth == 3 child of /cpus   */
    size_t   count          = 0;
    /* Per-cpu-node staging. */
    int      have_reg       = 0;
    uint64_t cur_reg        = 0;
    int      enabled        = 1;       /* default if no `status` prop  */

    while (cursor < end) {
        uint32_t token = read_be32(cursor); cursor += 4;
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)cursor;
            while (*cursor) cursor++;
            cursor++;
            while ((uintptr_t)cursor & 3u) cursor++;
            depth++;
            if (depth == 2 && (str_eq(name, "cpus") ||
                               str_starts_with(name, "cpus@"))) {
                in_cpus = 1;
            } else if (in_cpus && depth == 3) {
                /* Could be cpu@0 / cpu@1 / cpu-map.  The latter is
                 * topology metadata, not a real cpu; we filter it
                 * out by requiring device_type == "cpu" below. */
                in_cpu_node = 1;
                have_reg    = 0;
                cur_reg     = 0;
                enabled     = 1;
            }
            break;
        }
        case FDT_END_NODE:
            if (in_cpu_node && depth == 3) {
                /* Commit if we got a reg and the node was enabled.
                 * device_type filtering is implicit: cpu-map nodes
                 * don't have a `reg` property, so have_reg == 0. */
                if (have_reg && enabled && count < max) {
                    out_mpidrs[count++] = cur_reg;
                }
                in_cpu_node = 0;
            } else if (in_cpus && depth == 2) {
                in_cpus = 0;
            }
            if (depth > 0) depth--;
            break;
        case FDT_PROP: {
            const char    *pname;
            const uint8_t *pdata;
            uint32_t       plen;
            cursor = parse_prop(cursor, (const char *)strings,
                                &pname, &pdata, &plen);
            if (in_cpus && depth == 2) {
                /* Properties of the /cpus node itself. */
                if (str_eq(pname, "#address-cells") && plen >= 4)
                    cpus_addr_cells = read_be32(pdata);
                else if (str_eq(pname, "#size-cells") && plen >= 4)
                    cpus_size_cells = read_be32(pdata);
            } else if (in_cpu_node && depth == 3) {
                if (str_eq(pname, "reg") && plen >= 4) {
                    /* Read the first `cpus_addr_cells` cells.  On
                     * virt that's 1 cell == low 32 bits of MPIDR.
                     * Pad upper bits to 0 — MPIDR.{Aff1,2,3} are
                     * 0 for QEMU virt's default flat topology. */
                    uint64_t v = 0;
                    uint32_t cells = cpus_addr_cells;
                    if (cells > plen / 4) cells = plen / 4;
                    for (uint32_t j = 0; j < cells; j++)
                        v = (v << 32) | read_be32(pdata + j * 4);
                    cur_reg  = v;
                    have_reg = 1;
                } else if (str_eq(pname, "status") && plen > 0) {
                    /* "okay" / "disabled" / "fail" / "fail-sss".
                     * Anything other than "okay" disables the
                     * cpu.  An empty string is treated as okay
                     * (some DTBs do this). */
                    if (pdata[0] == 'd' || pdata[0] == 'f')
                        enabled = 0;
                }
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return count;
        default:
            return count;
        }
    }
    return count;
}

/* Chapter 96 — find a node whose `compatible` property contains
 * the NUL-terminated string "arm,pl031" and return its first
 * 64-bit `reg` cell as the MMIO base.
 *
 * `compatible` is a list of NUL-terminated strings packed back-
 * to-back inside the property; QEMU virt's pl031 has both
 * "arm,pl031" and "arm,primecell".  We scan for an exact match
 * to "arm,pl031" so that future "arm,pl031-foo" devices don't
 * spuriously match.
 *
 * The `reg` cell layout is parent-defined.  On virt the PL031
 * sits directly under the root with #address-cells = 2,
 * #size-cells = 2, so its reg property holds two 64-bit values
 * (base, length).  We assume that layout; it is the sole
 * configuration QEMU virt has ever shipped, and would change
 * only if Arm released a "Linux for Servers"-style virt-2.
 */
static int compatible_contains(const uint8_t *pdata, uint32_t plen,
                               const char *needle)
{
    /* Walk the packed string list, comparing each entry. */
    uint32_t off = 0;
    while (off < plen) {
        const char *cand = (const char *)(pdata + off);
        if (str_eq(cand, needle))
            return 1;
        /* Skip past this entry plus its NUL. */
        while (off < plen && pdata[off] != 0) off++;
        if (off < plen) off++;            /* skip the NUL itself */
    }
    return 0;
}

int fdt_read_pl031(const void *blob, uint64_t *base_out)
{
    if (!base_out)
        return 0;
    *base_out = 0;

    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (be32_to_cpu(h->magic) != FDT_MAGIC)
        return 0;

    const uint8_t *base    = (const uint8_t *)blob;
    const uint8_t *strings = base + be32_to_cpu(h->off_dt_strings);
    const uint8_t *cursor  = base + be32_to_cpu(h->off_dt_struct);
    const uint8_t *end     = cursor + be32_to_cpu(h->size_dt_struct);

    int      depth   = 0;
    /* Per-node staging.  We need to scan BOTH `compatible` and
     * `reg` of the same node before deciding whether to commit
     * — and the order they appear in the structure block is up
     * to dtc.  We track them per-node and check at END_NODE. */
    int      seen_compatible = 0;
    int      seen_reg        = 0;
    uint64_t cur_reg_base    = 0;

    while (cursor < end) {
        uint32_t token = read_be32(cursor); cursor += 4;
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)cursor;
            (void)name;                    /* match by compatible, not name */
            while (*cursor) cursor++;
            cursor++;
            while ((uintptr_t)cursor & 3u) cursor++;
            depth++;
            seen_compatible = 0;
            seen_reg        = 0;
            cur_reg_base    = 0;
            break;
        }
        case FDT_END_NODE:
            if (seen_compatible && seen_reg) {
                *base_out = cur_reg_base;
                return 1;
            }
            if (depth > 0) depth--;
            seen_compatible = 0;
            seen_reg        = 0;
            cur_reg_base    = 0;
            break;
        case FDT_PROP: {
            const char    *pname;
            const uint8_t *pdata;
            uint32_t       plen;
            cursor = parse_prop(cursor, (const char *)strings,
                                &pname, &pdata, &plen);
            if (str_eq(pname, "compatible")) {
                if (compatible_contains(pdata, plen, "arm,pl031"))
                    seen_compatible = 1;
            } else if (str_eq(pname, "reg") && plen >= 8) {
                /* Read first 64-bit cell as base.  Two BE 32-bit
                 * words, MSW first.  We don't validate that
                 * #address-cells is 2 because every virt
                 * variant we know of uses 2/2 for root-child
                 * MMIO devices. */
                uint64_t hi = read_be32(pdata + 0);
                uint64_t lo = read_be32(pdata + 4);
                cur_reg_base = (hi << 32) | lo;
                seen_reg = 1;
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return 0;
        default:
            return 0;
        }
    }
    return 0;
}
