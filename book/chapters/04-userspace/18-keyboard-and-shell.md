# Chapter 18 — Console keyboard input and a line-mode shell

## Where this chapter sits

After chapter 17 we had `init`, `spawn`, `wait`, and exit codes. The
kernel could load multiple user programs and let one parent supervise
several children. What it could not do was *take input*. Every user
program ran with `argc = 0` and read only from files we'd already
embedded into the ramfs. There was nobody at the keyboard.

This chapter fixes that. By the end:

- The kernel reads characters from the PL011 UART receive FIFO and
  exposes them as bytes from `read(0, ...)`.
- A small line discipline gives users the conveniences they expect:
  echo of typed characters, backspace, and "press Enter to commit
  the line."
- A new user program — `/bin/sh` — prompts, reads a line, treats it
  as a path, and spawns it via `SYS_SPAWN`. When the child exits, the
  shell loops back to the prompt.
- `init` now hands control to the shell after running its self-tests,
  so a real human can drive the system.

The diff is small. The interactive feeling it produces is enormous.

## The hardware: PL011 receive

We've been using the PL011 for output since chapter 3. The same
device handles input. Two registers matter for receive:

| Offset | Name | Bit / field used |
|--------|------|------------------|
| `0x000` | `DR` (data register)  | bottom 8 bits = received byte |
| `0x018` | `FR` (flag register)  | bit 4 (`RXFE`) = receive FIFO empty |

The polled-receive primitive lives in
[`kernel/core/serial.c`](../../../kernel/core/serial.c) and is
already in the tree from earlier milestones:

```c
int serial_try_getc(char *out)
{
    if (mmio_read32(PL011_FR) & FR_RXFE)
        return 0;            /* nothing in the FIFO */
    if (out)
        *out = (char)(mmio_read32(PL011_DR) & 0xFFu);
    return 1;
}
```

This is *non-blocking*: it returns 0 if the FIFO is empty, or 1 plus
the byte if a key has been pressed. A blocking driver could use the
PL011 RX interrupt and a sleep queue. We don't need that yet — busy
polling is dirt simple, and on a kernel with cooperative yield it
behaves itself: the polling thread releases the CPU between attempts.

### Why no RX interrupt yet?

A proper console driver would:

1. Enable the `RXIM` bit in `IMSC` so the PL011 raises SPI 33 (the
   `virt` machine's UART IRQ).
2. Enable that SPI in the GIC.
3. In the IRQ handler, drain the FIFO into a kernel ring buffer and
   wake any thread sleeping on stdin.

We have all the pieces (chapter 9 GIC, chapter 12 wakeups). What we
*don't* have is multiple keyboard-bound users competing for input,
or hard-realtime latency requirements. Our shell is the only reader.
Polling with cooperative yield gives the same observable behaviour
with about ten lines of code instead of a hundred. We'll upgrade
when we have a reason to. (See "Future work" at the end of the
chapter.)

## The kernel side: `read(fd 0)`

Earlier we made `vfs_read` return 0 (EOF) for the console fds. Now
it does real work for fd 0, while continuing to return 0 for fd 1
and fd 2 (writing-only descriptors). The full handler lives in
[`kernel/core/vfs.c`](../../../kernel/core/vfs.c):

```c
if (e->ramfs_index < 0) {
    if (fd != 0) return 0;
    char *dst = (char *)buf;
    size_t n = 0;
    while (n < len) {
        char c;
        while (!serial_try_getc(&c)) yield();

        if (c == '\r' || c == '\n') {
            serial_putc('\r');
            serial_putc('\n');
            dst[n++] = '\n';
            return (long)n;
        }
        if (c == 0x7f || c == 0x08) {
            if (n > 0) {
                n--;
                serial_putc('\b');
                serial_putc(' ');
                serial_putc('\b');
            }
            continue;
        }
        serial_putc(c);
        dst[n++] = c;
    }
    return (long)n;
}
```

There's a real protocol here, even if it's small. Let's name it
explicitly.

### Cooked mode

What the kernel implements is a **cooked-mode line discipline**. The
shell does not see individual key presses. It calls `read(0, buf, N)`
and the call does not return until the user has either:

1. typed Enter (CR or LF), in which case the buffer ends with a
   single `'\n'` byte and the byte count includes it; or
2. typed `N` non-newline characters, which fills the buffer.

While the user is composing the line, the kernel:

- Echoes printable bytes verbatim. (No fancy escape handling; if you
  type Ctrl-A you get `\x01` in the buffer and `\x01` echoed.)
- Treats both DEL (`0x7f`) and BS (`0x08`) as backspace. Most
  terminal emulators send DEL when you hit the Backspace key; xterm
  in some configurations sends BS. Both work.
- Translates Enter into `'\n'` *both* in the byte returned to user
  space *and* in the on-screen echo (CR LF, the usual two-byte
  newline of dumb terminals).

The "raw mode" alternative would deliver one byte per `read`, no
echo, with the program responsible for everything. We don't need it
yet. When we eventually port `vim` we'll add an `ioctl` to flip
modes.

### Why `yield()` instead of a sleep queue?

The polling loop:

```c
while (!serial_try_getc(&c)) yield();
```

The `yield()` here is what keeps the system honest. While the shell
is blocked waiting for keystrokes, every other ready thread runs.
Today there are no other ready threads (the boot thread is in WFE,
init is in `THREAD_WAITING`). But the cost is one extra context
switch per polling iteration, which is microscopic compared to the
millisecond-scale gap between human keystrokes.

When we add real interrupt-driven input, the change to `vfs_read`
will be one line: replace the `serial_try_getc / yield` loop with
`thread_block_on(&console_wait)`. The shape of the function stays
the same.

### A subtle correctness point: blocking inside a syscall

When `read(0)` blocks, we are *inside* the kernel's SVC handler, on
the kernel stack of the calling thread, with that thread's user
context already saved on the stack. `yield()` performs a normal
context switch. When the scheduler eventually runs us again,
control returns from `yield()`, the `while` loop checks the FIFO
again, and life continues.

This is fine because we have *one syscall handler per kernel stack*
— the `SP_EL0` save we added in chapter 17 means each thread has
its own SVC frame. There is no "currently servicing syscall N"
global state to corrupt. The work-in-progress lives on the
per-thread kernel stack, exactly where it should.

## The userspace side: `read` wrapper

The userspace wrapper for `read` already exists in
[`userspace/libc/syscall.h`](../../../userspace/libc/syscall.h) — it
just forwards x8=6, x0=fd, x1=buf, x2=len through `svc #0`. No
change needed.

We do need a new helper to print signed decimals when the shell
reports errors (like `errno=2`). We added `putd(long)` back in
chapter 17 already.

## `/bin/sh`

[`userspace/sh/sh.c`](../../../userspace/sh/sh.c) is the shortest
real shell I can imagine. ~100 lines, no allocator, no globbing, no
quoting, no piping, no environment, no redirection. The main loop
is:

```c
for (;;) {
    write(1, "$ ", 2);
    long n = read(0, line, LINE_MAX - 1);
    if (n <= 0) return 0;
    line[n] = '\0';
    n = trim(line, (int)n);
    if (n == 0) continue;

    if (streq(line, "help"))     { print_help(); continue; }
    if (streq(line, "exit"))     return 0;
    if (starts_with(line, "exit ")) return parse_uint(line + 5);

    int tid = spawn(line);
    if (tid < 0) {
        write(1, "[sh] no such command: ", 22);
        write(1, line, n);
        write(1, " (errno=", 8);
        putd(-tid);
        write(1, ")\n", 2);
        continue;
    }
    int code = 0;
    wait(&code);
    if (code != 0) {
        write(1, "[sh] exit ", 10);
        putd(code);
        write(1, "\n", 1);
    }
}
```

Things to notice:

- The line, after `trim`, *is* the path. `/bin/hello` runs
  `/bin/hello`. There's no PATH search and no implicit `/bin/`
  prefix. If you want it short, put the binary at `/h` in ramfs and
  type `/h`.
- Built-ins are matched first, so `exit` and `help` always work even
  if the ramfs doesn't have `/exit`.
- We don't bother with argv. `spawn` doesn't take any. (`execve` on
  real Unix was always how arguments crossed the process boundary;
  on a `posix_spawn`-style API the parent passes argv as part of the
  spawn call. We'd add argv to `SYS_SPAWN` when we start needing it
  — likely the same milestone as a real loader for shared
  libraries.)

### Why no `argv` yet, again

Compare to chapter 17's discussion of fork-vs-spawn. We're building
exactly the API we need for the next milestone, no more. Adding
argv now would mean:

- Defining where argv lives in the new process's address space (in
  the user stack? a separate allocation?).
- A copy_from_user routine to safely walk the parent's `char **argv`
  before we destroy the parent's address space — even though we
  don't have per-process address spaces yet.
- A user-space `crt0` extension to consume argv from a known
  location.

All three are fine to build. None of them are needed to type
`/bin/hello` and watch it print "hello from EL0!". So we don't.

## Wiring init -> sh

[`userspace/init/init.c`](../../../userspace/init/init.c) is now the
"system bring-up" program: it runs the same hello+cat self-tests it
ran in milestone 9, then hands control to the shell.

```c
puts("[init] launching /bin/sh");
int tid = spawn("/bin/sh");
if (tid < 0) { /* error */ }
int code = 0;
wait(&code);
write(1, "[init] /bin/sh exited code=", 27);
putd(code);
write(1, "\n", 1);
puts("[init] all done, exiting 0");
return 0;
```

When `sh` returns (via `exit` builtin or `exit N`), init learns the
exit code and shuts down cleanly. There is no automatic respawn —
on a real Unix init that's how the system goes from "logged in" to
"login: " again. Adding a respawn loop is one `for(;;)` away.

## Verification

Built and ran with input piped in:

```
$ printf '/bin/hello\n/bin/cat\nhelp\nbogus_cmd\nexit 7\n' \
      | timeout 8 qemu-system-aarch64 -M virt,gic-version=3 \
            -cpu host -accel hvf -m 8G -nographic \
            -serial mon:stdio \
            -device loader,file=assets/virt.dtb,addr=0x44000000 \
            -kernel build/kernel.elf
```

Trimmed output:

```
[init] launching /bin/sh
[sh] tiny shell ready.  type 'help' for a list of commands.
$ /bin/hello
hello from EL0!
pid=0x00000007
$ /bin/cat
... motd contents ...
$ help
built-ins:
  exit [code]   exit the shell
  help          this message
  ...
$ bogus_cmd
[sh] no such command: bogus_cmd (errno=2)
$ exit 7
[init] /bin/sh exited code=7
[init] all done, exiting 0
```

Things to notice in the trace:

- The PIDs grow each time you spawn — `/bin/hello` got pid 7 the
  second time, because pids 4, 5, 6 were already used (init's two
  self-test children and `/bin/sh` itself).
- `bogus_cmd` returns errno 2 = `ENOENT_VFS`. The shell prints the
  raw number; mapping it to `"No such file or directory"` would
  need a `strerror` table, which we don't have yet.
- `exit 7` propagates: shell exits with 7, init reaps with code=7,
  init prints it, init then exits with 0 (its own choice).

Try it interactively:

```
$ make all && \
  qemu-system-aarch64 -M virt,gic-version=3 -cpu host -accel hvf \
      -m 8G -nographic -serial mon:stdio \
      -device loader,file=assets/virt.dtb,addr=0x44000000 \
      -kernel build/kernel.elf
```

You'll see a `$ ` prompt. Type `/bin/hello`, press Enter, watch it
run, and you're back at the prompt. Backspace works. To exit QEMU
type `exit`, then Ctrl-A X.

## What we deferred (and why)

Each of these is a real feature on a real shell. Each is a
self-contained future milestone.

- **Interrupt-driven input.** Polled `serial_try_getc` plus
  `yield` is fine for one reader. Multiple readers, or a system
  with real CPU-bound competition, want a sleep queue + IRQ. (See
  also "Pin the PL011 IRQ in the GIC" in our backlog.)
- **argv / envp.** Spawn takes one path. Programs see `argc=0`.
  Adding argv requires deciding where it lives in the new
  address space, plus copy_from_user, plus a `crt0` extension to
  read it back. Wait until we need it.
- **PATH search.** The shell takes paths verbatim. Adding `PATH`
  resolution in userspace is ten lines once we have `getenv`. We
  don't have `getenv` because we don't have envp.
- **Pipes and redirection.** Both need at least one extra syscall:
  `dup2`, plus a `pipe` syscall that returns two ramfs-style fds
  backed by an in-kernel ring buffer. We'd need a simple in-kernel
  pipe object. Doable in an afternoon. Not today.
- **Job control / Ctrl-C.** Needs signals. Needs an interrupt-driven
  console (so Ctrl-C interrupts the *blocking* read). Needs process
  groups and a foreground-pgrp slot per terminal.
- **Quoting / globbing.** Pure userspace. Once we get `argv`,
  parsing `"hello world"` and expanding `*.txt` is just code.
- **History / readline.** Wants a richer line discipline (cursor
  movement, escape sequences) and a userspace history buffer. The
  current `vfs_read` already supports most of what's needed for
  history if we made backspace handle a longer rubout.

The shape of these additions is clear. The reason to do them later
is the same reason we did spawn before fork: ship the smallest
working thing, learn from running it, then add the next layer.

## Recap

- PL011 RX is polled, byte-by-byte, with `yield` between polls.
- `vfs_read(0, ...)` implements a tiny cooked-mode line discipline:
  echo, backspace, line-at-a-time delivery on Enter.
- `/bin/sh` is a 100-line program that prompts, reads a line,
  spawns it as a path, and waits for the child.
- `init` now hands control to `/bin/sh` after its self-tests, so
  the system has a human-driven foreground loop.
- Many shell features (argv, PATH, pipes, signals, history) are
  deferred. None of them are blocked by anything in milestone 10;
  they're just future work.

Next: Part V begins, where we leave the embedded ramfs behind and
talk to virtio-blk for real persistent storage.
