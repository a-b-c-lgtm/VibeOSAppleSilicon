# Chapter 56 — A clock in the taskbar

Milestone 47 gave us a taskbar that lists windows.  The bar's right
corner was empty real estate — exactly where every other desktop
puts a clock.  This chapter adds one: a 1-pixel-bordered cell that
displays `HH:MM:SS` from kernel uptime, ticking once per second.

## Geometry

The clock is anchored to the right edge so it doesn't move when the
taskbar grows more cells:

```
#define CLOCK_PAD 8
#define CLOCK_W   80                      /* enough for "HH:MM:SS"
                                           * = 8 glyphs * 8 px = 64 +
                                           * 8 px of side padding. */
#define CLOCK_X   (FB_W - CLOCK_W - CLOCK_PAD)   /* 1192 */
#define CLOCK_Y   4
#define CLOCK_H   (BAR_H - 8)             /* 20 */
```

Two colours, distinct from the cell palette so the clock doesn't
look like a fourth window button:

```
#define CLOCK_BG_BGRA  GUI_BGRA(0x10, 0x14, 0x24)   /* darker than the
                                                     * bar BG */
#define CLOCK_FG_BGRA  GUI_BGRA(0xC0, 0xE0, 0xFF)   /* near-cyan */
```

## Formatting

Pure integer arithmetic — no `printf`, no division-by-ten loops,
just modular slices of seconds-since-boot:

```c
static void format_clock(unsigned long secs_total, char out[9])
{
    unsigned long h = (secs_total / 3600) % 100;
    unsigned long m = (secs_total / 60)   % 60;
    unsigned long s =  secs_total         % 60;
    out[0] = '0' + (char)(h / 10);
    out[1] = '0' + (char)(h % 10);
    out[2] = ':';
    out[3] = '0' + (char)(m / 10);
    out[4] = '0' + (char)(m % 10);
    out[5] = ':';
    out[6] = '0' + (char)(s / 10);
    out[7] = '0' + (char)(s % 10);
    out[8] = '\0';
}
```

The `% 100` on hours is a deliberate cap: at 100 h the readout
wraps to 00, but no glyphs ever overflow the 80-px cell.  A real
desktop would gate this on a wall-clock RTC; we're shooting for
"obvious that the system is alive and progressing", and uptime is
a perfect indicator of that.

## Drawing

`draw_clock()` repaints the body, paints a 1-px border (same colour
as the cell border), and centres the eight glyphs.  Since
`gui_draw_text` paints a background fill on each glyph, the clock
text never bleeds over neighbouring digits when seconds tick:

```c
static void draw_clock(void)
{
    unsigned long secs = uptime_ms() / 1000ul;
    char buf[9];
    format_clock(secs, buf);

    gui_fill_rect(g_self_id, CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H,
                  CLOCK_BG_BGRA);
    /* 1-px border on all four sides */
    gui_fill_rect(g_self_id, CLOCK_X, CLOCK_Y,
                  CLOCK_W, 1, CELL_BORDER);
    gui_fill_rect(g_self_id, CLOCK_X, CLOCK_Y + CLOCK_H - 1,
                  CLOCK_W, 1, CELL_BORDER);
    gui_fill_rect(g_self_id, CLOCK_X, CLOCK_Y,
                  1, CLOCK_H, CELL_BORDER);
    gui_fill_rect(g_self_id, CLOCK_X + CLOCK_W - 1, CLOCK_Y,
                  1, CLOCK_H, CELL_BORDER);

    int tx = CLOCK_X + (CLOCK_W - 8 * GLYPH_W) / 2;
    int ty = CLOCK_Y + (CLOCK_H - GLYPH_H) / 2;
    gui_draw_text(g_self_id, tx, ty, buf,
                  CLOCK_FG_BGRA, CLOCK_BG_BGRA, 0);

    g_last_clock_sec = (int)secs;
}
```

(Centring by `8 * GLYPH_W` assumes the monospace bitmap font.
When chapter 102 swapped in TrueType the glyph advance became
proportional, and this line now reads
`CLOCK_X + (CLOCK_W - gui_measure_text(buf)) / 2`. Same
intent, asking the kernel for the actual pixel width instead
of multiplying.)

## Tick logic

The taskbar's main loop already wakes every 150 ms to poll
`gui_list_windows`.  The clock taps that same wakeup:

```c
unsigned long secs = uptime_ms() / 1000ul;
if (redraw || (int)secs != g_last_clock_sec) {
    draw_clock();
    gui_flush(g_self_id);
}
```

Two reasons it tries to redraw:

1. **The second has changed** — the natural one-Hertz tick.
2. **A full taskbar redraw just happened** — `render()` repaints
   the whole bar, including the clock area.  If we don't repaint
   the clock right after a window-list change, the bar would show
   garbage where the clock used to be until the next second.

`g_last_clock_sec` starts at `-1` and is updated inside
`draw_clock()`, so the very first iteration always paints (because
no clock value can equal -1).

## Smoke test

[`scripts/test_clock.py`](../../../scripts/test_clock.py) boots
fully headless (no shell input), waits for the prompt, then:

1. Snapshots the framebuffer.
2. Asserts the clock background pixel at the top-centre of the
   clock cell equals `(16, 20, 36)` ± 10.
3. Walks the digit row left-to-right and asserts at least one
   pixel is the foreground colour `(192, 224, 255)` ± 15 — proof
   that glyphs were painted.
4. Sleeps 1.4 s and snapshots again.
5. Compares the digit-row strip byte-for-byte across snapshots
   and asserts at least one byte differs — proof that the clock
   actually ticked.

```
$ python3 scripts/test_clock.py
PASS: shell prompt reached
PASS: clock BG painted (pixel = (16, 20, 36))
PASS: clock digit pixels present (fg=(192, 224, 255))
PASS: clock ticked between snapshots (12 bytes differ in the digit strip)

MILESTONE 48: ALL TESTS PASSED
```

The test deliberately doesn't check the *value* of the digits.  At
boot, depending on how long QEMU spent in the early kernel, the
display can be `00:00:01` or `00:00:03`; what matters is that
*something* is there and it changes.

## What the desktop looks like now

Three milestones into the desktop work, the user-visible
experience is:

* boot to a wallpapered desktop;
* a launcher in the top-left lets you start `gui_term`, `paint`,
  or `notepad`;
* a taskbar across the bottom shows every visible non-pinned
  window, with the focused one highlighted;
* the clock in the bottom-right corner shows uptime ticking up
  in `HH:MM:SS`.

That's a *desktop*.  No prompt, no `init=` magic, no manual
`gui_term &`.  It just boots into something you can use.
