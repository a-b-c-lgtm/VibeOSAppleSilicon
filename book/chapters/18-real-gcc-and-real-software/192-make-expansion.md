# Chapter 192 — Expanding `/bin/make` for a real multi-file build

> **Milestone in this chapter:** grow the chapter-126 toy
> `/bin/make` into a real Makefile interpreter — variables,
> automatic vars, pattern rules, `.PHONY`, line continuation.
> **Code referenced:**
> - [userspace/make/make.c](../../../userspace/make/make.c)
> - [scripts/test_make_v2.py](../../../scripts/test_make_v2.py)
>
> **At the end of this chapter** you will have `/bin/make`
> at ~720 LoC handling `VAR = value`, `$(VAR)` / `${VAR}` /
> `$$` expansion, the automatic vars `$@` / `$<` / `$^`,
> `%.o: %.c` pattern rules, `.PHONY:`, `@` (silent) and `-`
> (ignore-error) recipe prefixes, and line continuation — with
> `test_make_v2.py` at **PASS 9 / FAIL 0** and the chapter-126
> `test_make_port.py` still green. Prerequisites: chapter 162
> (the `/bin/make` skeleton), chapter 160 (`/bin/gcc` working
> in-guest).

---

## What you'll do in this chapter

1. Grow `userspace/make/make.c` from 351 LoC to ~720 LoC,
   adding `VAR = value` definitions, `$(VAR)` / `${VAR}` /
   `$$` expansion, automatic vars `$@` / `$<` / `$^`,
   `%.o: %.c` pattern rules, `.PHONY:`, `@` (silent) and
   `-` (ignore-error) recipe prefixes, and line
   continuation.
2. Size the capacity tables (`MK_MAX_RULES`,
   `MK_MAX_PATTERNS`, `MK_MAX_DEPS`, `MK_MAX_VARS`) so the
   DoomGeneric build with its 202-entry `OBJS` list fits.
3. Ship a three-file fixture under `assets/osfs/`
   (`mk_test.mk`, `mk_helloA.c`, `mk_helloB.c`) that
   exercises every new feature in one invocation, and
   stage the trio onto `/bin/` via mkosfs.
4. Add `scripts/test_make_v2.py` (4 steps, 9 expectations:
   fixture sanity, ELF check, recipe run, produced-binary
   run) and verify it lands 9/9 with chapter 162's
   `test_make_port.py` still PASS 14/14.

---

## Why now

Chapter 162 shipped a `/bin/make` whose entire vocabulary was:

```make
target: dep1 dep2
	cmd args
	cmd args
```

That's all. No variables. No `$@`. No pattern rules. No way
to say "for each `.c` file, compile a `.o`." Every rule had
to be written out longhand:

```make
mk_helloA.o:
	/bin/gcc -c /bin/mk_helloA.c -o /tmp/mk_helloA.o

mk_helloB.o:
	/bin/gcc -c /bin/mk_helloB.c -o /tmp/mk_helloB.o

mk_hello:
	/bin/gcc /tmp/mk_helloA.o /tmp/mk_helloB.o -o /tmp/mk_hello
```

For DoomGeneric's 202 source files that would be 200+ rules
written out by hand, with every file path repeated three or
four times. Not actually usable.

The standing rule for this OS is straightforward: when a new
OS feature lands, existing apps adopt it. `/bin/make` is the
app; the feature it needs is the same one every real
Makefile uses.

## What you'll write

| Feature | Before | After |
| --- | --- | --- |
| `VAR = value` | parse error | recorded in `g_vars[64]` |
| `$(VAR)` / `${VAR}` / `$$` | literal | recursive expand (depth 8) |
| `$@` `$<` `$^` | literal | per-target expansion at recipe-run |
| `%.o: %.c` pattern rule | absent | `mk_split_pattern` + synth slot |
| `.PHONY:` | absent | `g_phony[16]`; resets `built=0` |
| `@cmd` (silent) | echoed | suppressed |
| `-cmd` (ignore err) | propagated | spawn failure returns 0 |
| Line continuation `\\\n` | broken | pre-pass `mk_apply_continuations` |

Source: [`userspace/make/make.c`](../../../userspace/make/make.c),
720 LoC (was 351).

Things deliberately **not** added (each would be a future
chapter):

- `:=` (simple expand), `?=` (default), `+=` (append)
- `$(wildcard …)`, `$(patsubst …)`, `$(shell …)`,
  `$(addprefix …)`
- `ifeq` / `ifneq` / `ifdef` / `endif` conditionals
- `include` / `-include`
- Order-only prereqs (`foo: a b | c`)
- `-j` parallelism
- mtime-based out-of-date checking (every requested target
  still rebuilds unconditionally)

The Doom build doesn't need those. Chapter 193 ships a
hand-written tailored Makefile with an explicit `OBJS = ...`
list of all 202 .o files.

## Capacity numbers

```c
#define MK_MAX_RULES       32
#define MK_MAX_PATTERNS    16
#define MK_MAX_DEPS       256   /* doom: am_map.o ... ~200 .o */
#define MK_MAX_RECIPE      16
#define MK_MAX_VARS        64
#define MK_MAX_LINE       512
#define MK_MAX_NAME        96
#define MK_MAX_SRC      (96 * 1024)

static char g_rule_deps[MK_MAX_RULES][MK_MAX_DEPS][MK_MAX_NAME];
/*           768 KiB bss */
```

768 KiB of bss for the per-rule dep slabs sounds extravagant
but it's pragmatic: the alternative is heap-allocating per-rule
arrays at parse time, which complicates lifetime + leaks if
parsing fails. With 8 MiB user heap available it's noise.

## Pitfalls

### Pitfall — don't eat `$@` at parse time

**Symptom:** After teaching the expander to handle `$@`, the
link recipe in `mk_test.mk` blew up like this:

```text
[Linking $(OUT)]
/bin/gcc -O0 $(OBJS) -o $(OUT)
ld: cannot open output file $(OUT): Read-only file system
```

The `@echo` line was the recipe `[Linking $@]` — but `$@`
expanded to the literal string `$(OUT)` (not `/tmp/mk_hello`)
because the *target* of the rule was the unexpanded literal
`$(OUT)`. (That second part is the next pitfall.)

**Cause:** The first cut of `mk_expand_into` looked like this:

```c
if (in[i + 1] == '@') {
    if (autotarget) {
        /* copy autotarget into out[] */
    }
    i += 2; continue;     /* ⚠ eats $@ even if autotarget==NULL */
}
```

That `i += 2` runs unconditionally. So calling the expander
at parse time (when there is no target yet —
`autotarget == NULL`) silently consumes `$@`. The recipe
stored in `g_rules[].recipe[]` no longer contains `$@`; at
recipe-run time the substitution has nothing to bite.

**Fix:** When `autotarget == NULL`, copy `$@`/`$<`/`$^`
through verbatim. Recipe-time expansion handles them.

```c
if (in[i + 1] == '@') {
    if (autotarget) {
        /* copy autotarget */
        i += 2;
    } else {
        out[oi++] = in[i++];  /* verbatim; recipe-time will handle */
    }
    continue;
}
```

### Pitfall — rule headers MUST be expanded at parse time

**Symptom:** The `mk_test.mk` fixture looks like this:

```make
CC = /bin/gcc
CFLAGS = -O0
OUT = /tmp/mk_hello
OBJS = /tmp/mk_helloA.o /tmp/mk_helloB.o

all: $(OUT)

$(OUT): $(OBJS)
	@/bin/echo [Linking $@]
	$(CC) $(CFLAGS) $^ -o $@
```

The first cut parsed `$(OUT): $(OBJS)` and stored:

- `target = "$(OUT)"` (literal)
- `dep[0] = "$(OBJS)"` (literal, one dep)

Then `mk_build("all")` runs, depends on `"$(OUT)"`, finds
the rule whose target is the literal string `"$(OUT)"`,
recurses on dep `"$(OBJS)"`. There is no rule for that
literal name, so `mk_build` treats it as a leaf-file-exists
and returns 0. No .o files get built.

**Cause:** The link recipe then runs with literal `$(OBJS)`
and `$(OUT)` because the recipe-time expander has nothing
in `g_vars` for those names — they were defined, but the
recipe `$(CC) ... $^ -o $@` contains `$^` and `$@`, and
those expand to the rule's literal dep / target strings,
which are themselves unexpanded. End result: `ld -o $(OUT)`
writes to a file literally named `$(OUT)` in cwd, which is
`/`, which is read-only.

**Fix:** In `mk_parse`, before splitting the line on `:`,
expand the entire rule-header line through `mk_expand_into`
with `autotarget=NULL`. The expanded text lives in a static
buffer; the parsing code below the expand call reads from
that buffer instead of `g_src`.

```c
static char header[MK_MAX_LINE * 4];
{
    char raw[MK_MAX_LINE];
    /* copy raw line out of g_src */
    mk_expand_into(header, sizeof(header), raw, 0, 0, 0, 0);
}
const char *line_src = header;
int line_end = mk_strlen(header);
/* ... rule parser reads from line_src instead of g_src ... */
```

This makes a two-pass behavior implicit: variables must be
defined **before** they're referenced in a rule header.
Otherwise the expansion returns empty and the parser sees
`": :"` or similar nonsense. Same constraint GNU make
already imposes for `:=`-style variables; close enough.

### Pitfall — synthetic pattern slot vs recursion

**Symptom:** Pattern rules are stored in
`g_patterns[MK_MAX_PATTERNS]` without a concrete target —
just the prefix/suffix pair on either side of the `%`. When
`mk_build("am_map.o")` needs to find a rule,
`mk_find_pattern` synthesizes a concrete `mk_rule` on the
fly into a single shared slot:

```c
static mk_rule g_pattern_synth;    /* ⚠ shared! */
static char    g_pattern_synth_deps[1][MK_MAX_NAME];

static mk_rule *mk_find_pattern(const char *target) {
    /* ... match target against each pattern ... */
    mk_strncopy(g_pattern_synth.target, target, MK_MAX_NAME);
    mk_strncopy(g_pattern_synth_deps[0], dep, MK_MAX_NAME);
    /* ... copy recipe ... */
    return &g_pattern_synth;
}
```

If `mk_build` recurses on the dep and *that* dep is also a
pattern match, the synth slot gets clobbered before the
outer `mk_build` runs its recipe.

**Cause:** For the exact Makefile shape this chapter uses,
this never fires (the dep of a `%.o` pattern is `%.c`, which
has no rule and hits the leaf-exists return-0 path). But
the safety net matters for future Makefiles, and the cost is
small.

**Fix:** When `mk_build` got a rule from a pattern match,
snapshot the target, dep, and recipe onto stack locals
before recursing on the dep:

```c
if (from_pattern) {
    mk_strncopy(pat_target, r->target, MK_MAX_NAME);
    mk_strncopy(pat_dep, r->dep[0], MK_MAX_NAME);
    for (int i = 0; i < r->recipe_count; i++)
        mk_strncopy(pat_recipe[i], r->recipe[i], MK_MAX_LINE);
    use_target = pat_target;
    use_deps   = (char (*)[MK_MAX_NAME])pat_dep;
    use_recipe = pat_recipe;
} else {
    use_target = r->target;
    use_deps   = r->dep;
    use_recipe = r->recipe;
}
```

Snapshot size: 96 (target) + 96 (one dep) + 16×512 (recipes)
= ~8.5 KiB per recursion frame. The user stack is 64 KiB
(`USER_STACK_PAGES = 16` in
[`kernel/core/elf.c`](../../../kernel/core/elf.c)). At
3-level recursion depth (top → .o → .c-leaf-returns-0)
peak stack is ~25 KiB, comfortably inside the limit.

Plain (non-pattern) rules skip the snapshot entirely
because their target/dep/recipe storage is permanent in
`g_rules[]` and `g_rule_deps[]`.

The first draft snapshotted unconditionally (32 KiB per
frame). Three levels of recursion would have overflowed the
stack. Always check the stack budget when adding heavy
locals to a recursive function.

## Recipe execution: still no /bin/sh

The recipe-run path is unchanged from chapter 162:

```c
int src = i;
while (expanded[src] == ' ' || expanded[src] == '\t') src++;
int pid = spawn(path, expanded + src);
int code; waitpid(pid, &code, 0);
```

`spawn(path, rest)` calls `sys_spawn`, which uses the kernel
tokenizer (whitespace split) to build argv. There is no
`/bin/sh` invocation on the recipe line. That means:

- No pipes inside recipes (`a | b`)
- No redirections (`> /tmp/foo`)
- No shell glob (`*.c`)
- No `&&` chaining

For the tailored Makefiles this chapter targets that's fine
— but it does mean "compile + redirect output to file" needs
the program to take a `-o file` flag (which `/bin/gcc` does).
Anything that wants pipelines needs a wrapper program or a
chapter that adds an actual shell-runner mode.

## Pattern rule mechanics

`mk_split_pattern("%.o", prefix, suffix)` writes
`prefix=""`, `suffix=".o"`. Same for `%.c`. Match logic:

```c
mk_starts_with(target, pp->target_prefix) &&
mk_ends_with(target, pp->target_suffix)
```

…then the *stem* is `target[prelen .. tlen-suflen]`. The
synthesised dep is `dep_prefix + stem + dep_suffix`.

The fixture uses `/tmp/%.o: /bin/%.c`. For `target =
/tmp/mk_helloA.o`:

- target prefix `/tmp/`, suffix `.o` → stem = `mk_helloA`
- dep = `/bin/` + `mk_helloA` + `.c` = `/bin/mk_helloA.c`

Concrete dep path; passed straight to `/bin/gcc -c`. Clean.

## `.PHONY:` semantics

`g_phony[]` is a flat list of names. `mk_is_phony(target)`
walks it. If the target is phony, after running its recipe
`r->built` is reset to 0 so a subsequent `mk_build` on the
same target will re-run (matches GNU make: phony targets are
"always out of date").

In the fixture: `.PHONY: clean`. The smoke test doesn't run
`make clean`, but the line documents the syntax for this
chapter.

## Line continuation

```make
OBJS = /tmp/mk_helloA.o \
       /tmp/mk_helloB.o
```

`mk_apply_continuations` runs once before parsing:

```c
while (g_src[r]) {
    if (g_src[r] == '\\' && g_src[r + 1] == '\n') {
        g_src[w++] = ' ';
        r += 2;
        while (g_src[r] == ' ' || g_src[r] == '\t') r++;
        continue;
    }
    g_src[w++] = g_src[r++];
}
```

Removes `\\\n` plus leading whitespace on the next line,
replacing the whole sequence with a single space. The rest
of the parser sees logical lines.

## Test fixture: `assets/osfs/mk_*.{c,mk}`

Three files shipped on `/bin`:

- [`assets/osfs/mk_test.mk`](../../../assets/osfs/mk_test.mk)
  — the Makefile under test. Exercises every new feature
  in one invocation.
- [`assets/osfs/mk_helloA.c`](../../../assets/osfs/mk_helloA.c)
  — has `main()`, calls `hello_from_B()`, prints the result.
- [`assets/osfs/mk_helloB.c`](../../../assets/osfs/mk_helloB.c)
  — defines `hello_from_B()` returning 42.

Headers in `mk_helloA.c` use `<syscall.h>` / `<printf.h>`
(angle brackets, not `"../libc/foo.h"`) because in-guest
cpp finds them via `-isystem /bin` — chapter 190 convention.

Makefile additions: three entries appended to the OSFS-1
producer in
[`Makefile`](../../../Makefile) — `mk_test.mk=…`,
`mk_helloA.c=…`, `mk_helloB.c=…`, plus the three file paths
added to `OSFS_FILES` so the disk rebuilds when the fixtures
change.

## Run it / Test it

`scripts/test_make_v2.py` is **PASS 9 / FAIL 0**, four
steps, nine expectations:

1. **Fixture sanity** (2):
   - `cat /bin/mk_test.mk` shows the expected content
   - `cat /bin/mk_helloA.c` shows the expected content
2. **`/bin/make` is an ELF** (1)
3. **Run the Makefile** (4):
   - `[Compiling /bin/mk_helloA.c]` printed — proves `$<`
     expanded to the per-target dep
   - `[Compiling /bin/mk_helloB.c]` printed — proves the
     pattern rule fired for the second instance with a
     fresh stem
   - `[Linking /tmp/mk_hello]` printed — proves `$@`
     expanded to the rule's target AND that pattern rule
     fired (with deps built) before the top-level rule
   - `make: built 'all'` — proves the whole chain reached
     the top
4. **Produced binary is an ELF** (1)
5. **Produced binary runs and prints `hello A=42`** (1)
   — proves `$^` expanded to both .o files in the right
   order so the link succeeded with the symbol from B
   visible to A

Test run time: ~5 minutes (two full `/bin/gcc` invocations
inside QEMU + the link step + the produced-binary execution).

End-to-end transcript (excerpt):

```text
/$ /bin/make -f /bin/mk_test.mk
[Compiling /bin/mk_helloA.c]
/bin/gcc -O0 -c /bin/mk_helloA.c -o /tmp/mk_helloA.o
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
[Compiling /bin/mk_helloB.c]
/bin/gcc -O0 -c /bin/mk_helloB.c -o /tmp/mk_helloB.o
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
[Linking /tmp/mk_hello]
/bin/gcc -O0 /tmp/mk_helloA.o /tmp/mk_helloB.o -o /tmp/mk_hello
[sys_exit] thread '/bin/xgcc' exited with code 0x0000000000000000
make: built 'all'
/$ /tmp/mk_hello
hello A=42
/$
```

The `@/bin/echo` lines stay silent in this transcript
because the `@` prefix suppresses the recipe-echo print
(the recipe-echo line for the `echo` itself is gone). The
actual `/bin/echo` output (`[Compiling /bin/...]`) is still
printed — that's the program's stdout, not make's.

Regression sweep (60/0):

| Test | Result |
| --- | --- |
| `test_make_port.py` (chapter 162) | 14/0 |
| `test_make_v2.py` (this chapter)  | 9/0  |
| `test_tar.py`                     | 8/0  |
| `test_gcc_sys_stat.py`            | 6/0  |
| `test_gcc_hello.py`               | 10/0 |
| `test_gcc_bf.py`                  | 6/0  |
| `test_gcc_stdio.py`               | 7/0  |

Chapter 162's toy makefile still works because the simple
"target: deps + recipe" path is unchanged: no variables, no
`$@`, no patterns — the parser falls through to the
plain-rule branch and the executor doesn't have to expand
anything.

## What this unlocks

Chapter 193 can now write a hand-tailored Makefile for the
DoomGeneric source tree that looks like:

```make
CC      = /bin/gcc
CFLAGS  = -O2
OBJS    = am_map.o doomdef.o doomstat.o d_event.o ... (202 files)
OUT     = /data/doom

all: $(OUT)

$(OUT): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
```

…drop it on `/bin/doom.makefile` (or extract it into
`/data/Makefile` from the tarball), `cd /data/src &&
/bin/make -f /bin/doom.makefile`, and an in-guest Doom
binary built by the in-guest toolchain falls out. That's
the finish line of Phase 6 of the guest-gcc bring-up.

Per the standing "apps must use the OS features the book
builds" rule:

- Existing app rewritten to use the new feature: **none yet
  this chapter** — `/bin/make` *is* the feature; no other
  app calls it. Chapter 193 uses it from the in-guest Doom
  rebuild shell session.
- New app added to exercise the feature: **none** — `/bin/make`
  itself is the demonstration. The fixture
  (`mk_test.mk` + two .c files) is the regression scaffold.
- Existing test scripts upgraded: **none** — `test_make_port.py`
  was kept untouched as the chapter-126 regression.
- New test scripts added:
  [`scripts/test_make_v2.py`](../../../scripts/test_make_v2.py)
  — 9 expectations, the chapter-192 regression.

## What's next

Chapter 193 uses the expanded `/bin/make` from inside the
guest to drive the DoomGeneric build: extract the tarball
from chapter 191 onto `/data/src/`, drop a tailored
Makefile alongside it, and have `/bin/make` walk all 202
object files through `/bin/gcc`.

