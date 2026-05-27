# Chapter 97 — virtio-snd: a boot chime and a beep

> **Milestone in this chapter:** bring up the last virtio device
> on QEMU's `virt` bus — virtio-sound — and play a boot chime.
> **Code referenced:**
> - [kernel/device/virtio_snd.c](../../../kernel/device/virtio_snd.c)
> - [kernel/core/syscall.c](../../../kernel/core/syscall.c)
>   (`SYS_BEEP`)
> - [userspace/beep/](../../../userspace/beep/) (`/bin/beep`)
>
> **At the end of this chapter** you will have a
> minimum-viable virtio-snd driver, a `/bin/beep` tool, and a
> two-tone boot chime that plays once the desktop is up.

The kernel has talked to virtio-blk since chapter 10, virtio-gpu
since chapter 25, virtio-input since chapter 38, virtio-tablet
since chapter 40, and virtio-net since chapter 51. There is one
last virtio device on QEMU's `virt` bus that we have left
unspoken for: virtio-sound. Chapter 97 brings up a minimum-
viable virtio-snd driver, exposes a single new syscall
(`SYS_BEEP`), ships a `/bin/beep` userspace tool, and plays a
two-tone boot chime once the desktop is up.

The visible artefact is small — a 250 ms "ba-deep" when init
hands off to the shell, plus the ability to type `beep 880 100`
at any prompt and hear an A5 for one tenth of a second. The
invisible artefact is bigger: this is the first chapter where
the kernel actively *generates* an outgoing data stream rather
than merely shuffling bytes between two endpoints. The TX path
synthesises 16-bit signed PCM samples in the kernel's address
space, hands them to QEMU over a virtio descriptor chain, and
then *blocks the calling thread for the playback duration* by
waiting for the device to acknowledge the period. That blocking
behaviour is the architectural change worth understanding —
syscalls so far have either returned immediately
(`gettimeofday`, `getpid`) or returned when *the user* gave us
data to consume (`read` on a pipe). `SYS_BEEP` is the first
"wait until the world has caught up with what you asked it to
do" syscall in the system.

## What this chapter adds

* **`kernel/device/virtio_snd.h`** — the driver's API contract:
  `virtio_snd_init()` (probe + bring-up), `virtio_snd_present()`
  (yes/no), and `virtio_snd_play_square(freq_hz, duration_ms)`.

* **`kernel/device/virtio_snd.c`** — the full driver, ~520
  lines. Implements only two of the four virtqueues the spec
  defines (CONTROLQ and TXQ; we ignore EVENTQ and RXQ
  entirely), polls the used rings with `yield()` between
  iterations rather than wiring an IRQ, and keeps the PCM
  stream perpetually in the `START` state for the lifetime of
  the kernel. One stream, mono, S16, 44_100 Hz.

* **`kernel/core/main.c`** — one extra probe block after
  `virtio_net_init`, mirrored on the same boot sequence.

* **`kernel/core/syscall.[ch]`** — `SYS_BEEP = 79` with a
  documented two-argument shape `(uint32_t freq_hz,
  uint32_t duration_ms)`. A new `ENODEV = 19` errno is
  promoted out of `syscall.c`'s file-private `#define` block
  and into the public header so `/bin/beep` (and any future
  device-aware userland) can recognise it.

* **`userspace/libc/syscall.h`** — mirror enum entry and a
  `static inline int beep(unsigned int freq_hz, unsigned int
  duration_ms)` wrapper.

* **`/bin/beep`** — `userspace/beep/beep.c`. Tiny argv parser:
  `beep` defaults to 800 Hz / 200 ms, `beep <freq>` overrides
  the frequency, `beep <freq> <duration>` overrides both.
  Returns 1 with a one-line error if the kernel reports
  `-ENODEV`.

* **Two-tone boot chime in `userspace/init/init.c`** — after
  the desktop, taskbar and launcher have been spawned, init
  calls `beep(440, 100); beep(659, 150);` to play A4 → E5.
  These calls block init for ~250 ms before it spawns
  `/bin/sh`, which is intentional: the chime is the audible
  "kernel is up, GUI is up, type something" signal.

* **Makefile QEMU audio plumbing** — a new `QEMU_SND` block
  attaches `-device virtio-sound-device,audiodev=audio0` to
  the `run`, `run-graphical`, and `run-tcg` targets. The
  default backend is `none` (samples consumed silently);
  `run-graphical` overrides to `coreaudio` so the chime is
  audible on the host's speakers.

* **`scripts/test_beep.py`** — smoke test. Boots with
  `audiodev=none`, waits for the shell prompt, asserts the
  driver serviced the boot chime (the
  `[virtio-snd] played` line is in the pre-prompt log), then
  runs `beep 880 100` and asserts the user-triggered
  invocation also dispatches into the driver.

## Prerequisites

* **Chapter 10** — virtio-mmio bus probing. We reuse the same
  4 KiB scan over 32 slots starting at `0x0A000000` that
  virtio-blk taught us, just looking for device id 25 instead
  of 2.

* **Chapter 25** — virtio-gpu's CONTROLQ / RPC pattern. The
  CONTROLQ here has the same shape: write a request struct,
  publish the head descriptor index on the avail ring, ring
  the doorbell, wait for the device to write its response
  into the device-writable second descriptor in the chain.

* **Chapter 10 (again) / 17** — `pmem_alloc_page()` and
  `pmem_alloc_contig(N)`. virtio descriptors must point at
  physically contiguous memory; the 64 KiB TX slab needs
  `pmem_alloc_contig(16)`.

* **Chapter 55-style polling-with-yield** — `tx_submit_and_wait`
  and `ctrl_submit_and_wait` both call `yield()` between
  used-ring polls, the same idiom the early virtio-blk and
  malloc spinlock code use to keep the cooperative scheduler
  responsive.

## Why virtio-snd is different from every previous virtio device

The four drivers we already have all fit one of two shapes:

* **One queue, fire-and-forget pattern.** virtio-blk sends a
  request descriptor, the device fills in the response slot,
  done. virtio-net is the same: TX, RX, no protocol on top of
  the ring.

* **One queue, event push pattern.** virtio-input and
  virtio-tablet only consume the device's *eventq* — the host
  pushes an `input_event` whenever a key is pressed; the guest
  reads it. There is no control plane to speak of.

virtio-sound is different. The spec defines four virtqueues:

| ID | Direction      | Purpose                                  |
|----|----------------|------------------------------------------|
| 0  | guest → device | CONTROLQ — RPC for stream lifecycle      |
| 1  | device → guest | EVENTQ — async device notifications      |
| 2  | guest → device | TXQ — outbound PCM samples (playback)    |
| 3  | device → guest | RXQ — inbound PCM samples (capture)      |

A real driver wires up all four. We ship the absolute floor
that produces a beep:

* **CONTROLQ** is mandatory for any output. Before the device
  will accept a single TX descriptor, it has to receive (via
  the CONTROLQ) a `SET_PARAMS` describing the stream's audio
  format, a `PREPARE`, and a `START`. Skip any one of those
  and TX submissions are rejected with `VIRTIO_SND_S_BAD_MSG`.

* **TXQ** is what actually carries the audio. Each message is
  a 2-descriptor chain: a device-readable descriptor pointing
  at `[virtio_snd_pcm_xfer header][PCM samples]`, followed by
  a device-writable descriptor pointing at a
  `virtio_snd_pcm_status` slot.

* **EVENTQ** is the device's way to tell us "a jack was
  plugged in" or "your stream xran". Useful in a real OS, not
  for a chime; we never publish a single descriptor to it and
  the device tolerates that gracefully.

* **RXQ** is for *recording*. We don't capture audio anywhere;
  ignored.

The "perpetually-running stream" choice — `START` once at boot,
never `STOP`, never `RELEASE` — is a simplification. A general
audio system would `START` only when it has something to play,
because a started stream that runs out of TX descriptors logs
xruns and may underflow into a clicking sound depending on the
backend. We sidestep that because (a) `audiodev=none` doesn't
care, (b) `audiodev=coreaudio` simply outputs silence between
beeps. The TX path inside the device is "if the descriptor
queue has nothing to consume, output silence", which is exactly
what we want for a system that beeps occasionally.

## The shared 4 KiB ring page layout

Two virtqueues, both with `qsize = 8`, packed into one 4 KiB
allocation:

```
+0x000  CTRL desc table     (8 entries × 16 = 128 bytes)
+0x080  CTRL avail ring     (header 4 + 8 × 2 entries + tail 2 = 22, padded)
+0x0C0  CTRL used ring      (header 4 + 8 × 8 entries + tail 2 = 70, padded)
+0x140  TX   desc table     (128 bytes)
+0x1C0  TX   avail ring     (22 bytes, padded)
+0x200  TX   used ring      (70 bytes, padded)
+0x280  unused tail
```

The pad-to-16-byte alignment of every component is what makes
the offsets work; `vring_used_elem` is 8 bytes, so the used
ring's array is naturally aligned. The same trick keeps
virtio-blk and virtio-net's two queues each in a single page;
virtio-snd has the same number of queues and works the same
way.

The CTRL request and response buffers live on a *separate*
page (`g_snd_ctrl_page`), at fixed offsets `0x000` (request)
and `0x100` (response). This keeps the descriptor's `addr`
fields constant — slot 0 of the CTRL desc table always points
at `g_snd_ctrl_pa + 0x000`, slot 1 always at `g_snd_ctrl_pa +
0x100`, set once at chain-build time and never touched again.

## The TX slab: one 64 KiB allocation, three regions

The TX path needs three buffers per submission: the xfer
header, the PCM payload, and the status response. We allocate
*one* 16-page contiguous region and slice it three ways:

```
0x00000  virtio_snd_pcm_xfer  (4 bytes — just stream_id)
0x00004  PCM samples          (up to TX_DATA_MAX bytes)
                              (= TX_STATUS_OFF − sizeof(xfer))
0x0FFC0  virtio_snd_pcm_status (8 bytes; 64 bytes reserved)
```

Why 64 KiB? Two reasons collide nicely.

* It's the largest allocation the page allocator can hand us
  in a single contiguous chunk via `pmem_alloc_contig(16)`.
* At 44_100 Hz mono S16 it's 64 KiB / 88_200 B/s ≈ **742 ms**
  of audio. Every chime we ship today fits in one period; for
  longer notes the play loop fragments into period-sized
  submissions and the device stitches them back together.

We pick `period_bytes = TX_DATA_MAX` so one TX submission == one
period from the device's point of view. That gives us the
simplest possible "submit, wait for used, submit next"
synchronisation: the used ring fires exactly once per
submission, and we never need to track multiple outstanding
descriptors.

## The control-plane RPC

Every CONTROLQ request follows the same pattern:

```c
[ device-readable: request struct ]
[ device-writable: response struct (status word at minimum) ]
```

`ctrl_submit_and_wait` glues this into a 2-descriptor chain in
slots 0/1 of the CTRL desc table, publishes slot 0 on the
avail ring, kicks the doorbell, and yield-polls the used ring:

```c
struct vring_desc *d = ctrl_desc();
d[0].addr  = g_snd_ctrl_pa + CTRL_REQ_OFF;
d[0].len   = req_len;
d[0].flags = VRING_DESC_F_NEXT;
d[0].next  = 1;
d[1].addr  = g_snd_ctrl_pa + CTRL_RESP_OFF;
d[1].len   = resp_len;
d[1].flags = VRING_DESC_F_WRITE;
d[1].next  = 0;
/* ... publish & notify ... */
while (g_ctrl_used_seen == u->idx) {
    if (now > deadline) { serial_puts("[virtio-snd] CTRL timeout\n"); return 0; }
    yield();
    dmb();
}
```

The 500 ms timeout exists because QEMU's virtio-snd backend
takes a noticeable fraction of a second on the *first* call —
the coreaudio backend is allocating its host audio session
lazily. Subsequent calls return in microseconds. A non-zero
timeout (vs spin forever) is hugely valuable when bringing the
driver up: a misformed `SET_PARAMS` causes the device to
silently drop the request rather than respond with `BAD_MSG`,
which without a timeout would deadlock the kernel at boot.

The five high-level helpers (`SET_PARAMS`, `PREPARE`, `START`,
`STOP`, `RELEASE`) all call `ctrl_submit_and_wait` under the
hood. Only `SET_PARAMS` has an interesting payload; everything
else just sends `{code, stream_id}` and reads back a status
word.

## DRIVER_OK before the first CONTROLQ message

The bring-up sequence in `init_device()` is ten numbered
steps. Step 6 is the trap that cost a wasted afternoon:

```c
/* 6. DRIVER_OK — must come BEFORE we send any control
 *    messages.  The device rejects CTRL submissions otherwise. */
w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                        VIRTIO_STATUS_DRIVER |
                        VIRTIO_STATUS_FEATURES_OK |
                        VIRTIO_STATUS_DRIVER_OK);

/* 7. Configure the PCM stream once and leave it running. */
if (snd_pcm_set_params(...) < 0) return -1;
```

For virtio-blk and virtio-net the convention is "set up the
queues, then DRIVER_OK". For virtio-snd the spec requires
DRIVER_OK to be *latched* before the device will service any
control queue submission. If you set DRIVER_OK *after*
`SET_PARAMS`, the request gets ignored — no response written,
no status set, just silence — and the 500 ms CTRL timeout
fires. The fix is one line of reordering, but only because the
driver had timeouts in the first place. **Always wire timeouts
before you wire happy-path logic.**

## The TX path: square-wave synthesis

`virtio_snd_play_square(freq_hz, duration_ms)` does three
things:

1. **Clip inputs.** `freq_hz` to `[20, 22050]` (Nyquist for our
   sample rate), `duration_ms` to `[1, 5000]`. Done in the
   driver, not in the syscall handler — that way an out-of-
   range argument can never escape down to MMIO with whatever
   value userspace passed in.

2. **Synthesise period-by-period.** Compute total bytes,
   compute a half-period in samples, then loop:

   ```c
   uint32_t half_period = (PCM_RATE_HZ / freq_hz) / 2;
   if (half_period == 0) half_period = 1;
   const int16_t HI = +10000;
   const int16_t LO = -10000;
   uint32_t sq_phase = 0;
   while (bytes_total > 0) {
       uint32_t bytes_this = min(bytes_total, TX_DATA_MAX);
       int16_t *dst = (int16_t *)(g_snd_tx_slab + TX_DATA_OFF);
       for (uint32_t i = 0; i < bytes_this/2; i++) {
           dst[i] = (sq_phase < half_period) ? HI : LO;
           sq_phase++;
           if (sq_phase >= half_period * 2) sq_phase = 0;
       }
       if (tx_submit_and_wait(bytes_this) < 0) return -1;
       bytes_total -= bytes_this;
   }
   ```

   `sq_phase` is preserved across periods so the wave is
   continuous when a long beep fragments — no click at the
   period boundary. Amplitude is fixed at `±10000` rather than
   the S16 maximum `±32767`: that leaves ~10 dB of headroom so
   coreaudio's mixer doesn't clip when other host applications
   are also playing.

3. **Block.** `tx_submit_and_wait` waits up to 2 s for the
   device to consume the period (a 64 KiB period at 44_100 Hz
   plays for ~742 ms, so 2 s is comfortable slack). The wait is
   `yield()`-poll on the used ring, the same pattern as the
   CONTROLQ. The calling thread sleeps for approximately the
   playback duration, which is exactly what `beep(880, 100)`
   semantics want.

A real audio driver would queue multiple periods ahead of
time so the device never sees a starved TXQ — that's the
"double buffer" pattern from games. We don't, because the only
caller is `SYS_BEEP` and it actively *wants* to block until
playback is done; queuing periods would only matter if some
later thread started a second beep mid-flight, in which case
serialising on the TXQ is fine.

## SYS_BEEP and `/bin/beep`

The syscall is one of the smallest in the table:

```c
static long sys_beep(uintptr_t freq_hz, uintptr_t duration_ms)
{
    if (!virtio_snd_present())
        return -ENODEV;
    int rc = virtio_snd_play_square((uint32_t)freq_hz,
                                    (uint32_t)duration_ms);
    return (rc == 0) ? 0 : -EIO;
}
```

No `copy_from_user` because everything is by value. No range
checks because the driver already does them. Returns `-ENODEV`
if the kernel never found a virtio-sound device on its mmio
scan (i.e. QEMU was started without `-device
virtio-sound-device`); `/bin/beep` recognises that and prints
a one-line hint:

```
Mac:osdev$ beep 880 100
Mac:osdev$ beep
Mac:osdev$ beep 100
```

(If the device is missing:)

```
$ beep 880 100
beep: no virtio-sound device (re-run QEMU with -device virtio-sound-device)
```

The argv parser is small enough to inline:

```c
unsigned int freq_hz     = (argc >= 2) ? parse_uint(argv[1], 800) : 800;
unsigned int duration_ms = (argc >= 3) ? parse_uint(argv[2], 200) : 200;
```

The two-argument shape covers everything we want today. Adding
a third argument later (e.g. waveform = square / sine / saw)
is one syscall extension away; keeping `SYS_BEEP` to two args
matches every other "fire a quick action" syscall in the
table.

## The boot chime

`userspace/init/init.c` already had the spawn ordering:
desktop, taskbar, launcher, then sh. We slip the chime in
*after* launcher and *before* sh:

```c
puts("[init] launching /bin/launcher (background, GUI)");
int gtid = spawn("/bin/launcher", "");
/* ... */

/* Chapter 97 — boot chime. */
puts("[init] playing boot chime");
beep(440, 100);   /* A4 */
beep(659, 150);   /* E5 */

puts("[init] launching /bin/sh");
```

A4 at 440 Hz is the conventional concert pitch; E5 at 659 Hz
is a perfect fifth above and the most universally pleasant
two-note motif. Total duration is 250 ms, short enough to be a
status indicator rather than a fanfare.

Two beeps rather than one is a deliberate choice. A single
beep is hard to distinguish from an environmental click; two
notes in a clearly-rising interval are unambiguously the
machine talking to you. The same instinct shows up in every
desktop OS chime from Macintosh's `bong` to NT's startup
sound.

## QEMU plumbing: audiodev backends

QEMU's `virtio-sound-device` needs a paired `-audiodev` to
route samples to *somewhere*. On macOS the candidates are:

```
$ qemu-system-aarch64 -audiodev help
Available audio drivers:
none
coreaudio
dbus
wav
```

`none` consumes samples and discards them — perfect for tests
and for `make run` where we want kernel logs over `-serial
mon:stdio` without an audible chime competing with the
terminal. `coreaudio` routes to the host's default audio
output device. The Makefile defaults `make run` to `none` and
overrides `make run-graphical` to `coreaudio` via a target-
local variable:

```makefile
QEMU_AUDIO_BACKEND ?= none
QEMU_SND := -audiodev $(QEMU_AUDIO_BACKEND),id=audio0 \
            -device virtio-sound-device,audiodev=audio0

.PHONY: run-graphical
run-graphical: QEMU_AUDIO_BACKEND := coreaudio
run-graphical: QEMU_SND := -audiodev $(QEMU_AUDIO_BACKEND),id=audio0 -device virtio-sound-device,audiodev=audio0
run-graphical: $(KERNEL) $(DISK) $(DATA_DISK)
        ...
        $(QEMU_SND) \
        -kernel $(KERNEL)
```

The target-local re-derivation of `QEMU_SND` is needed because
make expands recursively-assigned variables at use-site, and
`$(QEMU_AUDIO_BACKEND)` at use-site for `QEMU_SND` would still
read the global default unless we re-bind in the target's
context. Override `QEMU_AUDIO_BACKEND=wav` on the command line
to dump the chime to a `.wav` file instead.

## The smoke test

`scripts/test_beep.py` boots the kernel with
`-audiodev none,id=audio0 -device virtio-sound-device,audiodev=audio0`
and asserts three things from userspace-visible signals:

1. **The driver serviced the boot chime.** By the time the
   shell prompt is reachable, init has called `beep()` twice
   and the driver has logged two `[virtio-snd] played` lines.
   The test searches for that substring in the pre-prompt
   log.

2. **`beep 880 100` dispatches into the driver.** The test
   sends `beep 880 100\n` over the serial socket and watches
   for a `[virtio-snd] played freq=0x370 Hz duration=0x64`
   line in the response (880 = 0x370, 100 = 0x64; the driver
   uses `serial_puthex` per project convention).

3. **The shell prompt returns after the beep.** Catches the
   case where `tx_submit_and_wait` deadlocks — the chime would
   "play" silently and `beep` would hang.

Crucially, the test does *not* assert on the boot-time
`[virtio-snd] PCM 0 ready` line, even though the driver does
print it. That line is emitted before the test client has
connected to the unix-socket serial port, and `unix:...,
server,nowait` discards data emitted before a listener is
alive. Chapter 96 (RTC) hit the same trap; chapter 97
inherits the lesson and works around it by relying on
*post*-boot output that the chime guarantees will appear after
the connection is up.

## Files added or modified

| Path                                              | Why                       |
|---------------------------------------------------|---------------------------|
| `kernel/device/virtio_snd.h`                      | new — driver API          |
| `kernel/device/virtio_snd.c`                      | new — driver impl         |
| `kernel/core/main.c`                              | + `virtio_snd_init` probe |
| `kernel/core/syscall.h`                           | + `SYS_BEEP=79`, `ENODEV` |
| `kernel/core/syscall.c`                           | + `sys_beep` + dispatch   |
| `userspace/libc/syscall.h`                        | + `beep()` wrapper        |
| `userspace/beep/beep.c`                           | new — `/bin/beep`         |
| `userspace/init/init.c`                           | + boot-chime calls        |
| `Makefile`                                        | + BEEP build, QEMU audio  |
| `scripts/test_beep.py`                            | new — smoke test          |
| `book/chapters/12-system-services/097-virtio-snd.md` | this chapter            |

## Build & run

```sh
make all                       # builds /bin/beep into the disk
make run                       # boot text-mode (silent, audiodev=none)
make run-graphical             # boot graphical (audible, audiodev=coreaudio)
python3 scripts/test_beep.py   # smoke test in isolation
```

At a shell:

```
$ beep              # default 800 Hz × 200 ms
$ beep 1000         # 1000 Hz × 200 ms
$ beep 1000 500     # 1000 Hz × 500 ms
$ beep 100 1000     # 100 Hz × 1 s — sounds like a tugboat horn
```

## Looking ahead

Chapter 98 introduces a PNG decoder so the wallpaper and icons
can be lossless RGB rather than a baked-down BGRA blob. It's
the next chapter that adds a meaningful amount of host-derived
data into the build pipeline — the `scripts/img_to_bgra.py`
Pillow self-bootstrap deserves its own discussion when we
revisit it.

Beyond that, an audio mixer (chapter ?) would let multiple
concurrent `beep` calls overlay rather than serialise, which
would let GUI apps emit notification chimes without blocking
each other. The serialisation is currently invisible because
nothing else uses `SYS_BEEP`, but the moment a notification
daemon wants to play a sound while the user is mid-beep, the
mixing decision has to be made.
