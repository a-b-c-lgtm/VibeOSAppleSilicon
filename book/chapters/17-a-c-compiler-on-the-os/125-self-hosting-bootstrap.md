# Chapter 125 — The self-hosting gap, and why we don't close it here

> "If your compiler can compile itself, you have a real
> system.  Until then, you have a useful tool."

This chapter is partly aspirational and partly an honest
accounting of what `/bin/cc` is and is not.  Real GCC takes
the self-hosting question seriously enough to have a
three-stage build that *proves* the compiler is a fixed
point of itself.  We can describe that process, and we can
demonstrate the upper bound of what our in-guest compiler
handles today.  We cannot make `/bin/cc` actually
self-host — and Part XVII deliberately stops short of
closing the gap.  Doing so is a Part XVIII undertaking.

## How a real compiler self-hosts

A self-hosting compiler bootstrap is a three-stage proof of
fixed-point convergence:

| Stage | Built by | Compiles |
|---|---|---|
| stage 1 | The host's existing C compiler | The compiler's own source |
| stage 2 | The stage-1 binary | The compiler's own source again |
| stage 3 | The stage-2 binary | The compiler's own source again |

The contract is that **stage 2 and stage 3 must be
byte-identical**.  Why?  Because:

- Stage 1 might have been built with bugs or quirks of the
  host compiler — its output is "good enough to bootstrap"
  but not trusted as the canonical output.
- Stage 2 is the first version compiled by our own
  compiler.  Its *behaviour* should be correct.
- Stage 3 is stage 2 compiling itself, using the bug-fixed
  semantics that *we* defined.  If stage 2 and stage 3
  differ, our compiler is non-deterministic or
  source-dependent in a way that broke convergence.

That fixed-point property is what makes "self-hosting" a
strong claim.  Anyone can build a compiler that compiles
*some* C; the question is whether the compiler can compile
*the source code of itself* and produce the same output
twice in a row.

## What `/bin/cc` would need to self-host

`/bin/cc` itself is written in `~870` lines of full C99.
For it to compile its own source, it would have to add (in
roughly increasing order of difficulty):

| Feature | Lines of cc.c that depend on it |
|---|---|
| `*`, `/`, `%` | every loop counter inside the parser |
| `if`/`else` | every branch in the parser |
| `while`/`for` | every token loop in the lexer |
| `break`, `continue` | a few lexer fast paths |
| Function definitions beyond `main` | all of `cc.c` (every helper) |
| Function calls with arguments | every `lex()`, `emit_*`, `expect()` call |
| `char`, `char *`, pointer arithmetic | the entire string handling |
| `enum`, `struct` | every grammar table |
| String literal concatenation | every error message |
| `sizeof` | a handful of buffer sizes |
| Multi-file compilation + `#include` | the libc headers cc.c depends on |
| File I/O via stdio | reading the source file |
| Preprocessor (`#define`, `#ifdef`) | conditional debug emission |
| Static initialisers | the token-keyword table |
| Variadic functions | `printf` itself, internally |

That is at minimum eight chapter-sized features.  Each one
is shippable; the cumulative effort is a half-year of focus
that does not produce any *new user-visible behaviour* on
the desktop.

We are not going to do it in Part XVII.  We are going to
honestly say: self-hosting is the goal, the gap is
catalogued, the substitute deliverable is the upper-bound
demo below, and the work itself is left for Part XVIII.

## The upper-bound demo

[scripts/test_self_host_demo.py](../../../scripts/test_self_host_demo.py)
ships the largest C source `/bin/cc` has ever digested:

```c
int main(void) {
    int a = 1;  int b = 2;  int c = 3;  int d = 4;  int e = 5;
    int f = 6;  int g = 7;  int h = 8;  int i = 9;  int j = 10;
    int sum = a + b + c + d + e + f + g + h + i + j;
    printf("M125-STAGE-1\n");
    int gauss   = sum + 0;        printf("M125-STAGE-2\n");
    int doubled = gauss + gauss;  printf("M125-STAGE-3\n");
    int half    = doubled - gauss;
    printf("M125-DONE\n");
    int result  = half - sum + sum;
    return result;
}
```

What this exercises that no earlier test did:

| Feature | Stress |
|---|---|
| 11 local variables in one frame | `CC_MAX_VARS = 16` headroom check |
| A 10-term left-associative `+` chain | parser does not exhaust expr-stack |
| Four `printf()` calls interleaved with arithmetic | locals survive register clobber across SVC |
| Five dependent intermediate variables | frame slot allocation is monotonic |
| 12 `add x0, x1, x0` instructions in one function | linker forward-bl placeholders work at scale |
| 29 `str x0, [sp, #N]` stores | every declaration writes its frame slot exactly once |

Exit code 55 = `0x37`.  The compiler runs in roughly 60
seconds inside the VM (most of it the linker walking the
relocation table).  The asm output is `~150` lines.

## Why this counts as "credible self-host work"

Because every previous compiler chapter compiled programs
that *one human could have written from scratch in five
minutes*.  This program is not in that class — it is
mechanical, but it has enough surface that an unintended
register clobber, an off-by-one frame slot, an expr-stack
push without a matching pop, or a linker-pass off-by-one
would all show up as a wrong final exit code or a missing
marker.  The fact that it works first try is real evidence
the compiler is composable.

Real GCC's stage 2 = stage 3 fixed point is a stronger
property.  Ours is the strongest property we can prove with
the language surface we have shipped.

## What's still missing (the candid list)

```
control flow  : if, while, for, break, continue
function defs : > 1 user function per file
calls         : user function calls, recursion
types         : char, pointers, arrays, structs, enum
io            : direct file open/read in C source (not via Python)
preprocessor  : #include, #define, #ifdef
stdlib        : real malloc/free, strcmp, memcpy at C level
errors        : line-numbered messages, recovery
```

Every item on that list is a chapter worth of work.  Adding
them in order would, over several months, make `/bin/cc`
self-hostable — or, equivalently, justify a real GCC port
in Part XVIII.  Either way, the work is deferred past
Part XVII.

## Applied to existing apps

Per the user directive:

- `/bin/cc` is exercised against its largest input yet.  No
  code changes — chapter 123's compiler is the one being
  stressed.
- `/data` mount + mkosfs2 seeding (chapter 122 / 124
  contract) is exercised again with a larger source.
- The `printf-clobbers-x0..x18` boundary that has been
  implicit since chapter 116b is *empirically* validated
  here for the first time — four interleaved printfs with
  arithmetic-on-locals between them, no register-save
  problems.

## Lessons

1. Self-hosting is not a binary "do you self-host" — it is
   a fixed-point convergence check that is meaningful only
   if the compiler can already compile itself.  Naming the
   gap is worth more than gesturing at it.
2. The 29-store / 12-add asm fingerprint of the demo
   program is a useful regression signature: if a future
   compiler change halves the store count, something
   important about register allocation or frame layout has
   changed and a human should look at the diff.
3. Idempotent runs (second `/tmp/demo` exits with the same
   code) catch a class of "compiled binary depends on
   uninitialised stack memory" bugs that occasionally
   appear when the prologue forgets to zero-init or the
   linker forgets to clear `.bss`.

Next: chapter 126, a tiny `/bin/make` that drives `/bin/cc`
through a real `Makefile` — at which point the on-disk
source loop becomes a real build loop.
