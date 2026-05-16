# Chapter 34 — Variable expansion and `./prog`

Now that the shell knows about environment variables (chapter
33), it should *use* them: `$HOME`, `${PATH}`, and `$?` should
work the way every Unix user expects. And bash users have a
muscle memory of `./prog` for "run the binary in the current
directory" — that should work too. Both are pure shell-side
features; the kernel doesn't change.

## What it looks like

```
/$ echo home is $HOME
home is /

/$ echo path is $PATH
path is /bin

/$ export GREETING=hi
/$ echo ${GREETING}-osdev
hi-osdev

/$ echo missing is [$NOPE]
missing is []                  # silent unknown-var expansion

/$ badcmd
[sh] no such command: badcmd (errno=2)
/$ echo exit was $?
exit was 127                   # bash convention for not-found

/$ hello
hello from EL0!
/$ echo exit was $?
exit was 0

/$ cd /bin
/bin$ ./hello                  # cwd-relative resolution
hello from EL0!
```

Five features, all in `userspace/sh/sh.c`:

1. `$VAR` expansion (one identifier).
2. `${VAR}` expansion (explicit braces).
3. `$?` — last command's exit code.
4. `$$` — literal `$` for now (bash uses it for shell PID;
   we'd need `getpid()` and don't yet need it).
5. `./prog` and `../prog` resolution against the cwd.

## Where expansion happens

Right after `read()` and `trim()`, before any builtin matching
or argv splitting. The expanded string lives in a separate
buffer; we never mutate the raw input.

```c
char raw[LINE_MAX];
char line[LINE_MAX];
/* ... */
read(0, raw, LINE_MAX - 1);
trim(raw, n);
expand_vars(raw, line, LINE_MAX);
/* ... continue using `line` ... */
```

That ordering matters: it means `cd $HOME`, `export FOO=$BAR`,
and `./$prog` all work without each builtin needing its own
expansion code path. The whole shell sees the expanded text.

The cost is two buffers' worth of stack for one read iteration —
256 bytes total, comfortable.

## The `expand_vars` walk

Conceptually trivial, but a few corners need handling:

```c
static void expand_vars(const char *src, char *dst, int cap)
{
    int  pos = 0;
    char val[256];

    while (*src && pos < cap - 1) {
        if (*src != '$') { dst[pos++] = *src++; continue; }
        src++;                                  /* eat '$' */
        if (*src == '?') {  ... append_int(g_last_exit) ...; continue; }
        if (*src == '$') {  ... emit literal '$' ...; continue; }
        char name[32];
        int  ni = 0;
        if (*src == '{') {
            src++;
            while (*src && *src != '}' && ni < (int)sizeof(name) - 1)
                name[ni++] = *src++;
            if (*src == '}') src++;
        } else {
            while (is_var_cont(*src) && ni < (int)sizeof(name) - 1)
                name[ni++] = *src++;
        }
        name[ni] = '\0';
        if (ni == 0) { /* lone '$' */ dst[pos++] = '$'; continue; }
        long got = getenv(name, val, sizeof(val));
        if (got > 0) append_str(dst, &pos, cap, val);
        /* Unknown var: silent empty expansion. */
    }
    dst[pos] = '\0';
}
```

Three corner-case decisions worth flagging:

**Unknown vars expand to empty silently.** Bash with default
options does the same; only `set -u` makes them an error. Our
shell doesn't have options at all.

**`${...}` braces are stripped if present and matched.** A
malformed `${FOO` (no closing brace) just runs to end-of-input;
the var name becomes whatever was inside.

**`$?` reads from a file-scope `g_last_exit`.** Updated in two
places: after every `wait()` (real exit code) and on every
spawn-failure (synthesized 127, matching bash's "command not
found" convention). Any builtin that fails could in principle
update it too; today we don't bother.

## Cwd-relative path resolution

In the shell's `resolve_path`:

```c
if (name[0] == '.' && name[1] == '/') {
    /* `./foo` — prepend cwd + '/' */
    char cwd[96];
    long cwn = getcwd(cwd, sizeof(cwd));
    /* ... copy cwd, ensure trailing '/', append name+2 ... */
    return;
}

if (name[0] == '.' && name[1] == '.' && name[2] == '/') {
    /* `../foo` — strip last cwd segment, then like `./foo` */
    /* ... */
    return;
}
```

For our flat namespace, `..` only meaningfully resolves from
`/bin` or `/mnt` (both go to `/`). From `/` itself we just
return `/foo`, which won't match anything. The code handles
both cleanly because the "strip last segment" loop falls back
to `/` when no internal slash exists.

We deliberately don't recurse on `././foo`, `../../foo`, etc.
A real `realpath()` resolver belongs in libc when we have a
hierarchical FS to resolve against.

## Why the expansion buffer is separate from the raw

Two buffers (`raw[LINE_MAX]` for the read, `line[LINE_MAX]` for
the expanded form) consumes 256 bytes of shell stack. A single
in-place expander would save those bytes but would have to
handle the case where the expansion is *longer* than the
original text (which is the common case — `$HOME` expands to
something longer than `$HOME`). Two buffers are dramatically
simpler. The shell stack has plenty of room.

## What's still missing

- **Word splitting on the expansion result.** Bash's
  `IFS`-driven splitting means `cmd $args` becomes one or
  multiple argv elements depending on whether `$args` contained
  spaces. We do *no* post-expansion splitting — the spawn
  syscall sees one args-string and tokenizes that. Equivalent
  to bash's `cmd "$args"` (always a single argument).
- **Quoting.** `echo "hello world"` and `echo 'hi $VAR'`
  both treat the quotes as literal characters. Adding double-
  vs-single-quote semantics requires a proper tokenizer.
- **Globbing.** `cat *.txt` doesn't expand. Needs a directory
  walker over PATH.
- **Backtick / `$()` substitution.** Both require nested shell
  invocation; not happening soon.
- **Arithmetic `$((expr))`.** Same.
- **`set` builtin to enumerate variables.** Today `env` lists
  exported vars but there's no concept of unexported (shell-
  local) vars yet.
- **`$#`, `$1..$9`.** No script execution yet, so positional
  parameters don't exist.

## What changed

```
userspace/sh/sh.c        +120 lines:
                          expand_vars(), append_str/int helpers,
                          is_var_cont(), g_last_exit,
                          ./prog and ../prog branches in
                          resolve_path, raw vs line buffers,
                          spawn-failure -> $?=127
kernel/core/main.c       banner -> milestone 25
```

Pure userspace — no kernel changes, no new syscalls. The shell
is now significantly more usable; everything you'd type
interactively works the way bash users expect.
