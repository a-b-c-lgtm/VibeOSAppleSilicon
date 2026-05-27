# Chapter 80 — Retiring `spawn` in favour of fork+exec everywhere

> **Milestone in this chapter:** plan only — implement after
> chapter 85 (persistence-in-practice).
> **Code referenced (for the migration inventory):**
> - [userspace/init/init.c](../../../userspace/init/init.c)
> - [userspace/sh/sh.c](../../../userspace/sh/sh.c)
> - [userspace/launcher/launcher.c](../../../userspace/launcher/launcher.c)
> - [userspace/notepad/notepad.c](../../../userspace/notepad/notepad.c)
>
> **At the end of this chapter** you will have a written contract for
> a future migration: every caller of `SYS_SPAWN`, `SYS_SPAWN_REDIR`,
> and `SYS_SPAWN_PIPE` is rewritten in terms of `fork`+`exec`, and
> the three `spawn`-family syscalls are deleted from the kernel.
> The chapter lays out the inventory, the per-caller transformation,
> and the order in which to land them. **No code lands in this
> chapter** — the implementation is scheduled after chapter 85 so
> the two pending filesystem chapters (83 journal, 84
> persistence-in-practice) ship first.

## Why this chapter exists at all

After chapter 83 wrapped (write-back cache + fsync) the system was
in an odd state: `init` still used `spawn` to launch every userspace
daemon — the desktop, taskbar, launcher, and shell — even though
`fork()` (chapter 72), `exec()` (chapter 73), `SIGCHLD`/`waitpid`
(chapter 77), and the `gui_term` rewrite (chapter 79) had all
landed.

The question this chapter answers: **if real Linux would use
fork+exec for these callers, why doesn't ours?**

Answer: it should. Carrying two parallel process-creation APIs
forever is dead weight, and worse, it is pedagogically confusing. A
reader walking the book in order learns "fork+exec is how processes
are born" in chapter 72 and then immediately sees `init` ignoring
that lesson when they flip to
[userspace/init/init.c](../../../userspace/init/init.c).

## What `spawn` is, and why it existed

`SYS_SPAWN` (and its variants `SYS_SPAWN_REDIR`, `SYS_SPAWN_PIPE`)
is a single-syscall combination of "create address space + load ELF
+ start thread." It predates `fork` by ~60 chapters: the kernel had
user processes as far back as chapter 16, but did not have an
address-space copy mechanism until chapter 72.

Conceptually `spawn` is what POSIX calls
[`posix_spawn(3)`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/posix_spawn.html):
a fused fork+exec for callers who do no setup between the two. It
is also genuinely faster — no AS to copy or COW, even briefly —
which matters when fork is eager (chapter 72) but stops mattering
once COW lands (chapter 74).

Three syscalls live in the `spawn` family:

| syscall                  | extra capability                          | shell shape           |
| ------------------------ | ----------------------------------------- | --------------------- |
| `SYS_SPAWN`        (8)   | none — pure create-and-launch             | `cat foo`             |
| `SYS_SPAWN_REDIR` (20)   | pre-opens a path as fd 0 in the child     | `cat < foo`           |
| `SYS_SPAWN_PIPE`  (24)   | dup2s parent's fd N to child's fd 0/1     | `cat foo \| wc -l`    |

The third is the awkward one. It exists because between "fork" and
"exec" you need to perform fd surgery (dup2 the read end of the
pipe onto fd 0; close the unused write end), and the kernel didn't
have fork+exec at the time. So the surgery was shoved into the
kernel and made atomic.

Once fork+exec is the only path, the surgery moves to userspace
where it belongs, and the kernel surface shrinks by three syscalls.

## Inventory of `spawn` callers (snapshot, May 2026)

Captured from `grep -nE 'spawn[(_]' userspace/**/*.c`:

| caller                                                        | sites | pattern                                           |
| ------------------------------------------------------------- | ----- | ------------------------------------------------- |
| [userspace/init/init.c](../../../userspace/init/init.c)             | 5     | sequential daemon launches                        |
| [userspace/launcher/launcher.c](../../../userspace/launcher/launcher.c) | 1     | fire-and-forget on click                          |
| [userspace/notepad/notepad.c](../../../userspace/notepad/notepad.c)   | 1     | `spawn("/bin/notify", "saved!")` after Ctrl-S     |
| [userspace/sh/sh.c](../../../userspace/sh/sh.c)                     | 3 + 1 | `spawn` / `spawn_redir` / `spawn_pipe` + pipeline |

Callers that already use fork+exec (and serve as the template):

| caller                                    | added in    |
| ----------------------------------------- | ----------- |
| [userspace/gui_term/gui_term.c](../../../userspace/gui_term/gui_term.c) | chapter 79 |
| [userspace/sigtest/sigtest.c](../../../userspace/sigtest/sigtest.c)     | chapter 76  |
| [userspace/chldtest/chldtest.c](../../../userspace/chldtest/chldtest.c) | chapter 77  |
| [userspace/cowtest/cowtest.c](../../../userspace/cowtest/cowtest.c)     | chapter 74  |

## What the migrated code looks like

### init: trivial

The whole-binary `spawn(path, "")` collapses to the textbook
fork+execv idiom:

```c
static int run_bg(const char *path, char *const argv[])
{
    int pid = fork();
    if (pid < 0) return pid;
    if (pid == 0) {
        execv(path, argv);
        _exit(127);              /* exec failed */
    }
    return pid;                  /* parent: pid for later waitpid */
}
```

Five sites in [init.c](../../../userspace/init/init.c) become five
calls to `run_bg`. The reaper loop at the bottom is
unchanged — it already uses `wait(&code)` which is itself
the POSIX-shaped path. The only subtlety is the `_exit(127)`
on exec failure: pre-migration `spawn(path)` returned
`-ENOENT` synchronously to the parent on a missing binary,
but post-migration the parent only learns via the child's
exit code. (127 is bash's convention for "command not found.")

### launcher: same as init

[launcher.c](../../../userspace/launcher/launcher.c) line 144 is one
fire-and-forget `spawn` per button click. Identical to init's
shape. The launcher already doesn't reap its children (init
does, since the launcher is spawned by init). Migration is
mechanical.

### notepad → notify: same as init

Same shape. One subtlety: notepad currently passes the
arg string `"saved!"` to spawn, which goes into the legacy
`thread.args` buffer that older programs read via
`SYS_GETARGS`. Post-migration we have to thread it through
argv:

```c
char *argv[] = { "/bin/notify", "saved!", NULL };
int pid = fork();
if (pid == 0) { execv(argv[0], argv); _exit(127); }
/* notepad does not reap; init eventually does. */
```

`/bin/notify` already accepts argv (it's a chapter-49 binary
that uses both APIs).

### shell: the hard one

The shell does three different things, and the third —
pipelines — is where most of the work lives.

#### Plain command (1 line of work)

```c
- tid = spawn(path, args);
+ int pid = fork();
+ if (pid == 0) { build_argv(path, args, argv); execv(path, argv); _exit(127); }
+ tid = pid;
```

`build_argv` is the function the kernel currently runs in
`sys_spawn` to split `args` on whitespace. We move it to
shell-side userspace.

#### Input redirection (2 lines of work)

```c
- tid = spawn_redir(path, args, redir_in);
+ int pid = fork();
+ if (pid == 0) {
+     int in = open(redir_in, O_RDONLY);
+     if (in < 0) _exit(127);
+     dup2(in, 0); close(in);
+     build_argv(path, args, argv); execv(path, argv); _exit(127);
+ }
+ tid = pid;
```

#### Pipelines: the fd-leak trap

This is the part that justifies a dedicated chapter rather
than a one-line patch. Today's pipeline body builds a chain
of pipes in the parent and hands each segment a stdin and
stdout fd via `spawn_pipe`. The kernel atomically dup2s them
into place before the child runs.

In a fork+exec world the parent does the same thing — build
the pipes — but the **child** must do the dup2-and-close
dance, AND the **parent** must close its own copies of the
pipe fds it gave to the child. If either side keeps a stray
reference open, the read end never sees EOF, and the
downstream segment hangs forever waiting for the upstream to
close.

Sketch (omitting argv plumbing for clarity):

```c
/* parent has:  pipe_fds[0..N] from chained pipe() calls */

for (int i = 0; i < n_segments; i++) {
    int pid = fork();
    if (pid == 0) {
        /* CHILD i */
        if (i > 0)             { dup2(pipe_fds[2*(i-1)+0], 0); }  /* read end of upstream pipe */
        if (i < n_segments-1)  { dup2(pipe_fds[2*i+1],     1); }  /* write end of downstream pipe */

        /* CRITICAL: close every pipe fd the child still has
         * open after the dup2s.  Without this the kernel's
         * pipe r_refs/w_refs counters never drain to zero
         * when upstream segments exit, and the downstream
         * read() hangs forever. */
        for (int j = 0; j < 2*(n_segments-1); j++) close(pipe_fds[j]);

        execv(path[i], argv[i]); _exit(127);
    }
    pids[i] = pid;
}

/* PARENT: same close discipline.  We hold one ref per pipe
 * fd; if we don't drop them, the children see no EOF. */
for (int j = 0; j < 2*(n_segments-1); j++) close(pipe_fds[j]);

for (int i = 0; i < n_segments; i++) waitpid(pids[i], &codes[i], 0);
```

This is the textbook "close unused pipe ends" pattern. Every
Unix systems-programming book teaches it. Today's shell
sidesteps it because `spawn_pipe` does the work in the
kernel and the parent never holds the child's pipe refs.
After migration the shell joins the rest of the world.

The chapter's main pedagogical payoff is here: spend a few
paragraphs on **why** the close dance is required, ideally
with a "we forgot to close one fd, here's the hang we got"
story analogous to chapter 79's
[sys_spawn-skipped-fd-inheritance bug](079-gui-term-real-processes.md).

## Implementation order

1. **Add a userspace `argv-from-string` helper.** Probably
   in [userspace/libc/](../../../userspace/libc/) as a static inline.
   Today's `MAX_SPAWN_ARGV = 16` token cap stays; just moves
   from kernel to libc.

2. **Migrate init** ([userspace/init/init.c](../../../userspace/init/init.c)).
   Smallest surface area, most pedagogical impact (it's the
   first userspace file readers see). Verify with
   [scripts/test_boot_to_desktop.py](../../../scripts/test_boot_to_desktop.py).

3. **Migrate launcher** ([userspace/launcher/launcher.c](../../../userspace/launcher/launcher.c)).
   Verify with [scripts/test_launcher.py](../../../scripts/test_launcher.py).

4. **Migrate notepad's notify call**
   ([userspace/notepad/notepad.c](../../../userspace/notepad/notepad.c)).
   Verify with [scripts/test_notepad.py](../../../scripts/test_notepad.py).

5. **Migrate the shell's plain-command and redir paths.**
   Two of the three sh.c sites. Verify with the existing
   shell smoke tests.

6. **Migrate the shell's pipeline path.** This is the
   chapter's centerpiece — the close-unused-ends trap, the
   reaping order, the waitpid pid-list dance. Verify with
   a new pipeline regression test (`test_pipetest.py`,
   added as part of this migration).

7. **Add a new regression test** for the
   `execv → _exit(127)` failure path that pre-migration
   `spawn` returned synchronously. Today's tests don't
   exercise this because they didn't need to.

8. **Retire the kernel surface.** Once steps 1-7 are green:

   - Delete `sys_spawn`, `sys_spawn_redir`, `sys_spawn_pipe`
     from [kernel/core/syscall.c](../../../kernel/core/syscall.c).
   - Delete the `case SYS_SPAWN*` arms in the dispatch
     switch.
   - **Keep** the `SYS_SPAWN*` enum values reserved (do NOT
     re-pack the numbering!) — they're referenced in book
     chapters 8, 30, 31, 79, etc., and a reader cross-
     referencing those chapters with the source must still
     find the same numeric values they read about. Add a
     comment that says "RESERVED — formerly SYS_SPAWN, see
     chapter 80".
   - Delete the `spawn`, `spawn_redir`, `spawn_pipe` inline
     wrappers from [userspace/libc/syscall.h](../../../userspace/libc/syscall.h).

9. **Add a forward-pointer note** to chapter 8
   ([book/chapters/04-userspace/016-init-spawn-wait.md](../04-userspace/016-init-spawn-wait.md))
   so book readers who hit chapter 16 first know the design
   eventually changes. One-paragraph "later in this book…"
   blurb pointing here.

## Trap reminders (already learned, do not relearn)

- **The reaper deadlock.** init's main loop has
  `for (;;) { wait(&code); ... }` — it IS a reaper. Every
  child it `fork+execv`s gets reaped here. Verify the loop
  still terminates on shell exit (it should — same control
  flow as today, only the child birth changed). The general
  rule: any long-lived kernel or init reaper must spawn
  before the children it intends to reap, never after a
  child that never exits.

- **`exec` rebuilds argv from the array, not the string.**
  Don't pass the legacy "args" string to execv expecting it
  to be re-split. execv takes a NULL-terminated `char *const
  argv[]`; the shell must do the splitting itself before
  forking.

- **`exec` survives fd inheritance, including pty.** The
  pty bug from chapter 79
  ([079-gui-term-real-processes.md](079-gui-term-real-processes.md))
  was specifically about `sys_spawn` not inheriting fds.
  fork+exec inherits fds by definition, so the bug can't
  recur — but verify the gui_term path stays green after
  the shell pipeline migration, because pipelines under
  gui_term involve both pty fds AND pipe fds simultaneously.

## What this chapter does NOT change

- The kernel's actual fork/exec/COW machinery from chapters
  73-75. Those are stable. We're only changing **callers**.
- `gui_term`, `sigtest`, `chldtest`, `cowtest`. They already
  use fork+exec; nothing to do.
- The `wait` / `waitpid` semantics. Reapers stay reapers.

## What this chapter *will* change in the book itself

- Chapter 16 ("init-spawn-wait") gains a forward-pointer
  note, but stays as written — it's still historically true.
- Chapter 29/31 ("pipes" / "pipelines") may need a small
  errata note that the kernel mechanism described
  (`sys_spawn_pipe` doing the dup2) is gone, replaced by
  userspace doing the dup2 in the child after fork.
  Whether to retroactively rewrite those chapters or just
  add the errata is a judgement call; recommend errata, on
  the principle that the book is a journey, not a current-
  state reference.

## Definition of done

- `grep -rn 'spawn' userspace/ kernel/` returns ONLY:
  - book references and historical comments
  - the kept-but-reserved enum entries
- All existing `scripts/test_*.py` pass.
- New `scripts/test_execv_failure.py` passes.
- Chapter 16 has the forward-pointer paragraph.
- Chapters 29/31 have the errata note (or a rewrite).
- This chapter (80) is rewritten from PLAN to RETROSPECTIVE,
  same shape as chapter 79: what we changed, what bug we
  hit (there's always one), and what shipped.
