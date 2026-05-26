# Chapter 94 — The browser parser thread: HTML/CSS/layout off the GUI core

> **Milestone in this chapter:** move the browser's parse-and-
> layout pipeline to a worker thread pinned to CPU 1, so the
> GUI event loop on CPU 0 stays responsive during a slow parse.
> **Code referenced:**
> - [userspace/browser/](../../../userspace/browser/) (the
>   parser-thread refactor)
> - [userspace/libc/thread.h](../../../userspace/libc/thread.h)
>
> **At the end of this chapter** you will have a browser whose
> drag-to-resize on a Hacker-News-sized page no longer freezes
> the window for the duration of relayout. Builds on chapter
> 93 (shared fd tables) and chapter 92 (`CLONE_CPU`).

The chapter-93 work bought us a way to share open file descriptors
between two threads in the same address space. Chapter 94 cashes
that in: the browser's parse-and-layout pipeline moves to a
parser thread pinned to CPU 1, and the GUI event loop on CPU 0
keeps polling for events at full speed while the parser works.

The user-visible win is the resize handler. Pre-chapter-94, drag-
to-resize on a Hacker-News-sized page froze the window for the
duration of the relayout (`layout_build_and_run` for HN takes
about 200 ms). The window manager couldn't redraw its borders;
the WM-side cursor moved but the browser's content was a
stuck rectangle. Chapter 94 makes the GUI loop spin through
hundreds of `gui_poll_event` ticks during the same 200 ms
window — toolbar hover state updates, scroll wheel events get
absorbed, and the close button still works mid-resize.

The structural payoff is bigger than the visible one. With
parse-off-the-GUI-core, every future browser feature that
doesn't need to be on the rendering hot path (image decode,
incremental DOM mutation from parsed HTML, JavaScript when we
ever ship it) can live on the parser side without any new
plumbing. The parser thread is the spine that everything in
parts XIII–XV will hang off.

The chapter ships with `userspace/browser/browser.c` parser-
thread machinery, a `--bench-resize` mode that exercises it
headlessly, and `scripts/test_browser_parser_thread.py` that
asserts `gui_iters > 0` (the GUI loop ran while the parser
worked) as the chapter-94 invariant in machine-checkable form.

## What this chapter adds

* **`struct parser_state`** in `userspace/browser/browser.c`.
  All the parser-thread state in one place: the request slot
  (`req_dom / req_css / req_viewport`), the result slot
  (`new_ldoc / new_pb`), the seq counters that drive
  request/done coalescing, a `mutex_t` over the slots, a
  `shutdown` flag, the parser's `tid`, and stat counters.
* **`parser_thread_main(void *arg)`** — the worker loop. Sleeps
  on `futex_wait(&req_seq)` when caught up; wakes, snapshots
  the request under the mutex, runs `layout_build_and_run` +
  `layout_paint_collect` into LOCAL structs, then publishes
  the result + bumps `done_seq` and wakes the GUI. Exits via
  `exit(0)` (NOT plain `return`; see floor caveat below).
* **`parser_init / parser_spawn / parser_request_relayout /
  parser_absorb_completion / parser_wait_idle /
  parser_shutdown`** — six small helpers wrapping the
  protocol so the GUI loop has a friendly interface.
* **GUI event-loop integration** — `parser_absorb_completion`
  is called once per event-loop iteration before the dirty
  check, so a freshly-published result swaps in within one
  frame. `GUI_EVENT_RESIZE` posts to the parser instead of
  blocking. Every `free_page` callsite (close, navigate_to,
  navigate_history, reload, ESC/q quit) drains the parser
  via `parser_wait_idle` first so the parser never holds a
  pointer through the free.
* **`browser --bench-resize <new_w>`** — a headless mode that
  loads the page, spawns the parser, posts one relayout
  request, and counts GUI-loop iterations until completion.
  Prints `BENCH parse_ms=N gui_iters=N work_done=N`. Used by
  the smoke test as the regression assertion target.
* **`scripts/test_browser_parser_thread.py`** — boots `-smp 2`,
  runs `browser --bench-resize 900 /mnt/test_layout.html 600`,
  asserts `gui_iters > 0`, `work_done == 1`, and that the
  document width changed (`new_doc_w != old_doc_w`).
* **Userspace heap lock** in `userspace/libc/malloc.h` — the
  parser thread allocates concurrently with the GUI thread,
  so the global free-list needs serialisation. Added a tiny
  spinlock with a `yield` back-off (NOT `wfe` — see floor).

## Prerequisites

* **Chapter 87** — atomics. The seq counters use
  `atomic_load32_u` / `atomic_add_return32_u` from
  `userspace/libc/thread.h`. The malloc lock uses raw
  LDAXR / STXR.
* **Chapter 91** — userspace threads, futexes, mutexes.
  `parser_state` is a textbook futex / mutex producer–consumer
  pair: GUI is producer (writes `req_*`), parser is consumer
  (reads `req_*`, writes `new_*`); roles flip on the result
  slot. Each side waits on the OTHER's seq word.
* **Chapter 92** — CPU pinning. The parser is spawned on
  CPU 1 specifically, so a hot resize loop on CPU 0 doesn't
  starve it.
* **Chapter 93** — CLONE_FILES. The parser is spawned via
  `thread_spawn_files`. Today it only reads the page's
  in-memory dom and runs CPU-bound layout, so it doesn't
  actually touch any fd; we still pass `CLONE_FILES` for
  three reasons: (a) future "fetch the next stylesheet"
  work will need the GUI's TCP sockets, (b) shared fds means
  exit() / close() on either side is coherent, and (c) it's
  the right semantic for a worker that's part of the same
  POSIX-style "process".

## The producer–consumer protocol

The parser thread runs an unbounded loop with one of three
states at any given moment:

1. **Sleeping**: `req_seq == last_seq` and `shutdown == 0`.
   Blocks in `futex_wait(&req_seq, last_seq)`. The GUI thread
   wakes it by bumping `req_seq` and calling
   `futex_wake(&req_seq, 1)`.

2. **Working**: parser is inside `layout_build_and_run` /
   `layout_paint_collect`, building local `layout_doc` and
   `layout_paint_buf` structs from the snapshotted dom + css.

3. **Publishing**: parser holds the mutex, swaps the local
   structs into `new_ldoc / new_pb`, sets `new_built = 1`,
   bumps `done_seq`, releases the mutex, and calls
   `futex_wake(&done_seq, 1)`.

The GUI thread's contract:

* `parser_absorb_completion(ps, page)` is called every event-
  loop iteration. Lockless quick-check (`done_seq ==
  consumed_seq` ⇒ nothing to do) so the common-case cost is
  one acquire-load. If a result is ready, takes the mutex,
  pulls `new_ldoc / new_pb` out, frees the page's old
  ldoc/pb, installs the new ones, releases.
* `parser_request_relayout(ps, page, viewport)` writes the
  request slot, bumps `req_seq`, wakes the parser. Non-
  blocking — returns immediately. The GUI keeps rendering
  the OLD ldoc/pb until the result swaps in.
* `parser_wait_idle(ps, page)` is called before any operation
  that mutates or frees `page->dom` or `page->author_css`
  (navigation, reload, exit). Blocks via `futex_wait(&done_seq)`
  until `req_seq == done_seq`, then absorbs any pending
  result.

The two seq counters are deliberately separate. `req_seq` is
the GUI → parser direction; `done_seq` is parser → GUI. When
the parser is mid-work, `req_seq > done_seq`. After the parser
publishes, `req_seq == done_seq`. After the GUI absorbs,
`consumed_seq == done_seq` (the parser doesn't see this
counter — it's GUI-thread-private).

### Coalescing

Drag-to-resize fires dozens of resize events per second. We
don't queue them — that would either need a real queue
(complexity) or we'd backlog into a 30-frame parse pipeline
(latency). Instead we OVERWRITE: each `parser_request_relayout`
clobbers `req_*` and bumps `req_seq`. By the time the parser
finishes its current pass, `req_seq` is far ahead of
`last_seq`, the loop predicate `req_seq != last_seq` is true,
and the parser immediately starts another pass on the latest
viewport. The user sees one extra frame of staleness during
the drag and the FINAL viewport after they let go — which is
all that actually matters for "I'm dragging the corner".

The "coalescing wins, queueing loses" pattern is a recurring
one in event-driven systems. macOS coalesces window-server
display refresh requests; Linux coalesces page-cache writeback;
React's setState batches synchronously within an event handler.
The shared insight: when only the LATEST value matters, queue
depth is pure latency tax.

### What the lock window covers

The mutex protects EXACTLY the request and response payload
fields. The work itself runs OUTSIDE the lock — that's the
whole point. If we held the lock through `layout_build_and_run`
the GUI's `parser_request_relayout` would block on the mutex
for the duration of the parse, defeating the parallelism we
came for.

The lock window is on the order of a dozen scalar field reads
and a couple of struct-pointer copies. Both sides traverse
the lock in microseconds. Contention is bounded by GUI-loop
poll rate and parser publish rate, both ~100 Hz; collisions
are rare.

## Wiring into the GUI loop

The GUI loop's pre-chapter-94 shape:

```c
for (;;) {
    if (s.dirty) { render(...); gui_flush(...); s.dirty = 0; }
    struct gui_event ev;
    if (!gui_poll_event(&ev)) { yield(); continue; }
    switch (ev.type) {
    case GUI_EVENT_RESIZE: {
        relayout_page(s.page, new_w);    /* SYNCHRONOUS */
        s.dirty = 1;
        break;
    }
    /* ... other event handlers ... */
    }
}
```

After chapter 94:

```c
for (;;) {
    /* Drain any completed parser work. */
    if (s.parser && parser_absorb_completion(s.parser, s.page)) {
        br_recompute_scroll(&s);
        s.dirty = 1;
    }
    if (s.dirty) { render(...); gui_flush(...); s.dirty = 0; }
    struct gui_event ev;
    if (!gui_poll_event(&ev)) {
        /* Stat: count GUI iters that ran while parser was busy. */
        if (s.parser &&
            atomic_load32_u(&s.parser->req_seq) !=
            atomic_load32_u(&s.parser->done_seq))
            atomic_add_return32_u(
                &s.parser->gui_iters_during_work, 1);
        yield(); continue;
    }
    switch (ev.type) {
    case GUI_EVENT_RESIZE: {
        s.win_w = new_w; s.win_h = new_h; s.viewport_w = new_w;
        br_recompute_scroll(&s);
        s.dirty = 1;
        if (s.parser)
            parser_request_relayout(s.parser, s.page, new_w);
        else
            relayout_page(s.page, new_w);   /* fallback */
        break;
    }
    /* ... navigation cases now call parser_wait_idle before
     *     any free_page / load_page that mutates the dom ... */
    }
}
```

Three things to notice:

* **Window geometry updates immediately.** The new
  `(win_w, win_h)` takes effect before the relayout starts.
  The renderer keeps using the OLD `ldoc/pb` to draw, which
  was laid out for the OLD viewport — so the page LOOKS
  the same shape during the drag, but cropped to the new
  window outline. The user sees the canvas size change
  smoothly, then the content rearranges in one snap when
  the parser publishes.

* **Renderer never blocks on the parser.** It reads the
  page's `ldoc/pb` directly. As long as we only swap those
  pointers atomically (the absorb step, under the mutex),
  the renderer never sees a half-built layout.

* **Synchronous fallback.** If `parser_spawn` failed (chapter
  93 init OOM, etc.) `s.parser == NULL` and we fall through
  to the old synchronous path. The browser still works,
  just with the old freeze-during-resize behaviour.

## The five free_page callsites

The parser's contract: while a request is in flight (`req_seq
> done_seq`), the parser holds READ-ONLY pointers into
`page->dom` and `page->author_css`. If the GUI thread frees
the page or replaces `page->dom` during this window, the
parser will dereference freed memory.

The fix is `parser_wait_idle(ps, page)` immediately before
every operation that frees or replaces the dom. There are
exactly five callsites in the browser:

1. **`navigate_to`** (toolbar URL bar Enter, link click) —
   calls `free_page(s->page)` and replaces with the new page.
2. **`navigate_history`** (back/forward toolbar) — same.
3. **Toolbar reload button** — calls `free_page(s.page)` and
   replaces.
4. **`GUI_EVENT_CLOSE`** — calls `parser_wait_idle` then
   `parser_shutdown` then `free_page` then returns 0.
5. **ESC/q in keyboard handler** — same shutdown sequence
   as `GUI_EVENT_CLOSE`.

Resize is NOT in this list — it neither frees the page nor
replaces the dom; only the layout outputs (`ldoc / pb`) are
swapped, and that swap is the parser's job, not the GUI's.

## SYS_CLONE3 with CLONE_FILES

The parser is spawned via:

```c
int tid = thread_spawn_files(parser_thread_main, ps, /*cpu=*/1);
```

Which underneath builds:

```c
struct clone_args a;
a.flags     = CLONE_FILES;     /* chapter 93 */
a.entry     = (uint64_t)parser_thread_main;
a.arg       = (uint64_t)ps;
a.stack_top = (uint64_t)mmap-d 64 KiB stack;
a.tls       = 0;
a.cpu_id    = 1;               /* chapter 92 */
a._pad      = 0;
clone3(&a);
```

The `cpu_id = 1` keeps the parser entirely off CPU 0 so the
GUI loop is never preempted by parser work. Linux desktop
browsers do the same thing for their main / compositor /
worker threads — Chrome's "main thread" is on a specific
core; "compositor" is on another; "raster workers" pool
across the rest.

The `CLONE_FILES` bit is not strictly necessary today (parse
+ layout are pure CPU work), but pre-baking it in means
chapter 95-and-beyond can have the parser thread do incremental
fetches from the same TCP sockets the GUI core opened, with
no API change.

## The userspace heap lock

This is where the chapter went sideways the first time.

Before chapter 94 the userspace was strictly single-threaded
(per address space). `userspace/libc/malloc.h` has a free
list with no synchronisation, which was correct: the only
other "user" of the heap was a fork-spawned child, which
got its own COW copy and never raced.

With a parser thread sharing the address space and aggressively
calling `malloc` (every layout box, every CSS rule, every
glyph run gets a fresh allocation — `layout_build_and_run`
on HN does ~3000 mallocs), the unsynchronised free list got
corrupted within seconds. The crash signature was telling:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x000000009200000d
        EC      = 0x0000000000000024
        FAR_EL1 = 0x0000000079646f6a   <- ASCII bytes "jody"
```

`FAR_EL1` was an ASCII string interpreted as a pointer. That's
the unmistakable smell of "the free-list `next` pointer was
overwritten with payload bytes" — the classic single-threaded-
malloc-meets-second-thread bug.

The fix is a tiny spinlock in `userspace/libc/malloc.h`:

```c
static inline volatile uint32_t *_ualloc_lock_ptr(void) {
    static volatile uint32_t g_lock = 0;
    return &g_lock;
}

static inline void _ualloc_lock_acquire(void) {
    volatile uint32_t *p = _ualloc_lock_ptr();
    uint32_t old, fail;
    for (;;) {
        __asm__ volatile(
            "1: ldaxr   %w0, [%2]            \n"
            "   cbnz    %w0, 2f              \n"
            "   stxr    %w1, %w3, [%2]       \n"
            "   cbnz    %w1, 1b              \n"
            "2:                              \n"
            : "=&r"(old), "=&r"(fail)
            : "r"(p), "r"((uint32_t)1)
            : "memory");
        if (old == 0) return;
        __asm__ volatile("yield" ::: "memory");  /* not WFE! */
    }
}

static inline void _ualloc_lock_release(void) {
    __asm__ volatile("stlr   wzr, [%0]" ::
                     "r"(_ualloc_lock_ptr()) : "memory");
}
```

Two design notes worth lingering on:

**No futex.** A futex-backed mutex would be the obvious choice
(we have one in `userspace/libc/thread.h`), but using it from
inside `malloc` is a layering violation — `mutex_lock` itself
calls `futex_wait`, which is a syscall, which on EFAULT-style
errors might want to call back into libc. Even without that
risk, the malloc critical section is a few hundred cycles of
linear free-list walk; spinning is cheaper than the syscall
round-trip would be. The chapter 91 mutex is right for the
parser-state lock (held microseconds, contention rare); a
raw spinlock is right for malloc (held nanoseconds, can be
contended hot in a parse storm).

**`yield` not `wfe`.** Our first cut used `wfe` (wait-for-
event), which lets the CPU drop into a low-power sleep until
a `sev` instruction wakes it — the textbook pattern for a
spinlock back-off. It crashed with:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x0000000007e00001
        EC      = 0x0000000000000001        <- WFE/WFI trap
```

`EC = 0x01` is "WFE/WFI trapped." Our kernel doesn't set
`SCTLR_EL1.nTWE`, so a WFE issued at EL0 traps to EL1 as a
synchronous exception. `yield` (which is `hint #1`) is the
correct equivalent that's safe at EL0 — same back-off intent,
no trap. (See repo memory `aarch64-el0-permissions.md` for
the broader pattern.) An alternative would have been to set
`SCTLR_EL1.nTWE = 1` in kernel boot, but that's a wider
behavioural change than chapter 94 wants to ship.

## The exit() trap

The parser's main loop is `static void parser_thread_main(void *)`.
On the shutdown path it falls off the end of the function:

```c
if (atomic_load32_u(&ps->shutdown))
    return;     /* WRONG */
```

The kernel's `user_clone_trampoline` zeroes every GPR before
`eret`, including `x30` (the link register). When the parser
function returns, `ret` branches to `x30 == 0`, and the next
instruction fetch faults at PC=0:

```
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x0000000002000000
        EC      = 0x0000000000000000  <- unknown reason / IF
        ELR_EL1 = 0x0000000000000000
```

The fix is a one-character change:

```c
if (atomic_load32_u(&ps->shutdown))
    exit(0);    /* RIGHT */
```

`exit()` is a syscall — it doesn't need a return path. The
contract is documented in `userspace/libc/thread.h` ("When
entry returns, the worker MUST call exit() — otherwise the
return into the trampoline goes nowhere") but it's the kind
of advice that's easy to skim past until the kernel reminds
you.

A more robust libc would put a `bl exit` thunk on top of
`x30` in `user_clone_trampoline` so this trap couldn't
happen — chapter 91-floor punted on it because there was
only ever one in-tree caller and the test was careful. With
chapter 94 there are two callers across the codebase, so
hardening the trampoline is worth a future polish.

## The headless benchmark

`browser --bench-resize <new_w>` runs the chapter-94 path
without any window manager involvement, so the test isn't
coupled to GUI input plumbing:

```c
static int run_bench_resize(const char *url, int initial_viewport,
                              int new_viewport)
{
    struct loaded_page *p = load_page(url, initial_viewport);
    struct parser_state *ps = malloc(sizeof(*ps));
    parser_init(ps);
    parser_spawn(ps, /*cpu_id=*/1);

    parser_request_relayout(ps, p, new_viewport);

    uint32_t gui_iters = 0;
    for (;;) {
        if (parser_absorb_completion(ps, p)) break;
        gui_iters++;
        yield();
    }
    printf("BENCH parse_ms=%lu gui_iters=%u work_done=%u\n", ...);
    parser_shutdown(ps);
    free(ps);
    free_page(p);
    return 0;
}
```

The smoke test asserts `gui_iters > 0`. Pre-chapter-94 the
relayout was synchronous, so `gui_iters` would never be
incremented at all (we'd never enter the spin loop). On the
SMP machine with the parser pinned to CPU 1 and the bench
on CPU 0 we observe `gui_iters` in the 150–200 range for a
600 → 900 viewport change on `/mnt/test_layout.html`. On a
real Hacker News page the count goes up by an order of
magnitude.

## Floor caveats

* **No incremental layout.** A relayout is a full rebuild of
  the box tree from the dom root. Real browsers cache layout
  per subtree and re-do only the parts touched by the resize
  / mutation. Box tree is small enough today that the
  rebuild is acceptable; we'll need incremental layout if we
  ever ship JavaScript that mutates the dom on a hot path.

* **No fetch on the parser thread.** The chapter-94 parser
  is purely CPU-bound. A real browser parser thread also
  fetches and parses external stylesheets, images, etc.
  We could add that today (we have CLONE_FILES) but the
  shape of the fetch APIs is currently blocking — if the
  parser thread blocks in `read(socket)`, the GUI thread is
  fine but the parser stops processing layout requests.
  Non-blocking sockets / async I/O is parked behind a
  later milestone.

* **One parser, one page.** Multiple-tab support would need
  one parser thread per page (or a thread pool with a work
  queue). Browser today only displays one page at a time so
  this isn't a constraint.

* **Lock-window inside `parser_absorb_completion` covers a
  struct copy.** `struct layout_doc` and `struct
  layout_paint_buf` are large (multi-KB). Copying them under
  the mutex is wasteful — a cleaner design would have the
  parser allocate them on the heap and the absorb step swap
  pointers. Today the structs are passed by value to keep
  the parser/GUI state machine readable; pointer-swap is
  a future polish.

* **No prioritisation between bench / GUI / parser.** All
  three threads run with the same scheduler weight. The
  parser pinned to CPU 1 means it doesn't STARVE the GUI,
  but it also means the bench-mode `for (;;) yield();`
  spin loop on CPU 0 burns 100% of CPU 0 — not a problem
  for a smoke test that runs once, would be a problem for
  a long-lived bench.

* **`malloc` lock holds across `sbrk`.** `_ualloc_grow`
  calls `sbrk` from inside the lock window. Any other
  thread calling `malloc` while we're growing the heap will
  spin for the duration of the syscall (a few microseconds).
  Acceptable in floor; a real allocator would drop the
  global lock around the syscall.

## What this unlocks

* **Image loading on the parser thread.** Future PNG / JPEG
  decode (chapter 12-system-services range) can run on the
  parser without a new threading scheffold. The parser
  already shares fds with the GUI; a `decode_image(fd)`
  call inherits that.

* **Incremental relayout under JavaScript.** Whenever we
  ship pocket-JS (chapter 15-browser-maturation), DOM
  mutations that change layout can post requests to the
  same parser-thread pipeline.

* **Text-shaping on a worker.** TrueType + sub-pixel
  rendering (chapter 12) is the same shape of CPU-bound
  work; goes on the parser thread the same way.

The single biggest payoff is the architectural one: any
future "do something CPU-expensive" pattern in the browser
now has a known home — `parser_request_*(ps, ...)`,
`parser_absorb_completion(ps, ...)`. We don't need to
re-evaluate "should this be on a thread" for every new
feature.

## Files added

* `scripts/test_browser_parser_thread.py`
* `book/chapters/11-smp-and-memory/94-browser-parser-thread.md`
  (this file)

## Files modified

* `userspace/browser/browser.c` — added `struct parser_state`,
  `parser_thread_main`, six lifecycle helpers, GUI-loop
  integration (event-loop drain, resize dispatcher, drain
  before every free_page), `--bench-resize` mode.
* `userspace/libc/malloc.h` — added `_ualloc_lock_acquire /
  release` spinlock and wrapped `malloc` / `free` critical
  sections.

## Build & test

```sh
make all
python3 scripts/test_browser_parser_thread.py    # ⇒ PASS

# Full sweep:
for f in scripts/test_*.py; do python3 "$f"; done
# 47/47 PASS (was 46 before; +test_browser_parser_thread)
```
