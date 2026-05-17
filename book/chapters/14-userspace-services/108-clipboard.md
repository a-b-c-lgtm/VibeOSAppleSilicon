# Chapter 108 — The system clipboard, as a userspace service

**Status:** Done. Tracking milestone 90b.

Notepad cannot copy text into the browser address bar.
The browser cannot copy a URL out. We have everything
we need to fix this, and — thanks to chapter 107's IPC
primitive — we can fix it *without* growing the kernel.

This chapter ships three things on top of chapter 107:

1. `/bin/clipboardd` — the system clipboard, as a
   ~250-line userspace daemon bound to `/srv/clipboard`.
2. `/bin/clip` — the command-line client (`clip set`,
   `clip get`, `clip gen`, `clip clear`) that any shell
   user can reach for, and that the regression test
   drives end-to-end.
3. A minimal supervisor in `init` that respawns
   `/bin/clipboardd` if it dies, so the clipboard is
   always available once boot finishes.

Plus the GUI integration that finally lets one app's
text reach another: notepad gains Ctrl-C (copy line),
Ctrl-X (cut line) and Ctrl-V (paste); the browser's
URL bar grows the same three keystrokes; and gui_term
gains Ctrl-V so terminal users can paste seeded
text into the inner shell.

The kernel grows zero new syscalls, zero new fd kinds,
and zero new policy. The cost of the feature is paid
in userspace, where the cost belongs.

## Why this isn't a kernel feature

Every mainstream OS puts the clipboard wherever the
compositor lives:

| OS | Compositor | Clipboard owner |
|---|---|---|
| macOS | WindowServer (userspace) | `pasteboardd` (userspace) |
| Linux/X11 | X server (userspace) | X server (selection atoms) |
| Linux/Wayland | compositor (userspace) | compositor protocol |
| **us, before chapter 107** | **kernel WM** | would have been kernel |
| **us, now** | kernel WM | **`/bin/clipboardd` userspace daemon** |

The clipboard isn't intrinsically a kernel thing — it's
a *compositor* thing, and our compositor just happens
to live in the kernel today. Before chapter 107, the
cheapest place to park the clipboard was beside the WM
because the WM was the only existing "always-on,
GUI-aware, multi-app" service. Three syscalls and a
kmalloc would have done it.

With chapter 107 in hand, that's no longer true. The
clipboard is now a small userspace program that:

- Binds `/srv/clipboard` at boot via `srv_bind`.
- Accepts connections from any GUI app, one message
  per connection.
- Holds the current payload (≤ 32 KiB plus a MIME tag)
  in its own heap.
- Bumps a monotonic generation counter on every `set`
  or `clear`.
- Survives crashes via `init`'s supervisor — losing
  the clipboard contents on respawn, which is exactly
  the X11 behaviour every Linux user is used to.

## What this chapter adds

- **`userspace/libc/clipboard.h`** — the protocol
  header. Defines `struct clip_msg` (a 48-byte fixed
  header) and four operations (`CLIP_OP_SET`,
  `CLIP_OP_GET`, `CLIP_OP_GEN`, `CLIP_OP_CLEAR`), plus
  four `static inline` client helpers (`clip_set`,
  `clip_get`, `clip_generation`, `clip_clear`) that any
  userspace program can include with no link
  dependency.
- **`userspace/clipboardd/clipboardd.c`** — the daemon.
  Binds the service, runs an accept loop, dispatches
  one operation per connection.
- **`userspace/clip/clip.c`** — the CLI client.
  `clip set foo bar`, `clip set <file`, `clip get`,
  `clip gen`, `clip clear`. The thing a user actually
  reaches for. Also the regression test's only puppet.
- **Notepad keystrokes** — Ctrl-C copies the current
  line, Ctrl-X cuts it, Ctrl-V pastes at the cursor
  (newlines in the payload become real newlines in the
  buffer).
- **Browser URL bar keystrokes** — when the address
  bar has focus, Ctrl-C copies its current contents,
  Ctrl-X cuts them, and Ctrl-V pastes printable bytes
  at the caret (stopping at the first `\n`/`\r` so a
  multi-line clipboard can't smear the bar).
- **gui_term paste** — Ctrl-V calls `clip_get`,
  translates `\n` → `\r` (the inner shell's line
  editor expects carriage returns), filters to
  `\r`/`\t`/printable ASCII, and writes the result
  onto the pty master.  Ctrl-C and Ctrl-X stay
  reserved for the inner shell (SIGINT / discard-line)
  — copying *out of* gui_term will land later, alongside
  text selection.
- **Init supervisor** — a tiny per-pid table
  (`g_supervised[]`) that respawns any "should always
  be running" service when the reap loop sees its pid
  exit. Today only `/bin/clipboardd` is supervised;
  the table is sized for ch113's audio mixer to slot
  in without a rewrite.

## Prerequisites

- Chapter 107 — Named IPC. The whole reason this
  chapter isn't in the kernel. `srv_bind`,
  `srv_accept`, `srv_connect`, and the framed
  read/write on `/srv` conns are the load-bearing
  primitives.
- Chapter 48 — Window manager + keyboard input.
  Notepad's Ctrl-C/X/V arrive through the same path
  as Ctrl-S, just three new char codes (0x03, 0x18,
  0x16).
- Chapter 17 — `init` / `spawn` / `wait`. The
  supervisor is one extra hook on the existing reap
  loop, not a separate process.

## Design decisions

### One process, in-memory state, no persistence

The clipboard is volatile by design. On macOS,
`pasteboardd` loses everything on logout. On X11, you
lose the selection when the source program quits. We
lose it when `/bin/clipboardd` dies (or the box
reboots). That's the right behaviour: users expect the
clipboard to be a working set, not a save state. If
someone wanted persistence, they could write
`/bin/clipboard-history` as a separate IPC client that
polls `clip_generation` and copies anything new into
`/data/clipboard.log` — a 30-line program that proves
the point.

### One generation counter, advertised on every reply

Every `GET` reply includes the generation alongside
the payload. Every `SET` reply includes the *new*
generation. `GEN` exists for the "paste menu
enabled-state" use case (the only thing you want to
know is "should I bother enabling this menu item?"),
but the steady-state pattern is *don't poll* — your
`GET` already told you the current generation, and
the next `SET` you do will tell you again. The
counter starts at 0 (empty clipboard, never written)
and is monotonic across the daemon's lifetime; it
resets to 0 on respawn, which apps treat as "the
world changed underneath you" — exactly the right
fail-safe.

### MIME as an opaque string

`text/plain` and `text/uri-list` are the two the
chapter-108 clients agree on; everything else is
`application/octet-stream`. The daemon never inspects
MIME — it's a tag the clipboard hands back unchanged
on `GET`. Apps that don't recognise the type just
refuse to paste.

### 32 KiB cap, silent truncation flag

`CLIP_DATA_MAX = 32768`. Anything longer is truncated
(at *both* the sender and the daemon as belt-and-braces)
and the `SET` reply sets `CLIP_FLAG_TRUNC`. The cap is
sized so header + payload comfortably fits within
chapter 107's per-message ceiling (`SRV_MSG_MAX` =
64 KiB), which is not a coincidence: we want "set the
clipboard" to be exactly one IPC datagram.

### Cut vs copy is the *app's* problem

`/bin/clipboardd` only knows `SET`. Notepad's Ctrl-X
is `SET(current line); delete(current line)`. The
daemon never sees the difference. This keeps the
protocol genuinely shaped around the data model
(payload + generation + MIME) and not around UI
metaphors.

### One message per connection

Every `clip_*` helper opens a fresh IPC connection,
sends one request, reads one reply, closes. No
persistent state across calls. This means the daemon
needs no per-connection bookkeeping — the conn fd's
sole purpose is to demarcate one request — and we
can keep the whole serve loop a clean two-call
sequence: `serve_one(cfd); close(cfd);`. Long-lived
clients pay a few microseconds of connect overhead
per op; for a clipboard that's far below noise.

### Line-granular Ctrl-C/X/V in notepad

Real text editors have selection regions. We don't,
yet — notepad's only cursor concept is the
single-character caret. Rather than build a selection
model just for this chapter, we ship line-granular:
Ctrl-C copies the current line, Ctrl-X cuts the
current line, Ctrl-V pastes (splitting any embedded
`\n` into separate inserted lines). This actually
makes a useful editor better — and when notepad
eventually grows mouse-drag selection, the only
change is the source range that feeds `clip_set`.

### Supervisor lives in init, not in the kernel

The kernel exposes the namespace and the framing
(chapter 107); the "keep the daemon alive" policy
lives entirely in userspace. `init.c` carries a
fixed-size table (`g_supervised[SUPERVISED_MAX = 4]`)
and a hook in its existing reap loop. When the loop
reaps a pid that matches a supervised entry, it
respawns the binary and updates the entry's tid in
place. No backoff today — if `clipboardd` is
crashing in a tight loop the right answer is to fix
it, not to back off. When chapter 113 adds the audio
mixer, the same `supervise()` call slots in beside
this one.

## Walkthrough

The on-wire protocol is one 48-byte header followed
by an optional payload, as a single chapter-107
datagram per direction:

```c
struct clip_msg {
    uint32_t op;                   /* CLIP_OP_* */
    uint32_t gen;                  /* generation */
    uint32_t len;                  /* payload byte count */
    uint32_t flags;                /* truncated bit, error codes */
    char     mime[CLIP_MIME_MAX];  /* null-terminated MIME tag */
    /* uint8_t data[len] follows on the wire. */
};
```

The daemon's main loop is six lines:

```c
int lfd = srv_bind(CLIP_SOCK_PATH);
for (;;) {
    int cfd = srv_accept(lfd);
    if (cfd < 0) continue;
    serve_one(cfd);
    close(cfd);
}
```

`serve_one` reads one datagram, switches on
`req->op`, and dispatches to one of four handlers.
`handle_set` copies the payload into `g_data[]`,
bumps `g_gen`, and writes back a SET reply.
`handle_get` writes back a GET reply that's the
fixed header plus `g_len` payload bytes. `handle_gen`
writes back just the header. `handle_clear` zeroes
`g_data[]`, bumps `g_gen`, writes back a CLEAR
reply.

The client side is all in `clipboard.h` —
`clip_set` builds a header+payload buffer, calls
`write()` once (so the chapter-107 framing sees one
datagram), then `read()`s back one header. The
helper hides the open/write/read/close cycle so
callers see this:

```c
int gen = clip_set("text/plain", buf, len, NULL);
if (gen < 0) /* daemon not bound, or IPC error */;
```

Notepad's Ctrl-C handler is one line. The same for
Ctrl-V. The cut handler is "copy, then delete the
current line". The whole feature in notepad is
~50 lines.

The browser's URL bar reuses the same three handlers
in the focused-input branch of its `GUI_EVENT_KEY`
dispatcher, with one extra rule: the paste loop stops
at the first newline.  A URL can't contain a literal
newline, so the cleanest thing to do is treat
clipboard-with-newline as "paste the first line" and
let the rest fall on the floor — which matches what
Firefox and Chrome do.

gui_term's handler is slightly different.  It runs
*before* the regular `key_to_bytes` translator so that
the raw 0x16 byte never reaches the pty (otherwise the
inner shell would see a literal Ctrl-V and try to
quote-insert the next byte).  After `clip_get`
returns, it converts `\n` to `\r` because the inner
shell's line editor terminates input on `\r`, not on
`\n`.  Successful pastes print one audit line —
`[gui_term] pasted N bytes` — to fd 1.  When gui_term
is spawned from the serial-attached outer shell
(`gui_term &`), that audit line lands on the host
serial; the cross-app paste regression keys off
exactly this line.

`init`'s supervisor is the second-shortest piece of
the chapter. The reap loop already iterated until the
shell exited; we add one call:

```c
if (supervise_check(reaped, code)) continue;
```

`supervise_check` scans `g_supervised[]`, finds the
matching entry, respawns, and updates the entry's
tid. Everything else in the reap loop is unchanged.

## Tests

`scripts/test_clipboard.py` is the chapter capstone —
fully hermetic, no host network. It boots the kernel,
waits for the shell prompt, and drives the `/bin/clip`
CLI through eight assertions:

1. Shell prompt reached (boot completed).
2. `[clipboardd] ready on /srv/clipboard` appeared
   (supervisor brought up the daemon).
3. `clip set Hello chapter 108` → daemon logs `SET
   gen=1`.
4. `clip get` → "Hello chapter 108" comes back on the
   serial console.
5. `clip gen` → daemon logs `GEN -> gen=1`.
6. Second `clip set world` → daemon logs `SET gen=2`
   (generation advanced).
7. Second `clip get` → daemon logs `GET -> gen=2
   len=5` (slot was overwritten).
8. `clip clear` → daemon logs `CLEAR gen=3` (clear
   also bumps generation).

End-to-end under ten seconds. That covers the
protocol, the daemon storage, and the supervisor.

`scripts/test_clipboard_paste.py` covers the GUI
keystroke wiring on top.  The trick is to boot QEMU
with `virtio-keyboard-device` *and* a serial console,
seed the clipboard from the serial-attached shell
(`clip set IPC_HELLO_FROM_HOST`), spawn gui_term as
a background child of that same shell (`gui_term &`)
so gui_term inherits the serial as its stdout, then
inject a real Ctrl-V via QMP `input-send-event`.
The focused gui_term window calls `clip_get`, writes
19 bytes onto its pty, and prints
`[gui_term] pasted 19 bytes` to stdout — which the
test greps for on the serial wire.  The same run
also verifies that `[clipboardd] GET -> gen=1 len=19`
appeared, so we know the bytes actually traversed
`/srv/clipboard` rather than coming from some
in-process cache.  Six assertions, ~10 s wall, fully
hermetic.

### Why no browser regression?

The browser's URL bar uses the same three handlers,
but spawning the browser pulls in css + layout + html
+ png + a parser thread, and the only audit signal
that would tell us "the paste landed" is the URL
bar's pixel content — which is exactly the fragile
path the chapter-44 headless-render iteration loop
was designed to *avoid*.  The hand-test (open the
browser, click the URL bar, Ctrl-V) is reliable
because it's the same code path as notepad and
gui_term; gui_term's regression is the canary for
all three.  When the browser eventually grows its
own serial-visible audit channel (or when we wire
up a `gui_screen_diff` primitive), the browser case
gets a near-identical test.

Notepad's wiring is exercised by the existing
`scripts/test_notepad*.py` tests when they save a
buffer that was filled via paste; a dedicated
notepad-paste regression isn't worth the boot cost
given gui_term already proves the end-to-end
keystroke → `clip_get` → app-side insertion path.

## What you'll learn

- The "owner of the compositor owns the clipboard"
  rule, and why it stays true even when you move the
  compositor out of the kernel later.
- Why every clipboard you've ever used has a
  generation counter (and why most apps don't need
  to poll it).
- That the difference between a clipboard daemon,
  an audio mixer daemon, and a TLS proxy is which
  messages they accept — not anything structural.
- A small-scale supervisor pattern that scales up
  (today: 1 entry; next chapter: 2; eventually: N
  with a `services.conf`).

## What gets exercised in tests

- New script: `scripts/test_clipboard.py` — 8 serial
  assertions, ~10 s wall, fully hermetic; covers the
  daemon and the CLI end-to-end.
- New script: `scripts/test_clipboard_paste.py` — 6
  assertions, QMP-injected Ctrl-V; proves real bytes
  flow from one process (the serial shell + `clip
  set`) through `/srv/clipboard` into a second
  process (gui_term's pty).
- New daemon: `userspace/clipboardd/clipboardd.c`
  (`/bin/clipboardd`).
- New CLI: `userspace/clip/clip.c` (`/bin/clip`).
- New header: `userspace/libc/clipboard.h` — protocol
  + thin client helpers, shared by daemon, CLI,
  notepad, browser, and gui_term.
- Modified app: `userspace/notepad/notepad.c` gains
  Ctrl-C / Ctrl-X / Ctrl-V (line-granular).
- Modified app: `userspace/browser/browser.c` gains
  Ctrl-C / Ctrl-X / Ctrl-V on the URL bar (newline-
  truncated, ASCII-printable only).
- Modified app: `userspace/gui_term/gui_term.c`
  gains Ctrl-V (\n → \r, filtered to
  \r/\t/printable-ASCII, audited via
  `[gui_term] pasted N bytes` on fd 1).
- Modified init: `userspace/init/init.c` gains a
  4-entry supervisor table and the
  `supervise()`/`supervise_check()` helpers.

## What this unlocks

- **Copy-paste between any two GUI apps.** Notepad to
  notepad, notepad to the browser address bar, the
  browser to the shell via `clip get`, and any of
  the above to gui_term's inner shell with Ctrl-V.
  Cross-app paste is now exercised by
  `test_clipboard_paste.py`, not just verified by
  hand.
- **Shell-level clipboard for pipelines.** `echo
  $(date) | clip set; clip get` is now a real thing
  you can type.
- **Chapter 113 audio mixer.** Same `supervise()`
  call; same `/srv/<name>` pattern. The mixer
  arbitrates virtio-snd writes across multiple GUI
  apps without putting mixing in the kernel.
- **Future TLS proxy.** Browser opens `/srv/tls`
  instead of speaking TLS itself. Same shape; same
  supervisor; zero kernel changes.
- **Per-user permissions later.** When we add a user
  model, the daemon already records peer pids; an
  ACL check is one `if` away from gating writes by
  uid.
