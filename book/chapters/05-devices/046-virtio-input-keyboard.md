# Chapter 46 — virtio-input: an evdev keyboard for the GUI

Chapter 45 gave us pixels.  This chapter gives us a keyboard.
By the end the same byte stream that fed the shell from the PL011
serial port (chapters 4 and 43) also flows through a
**virtio-input** device, so `sh`, the cooked line discipline, the
raw-TTY line editor, and the SIGINT plumbing of chapter 36 all
work unchanged whether you're typing into the host terminal or
into the QEMU Cocoa window.

We add three files:

- `kernel/device/virtio_input.{h,c}` — the device driver.
- `kernel/core/console_in.{h,c}` — a unified
  "any console source" `console_try_getc` that polls
  `virtio_input` first and `serial_try_getc` second.

Plus two small edits:

- `kernel/core/vfs.c` — both the raw and cooked read paths switch
  from `serial_try_getc` to `console_try_getc`.
- `kernel/core/main.c` — call `virtio_input_init()` right after
  `fb_init()`.

## Why polling, not interrupts

`virtio-input` produces an interrupt on its `eventq` for every
keystroke.  We do **not** wire one up.  The reason is design
symmetry: the existing `serial_try_getc` is already polled from
inside `vfs_read`, and the rest of the kernel assumes blocking
reads are implemented as a `yield()` loop around a non-blocking
`try_getc`.  Making the keyboard match means a single
`console_try_getc` that any caller can use, and zero IRQ-routing
work in this milestone.

A future improvement would be an SPI handler that just wakes a
condition variable; today, polling has zero observable cost
because the `try_getc` loop only runs when something already
called `read(stdin, ...)`.

## The driver in one screenful

```c
#define EVENT_QUEUE        0
#define QUEUE_SIZE         32
#define EVENT_SIZE         8

struct virtio_input_event {
    uint16_t type;     /* EV_KEY = 1, EV_SYN = 0, ... */
    uint16_t code;     /* keycode (Linux evdev) */
    uint32_t value;    /* 0=release, 1=press, 2=repeat */
};
```

We allocate one 4 KiB page and slice it like every other virtio
queue we've built:

| offset | content                              |
|--------|--------------------------------------|
| 0x000  | descriptor table (32 × 16 B)         |
| 0x200  | avail ring                           |
| 0x300  | used ring                            |
| 0x600  | 32 × 8 B event slots, one per descriptor |

At init we publish all 32 descriptors into the avail ring,
permanently bound to slots 0..31.  When the device fills slot N
and bumps `used.idx`, `virtio_input_poll` reads the slot, hands
the event to `handle_event`, then re-publishes that descriptor
back to the avail ring — the descriptor never moves.

```c
void virtio_input_poll(void)
{
    while (g_used_last != g_used->idx) {
        uint16_t pos     = g_used_last & (QUEUE_SIZE - 1);
        uint32_t desc_idx = g_used->ring[pos].id;
        uint32_t got_len  = g_used->ring[pos].len;
        if (desc_idx < QUEUE_SIZE &&
            got_len  >= sizeof(struct virtio_input_event)) {
            handle_event(event_slot(desc_idx));
        }
        re_publish(desc_idx);
        g_used_last++;
    }
}
```

## The Linux evdev → ASCII translation

`handle_event` filters on `EV_KEY` (type 1) and tracks key state
in two booleans:

```c
static int g_shift_down;
static int g_ctrl_down;
```

`translate_key(uint16_t code)` is a giant switch over Linux key
codes (`KEY_A` = 30, `KEY_ENTER` = 28, `KEY_LEFTSHIFT` = 42, …)
that pushes the right byte (or escape sequence) into a 128-byte
ring buffer.  Some highlights:

| key                     | byte / sequence                |
|-------------------------|--------------------------------|
| printable ASCII         | `'a'..'z'` / `'A'..'Z'` / etc. |
| `Enter`                 | `\r`                           |
| `Backspace`             | `0x7F`                         |
| `Esc`                   | `0x1B`                         |
| `Up` / `Down` / etc.    | `\e[A` / `\e[B` / `\e[C` / `\e[D` |
| `Ctrl-A` … `Ctrl-Z`     | `0x01` … `0x1A`                |

The `Ctrl-` lookup deliberately matches the values the cooked
line discipline already understands from serial input, so SIGINT
delivery on `Ctrl-C` (chapter 36) just works.

## Unified console input

`console_in.c` is the funnel:

```c
int console_try_getc(char *out)
{
    if (virtio_input_present()) {
        char c;
        if (virtio_input_try_getc(&c)) {
            if (wm_has_windows() && wm_keyboard_byte(c))
                return serial_try_getc(out);
            if (out) *out = c;
            return 1;
        }
    }
    return serial_try_getc(out);
}
```

There are three subtleties baked into eight lines:

1. **Always poll virtio-input first**, even if we don't end up
   returning a keyboard byte this tick.  Otherwise events
   accumulate forever in the device's used ring and the shift /
   ctrl state shadow falls out of sync.

2. **If a window has focus, the GUI session consumes the byte and
   stdin sees nothing.**  This is what makes the keyboard route
   exclusively to the focused GUI app in chapter 47.  The shell
   that spawned the app stays silent until all windows close.

3. **Otherwise the byte falls through to the cooked / raw line
   discipline.**  Same interface, same semantics, same line
   editor, same SIGINT.  No code in `vfs.c` cares whether the
   byte came from PL011 or from `virtio-input`.

## Smoke test

[scripts/test_virtio_input.py](../../../scripts/test_virtio_input.py)
boots QEMU `-display none` with both `pl011` serial (over a Unix
socket) and a `virtio-keyboard-device`.  It then drives the
keyboard via QMP:

```python
qmp_send({"execute": "input-send-event", "arguments": {"events": [
    {"type": "key", "data": {"down": True,
                              "key": {"type": "qcode", "data": "h"}}}]}})
```

The script types `hello\n` one qcode at a time and asserts that
the shell echoes the line and prints the prompt again on serial.

This pattern — Unix-socket serial + QMP `input-send-event` — is
the basis of every GUI smoke test from here on, including the
WM test in chapter 47.

## Three gotchas

1. **`memset` from struct initialisers** (again — see chapter 45).
   `virtio_input.c` has the same `req = { .field = ... }` pattern
   in the handshake; same one-line `memset` shim added here.
2. **Modifier keys never reach userspace.**  `KEY_LEFTSHIFT`,
   `KEY_LEFTCTRL`, etc. only update `g_shift_down` /
   `g_ctrl_down` — they don't push a byte.  This matches how a
   PS/2 keyboard would be filtered by a userspace evdev daemon
   on Linux.
3. **`KEY_VAL_REPEAT` (value=2) is treated as a press**, so
   holding a key autorepeats the byte.  This matches the QEMU
   behaviour for the Cocoa display and the readline UX users
   expect.

## Files changed

- `kernel/device/virtio_input.{h,c}` — new (~430 LOC).
- `kernel/core/console_in.{h,c}` — new.
- `kernel/core/vfs.c` — both the raw and the cooked read paths
  switched from `serial_try_getc` to `console_try_getc`.  No
  other behaviour change.
- `kernel/core/main.c` — call `virtio_input_init()` after
  `fb_init()` and log "ok (keyboard online)" or "none (serial-only
  input)".
- `Makefile` — `make run-graphical` now passes
  `-device virtio-keyboard-device`.

## What's deferred

- **virtio-tablet (mouse)** — same device id, separate instance
  on the bus; disambiguate by reading `VIRTIO_INPUT_CFG_ID_NAME`
  or by EV_ABS / EV_KEY capability bits.  Coming in a later chapter.
- **An IRQ-driven path** — see "Why polling, not interrupts"
  above.  Worth doing when we add USB or a busy-waiting GUI app.
- **A full keymap (US-only today)** — international layouts can
  be added by replacing the `translate_key` switch with a table
  indexed on shift state, or by routing raw scancodes to a
  userspace `xkbcommon` equivalent.
- **EV_REP autorepeat throttling** — we just emit whatever the
  device sends.  A fancier driver would coalesce.

The polling-from-`vfs_read` model is the part to keep; the rest
is straightforward extension.
