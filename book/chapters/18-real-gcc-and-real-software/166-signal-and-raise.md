# Chapter 166 — `raise()`, `abort()`, full POSIX signal table

> **Milestone in this chapter:** expand the libc signal surface
> from the chapter-77 `sigaction` minimum to the full C99 /
> POSIX table.
> **Code referenced:**
> - [userspace/libc/signal.h](../../../userspace/libc/signal.h)
> - [userspace/libc/syscall.h](../../../userspace/libc/syscall.h)
>   (the expanded `SIG*` macro table)
> - [scripts/test_signal_raise.py](../../../scripts/test_signal_raise.py)
>
> **At the end of this chapter** you will have `raise(int)` and
> `abort(void)` available to every userspace binary, the C99 /
> POSIX signal-number macros (`SIGSEGV`, `SIGFPE`, `SIGABRT`,
> `SIGUSR1/2`, `SIGILL`, `SIGBUS`, `SIGALRM`) declared, and a
> green `test_signal_raise.py` regression. Prerequisites:
> chapter 165 (setjmp / longjmp), chapter 76 (`sigaction`),
> chapter 77 (`SIGCHLD` / `waitpid`).

---

## What you'll do in this chapter

1. Expand the `SIG*` constant table in `userspace/libc/syscall.h`
   to cover the C99 / POSIX names that upstream code expects
   (`SIGSEGV`, `SIGFPE`, `SIGABRT`, `SIGUSR1/2`, `SIGILL`,
   `SIGBUS`, `SIGALRM`).
2. Add `raise(int)` and `abort(void)` to
   `userspace/libc/signal.h`.
3. Write `sigtest2` and `aborttest` regression binaries
   that exercise the catch-and-ignore paths plus the
   abort exit-status contract.
4. Run `scripts/test_signal_raise.py` and watch it land
   green.

Chapter 165 gave you non-local control flow. This chapter
fills in the other half of the C99 "control-flow not via
function calls" set: signals. The kernel already has them
(chapter 76 `sigaction`, chapter 77 `SIGCHLD`/`waitpid`), and
the libc already has `signal()`/`sigaction()` wrappers and a
`kill()` syscall stub. What's missing is the C99-named
shortcut **`raise(sig)`**, the C99-required **`abort()`**, and
the eight or so POSIX signal numbers nothing in-tree had
bothered to spell out yet.

## Why this chapter is small

Almost all the engineering already happened, in chapters 76
and 78. The kernel `sys_kill` already accepts any signal in
`1..31`, delivers it on the next syscall return, and lets the
user-installed handler run via the chapter-77 trampoline. So
`raise(sig)` is literally:

```c
static inline int raise(int sig)
{
    return (kill(getpid(), sig) == 0) ? 0 : -1;
}
```

That's it. The whole reason to add it is so real upstream
code — Doom's `I_Quit`, BearSSL's `t_*.c` selftests, GCC's
diagnostic path — compiles without needing a shim header. C99
[§7.14.2.1](https://port70.net/~nsz/c/c99/n1256.html#7.14.2.1)
specifies the function; provide it verbatim.

## What you'll write

1. [userspace/libc/syscall.h](../../../userspace/libc/syscall.h) —
   expand the `SIG*` constant table. Previously only
   `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGKILL`, `SIGPIPE`,
   `SIGTERM`, `SIGCHLD` were defined, with a comment that the
   others were "reserved for symmetry." Add `SIGILL=4`,
   `SIGABRT=6`, `SIGBUS=7`, `SIGFPE=8`, `SIGUSR1=10`,
   `SIGSEGV=11`, `SIGUSR2=12`, `SIGALRM=14`. Numbers match
   Linux so a future toolchain port doesn't need a translation
   table; the kernel side is already signal-number-agnostic
   (`sys_kill` validates `sig in 1..31` and stops there).
2. [userspace/libc/signal.h](../../../userspace/libc/signal.h) —
   add `raise(int)` and `abort(void)`. `abort()` follows
   the standard three-step idiom (raise; reset to default;
   raise again; backstop `exit(128 + SIGABRT)`) so callers
   that ignored or caught-and-returned `SIGABRT` can't keep
   the process alive past the second raise.
3. [userspace/sigtest2/sigtest2.c](../../../userspace/sigtest2/sigtest2.c) —
   regression for the `raise()` path. Installs handlers for
   `SIGUSR1` and `SIGUSR2`, raises each, asserts the handler
   ran exactly once with the right `signum`, asserts the other
   handler did not re-enter. Then sets `SIGINT` to `SIG_IGN`,
   raises `SIGINT`, asserts the process is still alive
   afterwards (i.e. ignore disposition is honoured).
4. [userspace/aborttest/aborttest.c](../../../userspace/aborttest/aborttest.c) —
   tiny binary that just calls `abort()`. The harness checks
   `$?` from the shell afterwards and asserts `134` (== 128 +
   `SIGABRT`).
5. [scripts/test_signal_raise.py](../../../scripts/test_signal_raise.py) —
   boots the kernel, runs `sigtest2` and `aborttest` back to
   back. ~30 s wall.

## Why `SIGSEGV` and friends, when nothing synthesises them yet?

Two reasons:

- **Upstream headers reference them by name.** Vendored code
  asks "does `SIGSEGV` resolve to an integer?" at preprocess
  time, not "does the kernel ever raise it?" at runtime.
  Defining the constants is enough for compilation; whether
  the kernel actually synthesises them on a page fault is a
  separate decision you can take later (likely chapter 174
  or once Doom triggers a real segfault you want to catch).
- **`raise(SIGSEGV)` works *today*.** The kernel's `sys_kill`
  doesn't care which signal you send — it just sets the bit
  in the target's pending mask and lets the chapter-77
  trampoline machinery handle delivery. So a userspace
  program *can* drive its own SIGSEGV path for testing, even
  if a real MMU fault doesn't (yet) deliver one.

## The `abort()` idiom

C99 [§7.20.4.1](https://port70.net/~nsz/c/c99/n1256.html#7.20.4.1):

> The `abort` function causes abnormal program termination
> to occur, unless the signal `SIGABRT` is being caught and
> the signal handler does not return. Whether
> open output streams are flushed or open streams are closed
> or temporary files are removed is implementation-defined.
> An implementation-defined form of the status _unsuccessful
> termination_ is returned to the host environment by means
> of the function call `raise(SIGABRT)`.

The standard idiom (also in glibc, musl, BSD libc) is:

```c
__attribute__((noreturn))
static inline void abort(void)
{
    raise(SIGABRT);                  /* try the catchable path */
    sigaction(SIGABRT, SIG_DFL);     /* defang any handler */
    raise(SIGABRT);                  /* this one DOES terminate */
    exit(128 + SIGABRT);             /* belt-and-braces */
}
```

The first `raise` exists so a process that installed a
SIGABRT handler gets one last chance to run it (POSIX
explicitly says abort calls raise(SIGABRT)). If that handler
returns, the second `raise` after `SIG_DFL` reset is
guaranteed to terminate the process — the kernel's default
action for every signal is "exit with code `128 + sig`." The
final `exit(128 + SIGABRT)` is a backstop that should never
run; if it does, the process still leaves with the right
status so a waiting parent's `waitpid()` doesn't see
something nonsensical.

`aborttest.c` proves the chain works end-to-end by relying on
the shell's `$?`:

```
[aborttest] about to abort
$ echo aborttest_status=$?
aborttest_status=134
```

`134 == 128 + 6 == 128 + SIGABRT`. The harness asserts on
that exact integer.

## Pitfall — shell TTY echo doubles your `$?` marker

**Symptom.** Your harness runs `aborttest`, types `echo
aborttest_status=$?\n` to the shell, and then a
`log.rfind("aborttest_status=")`-plus-"read the next integer"
parser reports *no digits found*.

**Cause.** The shell's TTY echoes typed bytes back as you
send them. The **first** appearance of `aborttest_status=` on
the serial line is the *echoed command* (with a literal `$?`
following), not the expansion.

**Fix.** Wait for the prompt that comes back *after* the echo
runs, then look for the marker that appears *at least twice*
in the log — the echoed one plus the expanded one. Take the
last occurrence's tail. This is the right pattern for any
future test that reads a value via `echo $?`-style probing.

## What this unlocks

- **`sh` / userspace test harness**: no behaviour change.
  Existing `signal(SIGINT, ...)` calls keep working unchanged.
- **`init` / `gui_term`**: gained nothing they couldn't do
  before, but `abort()` is now available if anything wants a
  hard-fail bail-out.
- **Chapter 132 (GCC port)**: `gcc/diagnostic.c`'s
  `internal_error` path calls `abort()` after the diagnostic.
  No shim needed.
- **Chapter 173 (Doom platform shim)**: Doom's `I_Error`
  calls `abort()` for unrecoverable conditions. No shim
  needed.

## What's deferred

- **`sigsetjmp` / `siglongjmp`** — these save and restore the
  *signal mask* in addition to chapter 165's register state.
  There's no `sigset_t` plumbed end-to-end yet; nothing
  in-tree wants one. Land when something does.
- **`sigprocmask` / `sigsuspend`** — same reason. Existing
  handlers run as soon as the kernel returns from a syscall;
  there's no support yet for "delay delivery until I say."
- **`SA_RESTART`** — slow syscalls (chiefly `read()` from a
  pty/socket) are not yet interrupted by a pending signal and
  don't return `-EINTR`. The handler runs only at the *next*
  syscall return.
- **Synthesising `SIGSEGV` / `SIGFPE` / `SIGILL` from
  faults** — the kernel currently terminates the offending
  thread on a synchronous exception (chapter 18's
  fault-on-page-fault path). Translating those into queued
  signals is a separate kernel change, likely landing once a
  real program needs to handle one. The constants are
  defined; the delivery isn't synthesised. The two are
  decoupled by design.

## Things to remember

- **Don't overspec on the kernel side just because libc
  names something.** Every libc on Linux exposes `SIGRTMIN`
  and 32 real-time signals; ours doesn't, and that's fine
  for now. Add what's needed when something asks; don't
  pre-build infrastructure for hypothetical use cases.
- **The shell TTY echo bites every interactive test once.**
  When parsing `$?` from serial output, anchor on the
  prompt that follows the echo, then take the *last*
  occurrence of the marker — never the first.

## What's next

[Chapter 167](167-ctype-assert-string.md) adds `<ctype.h>`,
`<assert.h>`, and the missing `string.h` entrypoints
(`memmem`, `strdup`, `strndup`, `strchrnul`). Pure header-only
additions, no kernel work. Then 168 (`<time.h>`), 169
(`qsort` / `bsearch` / `strtol` / `getopt`), 170 (real
`printf` / `scanf` / `fprintf`), and the kernel landmark —
**chapter 171** turns FP/SIMD on at EL0, the single biggest
unblocker for everything downstream.
