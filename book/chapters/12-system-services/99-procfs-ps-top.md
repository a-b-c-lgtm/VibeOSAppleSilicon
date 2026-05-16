# Chapter 99 — A /proc-shaped filesystem, ps, and top

**Status:** Stub. Tracking milestone 88.

The kernel knows things — runqueue length, per-thread state,
heap usage, virtio queue depth. None of it is observable
from userspace. This chapter exposes them through a tiny
`/proc/`-shaped pseudo-FS.

## What this chapter adds

- A new VFS mount point at `/proc/`, no on-disk backing.
- Files:
  - `/proc/uptime`         — replaces `SYS_UPTIME`-only path
  - `/proc/meminfo`        — pmem free/used
  - `/proc/<pid>/status`   — name, state, parent, RSS
  - `/proc/<pid>/cmdline`  — argv as the spawn'd it
  - `/proc/sched`          — per-CPU runqueue length
- `/bin/ps` — walks `/proc/`.
- `/bin/top` — refreshes once per second, picks heaviest by
  scheduler ticks.

## Prerequisites

- Chapter 16 — VFS

## Plan

- Pseudo-FS dispatch: `osfs2_lookup` first; if unmatched and
  path starts with `/proc/`, hand to `procfs_read`.
- Each file is "generated on read" — no caching.
- Permissions are a follow-up; everything is world-readable.

## What you'll learn

- Why /proc was a good idea and why /sys later was a better
  one (ours collapses both).
- The "everything is a file" trade-off: trivially scriptable,
  vaguely racy.

## What this unlocks

- Eyeball-friendly observability.
- The strace chapter (96) borrows the same reading
  infrastructure for trace output.
