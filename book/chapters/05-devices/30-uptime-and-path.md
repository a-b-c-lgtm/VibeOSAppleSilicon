# Chapter 30 — `uptime` and a real shell PATH

Two small quality-of-life features in this chapter, neither of
them deep, both of them the kind of thing you miss instantly when
they're absent.

1. **`SYS_UPTIME_MS`** — a one-call syscall that returns a
   monotonic millisecond counter since boot. The first thing any
   debug tool wants and the first thing any benchmark needs.
2. **Bare command names in the shell** — `ls` instead of
   `/bin/ls`, `hello` instead of `/bin/hello`. Just a `/bin/`
   prefix-prepend in the shell when the input doesn't start with
   `/`, but it makes the prompt feel a hundred times less like a
   research prototype and a hundred times more like a real OS.

## What it looks like in practice

```
$ help
built-ins:
  exit [code]   exit the shell
  help          this message
known programs (try `ls` for the full list):
  hello       greet from EL0 and exit
  ls          list every file in the FS
  cat <path>  print a file
  echo ...    echo arguments
  uptime      print monotonic ms since boot
  heaptest    exercise malloc/free
  printftest  exercise the libc printf

$ ls
       8  /motd
     234  /README
     113  /mnt/hello.txt
     ... etc ...

$ uptime
uptime: 0h 00m 01.000s  (1000 ms)

$ hello
hello from EL0!

$ echo hello bare command
hello bare command

$ exit
```

Five user commands, none of them prefixed `/bin/`. The shell does
the right thing.

## `SYS_UPTIME_MS`

The kernel already counts ticks (we set up the generic-timer
interrupt back in chapter 11). Each tick is `TICK_INTERVAL_MS`
milliseconds — 100 today. So uptime is just:

```c
static long sys_uptime_ms(void)
{
    return (long)(timer_ticks() * (uint64_t)TICK_INTERVAL_MS);
}
```

Two notes:

1. `TICK_INTERVAL_MS` was previously a `#define` inside `main.c`.
   We moved it to `timer.h` so `syscall.c` can read it without
   pulling `main.c` into its translation unit. Constants whose
   value is part of an ABI (and "milliseconds per tick" is part
   of the uptime ABI now) belong in a header.
2. The return is `long` — signed, because every other syscall
   returns negative for errno. Uptime never errors, but
   `_svc0`'s return type is `long`. The user-side wrapper casts
   to `unsigned long` since uptime is always non-negative:

```c
static inline unsigned long uptime_ms(void)
{
    return (unsigned long)_svc0(SYS_UPTIME_MS);
}
```

64 bits at 1 ms granularity wraps after ~584 million years.
Not a concern.

### Why milliseconds and not raw cycles or ticks

Three reasons:

1. **Ticks would lie about resolution.** A program that calls
   `uptime_ms` twice and subtracts gets `0` if the calls are in
   the same tick window. Returning ticks would make the
   resolution-vs-units confusion silent. Milliseconds make it
   explicit: "you get 100-ms resolution today; tomorrow it might
   be 10".
2. **Cycles vary by CPU.** The generic timer's frequency is
   readable but variable across boards. Forcing the user to
   convert is a footgun every time.
3. **Milliseconds are what programs ask for.** Sleep is "in
   milliseconds". HTTP timeouts are "in milliseconds". The unit
   choice should match the most common downstream operation.

## The user program

```c
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    unsigned long ms = uptime_ms();
    unsigned long s  = ms / 1000UL;
    unsigned long m  = s  / 60UL;
    unsigned long h  = m  / 60UL;
    unsigned long ms_part = ms % 1000UL;
    unsigned long s_part  = s  % 60UL;
    unsigned long m_part  = m  % 60UL;
    printf("uptime: %luh %02lum %02lu.%03lus  (%lu ms)\n",
           h, m_part, s_part, ms_part, ms);
    return 0;
}
```

Hours, minutes, seconds, then the raw ms in parentheses. The raw
ms is the load-bearing part — anyone scripting around `uptime`
will grep for it. The pretty form is for humans.

## Shell PATH lookup

The previous shell required full paths:

```
$ /bin/ls           # works
$ ls                # "no such command"
```

Real Unix shells search a `PATH` environment variable. We don't
have env vars yet (chapter 22+ work) so we hardcode the lookup:

```c
static void resolve_path(const char *name, char *out)
{
    if (name[0] == '/') {
        /* absolute -- copy as-is */
        ...
        return;
    }
    /* relative -- prepend "/bin/" */
    ...
}
```

Then in the read-eval loop:

```c
char path[PATH_MAX];
resolve_path(line, path);   /* line is the command word */
int tid = spawn(path, args);
```

Three observations:

1. **No fallback list.** Real shells try each `PATH` entry until
   one resolves. We have one directory (`/bin`) so we just
   prepend it. When we add a second binary directory we'll
   spend an evening implementing real `PATH`-walking.
2. **No "search current directory".** Bash deliberately doesn't
   put `.` in `PATH` for security reasons (an attacker who can
   write a file named `ls` in your cwd gets to execute it).
   We're not going to introduce that bug just because it's a
   hobby OS.
3. **Builtins still take precedence.** `exit` and `help` are
   checked before `resolve_path` runs, so `/bin/exit` wouldn't
   override the builtin even if it existed. Same as bash.

## Why these two changes ship together

They're both about making the shell *feel* like a shell. Each is
a few dozen lines. Either alone is a forgettable footnote; both
together change the texture of every interactive session.

Also, this is the first chapter where neither change required
new kernel infrastructure — `SYS_UPTIME_MS` is one function,
the PATH lookup is pure userspace. After six chapters of address
spaces, page tables, copy-from-user, ELF loading, and per-process
heaps it's nice to spend an hour writing actual OS *features*.

## What's still missing

- **No `time cmd` builtin.** Now that we have `uptime_ms`, the
  shell could timestamp before/after `spawn`+`wait` and print
  the delta. Five-line patch, deliberately deferred so we have
  something to do in a future polish chapter.
- **No `PATH` env var.** When env vars arrive, `resolve_path`
  becomes a `getenv("PATH")` walk.
- **No `cd` / `pwd`.** Per-process cwd is a future address-space
  field; the syscall pair is `chdir`/`getcwd`.
- **No tab completion.** Needs cooked-mode escape processing on
  the keyboard side and a directory-walking matcher on the shell
  side. Not hard, just nowhere near critical-path.
- **No history.** Same.

## What changed

```
kernel/core/timer.h             TICK_INTERVAL_MS moved here from main.c
kernel/core/syscall.{h,c}       SYS_UPTIME_MS = 13; sys_uptime_ms;
                                dispatch case
userspace/libc/syscall.h        SYS_UPTIME_MS enum + uptime_ms() wrapper
userspace/uptime/uptime.c       NEW -- 25 lines, prints uptime
userspace/sh/sh.c               resolve_path() prepends /bin/ for bare
                                command names; help text updated
kernel/core/main.c              banner -> milestone 21
Makefile                        wires uptime into the disk image
```

About 80 lines of new code, two new user-visible features. Out of
13 OSFS files, 11 are now invokable by their bare name.
