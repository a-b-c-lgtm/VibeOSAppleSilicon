# Chapter 31 — A `time` builtin for the shell

Now that the kernel exposes a monotonic uptime counter (chapter
30), we can do something useful with it: time how long a child
process takes to run. This is a five-line shell change with
disproportionate ergonomic value.

## Usage

```
$ time hello
hello from EL0!
[time] 0.000s real

$ time ls
... (lots of files) ...
[time] 0.000s real

$ time
uptime 1.000s
```

Three modes:

1. `time <cmd> [args]` — spawn `<cmd>`, wait, print
   wall-clock duration.
2. `time` (alone) — print current uptime. Same as the
   `uptime` program, just inline. Useful for back-of-the-envelope
   timing without spawning a process.
3. (Anything else) — runs as before.

The `0.000s real` results above aren't a bug: every demo command
finishes well inside one 100 ms tick. With `TICK_INTERVAL_MS=100`
the smallest non-zero result is `0.100s`. To see real timing
you'd run `printftest` (200+ printf calls — still well under
100 ms on modern hardware) or wait until we have CPU-intensive
programs in a future chapter.

## Implementation

The `time` builtin is a four-line addition to the shell's
read-eval loop:

```c
int   timed = 0;
char *cmd   = line;
if (streq(line, "time")) {
    /* bare `time` — print uptime and continue */
    print_uptime(); continue;
}
if (starts_with(line, "time ")) {
    timed = 1;
    cmd   = line + 5;       /* skip "time " */
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') continue;
}
/* ... rest of loop now operates on `cmd` instead of `line` ... */
```

Then bracket the spawn/wait pair with two `uptime_ms()` calls:

```c
unsigned long t0 = timed ? uptime_ms() : 0;
int tid = spawn(path, args);
/* error handling ... */
wait(&code);
if (timed) {
    unsigned long elapsed = uptime_ms() - t0;
    /* print "[time] %lu.%03lus real\n" */
}
```

The shell does its own `%lu.%03lu`-style printing rather than
pulling in `printf` because (a) it's just two integer divides
and a fixed three-character output, and (b) we already pay for
`putd`. No reason to inflate the binary.

## Why this is interesting

It's the first piece of *measurement* infrastructure in the
system. Before this chapter the only way to know whether a
change made the kernel slower was to feel it. Now you can
pipe-time anything that runs:

```
time printftest      # how fast can we format text
time cat /motd       # how fast can we open+read a file
time ls              # how fast can we walk the namespace
```

Once we have something CPU-intensive to time (a JSON parser, a
mini-compress, a hash benchmark — all future programs) we get
real numbers without writing benchmark scaffolding.

## Why we don't have `user`/`sys` time

GNU `time` prints three lines: `real`, `user`, `sys`. We only
print `real`. The reason is that "user CPU time" and "system CPU
time" require per-thread accounting in the scheduler — every
context switch has to debit elapsed cycles to whichever bucket
(user vs kernel) the thread was in. We don't have that yet, and
adding it now would be a one-line builtin riding on top of a
substantial scheduler change. Defer.

## Why we did this *now*

Hobby OS development has a gravity well: there's always
*something* deeper to dig into (a clever virtio extension, a
more aggressive page allocator, a real fork). Spending an hour
on a five-line builtin instead feels indulgent. But:

1. Every measurement we'll want to do for the rest of the book
   (does the new allocator make things slower? did batching
   block I/O help?) starts with `time cmd`. Building it once
   pays off for every benchmark to come.
2. It's a forcing function: now that timing is one keystroke
   away, you'll *do* it. Before, you'd have to invent ad-hoc
   instrumentation each time. The friction kept you from
   measuring. The friction is gone.

## What's still missing

- **`user` and `sys` time.** Needs scheduler accounting.
- **Sub-tick resolution.** `TICK_INTERVAL_MS=100` puts a hard
  floor on the smallest visible duration. A future
  `arch_now_ns()` reading the generic-timer counter directly
  would give nanosecond resolution.
- **`time` for shell builtins.** `time help` would currently
  fall through to "no such command" because `time <cmd>` looks
  up `<cmd>` as a binary. Fixing it is one extra builtin check
  inside the time path.
- **Pipelines.** `time (a | b)` requires both pipes (chapter
  not yet written) and grouping syntax. Both deferred.

## What changed

```
userspace/sh/sh.c        time builtin (3 modes), help text
kernel/core/main.c       banner -> milestone 22
```

That's it. Two files, ~50 lines. Smallest milestone in the book.
Highest leverage-per-line of any milestone in the book so far.
