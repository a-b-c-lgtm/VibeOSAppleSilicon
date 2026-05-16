# Chapter 101 — The system clipboard

**Status:** Stub. Tracking milestone 90.

Notepad cannot copy text into the browser address bar.
The browser cannot copy a URL out. We have everything we
need to fix this in three syscalls and a small WM-owned
buffer.

**Question for agent:** WHY IS CLIPBOARD NOT IN USERSPACE AS A SERVICE?

## What this chapter adds

- `SYS_CLIP_SET(buf, len, mime)` — overwrite the clipboard.
- `SYS_CLIP_GET(buf, cap, mime_out)` — read and report MIME.
- `SYS_CLIP_GENERATION()` — a monotonic counter the WM
  bumps on every set; clients poll it to redraw a "Paste"
  menu's enabled state.
- Notepad: Ctrl-C / Ctrl-X / Ctrl-V.
- Browser: Ctrl-C in URL bar copies, Ctrl-V pastes.
- gui_term: same.

We will also support Ctrl-A to select all in text boxes.

## Prerequisites

- Chapter 48 — WM (owns the buffer)

## Plan

- Buffer cap: 64 KiB; longer copies are silently truncated
  with a flag.
- MIME types we care about: `text/plain`, `text/uri-list`.
  Everything else is `application/octet-stream`.
- Cut vs Copy is a userspace concern; the syscall is just
  "set."

## What you'll learn

- The X11 vs Mac vs Windows debate condensed to one
  sentence ("we own the buffer").
- Why clipboards always seem to involve generation
  counters.

## What this unlocks

- Copy-paste between any two apps.
- Easy URL transport into the browser address bar.
