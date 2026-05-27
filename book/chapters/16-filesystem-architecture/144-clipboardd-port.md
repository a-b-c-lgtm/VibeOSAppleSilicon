# Chapter 144 — Step 4: porting `clipboardd` to userfs

Chapter 113 built a userspace clipboard daemon
(`/bin/clipboardd`) on top of the chapter-107 IPC bus. It
defined a tiny protocol with `SET`, `GET`, `GEN`, and
`CLEAR` opcodes, a 48-byte header, a MIME tag, and a
generation counter; callers in `notepad`, `browser`, and
`gui_term` all knew about the opcode numbers and the header
layout because there was no other choice — the only way to
reach the clipboard was to open the named-IPC socket and
speak its protocol.

This chapter throws all of that away. After it, the
clipboard is *a file*: `/clipboard/text`. Reading the file
gets you the current payload. Writing the file replaces it.
Truncating the file (the same `O_TRUNC` flag your shell's
`>` redirection passes) clears it. There is no protocol to
learn; the existing `cat`, `echo`, `cp`, `grep`, and
`head` already know how to drive it because they already
know how to drive files.

That's the whole point of chapter 140. Userfs gave us a way
to expose a userspace daemon as a filesystem. The very
first thing we should do with that capability is rewrite a
daemon we already have, see how much code falls out, and
write down what was easy and what was hard. The next
daemon we port (procd, [chapter 145](145-procd-port.md)) is
twice the size; this one is the warm-up.

## What got deleted

The chapter-108 implementation was four files plus three
Makefile blocks:

| Path | Size | Fate |
| --- | --- | --- |
| `userspace/clipboardd/clipboardd.c` | 234 lines | rewritten (160 lines) |
| `userspace/libc/clipboard.h` | 296 lines | rewritten (104 lines) |
| `userspace/clip/clip.c` | 150 lines | **deleted** |
| `scripts/test_clipboard.py` | 220 lines | rewritten (200 lines, archived as `_dbg_test_clipboard_chapter108.py`) |

The clipboard.h shrank because every line that defined a
protocol struct (`struct clip_msg`), op code (`CLIP_OP_SET`,
…), MIME constant (`CLIP_MIME_TEXT`, …), endpoint name
(`CLIP_SOCK_PATH`), or wire-error code (`CLIP_ERR_PROTO`,
`CLIP_ERR_TOOBIG`) is gone. The new header has three
inline helpers — `clip_set`, `clip_get`, `clip_clear` —
that each do `open + read-or-write + close` on
`/clipboard/text`. They're convenience wrappers, not
abstractions: a caller that doesn't want the wrapper can
call `open` directly with no information loss.

`/bin/clip`, the chapter-108 CLI, didn't survive at all.
Its job was to let humans poke the clipboard from the
shell. With chapter 140 in place, `echo foo > /clipboard/text`
and `cat /clipboard/text` already do that — there is no
gap left to fill. The source is preserved under
`userspace/clip.deleted/` for the curious; the Makefile no
longer mentions it; init no longer spawns it.

The test rewrite shrank too, but for a different reason.
Chapter 113's regression had to drive eight assertions
through a fork of `/bin/clip` (one per opcode), because that
was the only way to exercise the daemon without writing C.
The chapter-114 regression has six assertions, all driven
through plain shell builtins:

```python
ser.sendall(b"echo Hello chapter 140 > /clipboard/text\n")
ser.sendall(b"cat /clipboard/text\n")  # expects "Hello chapter 140"
ser.sendall(b"echo world > /clipboard/text\n")
ser.sendall(b"cat /clipboard/text\n")  # expects "world", no leftover "Hello"
ser.sendall(b": > /clipboard/text\n")
ser.sendall(b"cat /clipboard/text\n")  # expects neither "world" nor "Hello"
```

The middle assertion (`O_TRUNC` resets the payload) is the
one that proves the daemon's `on_open` truly honours the
truncate bit; without it, a 100-byte payload followed by a
50-byte payload would leave `world50` + `Hello` in the
file. We test for the absence of `Hello chapter 140` in the
second `cat`'s output to catch that case.

## What `clipboardd` looks like now

160 lines. The shape is identical to `echofs` from
[chapter 143](143-libfs-and-echofs.md):

```c
struct userfs_handler h;
h.on_open    = on_open;
h.on_read    = on_read;
h.on_write   = on_write;
h.on_close   = on_close;
h.on_listdir = on_listdir;
h.on_is_dir  = on_is_dir;
h.userdata   = NULL;
return userfs_serve("/clipboard", &h);
```

State is two globals:

```c
static uint8_t  g_data[CLIP_DATA_MAX];   /* 32 KiB */
static uint32_t g_len = 0;
```

That's the entire chapter-108 state machine. No generation
counter, no MIME tag, no in-flight slot table. `g_len`
serves the role the generation counter used to — a reader
that wants change notification stats the file (and a kernel
that wants to surface mtime can do so in any future
chapter). The MIME tag is gone because there's only one
file (`text`) and the file itself IS the type; if a future
need for binary data shows up, we add `/clipboard/png`
alongside `/clipboard/text` and the tag returns implicitly.

The four callbacks are short. `on_open` is the most
interesting because it implements `O_TRUNC`:

```c
static int on_open(void *ud, const char *path, int flags, uint32_t *h)
{
    (void)ud;
    if (!eq(path, "text")) return -2;  /* -ENOENT */
    if (flags & 0x200) {               /* O_TRUNC */
        g_len = 0;
    }
    *h = H_TEXT;
    return 0;
}
```

That's all the chapter-108 `SET` machinery: a single
truncate bit, applied unconditionally because there is
only one payload slot. `on_write` then appends from the
caller's offset:

```c
static int on_write(void *ud, uint32_t h, uint64_t off,
                    const void *buf, uint32_t n)
{
    if (h != H_TEXT) return -9;            /* -EBADF */
    if (off > CLIP_DATA_MAX) return -22;   /* -EINVAL */
    uint32_t room = CLIP_DATA_MAX - (uint32_t)off;
    uint32_t take = n < room ? n : room;
    /* memcpy, then bump g_len */
    ...
}
```

A misbehaving client can't blow up the daemon: the buffer
is fixed-size, every offset is sanity-checked, and the
short-write at the limit propagates back through the
kernel-side `userfs_op_write` to the caller's `write()`
return value as a short-write — exactly what would happen
on a quota-bound file.

## The user-pointer trap we hit on the way

The notepad/browser/gui_term port itself was mechanical:
every site that did

```c
clip_set(CLIP_MIME_TEXT, buf, len, NULL);
clip_get(buf, sizeof buf, &len, mime);
```

became

```c
clip_set(buf, len);
clip_get(buf, sizeof buf, &len);
```

But the very first cross-app paste regression
(`test_clipboard_paste.py`) panicked the kernel with a data
abort at EL1, FAR pointing into `gui_term`'s `.bss`. The
crash was in `pipe_read`:

```c
for (size_t i = 0; i < n; i++) {
    dst[i] = p->buf[p->head];     /* ← strb at this site */
    ...
}
```

`pipe_read` was raw-byte-storing into the caller's user
buffer. For a fresh `fork`+`exec` child whose `.bss` page
was still COW-shared with the parent (or hadn't been
demand-faulted at all), that store traps with `DFSC=0xF`
— permission fault at the last-level page table. The
write path didn't hit it because `handle_write` already
stages user bytes through a 256-byte kernel chunk before
calling `g_userfs_ops.write`; the read path had no such
staging.

The fix lives in `kernel/core/userfs.c::userfs_op_read`:
`kmalloc(n)` a kernel buffer, let `userfs_call` populate
it, then `copy_to_user(buf, kbuf, got)`. `copy_to_user`
runs the existing `prefault_user_write` helper which
breaks COW shares and installs lazy-anon pages cleanly —
the same discipline the rest of the kernel's syscalls
already use. The rule:
**any future userfs op that writes into a user buffer must
stage through a kernel buffer and `copy_to_user` at the
boundary**; `pipe_read` is not safe to drive directly from
a userfs path.

The bug was latent: chapter 143's `echofs` test
(`test_userfs_echo.py`) worked because it ran from the
shell, whose data pages were already touched (and so
writable) by the time the read came in. It took a fresh
`fork`+`exec` paste path to expose it.

## What this unlocks

Per the apps-must-use-features discipline:

- **Existing app(s) modified to use the feature**:
  `userspace/notepad/notepad.c`,
  `userspace/browser/browser.c`,
  `userspace/gui_term/gui_term.c` — Ctrl-C / Ctrl-X /
  Ctrl-V keystrokes all now `open()`/`read()`/`write()`
  `/clipboard/text` instead of speaking the chapter-108
  IPC protocol.
- **New app(s) added**: none — `/bin/clip` was deleted
  because shell builtins (`echo`, `cat`, `: >`)
  superseded it. This is the cleanest possible
  demonstration of why "everything is a file" is worth
  the trouble.
- **Existing test scripts upgraded**:
  `scripts/test_clipboard.py` (rewritten),
  `scripts/test_clipboard_paste.py` (updated for the new
  audit byte count).
- **New test scripts added**: none required; the rewritten
  `test_clipboard.py` covers what the chapter-108 tests
  did and more.

## Side-effects for the next chapter

The userfs read-side staging fix in `userfs_op_read` is now
load-bearing for every future userfs daemon. Chapter 145
will exercise the same path under heavier load (procd's
`/proc/<pid>/status` files are kilobytes apiece, where
clipboardd's are single-line reads); chapter 146 will add
the per-request 5-second timeout that closes the
last-remaining failure mode (daemon hangs holding off
every other client).
