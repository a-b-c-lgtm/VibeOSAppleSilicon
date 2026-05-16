# Chapter 95 — A real RTC and wall-clock time

**Status:** Done.

The taskbar clock has been ticking since chapter 56, but until
this chapter it counted seconds-since-boot and not seconds-since-
1970. Open the launcher, watch the clock, hard-reboot the VM,
watch the clock again — same low values, restarting from
00:00:00. Nothing in the system knew what year it was; nothing
could; there was no datum.

Chapter 95 wires the QEMU `virt` machine's PL031 RTC into the
kernel, exposes a single new syscall (`SYS_GETTIMEOFDAY`) with a
POSIX-shaped `struct timeval`, ships a tiny header-only civil-
time library in libc, and replaces the taskbar's uptime-based
clock with the same wall-clock-derived value. There is also a
new userspace tool — [`/bin/date`](../../../userspace/date/date.c)
— that prints `YYYY-MM-DD HH:MM:SS UTC`. The smoke test
[scripts/test_rtc.py](../../../scripts/test_rtc.py) boots the
kernel, runs `date` twice with a 1.5 s sleep between them, and
asserts that the timestamps are well-formed, plausible (year
≥ 2025), and that the wall clock advances.

The architectural payoff is bigger than the user-visible one.
Every later chapter that needs to know the date — file mtimes
in OSFS-2, cookie-jar expiry in the browser, log line
timestamps, future `strace -t` output — calls the same one-line
`gettimeofday()`. Wall-clock time stops being a "we'll add it
later" caveat in chapters that need it.

The kernel-side cost is small. PL031 is a deliberately tiny
device — three relevant registers, no setup needed because QEMU
hard-wires the enable bit on. We do exactly one MMIO read at
boot, pair it with a `timer_ticks()` snapshot, and from then on
extrapolate everything from the live tick counter. Per-syscall
walltime is pure arithmetic; the hot path never touches the
device.

## What this chapter adds

* **`kernel/core/walltime.[ch]`** — the boot-snapshot driver and
  three-function API: `walltime_init(dtb)` walks the DTB for
  `compatible = "arm,pl031"`, reads RTCDR once, snapshots the
  result alongside `timer_ticks()`. `walltime_have_rtc()` is a
  yes/no probe. `walltime_now_us(*secs, *usecs)` derives the
  current time from the snapshot plus the live tick delta.

* **`fdt_read_pl031(blob, *base)`** in `kernel/core/fdt.c` — DTB
  scanner that finds the first node whose `compatible` string
  list contains `"arm,pl031"` and reads the first 64-bit `reg`
  cell as the MMIO base. Helper `compatible_contains` walks the
  NUL-separated string list inside a packed `compatible`
  property.

* **`SYS_GETTIMEOFDAY = 78`** — single-argument syscall that
  copies a `struct timeval` into the user's buffer. Returns 0
  on success or `-EFAULT` on a bad pointer. Hand-off to
  userspace via `copy_to_user`.

* **`struct timeval`** — defined byte-for-byte identically in
  `kernel/core/syscall.h` and `userspace/libc/syscall.h`:
  `{ int64_t tv_sec; uint32_t tv_usec; uint32_t _pad; }` (16
  bytes total). The 4-byte `_pad` keeps the trailing alignment
  obvious so future ABI extensions don't accidentally reuse
  the slot.

* **`userspace/libc/syscall.h`** wrappers — `gettimeofday(*tv)`
  static-inline that issues the SVC, `time(*out)` convenience
  wrapper that returns just the seconds, and a `typedef int64_t
  time_t;`.

* **`userspace/libc/time.h`** — header-only civil-time library:
  `struct civil_time` field-broken POSIX-`struct tm` shape,
  `is_leap(y)`, `days_in_month(y, m)`, `gmtime_r(time_t,
  *civil)` that walks years from 1970, and a single-format
  `strftime_iso(buf, cap, *civil)` that prints the
  `YYYY-MM-DD HH:MM:SS` shape into 20 bytes.

* **`/bin/date`** — `userspace/date/date.c`. Calls
  `gettimeofday`, breaks the seconds via `gmtime_r`, prints
  `"%s UTC\n"`. No flags today.

* **Taskbar wall-clock integration** — `userspace/taskbar/
  taskbar.c`'s `format_clock` now takes a `struct civil_time *`
  and renders only HH:MM:SS. `draw_clock` calls `gettimeofday`
  + `gmtime_r` instead of `uptime_ms`. The main-loop "did the
  second tick?" comparison now also reads `tv_tick.tv_sec` so
  the value matches what `draw_clock` writes into
  `g_last_clock_sec` (see "The taskbar tick-source gotcha"
  below).

* **`scripts/test_rtc.py`** — smoke test. Boots the kernel,
  drops to `/bin/sh`, runs `date`, asserts (1) ISO regex
  match, (2) year ≥ 2025, (3) the wall clock advances between
  two `date` calls 1.5 s apart.

## Prerequisites

* **Chapter 7** — the device tree and `fdt.c`'s structure-block
  walker. We add one more node-matching loop that reuses the
  same FDT primitives the timer / GIC / virtio code already use.

* **Chapter 10** — the timer subsystem. `timer_ticks()` and
  `TICK_INTERVAL_MS` are the entire basis for "extrapolate from
  the boot snapshot" — we don't have any other clock to lean
  on.

* **Chapter 16** — the kernel/user boundary. `copy_to_user` is
  what makes the syscall safe; without it `sys_gettimeofday`
  would be one fault away from a kernel crash.

## The PL031 in 30 seconds

The PL031 is an Arm-licensed real-time-clock IP block. Linux's
own driver is ~200 lines because the device is so small that
most of those lines are comments. The whole MMIO surface is
four 32-bit registers:

```
+0x00  RTCDR  (RO)  current time, 32-bit seconds-since-epoch
+0x04  RTCMR  (RW)  match register (alarm IRQ; unused here)
+0x08  RTCLR  (RW)  load register (write to set the time)
+0x0C  RTCCR  (RW)  control register (bit 0 = enable)
```

QEMU's `virt` machine pre-arms the device — `RTCCR.bit0 = 1` is
hard-wired on, the host's wall clock is mirrored into RTCDR, and
the alarm interrupt is wired through the GIC even though we
never use it. We do not write any of the registers; the only
operation chapter 95 performs is a single 32-bit read at
`base + 0x00`.

The address is `0x09010000` — sandwiched between the PL011 UART
at `0x09000000` and the GPIO at `0x09030000`. Crucially, this
is inside the `0x09000000–0x09FFFFFF` device range that the
kernel's early L1 page-table block descriptor already maps as
`Device-nGnRnE`, so the PL031 read works the moment we know the
address. No new page-table work, no `pmap` touchup.

DTB-wise the node looks like this:

```
pl031@9010000 {
    clock-names = "apb_pclk";
    clocks = <0x8000>;
    interrupts = <0x00 0x02 0x04>;
    reg = <0x00 0x9010000 0x00 0x1000>;
    compatible = "arm,pl031\0arm,primecell";
};
```

The two-string `compatible` list is the giveaway that we should
match by `arm,pl031` specifically and not by node name —
`pl031@9010000` is a name convention, but other vendors who
reuse Arm's `primecell` framework also call their nodes
`primecell` in the FDT. Matching by the first string in
`compatible` is the durable pattern.

## DTB lookup: `fdt_read_pl031`

The chapter-7 FDT scanner already had to walk the `/memory`
node (for the RAM size). We add one more matcher to the same
file ([kernel/core/fdt.c](../../../kernel/core/fdt.c)):

```c
int fdt_read_pl031(const void *blob, uint64_t *base_out)
{
    /* Standard FDT structure-block walk. */
    int      seen_compatible = 0;
    int      seen_reg        = 0;
    uint64_t cur_reg_base    = 0;

    while (cursor < end) {
        switch (token) {
        case FDT_BEGIN_NODE:
            seen_compatible = 0;
            seen_reg        = 0;
            cur_reg_base    = 0;
            break;

        case FDT_END_NODE:
            if (seen_compatible && seen_reg) {
                *base_out = cur_reg_base;
                return 1;
            }
            break;

        case FDT_PROP:
            if (str_eq(pname, "compatible")) {
                if (compatible_contains(pdata, plen, "arm,pl031"))
                    seen_compatible = 1;
            } else if (str_eq(pname, "reg") && plen >= 8) {
                uint64_t hi = read_be32(pdata + 0);
                uint64_t lo = read_be32(pdata + 4);
                cur_reg_base = (hi << 32) | lo;
                seen_reg = 1;
            }
            break;
        }
    }
    return 0;
}
```

Two small subtleties.

The properties of an FDT node arrive in whatever order `dtc`
emits them, which means we cannot decide "is this the node?"
mid-property — we have to stage `seen_compatible` and `seen_reg`
across `FDT_PROP` tokens and check at `FDT_END_NODE`. That's a
slightly different shape from the old `fdt_read_memory` which
matches by node name and is allowed to short-circuit.

The `compatible` property value is not one C string but a
*list* of NUL-terminated strings packed back-to-back, total
length given by `plen`. The PL031 node above has the bytes
`"arm,pl031\0arm,primecell\0"` for plen = 24. The
`compatible_contains` helper walks the list one entry at a
time, comparing each against `"arm,pl031"`:

```c
static int compatible_contains(const uint8_t *pdata,
                               uint32_t plen, const char *needle)
{
    uint32_t off = 0;
    while (off < plen) {
        const char *cand = (const char *)(pdata + off);
        if (str_eq(cand, needle)) return 1;
        while (off < plen && pdata[off] != 0) off++;
        if (off < plen) off++;            /* skip the NUL */
    }
    return 0;
}
```

We assume `#address-cells = 2, #size-cells = 2` for the root,
which means each `reg` cell is a 64-bit base + 64-bit length.
That's the only configuration QEMU `virt` has ever shipped, and
the only one chapters 7 / 36 / 39 already assumed.

If the lookup fails we fall back to the well-known
`PL031_FALLBACK_BASE = 0x09010000` and log it, so a missing /
malformed DTB doesn't crash the kernel. In practice the
fallback never fires; it exists so `walltime_init` cannot fail.

## The boot snapshot

The temptation when wiring up a clock is to read the device on
every `gettimeofday`. Don't. Two reasons:

1. **MMIO costs more than arithmetic.** A PL031 RTCDR read is
   typically free under HVF (no VM exit), but on KVM/x86 hosts
   and on real hardware it's a slow uncached load. Pure
   arithmetic from cached state is always the cheaper path.

2. **PL031's RTCDR ticks once per second.** Two reads spanning a
   tick can differ by a full second. We can paper over that
   with a "read twice and re-read on disagreement" loop, but
   the simpler answer — read it once and never again — sidesteps
   the race entirely.

The boot-snapshot pattern is one MMIO read at `walltime_init`
plus one `timer_ticks()` read paired with it. Both go into
file-static globals. From then on:

```c
void walltime_now_us(int64_t *secs_out, uint32_t *usecs_out)
{
    uint64_t ticks_now   = timer_ticks();
    uint64_t delta_ticks = ticks_now - g_boot_ticks;
    uint64_t delta_us    = delta_ticks
                         * (uint64_t)TICK_INTERVAL_MS
                         * 1000ULL;

    int64_t  secs  = g_boot_walltime
                   + (int64_t)(delta_us / 1000000ULL);
    uint32_t usecs = (uint32_t)(delta_us % 1000000ULL);

    if (secs_out)  *secs_out  = secs;
    if (usecs_out) *usecs_out = usecs;
}
```

Five integer ops. No locks (the snapshot is set once before
any other CPU exists; the live `timer_ticks()` is already
SMP-safe from chapter 92). No syscall dispatch overhead beyond
the inevitable SVC.

The downside: we don't pick up host-clock adjustments after
boot. If the host's NTP daemon steps the clock by 0.5 s during
runtime, we miss it. That's acceptable for chapter 95's floor —
NTP-style adjustments are a separate milestone and would
require a `walltime_resync()` API (re-snapshot under a
seqcount) that we'll add when something actually cares.

The boot-snapshot also uses `int64_t` for the seconds
internally even though the PL031 register is only 32 bits.
That's the whole story for our Y2038 mitigation: the kernel
promotes the 32-bit value to 64 bits at the boot read and
exports 64 bits over the syscall. The PL031 will roll over in
January 2038, but on a long-lived system (which we don't have)
we'd handle that with a periodic re-snapshot. For chapter 95
the kernel's wall-clock value just won't wrap until well after
the heat death of any QEMU VM I expect to run.

## The syscall surface

`SYS_GETTIMEOFDAY` is enum value 78, slot directly after
`SYS_CLONE3 = 77`. The dispatcher entry is one line:

```c
case SYS_GETTIMEOFDAY:
    ret = sys_gettimeofday(a0);
    break;
```

The handler:

```c
static long sys_gettimeofday(uintptr_t out_ptr)
{
    struct timeval tv;
    int64_t  secs  = 0;
    uint32_t usecs = 0;
    walltime_now_us(&secs, &usecs);

    tv.tv_sec  = secs;
    tv.tv_usec = usecs;
    tv._pad    = 0;

    if (copy_to_user(out_ptr, &tv, sizeof(tv)) < 0)
        return -EFAULT;
    return 0;
}
```

The shape is intentionally minimal: one IN-OUT pointer, one
`copy_to_user` for the whole struct, one return value
(`0` or `-EFAULT`). We do not take an optional `struct timezone
*tz` — POSIX still defines that pointer but it's been formally
deprecated since 2001 and every libc passes it as NULL anyway.

`struct timeval` is defined identically in
[kernel/core/syscall.h](../../../kernel/core/syscall.h) and
[userspace/libc/syscall.h](../../../userspace/libc/syscall.h):

```c
struct timeval {
    int64_t  tv_sec;     /* seconds since 1970-01-01 UTC      */
    uint32_t tv_usec;    /* microseconds, 0..999_999          */
    uint32_t _pad;
};
```

The trailing `_pad` is explicit, not implicit. AArch64's natural
alignment would pad anyway, but writing the slot down makes
the ABI deliberate: future extensions that want to use those
4 bytes (e.g. for a stable "epoch generation" counter that NTP
resync would bump) can do so without changing the struct size.

## The libc side

Two new bits in `userspace/libc/syscall.h`:

```c
typedef int64_t time_t;          /* Y2038-safe                  */

static inline int gettimeofday(struct timeval *tv) {
    return (int)_svc1(SYS_GETTIMEOFDAY, (long)(uintptr_t)tv);
}

static inline time_t time(time_t *out) {
    struct timeval tv;
    if (gettimeofday(&tv) != 0) return (time_t)-1;
    if (out) *out = tv.tv_sec;
    return tv.tv_sec;
}
```

Then a new header
[userspace/libc/time.h](../../../userspace/libc/time.h) for civil-
time conversion. POSIX's `gmtime_r` shape is "take seconds, fill
in a `struct tm`" — we keep that, but rename the struct fields
from `tm_sec / tm_min / ...` to `sec / min / ...` so a debugger
prints the layout obviously:

```c
struct civil_time {
    int year, month, mday, hour, min, sec, wday, yday;
};
```

The `gmtime_r` implementation walks years from 1970 forward,
subtracting days as it goes. O(years-since-1970) which is fine
for a function called once per render frame — the cold-cache
cost of the year loop is dwarfed by `gui_fill_rect`. A
constant-time implementation (Howard Hinnant's
"days_from_civil") exists and would be a one-screen drop-in
replacement; we punt because the input range is small enough
that the linear walk is invisible.

`strftime_iso` is the only formatter we need for chapter 95
and prints exactly one shape:

```c
static inline int strftime_iso(char *buf, unsigned long cap,
                               const struct civil_time *ct)
{
    if (cap < 20) return -1;
    /* Hand-rolled decimal write; see source. */
    return 19;
}
```

Why hand-roll instead of `snprintf("%04d-%02d-%02d ...")`? We
want `/bin/date` to be small enough to fit in two OSFS sectors
(it's 1024 bytes including stripping). A `printf` call would
pull in the format-string interpreter from
[userspace/libc/printf.h](../../../userspace/libc/printf.h)
twice (once for the formatting itself, once via the
`%s UTC\n` print). Three `digits[i]` table lookups per field is
both shorter object code and easier to read.

The "no localtime" stance is deliberate. A correct `localtime`
needs a tz database (zoneinfo or POSIX TZ strings + DST
calendars); shipping that is a milestone of its own. Chapter
95's floor is UTC throughout, including the taskbar clock. A
future polish can introduce a one-line `/data/timezone =
"+10:00"` scalar offset that the taskbar reads at startup and
adds before `gmtime_r`.

## /bin/date

The entire program:

```c
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/time.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct timeval tv;
    if (gettimeofday(&tv) != 0) { return 1; }

    struct civil_time ct;
    gmtime_r((time_t)tv.tv_sec, &ct);

    char buf[24];
    strftime_iso(buf, sizeof(buf), &ct);
    printf("%s UTC\n", buf);
    return 0;
}
```

That's it. On a system where the kernel never found an RTC
(PL031 absent, broken DTB) the output starts at `1970-01-01
00:00:0X UTC`, which is the test's negative signal — anything
in the past two decades is "real RTC", anything around 1970 is
"fallback".

Adding `date` to OSFS is the now-routine three-line pattern in
the [Makefile](../../../Makefile):

```makefile
DATE_OBJS     := build/userspace/date/date.o
DATE_ELF      := build/userspace/date/date.elf
DATE_STRIPPED := build/userspace/date/date

OSFS_BIN_FILES += $(DATE_STRIPPED)
# in mkosfs.py invocation:
date=$(DATE_STRIPPED) \
```

After `make`, the date tool lives at `/bin/date` and is `~1
KiB`.

## Wiring the taskbar clock to wall time

The pre-95 taskbar clock looked like:

```c
unsigned long up_secs = uptime_ms() / 1000;
int hh = (int)(up_secs / 3600);
int mm = (int)((up_secs / 60) % 60);
int ss = (int)(up_secs % 60);
/* render HH:MM:SS into the clock cell */
```

It was an "uptime-since-boot" formatter that wrapped at 100
hours. Replacing it with wall time looks straightforward at
first glance — pull a `struct civil_time` and render the same
HH:MM:SS — and it is, modulo one gotcha that bit during
implementation. (Documented next, because it's the
generalisable lesson.)

The new `draw_clock`:

```c
static void draw_clock(void)
{
    struct timeval tv;
    int rc = gettimeofday(&tv);
    long secs_total = (rc == 0) ? (long)tv.tv_sec
                                : (long)(uptime_ms() / 1000ul);

    struct civil_time ct;
    gmtime_r((time_t)secs_total, &ct);

    char buf[9];
    format_clock(&ct, buf);          /* HH:MM:SS */
    /* paint cell + glyphs (unchanged) */

    g_last_clock_sec = (int)secs_total;
}
```

The fallback path (uptime if `gettimeofday` fails) is
defensive only — the syscall can only return `-EFAULT` and
we're passing a stack pointer, so it's "this should never
fire" code. It's there because in user code "this should
never fire" routinely fires.

## The taskbar tick-source gotcha

Here's the lesson worth promoting to a memory file. The
taskbar's main loop polls events and decides every iteration
whether to repaint the clock cell:

```c
/* WRONG — pre-fix shape */
unsigned long secs = uptime_ms() / 1000;
if (redraw || (int)secs != g_last_clock_sec) {
    draw_clock();
    gui_flush(g_self_id);
}
```

This is the code that *was already in the file* before chapter
95: `g_last_clock_sec` was uptime-derived, the comparison was
uptime-derived, all consistent. Repainting the clock once per
uptime-second worked.

After the chapter-95 changes, `draw_clock` writes the *wall-
clock* second into `g_last_clock_sec`. If the comparison still
reads from `uptime_ms()`, the two values are from different
time sources and will never match — `(int)(uptime_ms()/1000)`
is single-digit shortly after boot while `tv.tv_sec` is in the
1.7 billions. Every poll iteration would now decide "the
second has changed", call `draw_clock` and `gui_flush`, and
burn a noticeable chunk of CPU painting the same pixels.

The fix is one block:

```c
/* RIGHT — post-fix shape */
struct timeval tv_tick;
long secs = (gettimeofday(&tv_tick) == 0)
          ? (long)tv_tick.tv_sec
          : (long)(uptime_ms() / 1000ul);
if (redraw || (int)secs != g_last_clock_sec) {
    draw_clock();
    gui_flush(g_self_id);
}
```

Same source for write and read. The general lesson is: **a
"did the value change?" check must read from the SAME source
that produced the stored value**. Mixing time bases (uptime
vs walltime, monotonic vs ntp-adjusted, host-clock vs guest-
clock) anywhere in the read/write path is a deferred-bug
generator. Cached-state comparisons should always traverse
exactly the same accessor as the cache write.

This is the same family of bug as `ui-event-order-vs-render`
(see user memory): a cache layer is silently fed from two
different upstreams and the consumer doesn't notice until the
two upstreams disagree.

## The test

[scripts/test_rtc.py](../../../scripts/test_rtc.py) boots the
kernel into `/bin/sh`, runs `date` twice, and asserts:

1. `/bin/date`'s output matches `YYYY-MM-DD HH:MM:SS UTC`.
2. The year is ≥ 2025. (QEMU's PL031 surfaces the host clock,
   so a year that recent is a meaningful "did the kernel
   actually read the device?" check rather than a tautology.)
3. Two `date` calls 1.5 s apart show different timestamps,
   with a delta in [1, 5] s.

The test does *not* assert on the kernel's `[walltime] PL031
base = ...` log line, which is what an earlier draft tried.
That draft failed reliably with "kernel did not log PL031
init" — a head-scratcher, because the kernel definitely was
logging it (`make run-graphical` showed the line cleanly).

The cause turned out to be the `-serial` socket configuration
the test uses:

```python
"-serial", f"unix:{SOCK},server,nowait"
```

The `nowait` flag tells QEMU "don't block waiting for a client
to connect; start the guest immediately." The Python test
then opens its own client connection a moment later. **Any
serial output emitted between QEMU starting the guest and the
client connecting is silently discarded.** On HVF the kernel
boots fast enough that several hundred milliseconds of early
log goes to the void; the test client only ever sees output
from somewhere mid-init.

Two ways to fix that. One is `nowait → wait` so QEMU blocks
the guest until the client connects — but that changes timing
for every test, and a Python client that takes too long to
connect would deadlock the boot. The simpler fix is the one
we adopted: **don't put assertions on kernel-log lines that
happen during early boot**. Use *userspace-observable*
signals instead. The "year ≥ 2025" check proves the kernel
read the RTC just as conclusively as the kernel log line
would, but the proof is delivered through `/bin/date` which
runs after the test client is connected, so it's reliably
visible.

The general lesson — promoted to a memory file alongside this
chapter — is: **tests that boot QEMU with `-serial unix:SOCK,
server,nowait` cannot reliably assert on early kernel output.
Always design the assertion around something userspace prints
after the shell prompt is up.**

## Floor caveats

* **No NTP / no clock adjustment after boot.** We snapshot the
  RTC once, then extrapolate. If the host steps its clock
  during runtime we miss it. The fix is a `walltime_resync()`
  that re-reads the RTC under a seqcount; not shipped today
  because nothing needs it.

* **UTC only.** No timezone support. The taskbar clock and
  `/bin/date` both display UTC. A future polish can read a
  scalar offset from `/data/timezone` and add it before
  `gmtime_r`. A real `localtime` (zoneinfo + DST) is its own
  milestone.

* **Y2038 on real PL031.** Our kernel-internal value is
  `int64_t`, but the underlying RTCDR is 32-bit. On a system
  that runs uninterrupted past 2038, `walltime_init` after a
  reboot would re-read the (now-wrapped) register and snapshot
  a negative-shaped value. A 64-bit RTC (e.g. ARMv8's generic
  timer + persistent NV counter) would sidestep that; QEMU
  virt doesn't expose one today.

* **Per-syscall MMIO would let us observe host adjustments.**
  We deliberately don't, for the reasons in "The boot
  snapshot" above. If a future feature *needs* sub-millisecond
  fidelity to the host clock (e.g. precise `clock_nanosleep`),
  the right answer is an explicit `clock_gettime(REALTIME)`
  variant that re-reads under a seqcount, leaving `walltime_
  now_us` alone.

* **No leap seconds.** `gmtime_r` assumes 86400-second days.
  Real wall-clock time has 27 (and counting) extra seconds
  inserted since 1972. POSIX itself models them away; we
  inherit that simplification by accident rather than
  design.

* **`gmtime_r` is O(years).** Linear walk from 1970 forward.
  Negligible today (~0.5 µs amortised); would be the wrong
  shape for a logging hot path that calls it millions of
  times per second.

* **`strftime_iso` is one-shape.** No `%F`, no `%T`, no
  formatter at all — just the hard-coded ISO string. A real
  `strftime` lives in libc when something cares about another
  format (the most likely first caller is `cron`, which
  doesn't exist yet either).

* **No file mtimes or ctimes.** OSFS-2 (chapter 80) will be
  the first consumer that stores `time_t` values; today the
  filesystems have no time fields at all, so the wall-clock
  is purely a display + diagnostic concern.

## What this unlocks

* **OSFS-2 timestamped inodes.** When the journaled writable
  filesystem lands (chapter 80), file metadata gains
  `mtime / ctime`. They'll come straight out of `walltime_now_us`.

* **Browser cookie expiry.** Cookies have absolute-time `Expires`
  attributes. The cookie jar (chapter 110) needs to compare
  them against `time(NULL)`.

* **Real timestamps in logs.** Future kernel-side log lines
  (and userspace `notify` payloads) can replace
  `[uptime=12345 ms]` with `[2026-05-15T20:38:53Z]`.

* **A `cron`-shaped scheduler.** Once we have wall-clock time,
  a periodic job runner is a tiny shell + `time(NULL) %
  schedule_period == 0` test. Not high priority but cheap to
  build now that the time source exists.

* **Bench output stamps.** The browser's
  `--bench-resize` mode (chapter 94) currently prints
  `parse_ms` from `uptime_ms()` deltas. Adding wall-clock
  start/end stamps to the bench output makes long-running
  comparisons across reboots actually meaningful.

The architectural payoff is the same as for any "small
ubiquitous primitive" chapter (uptime in 10, getpid in 18,
random in… some future chapter): once the syscall exists,
every later milestone that wants it gets it for free.

## Files added

* `kernel/core/walltime.h`
* `kernel/core/walltime.c`
* `userspace/libc/time.h`
* `userspace/date/date.c`
* `scripts/test_rtc.py`
* `book/chapters/12-system-services/95-rtc-and-wallclock.md`
  (this file)

## Files modified

* `kernel/core/fdt.h` / `kernel/core/fdt.c` — `fdt_read_pl031`
  + the `compatible_contains` helper.
* `kernel/core/syscall.h` — `SYS_GETTIMEOFDAY = 78`,
  `struct timeval`.
* `kernel/core/syscall.c` — `#include "walltime.h"`,
  `sys_gettimeofday` handler, dispatcher case.
* `kernel/core/main.c` — `#include "walltime.h"` and
  `walltime_init(dtb)` call right after the FDT memory
  scan.
* `userspace/libc/syscall.h` — `SYS_GETTIMEOFDAY` enum,
  `struct timeval` mirror, `time_t`, `gettimeofday()` and
  `time()` static-inline wrappers.
* `userspace/taskbar/taskbar.c` — `format_clock` rewritten to
  take `civil_time *`, `draw_clock` switched to wall-clock,
  main-loop tick comparison switched to `gettimeofday`.
* `Makefile` — `kernel/core/walltime.c` added to `C_SRCS`,
  `DATE_OBJS / ELF / STRIPPED` rules, `$(DATE_STRIPPED)` in
  `OSFS_BIN_FILES`, `date=$(DATE_STRIPPED)` in the mkosfs
  invocation.

## Build & test

```sh
make all
python3 scripts/test_rtc.py    # ⇒ PASS

# Full sweep:
for f in scripts/test_*.py; do timeout 240 python3 "$f"; done
# 48/48 PASS (was 47 before; +test_rtc)
```

A by-hand check that the wall-clock end-to-end flow works:

```sh
$ make run
...
/$ date
2026-05-15 20:38:53 UTC
/$ date
2026-05-15 20:38:55 UTC
```

And the taskbar shows `20:38:53`-shaped HH:MM:SS rather than
the pre-95 "seconds since boot wrapped at 100h" output.
