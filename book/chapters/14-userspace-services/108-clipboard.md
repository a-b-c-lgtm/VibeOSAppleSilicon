# Chapter 108 — The system clipboard, as a userspace service

**Status:** Stub. Tracking milestone 90.

Notepad cannot copy text into the browser address bar.
The browser cannot copy a URL out. We have everything
we need to fix this, and — thanks to chapter 107's IPC
primitive — we can fix it *without* growing the kernel.

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
a *compositor* thing, and our compositor just happens to
live in the kernel today. Before chapter 107, the
cheapest place to park the clipboard was beside the WM
because the WM was the only existing "always-on,
GUI-aware, multi-app" service. Three syscalls and a
kmalloc would have done it.

With chapter 107's named-IPC primitive, that's no
longer true. The clipboard is now a 300-line userspace
program that:

- Binds `/srv/clipboard` at boot via `SYS_SRV_BIND`.
- Accepts connections from any GUI app.
- Holds the current clipboard payload (≤ 64 KiB plus
  a MIME tag) in its own heap.
- Bumps a monotonic generation counter on every `set`.
- Survives crashes via `init`'s respawn loop (chapter
  107) — losing the clipboard contents on respawn,
  which is exactly the X11 behaviour every Linux user
  is used to.

The kernel grows zero new syscalls, zero new fd kinds,
and zero new policy. The cost of the feature is paid in
userspace, where the cost belongs.

## What this chapter adds

- `/bin/clipboardd` — the clipboard daemon. Listens on
  `/srv/clipboard`. Four message kinds: `SET`, `GET`,
  `GEN`, `CLEAR`.
- `userspace/libc/clipboard.h` — three thin client
  helpers (`clip_set`, `clip_get`, `clip_generation`)
  that open `/srv/clipboard`, send one message, return.
- `notepad`: Ctrl-C / Ctrl-X / Ctrl-V wired through
  the client helpers.
- `browser`: Ctrl-C in the URL bar copies, Ctrl-V
  pastes; Ctrl-C on selected page text copies plain.
- `gui_term`: Ctrl-Shift-C / Ctrl-Shift-V (not Ctrl-C,
  which is reserved for SIGINT once chapter 77 lands).
- `init` is taught to start `/bin/clipboardd` on boot.

We also support Ctrl-A to "select all" in text boxes —
not a clipboard feature strictly, but the keystroke
people reach for in the same breath.

## Prerequisites

- Chapter 48 — Window manager. Owns focus and routes
  Ctrl-keys to the focused window.
- Chapter 107 — IPC. The whole reason this chapter
  isn't in the kernel.
- Chapter 39 — Pipes / THREAD_BLOCKED. The daemon
  blocks in `accept` and in per-connection `read`.

## Design decisions

### One generation counter, advertised on every reply

Every `GET` reply includes the generation alongside the
payload. Every `SET` reply includes the *new*
generation. Polling `GEN` exists for the "Paste menu
enabled-state" use case, but the steady-state pattern
is *don't poll* — your `GET` already told you the
current generation, and the next `SET` you do will tell
you again.

### MIME as an opaque string

`text/plain` and `text/uri-list` are the two we promise
to handle; everything else is `application/octet-stream`
and apps that don't recognise the type just refuse to
paste. The daemon never inspects MIME — it's a tag the
clipboard hands back unchanged.

### 64 KiB cap, silent truncation flag

Anything longer is truncated and the reply sets a
`truncated` bit. The cap matches chapter 107's per-
message ceiling, which is not a coincidence: we want
"set the clipboard" to be one IPC message.

### Cut vs copy is the *app's* problem

`/bin/clipboardd` only knows `SET`. Notepad's Ctrl-X is
`SET(selection); delete(selection)`. The daemon never
sees the difference.

### What about race-safety between the WM and the
daemon?

The WM routes Ctrl-C to the focused window; the
focused window calls `clip_set`; `clip_set` opens
`/srv/clipboard` and sends a message. There is exactly
*one* writer per Ctrl-C event (the focused app), so
there's no two-writer race. If two windows race to
copy "simultaneously" the generation counter
serialises them — the second wins, just like every
other OS.

## Plan

1. Define the four-message protocol (`SET`, `GET`,
   `GEN`, `CLEAR`) in a header shared by `clipboardd`
   and `libc/clipboard.h`.
2. Write `/bin/clipboardd`: `SYS_SRV_BIND` →
   `SYS_SRV_ACCEPT` loop → per-message switch.
3. Write `userspace/libc/clipboard.h` (three inline
   functions, no syscalls of their own).
4. Wire `notepad`, `browser`, `gui_term` to use it.
5. Add `clipboardd` to `init`'s service list (chapter
   101's supervisor).
6. Regression test (`scripts/test_clipboard.py`): boot,
   `clip_set` from one shell, `clip_get` from another,
   verify byte-for-byte and that the generation
   incremented.

## What you'll learn

- The "owner of the compositor owns the clipboard"
  rule, and why it stays true even when you move the
  compositor out of the kernel later.
- Why every clipboard you've ever used has a generation
  counter (and why most apps don't need to poll it).
- That the difference between a clipboard daemon and
  an audio mixer daemon, structurally, is which
  messages they accept.

## What this unlocks

- Copy-paste between any two apps.
- URL transport: copy a link from a search-result page
  in `/bin/browser`, paste into the address bar of
  another browser window.
- The same pattern, instantiated again, for the audio
  mixer in a future chapter and for the OS-level
  notification history in another.
