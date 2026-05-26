# Chapter 126 — `/bin/make`: a tiny build driver

> "If you've got a compiler and a linker, you need a thing
> that knows what to do with them."

By chapter 125 we have everything required to build a
program — `/bin/cc`, `/bin/as`, `/bin/ld` — but the user
has to type the right invocations in the right order, by
hand, every time.  Chapter 126 ships `/bin/make`: a
deliberately tiny port of GNU make's core idea.  No
variables, no pattern rules, no parallelism — but a real
parser, a real dependency DAG, and a real recipe driver.

## What we built

[userspace/make/make.c](../../../userspace/make/make.c) —
about 280 lines of freestanding C.  The file has one
public symbol (`main`) and reads a single text buffer into
an array of rule structs.

| Knob | Compile-time value |
|---|---|
| Max rules per Makefile | 16 |
| Max deps per rule | 8 |
| Max recipe lines per rule | 8 |
| Max characters per line | 256 |
| Max characters per target/dep name | 64 |
| Max characters per Makefile | 32 KiB |

That's enough for the kind of project a chapter-126 user
would write by hand.  Bumping any limit is a one-line edit.

## Grammar

```
Makefile = ( rule | blank | comment )*

rule     = target ':' deps NEWLINE
           ( '\t' recipe NEWLINE )*

target   = NAME
deps     = NAME*                  (zero or more, whitespace-separated)
recipe   = any line whose first byte is TAB
blank    = NEWLINE
comment  = '#' anything NEWLINE
```

Subset of POSIX make.  Notably absent:

| Missing feature | Why we deferred it |
|---|---|
| `$(VAR)` substitution | Needs a symbol table; one more chapter's worth |
| `$@`, `$<`, `$^` automatics | Same, plus a per-recipe context |
| `%.o : %.c` pattern rules | Needs file extension matching + stat() |
| `.PHONY` | We always rebuild, so the distinction is moot today |
| `-j` parallelism | We don't have a job server, just spawn+waitpid |
| Timestamp-based rebuild skipping | We have no stat() on `/data/` yet |
| `@` line prefix (suppress echo) | We always echo recipe lines |

All seven are honest deferrals — each one is a chapter of
work, and none of them is needed to demonstrate "make works
end-to-end on a real disk file."

## CLI

```
make                       # default Makefile, first target
make TARGET                # default Makefile, named target
make -f /data/Makefile     # explicit Makefile path
make -f X TARGET           # both
```

That's it.  Three flag-shaped forms.

## The spawn() argv gotcha (kept here so it doesn't bite us
again)

The kernel's `sys_spawn(path, args)` ALWAYS sets
`argv[0] = path` and then appends `args` split on
whitespace as `argv[1..]`.  See
[kernel/core/syscall.c](../../../kernel/core/syscall.c)
line 386.

That means when /bin/make sees a recipe line like

```
/bin/cc /data/greeter.c -o /tmp/greeter
```

it must call `spawn("/bin/cc", "/data/greeter.c -o /tmp/greeter")`
— note that the args string does NOT include `/bin/cc`.
The first version of /bin/make passed the whole line as
args, which gave `/bin/cc` an argv of:

```
argv[0] = "/bin/cc"
argv[1] = "/bin/cc"         <-- bug
argv[2] = "/data/greeter.c"
```

and `/bin/cc` then saw argv[1] as a second source file and
errored out with `cc: only one source file accepted`.  The
fix in [userspace/make/make.c](../../../userspace/make/make.c)
is two lines: skip the first whitespace-delimited token of
the recipe line before copying the rest into args.

This is the same convention `/bin/cc` itself uses when it
spawns `/bin/as` and `/bin/ld` — it passes args WITHOUT
the program name.  We just needed to learn it the hard
way.

## The test

[scripts/test_make_port.py](../../../scripts/test_make_port.py)
seeds two files onto `/data/`:

```c
// /data/greeter.c
int main(void) { printf("M126-GREETER-OK\n"); return 0; }
```

```make
# /data/Makefile
all: prepare build
	/bin/echo M126-MAKE-ALL-DONE

prepare:
	/bin/echo M126-PREPARE-RAN

build:
	/bin/cc /data/greeter.c -o /tmp/greeter
```

Then boots osdev and runs `/bin/make -f /data/Makefile`.
14 assertions verify:

| # | Assertion |
|---|---|
| 1 | `/bin/make` is installed at `/bin/make` |
| 2-3 | Both seed files are visible on `/data/` |
| 4 | The `prepare` rule's recipe ran |
| 5 | The `build` rule invoked `/bin/cc` and got `cc: wrote /tmp/greeter` |
| 6 | The `all` rule's own recipe ran AFTER its deps |
| 7 | `/bin/make` announced `built 'all'` |
| 8 | Recipe order in the log is strictly `prepare < build < all` |
| 9 | `/tmp/greeter` (the compiled binary) prints its marker |
| 10 | `/tmp/greeter` exits with code 0 |
| 11 | A second invocation of make also succeeds (no state leak) |
| 12-14 | `make prepare` (explicit target) runs ONLY the prepare rule, not the cc rule |

All 14 pass on the first deterministic run.

## What stays missing

| Missing capability | Where it'll be useful |
|---|---|
| Variables (CC, CFLAGS, LDFLAGS) | Once we have more than one Makefile in the OS |
| Automatic vars (`$@`, `$<`, `$^`) | Pattern rules, once we have stat() |
| stat-based skip | Real incremental builds (today every `make` rebuilds) |
| Parallelism (-j) | Multi-core compile time, after SMP scheduler tuning |
| `@cmd` (suppress echo) | Cleaner build logs, cosmetic only |
| `-include` | Modular Makefiles, after we have multiple Makefiles |

## Applied to existing apps

Per the user directive (apps use OS features):

- `/bin/cc` is invoked by `/bin/make` — first time an
  in-guest program drives the compiler other than the
  shell.  This proves the spawn-based driver pattern works
  for *any* userspace, not just `/bin/cc` itself.
- `/bin/echo` is invoked by `/bin/make` — exercises the
  generic recipe path with a non-compiler binary.
- `/data/` mount continues to be the canonical "stuff the
  user wrote" filesystem; the Makefile lives there
  alongside the source.
- The chapter-120 `crt0 + atexit` machinery runs for the
  `/bin/make` binary itself, since make uses printf which
  triggers stdio's atexit-registered flush.  Implicit
  validation that crt0 still works for a tool that spawns
  other tools.

The notepad app (chapter 127) will tie this all together
by adding a "Build" button that runs `/bin/make` on a file
the user is currently editing.

## Lessons

1. The argv-prepend convention is a real protocol that
   every spawn-using userspace tool now has to know.
   Worth a comment in every recipe-runner: "do not include
   the program name in args."
2. A real dependency DAG is shockingly little code (29
   lines for the recursive builder).  Most of the work in
   make is the *grammar*, not the *graph*.
3. Per the apps-use-features rule, the chapter-126
   deliverable is not just a tool — it is a tool the next
   chapter will *use* (the notepad Build button calls
   `/bin/make`).  Without that downstream use the tool
   would be dead weight.

Next: chapter 127, the notepad Build button.  When the
user clicks Build, notepad saves the current buffer to
`/tmp/scratch.c`, spawns `/bin/make -f /tmp/Makefile`, and
displays the output in a popup.  Full loop: type code in
the GUI, build it from the GUI, run it from the GUI.
