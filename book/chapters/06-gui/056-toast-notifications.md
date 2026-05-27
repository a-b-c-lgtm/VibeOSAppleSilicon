# Chapter 56 — Toast notifications and proper child reaping

A real desktop tells you when something happened.  This chapter
adds `/bin/notify`: a tiny userspace program any application can
spawn with a one-line message that pops up a toast in the top-right
corner and auto-dismisses after three seconds.  Along the way we
fix a kernel bug that would have leaked a thread struct, a stack,
and a whole address space every time anyone called the toast.

## The userspace half

`/bin/notify "<message>"` is ~140 LOC.  It:

1. Joins `argv[1..]` into a single message string (so both
   `notify Hello world!` and `notify "Hello world!"` work).
2. Creates a `360×80` window at `(FB_W - 360 - 16, 16)` with both
   `GUI_WIN_FLAG_NO_DECORATION` and `GUI_WIN_FLAG_ALWAYS_ON_TOP`
   — borderless, pinned above everything, doesn't steal focus.
3. Paints the toast: a 1-px border, a 4-px-wide accent bar on the
   left edge (a familiar visual signature for "this is a
   notification, not a window you can do anything with"), a
   "Notice" title row, and the message body.
4. Polls `gui_poll_event` and `uptime_ms` for `DISMISS_MS = 3000`.
   Any KEY, MOUSE_DOWN, or CLOSE event dismisses the toast early.
5. Calls `gui_destroy_window` and exits.

The whole thing is fire-and-forget: the caller just does
`spawn("/bin/notify", "saved!")` and never `wait`s.  That ergonomic
freedom is what unlocks the kernel bug.

## The kernel bug

Before this chapter, the only way a child thread's resources got
freed was through `wait()`:

```c
int thread_wait(int *code_out)
{
    /* ... scan for an EXITED child of caller ... */
    if (exited) {
        all_remove(exited);
        if (exited->stack_base) kfree(exited->stack_base);
        if (exited->as) address_space_destroy(exited->as);
        kfree(exited);
        ...
    }
}
```

If the parent never called `wait()`, those children stuck around
as zombies.  In a long-running editor that calls `notify("saved!")`
on every Ctrl-S, that's an address-space leak per save —
catastrophic over time.

Worse: when the *parent* eventually exits, its EXITED children
are still in `g_all_head` with `parent_id` pointing at a freed
thread.  No code path ever reaps them.  Memory leak forever.

## The fix: reap-and-orphan at parent exit

`thread_exit()` now sweeps its children before yielding away:

```c
{
    int my_id = g_current->id;
    struct thread *t = g_all_head;
    while (t) {
        struct thread *next = t->all_next;
        if (t->parent_id == my_id) {
            if (t->state == THREAD_EXITED) {
                /* Already-dead child the parent never wait()ed for.
                 * Reap it ourselves before we go. */
                all_remove(t);
                if (t->stack_base) kfree(t->stack_base);
                if (t->as) address_space_destroy(t->as);
                kfree(t);
                g_thread_count--;
            } else {
                /* Living child — orphan it.  Its own thread_exit
                 * will then take the no-parent path and self-
                 * remove. */
                t->parent_id = -1;
            }
        }
        t = next;
    }
}
```

Two safety checks worth highlighting:

1. **`next = t->all_next` BEFORE the conditional remove.**
   `all_remove(t)` mutates `t->all_next`; reading it after
   the call would walk a different list.

2. **`if (t->stack_base) kfree(t->stack_base);`**
   A child that already exited had its stack queued to
   `g_stack_to_free`, which a context switch later set
   `stack_base = NULL` after the actual `kfree`.  So this
   `kfree(NULL)` is a no-op and we don't double-free.  The
   `address_space_destroy` and the struct `kfree` are still
   needed — `wait()` was the only thing that ever did those.

## Two reasons this matters even without notifications

The same bug fires for any background process the shell starts
with `&` and the user never `wait`s for via a foreground command.
The `init.c` reap loop catches those because init explicitly
loops, but anything spawned from a *non-init* parent that exits
without waiting was leaking.

## The smoke test

[`scripts/test_notify.py`](../../../scripts/test_notify.py) boots,
types `/bin/notify Hello world!` at the shell, screendumps, then:

1. Asserts the toast body BG `(32, 40, 64)` is at
   `(WIN_X + 200, WIN_Y + 30)` ± 4.  (The wallpaper near the top
   is `(24, 32, 64)` — within ±10 — so we deliberately tighten
   the tolerance to avoid a false positive.)
2. Asserts the unmistakable bright accent bar `(64, 128, 255)`
   ± 15 is at `(WIN_X + 2, WIN_Y + WIN_H/2)`.  This colour
   appears nowhere else on the desktop.
3. Asserts the border `(128, 160, 224)` ± 18 is at the top edge.
4. Sleeps 3.5 s and snapshots again.  Asserts the accent-bar
   pixel is *no longer* `(64, 128, 255)` — the toast has
   dismissed and the wallpaper has returned.

```
$ python3 scripts/test_notify.py
PASS: shell prompt reached
PASS: toast body painted (pixel = (32, 40, 64))
PASS: accent bar painted (pixel = (64, 128, 255))
PASS: border painted (pixel = (128, 160, 224))
PASS: toast auto-dismissed (accent now = (23, 31, 62))

ALL TESTS PASSED
```

## Wiring notepad's Ctrl-S

The editor's save handler now spawns the toast on a successful
save:

```c
if (c == CTRL_S) {
    int rc = save_file(g_path);
    if (rc < 0) {
        set_status("save failed", 2);
    } else {
        set_status("saved.", 2);
        spawn("/bin/notify", "saved!");
    }
    render();
    break;
}
```

No `wait()`.  The kernel reaps for us when notify exits or when
notepad itself exits.

## Lessons

* **Resource ownership across processes is a kernel problem.**
  Userspace can't be required to call `wait()` for fire-and-forget
  children; the kernel must clean up at parent-exit time.
* **Pin the colour palette.**  Choose at least one toast colour
  no other surface uses (here, the cyan accent bar at
  `(64, 128, 255)`) so an automated test can detect it
  unambiguously.  We hit this in the test directly: BG `(32, 40,
  64)` is too close to wallpaper `(24, 32, 64)`.
* **Two-phase iteration when mutating a linked list.**  Cache
  `next = t->all_next` *before* the conditional `all_remove(t)`,
  because the helper sets `t->all_next = NULL` and the loop would
  otherwise terminate one iteration too early.
