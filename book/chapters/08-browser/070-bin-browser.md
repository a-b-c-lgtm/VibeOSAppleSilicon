# Chapter 70 — `/bin/browser`

[Chapter 69](069-block-and-inline-layout.md) ended with `/bin/layout`
emitting a paint command stream — a back-to-front list of `RECT`,
`TEXT`, and `UNDERLINE` ops with absolute pixel coordinates. The
stream is *what* the page looks like; this chapter is about *who
draws it, where, and how the user interacts with it once it's on
the screen*.

`/bin/browser` is the capstone for Part VIII. It glues together
every prior piece of the system:

- `/bin/httpget` (chapters 63–65) — to fetch pages over our own TCP/IP
  stack and our own URL/HTTP parser, with one-hop redirects
  resolved against the origin.
- `htmltok` → `dom` → `cssparse` →
  `layout` — the rendering pipeline, run end-to-end on
  whatever HTML/CSS comes back.
- The window manager — to host the resulting
  pixels in a window the user can move, raise, minimize, and
  *resize*.

By the end of the chapter `/bin/browser` runs in four modes:

| mode | flag | who's it for? |
|------|------|---------------|
| `--paint` | dumps the paint stream verbatim | regression diff against layout fixtures |
| `--plain` (default) | renders to a plain ASCII grid | scripted tests, copy-paste of body text |
| `--ansi` | same grid, colour via ANSI SGR | terminals with truecolor support |
| `--gui` | renders to a resizable window via the WM | the actual user-facing browser |

All four share the same layout/paint output. The only difference
is what consumes the paint commands.

## A 60-second tour of `main`

Every mode follows the same five-step pipeline:

```
src ──▶ http or fs ──▶ html_buf ──▶ tokenize+dom ──▶ layout ──▶ paint stream
                                                                     │
                                                       ┌─────────────┼─────────────┐
                                                       ▼             ▼             ▼
                                                    plain         ANSI         render_gui_frame
                                                    grid          grid         (per WM event)
```

A few pieces are worth calling out before we look at the GUI mode.

### Source resolution

`browser` accepts either a path on the file system (the OSFS or
tmpfs) or an `http://` URL. URLs go through the same code paths
that `/bin/httpget` uses; the response body becomes `html_buf`,
the request URL becomes `origin` so relative links can be
resolved, and any `<link rel=stylesheet>` is fetched the same way.

### Author CSS collection

Before layout runs, `browser` walks the DOM, collects the text
of every `<style>` block plus every external stylesheet body,
and concatenates them into a single `author_css` buffer. The
matcher takes `(user_agent_css, author_css)` and walks
the DOM emitting matches; the layout engine turns those
into computed styles per box.

### Initial viewport

Each mode picks an initial viewport width:

- `--paint` and `--plain`: the integer the user passed on the
  command line (default 800).
- `--ansi`: same.
- `--gui`: the user's value, *but* the GUI will re-do layout
  whenever the user resizes the window (see below).

The output of the layout pass is two structures:

```c
struct layout_doc {
    int doc_width_px;     // tightest content width that fit at the requested viewport
    int doc_height_px;    // total content height after block + inline pass
    /* ...box tree... */
};

struct layout_paint_buf {
    struct layout_paint_cmd *cmds;
    int n;
    int cap;
};
```

The plain and ANSI grids are sized from `doc_width_px /
LAYOUT_BASE_GLYPH_W`. The GUI window is sized from
`doc_width_px` capped at a default, and learns to deal with the
fact that the user might subsequently shrink or grow it.
(`LAYOUT_BASE_GLYPH_W` is still 8 -- the legacy bitmap font's
cell width. After chapter 104 the GUI font is proportional, so
the grid columns no longer line up pixel-perfectly with rendered
text; the URL field and toolbar were updated to use
`gui_measure_text` for caret and label positioning instead.)

## GUI mode in detail

### `gui_create_window_ex` and `GUI_WIN_FLAG_RESIZABLE`

Up through the early WM chapters the WM created windows with two
behaviours: drag from the title bar, and click the `×` to close.
Later chapters added z-order and minimize. None of these supported a *user-
initiated* size change — windows were born at one size and stayed
there for life.

For `/bin/browser` we lift that restriction. The request goes
through a new opt-in flag:

```c
int win_id = gui_create_window_ex(
    win_w, win_h, title,
    GUI_WIN_FLAG_RESIZABLE,            // opt-in
    GUI_WIN_POS_AUTO, GUI_WIN_POS_AUTO);
```

Three things change in the WM when a window has that flag set:

1. **A grip is drawn** in the bottom-right corner — three
   diagonal white stripes inside a 14 px square. Other GUI apps
   that don't pass the flag (notepad, launcher, taskbar,
   gui_term, hellogui, the desktop) see no change.
2. **`classify_click` recognizes the grip** — clicking inside
   that square enters resize-drag mode just as clicking the
   title enters move-drag mode.
3. **`wm_pointer_move` resizes during drag** — the WM
   reallocates the window's pixel buffer to the new size, copies
   the overlap region from the old buffer (top-left anchored),
   fills any newly-exposed area with the default gray
   (`0x202020`), and pushes a `GUI_EVENT_RESIZE` event into the
   window's event ring.

The contract for the application side is documented in
[`kernel/core/wm.h`](../../../kernel/core/wm.h):

> The WM swaps the pixel buffer **first**, then delivers the
> event. By the time the app sees `GUI_EVENT_RESIZE`, the buffer
> is already at the new size — the app must redraw to fill it.

`GUI_EVENT_RESIZE` carries `(arg0=new_w, arg1=new_h)`. Resize
events are coalesced in the event ring exactly like `MOUSE_MOVE`:
if there's already an unread `GUI_EVENT_RESIZE` at the tail, the
new dimensions overwrite it. That's important — without
coalescing, a slow re-layout (1000+ paint commands) could fall
behind a fast mouse drag, and the window would visibly "chase"
the cursor for a while after the user lets go.

### What the browser does with a resize

The application-side handler is the heart of this chapter:

```c
case GUI_EVENT_RESIZE: {
    int new_w = (int)ev.arg0;
    int new_h = (int)ev.arg1;

    /* Tear down the old layout/paint buffer. */
    layout_paint_buf_destroy(&pb);
    layout_doc_destroy(&ldoc);

    /* Re-run layout at the new viewport width. */
    layout_build_and_run(&ldoc, dom_root_node,
                         author_css, author_css_len, new_w);
    layout_paint_collect(&ldoc, &pb);

    win_w     = new_w;
    win_h     = new_h;
    content_h = win_h - BR_GUI_STATUS_H;

    max_scroll_y = ldoc.doc_height_px - content_h;
    max_scroll_x = ldoc.doc_width_px  - win_w;
    if (max_scroll_y < 0) max_scroll_y = 0;
    if (max_scroll_x < 0) max_scroll_x = 0;

    if (scroll_y > max_scroll_y) scroll_y = max_scroll_y;
    if (scroll_x > max_scroll_x) scroll_x = max_scroll_x;
    dirty = 1;
    break;
}
```

Two things deserve a closer look.

**Why re-layout instead of re-rasterising the cached paints?**
The paint stream is positioned in *document* coordinates, not
viewport coordinates. A wider window doesn't just mean "draw the
same paints further apart"; it means "redo block formatting at a
wider viewport so that long paragraphs reflow into fewer lines."
Real browsers reflow on resize for the same reason. Our layout
is fast enough that a full rebuild on every resize tick is fine
(under 100 ms even on the largest pages we render).

**Why two scroll axes?** Most pages reflow to fit. Some don't —
a page like plaintextworld.com/block has three columns of fixed
320 px width, deliberately wider than a typical phone. When the
window is narrower than the page, `doc_width_px > win_w`, the
horizontal scrollbar wakes up, and the user can pan with
←/→ keys. When the window is wider, `doc_width_px == win_w`
exactly, `max_scroll_x == 0`, and the horizontal axis is locked.

### `render_gui_frame`

The renderer takes both axes into account. Each paint command's
document coordinates are translated by `-(scroll_x, scroll_y)`
and then by `+content_top` (to push it past the status bar). The
loop culls boxes that lie entirely outside the visible content
rectangle, and clips the rest to the window edges so the WM
never sees out-of-bounds coordinates.

Text is the only case that needs care: when a glyph straddles
the left edge of the window, we draw it at `x = 0` *and* discard
the glyphs that fell entirely off-screen. Otherwise a single
horizontal scroll step would visually duplicate the leftmost
visible character. The status bar — drawn last, on top of
everything — shows two percentages, e.g. `42%/0%` for "scrolled
to 42% horizontal, 0% vertical." Either axis that's not
scrollable shows `-`.

## What it took, and what it didn't

`/bin/browser` is one .c file, about 1100 lines including all
four output modes and the GUI event loop. It does not own any
parsing, layout, or networking code — every one of those is a
header-only library shared with its dedicated test driver
(`htmltok`, `htmldom`, `cssparse`, `layout`, `httpget`). The
browser is purely glue.

The kernel changes for resizable windows are:

- `kernel/core/wm.h` — three new `#define`s
  (`GUI_WIN_FLAG_RESIZABLE`, `GUI_EVENT_RESIZE`, `WM_GRIP_SIZE`)
  with documented contracts.
- `kernel/core/wm.c` — six new functions or branches:
  grip-paint in `blit_window`, grip-hit in `classify_click`,
  `resize_window_to` (the buffer swap), the resize branch in
  `wm_pointer_move`, the press/release plumbing in
  `wm_pointer_button`, and `ring_coalesce_resize`.
- `userspace/libc/syscall.h` — mirrored constants and one new
  flag in `gui_create_window_ex`.

That's the whole feature. Notepad, the launcher, the taskbar,
the GUI terminal, hellogui, and the desktop continue to ignore
the new flag and behave exactly as before. The WM's resize
machinery is only paid for by windows that ask for it, and the
only window in the system that asks for it today is
`/bin/browser`.

What's still missing on the browser side:

- **Image fetching.** `<img>` lays out at its `width`/`height`
  attributes (clamped to the parent's content width with the
  aspect ratio preserved) but the rectangle is filled with a
  placeholder colour, not actual decoded pixel data. This is
  one chapter of its own (a JPEG/PNG decoder library plus a sprite
  cache).
- **Forms.** The DOM knows about `<a href>` and `<input>`;
  layout knows about anchor underlines. Anchor *clicks* are
  wired up in the postscript below;
  forms still aren't.
- **TLS.** `httpget` deliberately speaks plain HTTP only. A host-
  side TLS terminator (stunnel, or a tiny Go program on the host)
  is the unofficial workaround. Native TLS in the kernel would
  be its own months-long project.

But even without those, the browser does the thing the title of
this part of the book promised: it loads HTML over our network
stack, renders it via our layout engine, and draws the result
inside a window the user can resize. End to end, on bare ARM64.

## Postscript: toolbar, history, and clickable links

The browser of this chapter could only display whatever
URL the user typed on the launcher's command line. Once the
window was open there was no way to follow a link, type a new
URL, or back-button out of a wrong turn — every navigation
meant closing the window and re-spawning the binary.

The postscript adds the missing UI without touching any of the
pipeline above the GUI mode:

- A 28-pixel toolbar across the top of the window, holding a
  back button, a forward button, a reload button, and a URL
  field that doubles as a status bar.
- A 32-entry history stack with the conventional behaviour:
  back/forward navigate within it; navigating to a new URL
  truncates the forward tail.
- Click-to-navigate via DOM hit-testing: any click on the
  page area looks up the deepest box containing the point,
  walks its DOM ancestors for the nearest `<a href=...>`,
  and follows it.

None of this required a new kernel syscall. Mouse moves and
button presses already arrive as `GUI_EVENT_MOUSE_MOVE` /
`GUI_EVENT_MOUSE_DOWN`; the browser's event loop just learned
to care about them.

### URL canonicalisation

The address bar accepts the shapes a real user actually types,
and `canonicalize_url` reduces them to one absolute URL the
fetcher can handle. The rules, in priority order:

1. Filesystem paths (`/mnt/...`, `/bin/...`) — pass through.
2. `http://...` — pass through.
3. `https://...` — strip the scheme and prepend the proxy
   prefix; the host-side `scripts/https_proxy.py` upgrades to
   TLS for us.
4. `//host/path` — protocol-relative, treat as the proxy form.
5. `/path` — root-relative against the current page's
   `scheme://host` (this is what the proxy's link-rewrites
   look like, e.g. `/news.ycombinator.com/item?id=...`).
6. Bare `host` or `host/path` — prepend the proxy prefix.

The proxy prefix is `http://10.0.2.2:8080/` by default and can
be overridden at startup with `BROWSER_PROXY=...`. The browser
itself never speaks TLS — same trade-off as before — but the
six-rule canonicaliser means the address bar feels normal even
though the network path under the hood always lands on plain
http through the proxy.

### Hit-testing without recursion

The naive implementation of "find the box at (x, y)" is a
recursive DFS, but Part V's user thread stack is small enough
that a deep document already pushed `layout_build_subtree` past
its limit (see chapter 26's postscript). The hit-tester walks
the box tree iteratively using the `parent`/`first_child`/
`next_sibling` pointers already on every box, so the worst-case
stack usage is one frame regardless of document depth. From the
resulting box we walk DOM `parent` pointers — also a flat loop
— looking for the nearest `<a>` ancestor with an `href`.

### What a navigation does

The heart of the new event loop is one function:

```c
static void navigate_to(struct browser_state *s,
                        const char *url, int push)
{
    char *abs = canonicalize_url(url, s->page ? s->page->url : NULL);
    struct loaded_page *next = load_page(abs, s->viewport_w);
    if (!next) { free(abs); return; }       /* keep old page */
    free_page(s->page);
    s->page = next;
    s->scroll_x = s->scroll_y = 0;
    if (push) hist_push(s, abs);
    free(abs);
    br_url_set(s, s->page->url);
    br_recompute_scroll(s);
    s->dirty = 1;
}
```

`load_page` runs the full pipeline (fetch + tokenize
+ DOM + CSS + layout + paint) and packages the result into a
`struct loaded_page` that owns its DOM, computed styles, paint
buffer, and origin URL. On any failure — fetch error, parse
OOM, layout OOM — it returns a synthetic page with an HTML
error message so the user always gets something viewable
rather than a hung window.

### Two wider lessons

Adding navigation surfaced two unrelated bugs that were latent
in lower layers, both documented in their own chapters'
postscripts:

- The first attempt to render a real-world page (HN's index)
  took 45 seconds because of the receive-window /
  persist-timer interaction — see [chapter 62](../07-networking/062-tcp-and-sockets.md)'s
  postscript.
- The first click into the comment thread segfaulted in
  `css_match_chain` because the user thread stack was 16 KiB and
  the document nests 50 levels deep — see [chapter 26](../05-devices/026-argc-argv.md)'s
  postscript.

Both fixes were two-line changes once the diagnosis was right.
The value of building the browser as a real consumer of the
lower layers is exactly that: it produces real-world workloads
that shake out the assumptions the test fixtures didn't.

