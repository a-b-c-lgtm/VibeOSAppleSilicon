/*
 * userspace/make/make.c — chapter 133b /bin/make.
 *
 * A small GNU-make-shaped build driver, intentionally NOT a
 * conformance implementation.  Chapter 126 shipped the original
 * "rules + recipes only" version (351 LoC); chapter 133b adds
 * the features needed to drive a real multi-file C build like
 * Doom:
 *
 *   - Variable definitions:      VAR = value                  (133b)
 *   - Variable expansion:        $(VAR) and ${VAR}            (133b)
 *   - Automatic variables:       $@  $<  $^                   (133b)
 *   - Pattern rules:             %.o: %.c                     (133b)
 *   - Recipe prefixes:           @cmd  -cmd                   (133b)
 *   - .PHONY:                    declared targets bypass cache
 *   - Line continuation:         trailing backslash joins lines
 *
 * Still NOT implemented (each would be a future chapter):
 *
 *   - := (simple expand)  ?=  += (compose forms)
 *   - $(wildcard ...), $(patsubst ...), $(shell ...), etc.
 *   - ifeq / ifneq / ifdef / endif (conditionals)
 *   - include / -include nested makefiles
 *   - Order-only prereqs (foo: a b | c)
 *   - Parallelism (-j)
 *   - mtime-based out-of-date checking; we still rebuild
 *     every requested target unconditionally
 *
 * Build algorithm is unchanged from chapter 126: recursive
 * depth-first with `in_progress` flag for cycle detection.
 *
 * Capacity (compile-time, sized for an in-guest Doom rebuild):
 *
 *   32 rules, 16 pattern rules, 256 deps per rule,
 *   16 recipe lines per rule, 64 variables, 512 bytes per
 *   line, 96 bytes per name, 96 KiB total Makefile source.
 *
 * Exit codes:  0 on success; 1 on parse / spawn / recipe failure.
 *
 * Recipe execution remains the chapter-126 model: split on the
 * first whitespace, spawn(path, rest).  The kernel's sys_spawn
 * tokenises `rest` into argv; we do NOT invoke /bin/sh, which
 * means no pipes / redirections / globs / && inside a recipe.
 * Doom doesn't need any of those, and avoiding /bin/sh keeps
 * the dependency graph clean.
 */

#include "../libc/syscall.h"
#include "../libc/fcntl.h"
#include "../libc/printf.h"
#include "../libc/errno.h"

#define MK_MAX_RULES       32
#define MK_MAX_PATTERNS    16
#define MK_MAX_DEPS       256
#define MK_MAX_RECIPE      16
#define MK_MAX_LINE       512
#define MK_MAX_NAME        96
#define MK_MAX_SRC      (96 * 1024)
#define MK_MAX_VARS        64
#define MK_MAX_VAL       4096    /* chapter 133d: Doom's OBJS list
                                    after continuation-join is
                                    ~1500 chars (78 file paths).
                                    4 KiB gives headroom for full
                                    in-guest builds.  Recipe lines
                                    stay capped at MK_MAX_LINE. */
#define MK_MAX_PHONY       16

typedef struct {
    char target[MK_MAX_NAME];
    char (*dep)[MK_MAX_NAME];    /* points at a per-rule slot in
                                    g_rule_deps[] */
    int  dep_count;
    char recipe[MK_MAX_RECIPE][MK_MAX_LINE];
    int  recipe_count;
    int  in_progress;
    int  built;
} mk_rule;

typedef struct {
    /* Pattern: split on the single '%' in both target and dep.
     * Example: target "%.o" -> target_prefix="", target_suffix=".o".
     * dep "%.c" -> dep_prefix="", dep_suffix=".c". */
    char target_prefix[MK_MAX_NAME];
    char target_suffix[MK_MAX_NAME];
    char dep_prefix[MK_MAX_NAME];
    char dep_suffix[MK_MAX_NAME];
    char recipe[MK_MAX_RECIPE][MK_MAX_LINE];
    int  recipe_count;
} mk_pattern;

typedef struct {
    char name[MK_MAX_NAME];
    char value[MK_MAX_VAL];
} mk_var;

/* Static dep storage: one slab per rule slot.  Each row holds
 * up to MK_MAX_DEPS dep names; total = 32 * 256 * 96 = 768 KiB
 * of bss, which is acceptable for our process budget. */
static char g_rule_deps[MK_MAX_RULES][MK_MAX_DEPS][MK_MAX_NAME];
static mk_rule    g_rules[MK_MAX_RULES];
static int        g_rule_count;
static mk_pattern g_patterns[MK_MAX_PATTERNS];
static int        g_pattern_count;
static mk_var     g_vars[MK_MAX_VARS];
static int        g_var_count;
static char       g_phony[MK_MAX_PHONY][MK_MAX_NAME];
static int        g_phony_count;
static char       g_src[MK_MAX_SRC];

/* ── string helpers ─────────────────────────────────────── */

static int mk_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int mk_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int mk_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void mk_strncopy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int mk_ends_with(const char *s, const char *suf)
{
    int ls = mk_strlen(s), lf = mk_strlen(suf);
    if (lf > ls) return 0;
    for (int i = 0; i < lf; i++)
        if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

static int mk_starts_with(const char *s, const char *pre)
{
    while (*pre) {
        if (*s != *pre) return 0;
        s++; pre++;
    }
    return 1;
}

/* ── variable store ─────────────────────────────────────── */

static const char *mk_var_lookup(const char *name)
{
    for (int i = 0; i < g_var_count; i++)
        if (mk_streq(g_vars[i].name, name))
            return g_vars[i].value;
    return 0;
}

static int mk_var_set(const char *name, const char *value)
{
    for (int i = 0; i < g_var_count; i++) {
        if (mk_streq(g_vars[i].name, name)) {
            mk_strncopy(g_vars[i].value, value, MK_MAX_VAL);
            return 0;
        }
    }
    if (g_var_count >= MK_MAX_VARS) {
        printf("make: too many variables\n");
        return -1;
    }
    mk_strncopy(g_vars[g_var_count].name, name, MK_MAX_NAME);
    mk_strncopy(g_vars[g_var_count].value, value, MK_MAX_VAL);
    g_var_count++;
    return 0;
}

/* Expand $(VAR) / ${VAR} and the automatic vars $@ $< $^.
 * Recursive: the value of a variable is re-expanded.  Bounded
 * to 8 levels of nesting so a cycle (`A = $(A)`) is a polite
 * "expansion too deep" rather than a runaway. */
static int mk_expand_into(char *out, int cap, const char *in,
                          const char *autotarget,
                          char (*autodeps)[MK_MAX_NAME],
                          int autodep_count,
                          int depth)
{
    if (depth > 8) {
        printf("make: variable expansion too deep "
               "(circular VAR?)\n");
        return -1;
    }
    int oi = 0;
    int i = 0;
    while (in[i] && oi < cap - 1) {
        if (in[i] != '$') {
            out[oi++] = in[i++];
            continue;
        }
        /* $$ -> literal $ */
        if (in[i + 1] == '$') { out[oi++] = '$'; i += 2; continue; }

        /* $@ $< $^  — automatic vars.  When `autotarget` is
         * NULL we are expanding a rule header at parse time;
         * leave the autos verbatim so they can be re-expanded
         * per-target when the recipe runs. */
        if (in[i + 1] == '@') {
            if (autotarget) {
                int n = mk_strlen(autotarget);
                if (oi + n >= cap) n = cap - 1 - oi;
                for (int k = 0; k < n; k++) out[oi++] = autotarget[k];
                i += 2;
            } else {
                out[oi++] = in[i++];
            }
            continue;
        }
        if (in[i + 1] == '<') {
            if (autotarget && autodeps && autodep_count > 0) {
                const char *d = autodeps[0];
                int n = mk_strlen(d);
                if (oi + n >= cap) n = cap - 1 - oi;
                for (int k = 0; k < n; k++) out[oi++] = d[k];
                i += 2;
            } else {
                out[oi++] = in[i++];
            }
            continue;
        }
        if (in[i + 1] == '^') {
            if (autotarget && autodeps) {
                for (int j = 0; j < autodep_count; j++) {
                    if (j > 0 && oi < cap - 1) out[oi++] = ' ';
                    int n = mk_strlen(autodeps[j]);
                    for (int k = 0; k < n && oi < cap - 1; k++)
                        out[oi++] = autodeps[j][k];
                }
                i += 2;
            } else {
                out[oi++] = in[i++];
            }
            continue;
        }

        /* $(VAR) or ${VAR} */
        char open = in[i + 1];
        char close;
        if (open == '(') close = ')';
        else if (open == '{') close = '}';
        else {
            /* Bare $X — treat as literal. */
            out[oi++] = in[i++];
            continue;
        }
        int ns = i + 2;
        int ne = ns;
        while (in[ne] && in[ne] != close) ne++;
        if (in[ne] != close) {
            /* Unterminated; copy verbatim. */
            out[oi++] = in[i++];
            continue;
        }
        char name[MK_MAX_NAME];
        int nl = ne - ns;
        if (nl >= MK_MAX_NAME) nl = MK_MAX_NAME - 1;
        for (int k = 0; k < nl; k++) name[k] = in[ns + k];
        name[nl] = 0;

        const char *val = mk_var_lookup(name);
        if (val) {
            /* Recursively expand the value before splicing. */
            char tmp[MK_MAX_VAL];
            if (mk_expand_into(tmp, sizeof(tmp), val,
                               autotarget, autodeps,
                               autodep_count, depth + 1) < 0)
                return -1;
            int n = mk_strlen(tmp);
            if (oi + n >= cap) n = cap - 1 - oi;
            for (int k = 0; k < n; k++) out[oi++] = tmp[k];
        }
        /* Undefined var expands to empty (gnu-make behaviour). */
        i = ne + 1;
    }
    out[oi] = 0;
    return 0;
}

/* ── source preload ─────────────────────────────────────── */

static int mk_read_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("make: cannot open '%s' (errno=%d)\n", path, errno);
        return -1;
    }
    int total = 0;
    while (total < MK_MAX_SRC - 1) {
        long n = read(fd, g_src + total, MK_MAX_SRC - 1 - total);
        if (n <= 0) break;
        total += (int)n;
    }
    close(fd);
    g_src[total] = 0;
    return total;
}

/* Apply line continuation in-place: any '\\' immediately before
 * '\n' is removed along with the newline and any following
 * leading whitespace on the next line.  Runs as a pre-pass so
 * the parser sees a clean stream of logical lines. */
static void mk_apply_continuations(void)
{
    int r = 0, w = 0;
    while (g_src[r]) {
        if (g_src[r] == '\\' && g_src[r + 1] == '\n') {
            g_src[w++] = ' ';
            r += 2;
            while (g_src[r] == ' ' || g_src[r] == '\t') r++;
            continue;
        }
        g_src[w++] = g_src[r++];
    }
    g_src[w] = 0;
}

/* ── parsing ────────────────────────────────────────────── */

static int mk_is_var_assign(const char *line, int len, int *eq_at)
{
    /* Recognise NAME = value where NAME is [A-Za-z_][A-Za-z0-9_]*.
     * We deliberately do NOT recognise := += ?= -- chapter 133b
     * defers those. */
    int i = 0;
    if (i >= len) return 0;
    char c = line[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          c == '_')) return 0;
    i++;
    while (i < len) {
        c = line[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')
            i++;
        else break;
    }
    /* Optional whitespace, then '='. */
    int j = i;
    while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
    if (j >= len) return 0;
    if (line[j] == '=') {
        /* Reject := += ?=  -- chapter 133b doesn't implement those. */
        if (j > 0 &&
            (line[j - 1] == ':' || line[j - 1] == '+' ||
             line[j - 1] == '?')) return 0;
        *eq_at = j;
        return 1;
    }
    return 0;
}

/* Try to split a pattern token like "%.o" into prefix/suffix.
 * Returns 1 on success.  Pattern must contain exactly one '%'. */
static int mk_split_pattern(const char *tok, char *prefix, char *suffix)
{
    int pct = -1;
    int n = 0;
    while (tok[n]) {
        if (tok[n] == '%') {
            if (pct >= 0) return 0;
            pct = n;
        }
        n++;
    }
    if (pct < 0) return 0;
    for (int i = 0; i < pct; i++) prefix[i] = tok[i];
    prefix[pct] = 0;
    int j = 0;
    for (int i = pct + 1; i < n; i++) suffix[j++] = tok[i];
    suffix[j] = 0;
    return 1;
}

/* Parse the preloaded g_src into g_rules, g_patterns, g_vars.
 * Returns 0 on success, -1 on syntax error. */
static int mk_parse(void)
{
    int i = 0, line_no = 1;
    mk_rule *current_rule = 0;
    mk_pattern *current_pattern = 0;

    while (g_src[i]) {
        int start = i;
        while (g_src[i] && g_src[i] != '\n') i++;
        int end = i;
        if (g_src[i] == '\n') i++;
        if (end > start && g_src[end - 1] == '\r') end--;

        /* Skip blank lines and comment lines.  Recipe lines
         * start with '\t' so are processed separately below. */
        int p = start;
        while (p < end && (g_src[p] == ' ' || g_src[p] == '\t')) p++;
        if (p == end) { line_no++; continue; }
        if (g_src[p] == '#' && g_src[start] != '\t') {
            line_no++; continue;
        }

        /* Recipe line (starts with tab)? */
        if (g_src[start] == '\t') {
            if (!current_rule && !current_pattern) {
                printf("make: line %d: recipe with no rule\n",
                       line_no);
                return -1;
            }
            int rstart = start + 1;
            int rlen = end - rstart;
            if (rlen >= MK_MAX_LINE) rlen = MK_MAX_LINE - 1;
            char (*dst_recipe)[MK_MAX_LINE];
            int  *dst_count;
            if (current_pattern) {
                dst_recipe = current_pattern->recipe;
                dst_count  = &current_pattern->recipe_count;
            } else {
                dst_recipe = current_rule->recipe;
                dst_count  = &current_rule->recipe_count;
            }
            if (*dst_count >= MK_MAX_RECIPE) {
                printf("make: line %d: too many recipe lines\n",
                       line_no);
                return -1;
            }
            char *dst = dst_recipe[*dst_count];
            for (int k = 0; k < rlen; k++) dst[k] = g_src[rstart + k];
            dst[rlen] = 0;
            (*dst_count)++;
            line_no++;
            continue;
        }

        /* Variable assignment?  Note `tmp[]` is sized to
         * MK_MAX_VAL (4 KiB), not MK_MAX_LINE -- the value of
         * a variable like OBJS can be much longer than a recipe
         * line after continuation-joining (chapter 133d). */
        int eq_at;
        {
            int linelen = end - p;
            char tmp[MK_MAX_VAL];
            int tl = linelen < (int)sizeof(tmp) - 1 ? linelen
                                                    : (int)sizeof(tmp) - 1;
            for (int k = 0; k < tl; k++) tmp[k] = g_src[p + k];
            tmp[tl] = 0;
            if (mk_is_var_assign(tmp, tl, &eq_at)) {
                int ne = eq_at;
                while (ne > 0 && (tmp[ne - 1] == ' ' ||
                                  tmp[ne - 1] == '\t')) ne--;
                char name[MK_MAX_NAME];
                int nl = ne;
                if (nl >= MK_MAX_NAME) nl = MK_MAX_NAME - 1;
                for (int k = 0; k < nl; k++) name[k] = tmp[k];
                name[nl] = 0;

                int vs = eq_at + 1;
                while (vs < tl && (tmp[vs] == ' ' ||
                                   tmp[vs] == '\t')) vs++;
                int ve = tl;
                while (ve > vs && (tmp[ve - 1] == ' ' ||
                                   tmp[ve - 1] == '\t')) ve--;
                char value[MK_MAX_VAL];
                int vl = ve - vs;
                if (vl >= MK_MAX_VAL) vl = MK_MAX_VAL - 1;
                for (int k = 0; k < vl; k++) value[k] = tmp[vs + k];
                value[vl] = 0;
                if (mk_var_set(name, value) < 0) return -1;
                current_rule = 0;
                current_pattern = 0;
                line_no++;
                continue;
            }
        }

        /* Otherwise it's a rule header.  Must contain ':'.
         * Expand $(VAR) / ${VAR} in the line first; leave
         * $@ / $< / $^ verbatim (they're per-target and only
         * make sense when the recipe runs).  We expand into
         * a fresh `header[]` and then re-anchor the parse
         * range to it so the rest of this block doesn't have
         * to know whether expansion happened.
         *
         * header[] sized for `all: $(OBJS)` where $(OBJS)
         * expands to ~1.5 KiB (Doom 78-file build).  16 KiB
         * gives 10x headroom (chapter 133d). */
        static char header[16 * 1024];
        {
            char raw[MK_MAX_VAL];
            int linelen = end - p;
            int tl = linelen < (int)sizeof(raw) - 1 ? linelen
                                                    : (int)sizeof(raw) - 1;
            for (int k = 0; k < tl; k++) raw[k] = g_src[p + k];
            raw[tl] = 0;
            if (mk_expand_into(header, sizeof(header), raw,
                               0, 0, 0, 0) < 0)
                return -1;
        }
        /* Re-point the source pointer used by the rule-header
         * parsing below at the expanded buffer.  Reset p/end. */
        const char *src_save = g_src;
        int    p_save        = p;
        int    end_save      = end;
        /* Switch g_src to the local expanded buffer for the
         * remainder of this iteration.  All offsets are
         * recomputed; the original cursor `i` is preserved
         * in the outer loop. */
        const char *line_src = header;
        int line_end = mk_strlen(header);
        int line_p   = 0;

        int colon = -1;
        for (int k = line_p; k < line_end; k++)
            if (line_src[k] == ':') { colon = k; break; }
        if (colon < 0) {
            printf("make: line %d: expected 'target:', "
                   "'VAR = value', or recipe\n", line_no);
            return -1;
        }

        /* Target = first whitespace-delimited token before ':'. */
        int t_start = line_p;
        int t_end = t_start;
        while (t_end < colon && !mk_isspace(line_src[t_end])) t_end++;
        char target[MK_MAX_NAME];
        int t_len = t_end - t_start;
        if (t_len == 0) {
            printf("make: line %d: empty target\n", line_no);
            return -1;
        }
        if (t_len >= MK_MAX_NAME) t_len = MK_MAX_NAME - 1;
        for (int k = 0; k < t_len; k++) target[k] = line_src[t_start + k];
        target[t_len] = 0;

        /* Suppress unused warnings for save vars (we keep them
         * in case future code needs to roll back). */
        (void)src_save; (void)p_save; (void)end_save;

        /* .PHONY: deps  -- record each dep as a phony marker. */
        if (mk_streq(target, ".PHONY")) {
            int d = colon + 1;
            while (d < line_end) {
                while (d < line_end && mk_isspace(line_src[d])) d++;
                if (d >= line_end) break;
                int ds = d;
                while (d < line_end && !mk_isspace(line_src[d])) d++;
                int dl = d - ds;
                if (dl == 0) break;
                if (g_phony_count >= MK_MAX_PHONY) {
                    printf("make: too many .PHONY entries\n");
                    return -1;
                }
                if (dl >= MK_MAX_NAME) dl = MK_MAX_NAME - 1;
                char *dst = g_phony[g_phony_count++];
                for (int k = 0; k < dl; k++) dst[k] = line_src[ds + k];
                dst[dl] = 0;
            }
            current_rule = 0;
            current_pattern = 0;
            line_no++;
            continue;
        }

        /* Pattern rule?  Target contains '%' and so does the
         * (single) dependency. */
        int has_pct = 0;
        for (int k = 0; k < t_len; k++)
            if (target[k] == '%') { has_pct = 1; break; }
        if (has_pct) {
            if (g_pattern_count >= MK_MAX_PATTERNS) {
                printf("make: too many pattern rules\n");
                return -1;
            }
            mk_pattern *pp = &g_patterns[g_pattern_count++];
            pp->recipe_count = 0;
            if (!mk_split_pattern(target, pp->target_prefix,
                                  pp->target_suffix)) {
                printf("make: line %d: bad target pattern '%s'\n",
                       line_no, target);
                return -1;
            }

            /* Grab the first dep token only. */
            int d = colon + 1;
            while (d < line_end && mk_isspace(line_src[d])) d++;
            if (d >= line_end) {
                printf("make: line %d: pattern rule needs a dep\n",
                       line_no);
                return -1;
            }
            int ds = d;
            while (d < line_end && !mk_isspace(line_src[d])) d++;
            int dl = d - ds;
            char deppat[MK_MAX_NAME];
            if (dl >= MK_MAX_NAME) dl = MK_MAX_NAME - 1;
            for (int k = 0; k < dl; k++) deppat[k] = line_src[ds + k];
            deppat[dl] = 0;
            if (!mk_split_pattern(deppat, pp->dep_prefix,
                                  pp->dep_suffix)) {
                printf("make: line %d: bad dep pattern '%s'\n",
                       line_no, deppat);
                return -1;
            }
            current_rule = 0;
            current_pattern = pp;
            line_no++;
            continue;
        }

        /* Plain rule. */
        if (g_rule_count >= MK_MAX_RULES) {
            printf("make: too many rules (max %d)\n", MK_MAX_RULES);
            return -1;
        }
        current_rule = &g_rules[g_rule_count];
        current_pattern = 0;
        current_rule->dep = g_rule_deps[g_rule_count];
        current_rule->dep_count = 0;
        current_rule->recipe_count = 0;
        current_rule->in_progress = 0;
        current_rule->built = 0;
        for (int k = 0; k < t_len; k++)
            current_rule->target[k] = target[k];
        current_rule->target[t_len] = 0;
        g_rule_count++;

        /* Deps = whitespace tokens after ':'. */
        int d = colon + 1;
        while (d < line_end) {
            while (d < line_end && mk_isspace(line_src[d])) d++;
            if (d >= line_end) break;
            int ds = d;
            while (d < line_end && !mk_isspace(line_src[d])) d++;
            int dl = d - ds;
            if (dl == 0) break;
            if (current_rule->dep_count >= MK_MAX_DEPS) {
                printf("make: line %d: too many deps (max %d)\n",
                       line_no, MK_MAX_DEPS);
                return -1;
            }
            if (dl >= MK_MAX_NAME) dl = MK_MAX_NAME - 1;
            char *dst = current_rule->dep[current_rule->dep_count];
            for (int k = 0; k < dl; k++) dst[k] = line_src[ds + k];
            dst[dl] = 0;
            current_rule->dep_count++;
        }
        line_no++;
    }
    return 0;
}

/* ── rule lookup ────────────────────────────────────────── */

static mk_rule *mk_find_exact(const char *target)
{
    for (int i = 0; i < g_rule_count; i++)
        if (mk_streq(g_rules[i].target, target))
            return &g_rules[i];
    return 0;
}

/* Synthetic rule for pattern matches.  We borrow a static slot
 * because mk_build is single-threaded and we only need the
 * synth rule alive for the duration of one pattern lookup +
 * recursive build.  If pattern rules could chain to other
 * pattern rules this would need a stack of slots; for the Doom
 * build (one pattern rule, .c -> .o, never chained) one slot
 * is enough. */
static mk_rule g_pattern_synth;
static char    g_pattern_synth_deps[1][MK_MAX_NAME];

static mk_rule *mk_find_pattern(const char *target)
{
    for (int i = 0; i < g_pattern_count; i++) {
        mk_pattern *pp = &g_patterns[i];
        if (!mk_starts_with(target, pp->target_prefix)) continue;
        if (!mk_ends_with(target, pp->target_suffix)) continue;
        int tlen = mk_strlen(target);
        int prelen = mk_strlen(pp->target_prefix);
        int suflen = mk_strlen(pp->target_suffix);
        int stem_len = tlen - prelen - suflen;
        if (stem_len < 0) continue;
        /* Build the synthetic dep: dep_prefix + stem + dep_suffix. */
        char dep[MK_MAX_NAME];
        int  di = 0;
        int  prefix_n = mk_strlen(pp->dep_prefix);
        for (int k = 0; k < prefix_n && di < MK_MAX_NAME - 1; k++)
            dep[di++] = pp->dep_prefix[k];
        for (int k = 0; k < stem_len && di < MK_MAX_NAME - 1; k++)
            dep[di++] = target[prelen + k];
        int suffix_n = mk_strlen(pp->dep_suffix);
        for (int k = 0; k < suffix_n && di < MK_MAX_NAME - 1; k++)
            dep[di++] = pp->dep_suffix[k];
        dep[di] = 0;

        mk_strncopy(g_pattern_synth.target, target, MK_MAX_NAME);
        mk_strncopy(g_pattern_synth_deps[0], dep, MK_MAX_NAME);
        g_pattern_synth.dep = g_pattern_synth_deps;
        g_pattern_synth.dep_count = 1;
        g_pattern_synth.recipe_count = pp->recipe_count;
        for (int k = 0; k < pp->recipe_count; k++)
            mk_strncopy(g_pattern_synth.recipe[k], pp->recipe[k],
                        MK_MAX_LINE);
        g_pattern_synth.in_progress = 0;
        g_pattern_synth.built = 0;
        return &g_pattern_synth;
    }
    return 0;
}

static int mk_is_phony(const char *target)
{
    for (int i = 0; i < g_phony_count; i++)
        if (mk_streq(g_phony[i], target)) return 1;
    return 0;
}

/* ── recipe execution ───────────────────────────────────── */

static int mk_run_recipe(const char *raw, const char *target,
                         char (*deps)[MK_MAX_NAME], int dep_count)
{
    /* Strip leading '@' (silent) and '-' (ignore errors). */
    int silent = 0, ignore_err = 0;
    while (*raw == '@' || *raw == '-') {
        if (*raw == '@') silent = 1;
        else             ignore_err = 1;
        raw++;
    }

    /* Expand $(VAR) and automatic vars into a stack buffer. */
    static char expanded[MK_MAX_LINE * 4];
    if (mk_expand_into(expanded, sizeof(expanded), raw,
                       target, deps, dep_count, 0) < 0)
        return -1;

    if (!silent) printf("%s\n", expanded);

    /* Split on first whitespace: argv0 = program path, rest =
     * args.  sys_spawn re-tokenises args into argv. */
    char path[MK_MAX_NAME];
    int  i = 0;
    while (expanded[i] && expanded[i] != ' ' && expanded[i] != '\t'
           && i < MK_MAX_NAME - 1) {
        path[i] = expanded[i];
        i++;
    }
    path[i] = 0;
    if (i == 0) {
        if (ignore_err) return 0;
        printf("make: empty recipe line\n");
        return -1;
    }
    int src = i;
    while (expanded[src] == ' ' || expanded[src] == '\t') src++;

    int pid = spawn(path, expanded + src);
    if (pid < 0) {
        if (ignore_err) return 0;
        printf("make: spawn '%s' failed (errno=%d)\n", path, -pid);
        return -1;
    }
    int code = 0;
    int w = waitpid(pid, &code, 0);
    if (w < 0) {
        if (ignore_err) return 0;
        printf("make: waitpid on '%s' failed\n", path);
        return -1;
    }
    if (code != 0) {
        if (ignore_err) return 0;
        printf("make: recipe for '%s' exited with code %d\n",
               target, code);
        return code;
    }
    return 0;
}

/* ── build driver ───────────────────────────────────────── */

static int mk_build(const char *target)
{
    mk_rule *r = mk_find_exact(target);
    int from_pattern = 0;
    if (!r) {
        r = mk_find_pattern(target);
        from_pattern = (r != 0);
    }

    if (!r) {
        /* No rule for `target`.  We don't do stat-based
         * out-of-date checking, so for now we trust the file
         * exists and return success.  The user-requested top
         * target is handled separately in main() and will
         * error out before we get here. */
        return 0;
    }
    if (r->built) return 0;
    if (r->in_progress) {
        printf("make: circular dependency on '%s'\n", target);
        return -1;
    }
    r->in_progress = 1;

    /* The synthetic pattern slot is shared across recursive
     * pattern lookups.  Snapshot it on the stack so the
     * recursive mk_build call for our dep can clobber the
     * slot without corrupting us.  Plain (non-pattern) rules
     * have their own permanent storage in g_rules[] / their
     * dep slab; for those we just use the rule pointer
     * directly and skip the (~8 KiB) snapshot.  This is what
     * keeps us inside the 64 KiB user stack. */
    char  pat_target[MK_MAX_NAME];
    char  pat_dep[MK_MAX_NAME];
    char  pat_recipe[MK_MAX_RECIPE][MK_MAX_LINE];
    int   pat_recipe_count = 0;
    const char *use_target;
    char (*use_deps)[MK_MAX_NAME];
    int  use_dep_count;
    char (*use_recipe)[MK_MAX_LINE];
    int  use_recipe_count;

    if (from_pattern) {
        mk_strncopy(pat_target, r->target, MK_MAX_NAME);
        mk_strncopy(pat_dep, r->dep[0], MK_MAX_NAME);
        pat_recipe_count = r->recipe_count;
        for (int i = 0; i < pat_recipe_count; i++)
            mk_strncopy(pat_recipe[i], r->recipe[i], MK_MAX_LINE);
        use_target = pat_target;
        use_deps = (char (*)[MK_MAX_NAME])pat_dep;
        use_dep_count = 1;
        use_recipe = pat_recipe;
        use_recipe_count = pat_recipe_count;
    } else {
        use_target = r->target;
        use_deps = r->dep;
        use_dep_count = r->dep_count;
        use_recipe = r->recipe;
        use_recipe_count = r->recipe_count;
    }

    for (int i = 0; i < use_dep_count; i++) {
        if (mk_build(use_deps[i]) != 0) {
            r->in_progress = 0;
            return -1;
        }
    }

    for (int i = 0; i < use_recipe_count; i++) {
        int rc = mk_run_recipe(use_recipe[i], use_target,
                               use_deps, use_dep_count);
        if (rc != 0) {
            r->in_progress = 0;
            return rc;
        }
    }

    r->in_progress = 0;
    r->built = 1;
    if (mk_is_phony(target)) r->built = 0;   /* re-run if asked */
    return 0;
}

/* ── main ───────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *makefile_path = "Makefile";
    const char *want_target   = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (mk_streq(a, "-f")) {
            if (i + 1 >= argc) {
                printf("make: -f needs a path\n");
                return 1;
            }
            makefile_path = argv[++i];
        } else if (a[0] == '-') {
            printf("make: unknown flag '%s'\n", a);
            return 1;
        } else {
            want_target = a;
        }
    }

    int sz = mk_read_file(makefile_path);
    if (sz < 0) return 1;
    mk_apply_continuations();
    if (mk_parse() < 0) return 1;

    /* Default goal = first non-pattern rule. */
    if (!want_target) {
        if (g_rule_count == 0) {
            printf("make: no rules in '%s'\n", makefile_path);
            return 1;
        }
        want_target = g_rules[0].target;
    }

    /* The user-requested top target MUST have a rule (or be
     * matched by a pattern).  Leaf files we hit during
     * recursion are allowed to be ruleless. */
    if (!mk_find_exact(want_target) &&
        !mk_find_pattern(want_target)) {
        printf("make: no rule to make target '%s'\n", want_target);
        return 1;
    }

    int rc = mk_build(want_target);
    if (rc != 0) return 1;
    printf("make: built '%s'\n", want_target);
    return 0;
}
