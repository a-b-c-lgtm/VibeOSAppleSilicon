# Chapter 168 — POSIX `<time.h>`

> **Milestone in this chapter:** replace chapter 96's placeholder
> `<time.h>` with the C99 POSIX surface that Doom's RNG seed,
> GCC's diagnostic timestamps, and BearSSL cert-expiry checks
> all expect.
> **Code referenced:**
> - [userspace/libc/time.h](../../../userspace/libc/time.h)
> - [userspace/date/](../../../userspace/date/),
>   [userspace/taskbar/](../../../userspace/taskbar/) (the two
>   existing consumers re-ported to the new shapes)
> - [scripts/test_libc_time.py](../../../scripts/test_libc_time.py)
>
> **At the end of this chapter** you will have `struct tm`,
> `gmtime_r`, `localtime_r`, `mktime`, `timegm`, `strftime`,
> `asctime`, `ctime`, `clock_gettime`, and `clock()` available
> to every userspace binary, with `difftime` declared (callable
> after chapter 171 lifts `-mgeneral-regs-only`). Prerequisite:
> chapter 167 (string / ctype / assert).

---

## What you'll do in this chapter

1. Replace chapter 96's placeholder `<time.h>` (which used a
   non-POSIX `struct civil_time`) with the C99 POSIX surface:
   `struct tm`, `gmtime_r`, `localtime_r`, `mktime`, `timegm`,
   `strftime`, `asctime`, `ctime`, `clock_gettime`, `clock()`.
2. Re-port the two existing in-tree consumers (`date`,
   `taskbar`) to the new shapes and delete their
   placeholder-era helpers.
3. Declare `difftime` as `static inline` knowing it can't be
   *called* until chapter 171 lifts `-mgeneral-regs-only`.
4. Write a `timetest` binary that byte-exactly matches the
   `strftime` outputs Doom and GCC will use.
5. Run `scripts/test_libc_time.py` and watch it land green.

## Why now

The header `<time.h>` is the second-most-included system header
after `<stdio.h>` in the upstream code Part XVIII is targeting.
Doom's `m_random.c` calls `time(NULL)` to seed the
random-number table. GCC's `diagnostic.cc` calls `localtime` +
`strftime` for the `-fdiagnostics-format=json` timestamp field.
BearSSL's `x509_minimal.c` wants `time()` for cert-not-after
validation.

Chapter 96 shipped a placeholder `<time.h>` using a non-POSIX
`struct civil_time` and a `gmtime_r(time_t, struct civil_time *)`
signature. It worked for the in-tree `date` and taskbar binaries
but no upstream code includes the header without expecting
POSIX shapes. Chapter 168 swaps in the POSIX surface and
re-ports the two existing consumers.

---

## What ships

| Surface | What |
|---|---|
| Types | `time_t` (already in syscall.h), `clock_t`, `clockid_t`, `struct timespec`, `struct tm` |
| Clocks | `CLOCK_REALTIME`, `CLOCK_MONOTONIC` (both currently alias gettimeofday) |
| Wall clock | `time()` (already), `clock_gettime()` |
| CPU clock | `clock()` (monotonic ms since first call; `CLOCKS_PER_SEC = 1000`) |
| Calendar | `gmtime_r`, `gmtime`, `localtime_r`, `localtime` (alias of gmtime), `mktime`, `timegm` |
| Formatting | `asctime_r`, `asctime`, `ctime_r`, `ctime`, `strftime` (`%Y %y %m %d %H %M %S %j %p %%`) |
| Arithmetic | `difftime` (declared, not usable until ch. 129 turns on FP) |

The entire surface is header-only `static inline`. No new .c
file, no new linker dependency. The only translation-unit-level
state is the per-TU static buffer that `gmtime`, `localtime`,
`asctime`, and `ctime` use to satisfy the C99 single-buffer
contract.

---

## The placeholder-to-POSIX swap

Chapter 96's `time.h` exported:

```c
struct civil_time { int year, month, mday, hour, min, sec, wday, yday; };
static inline void gmtime_r(time_t secs, struct civil_time *out);
static inline int  strftime_iso(char *buf, unsigned long cap,
                                 const struct civil_time *ct);
```

Chapter 168 replaces it with the C99 shape:

```c
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
                tm_wday, tm_yday, tm_isdst; };
static inline struct tm *gmtime_r(const time_t *t, struct tm *out);
static inline size_t strftime(char *dst, size_t cap,
                               const char *fmt, const struct tm *tm);
```

Both `gmtime_r` symbols share a name; the signatures differ
enough that the compiler catches every old caller at compile
time. Two callers existed:

1. `userspace/date/date.c` — printed `YYYY-MM-DD HH:MM:SS UTC`.
   Re-ported to `gmtime_r(&t, &tm); strftime(buf, sizeof buf,
   "%Y-%m-%d %H:%M:%S", &tm);`.
2. `userspace/taskbar/taskbar.c` — drew `HH:MM:SS` in the
   clock corner. Re-ported to the same pattern + `strftime(buf,
   sizeof buf, "%H:%M:%S", &tm)`. The hand-rolled `format_clock`
   helper is gone.

This is the "apps must use the OS features the book builds"
user directive in practice — the chapter doesn't just *add* the
POSIX surface, it *deletes* the placeholder shapes and migrates
their callers.

---

## Algorithm choices

### gmtime_r: linear year walk

Walk years from 1970 forward, subtracting `is_leap(y) ? 366 :
365` days each iteration. O(years-since-1970). For any plausible
runtime length (you'd have to be at year 2200 for the loop to
take more than 230 iterations) this is fine. The "civil from
days" Hinnant algorithm would be O(1) but adds 30 lines and is
harder to verify by inspection; the linear walk is what chapter
95 had and what's stayed.

### mktime / timegm: same walk in reverse

Sum up days for `year` (1970..tm_year+1900) and `month`
(1..tm_mon+1), add `tm_mday - 1`, multiply by 86400, add
`tm_hour*3600 + tm_min*60 + tm_sec`, then re-derive the rest
via gmtime_r to normalize wday/yday/etc.

### strftime: switch over format chars

No widths, no flags beyond default zero-pad for numeric fields.
The `__OSDEV_PUTN(val, width)` macro builds the integer
representation backwards into a 8-byte temp, then emits with
leading zeros until `width` is reached. Real C99 supports
flags like `-` (left-justify) and `_` (space-pad); the
implementation here doesn't, because nothing in the target
software uses them.

### difftime: declared but unusable pre-129

C99 says `difftime` returns `double`. With `-mgeneral-regs-only`
(set at the top of the Makefile until chapter 171), the
compiler refuses to use FP registers; a return-by-value double
goes through `d0`, which freestanding-without-FP code can't
touch. The inline body compiles only because nothing in this
chapter actually calls it — GCC's dead-code path elides the FP
move before codegen.

Once chapter 171 lifts `-mgeneral-regs-only`, the existing
inline starts working with no source change. The timetest
script skips the difftime check with a comment pointing at
129.

---

## Pitfalls

### Pitfall — `-Werror=maybe-uninitialized` on `struct timespec ts;`

**Symptom.** Build fails with
`error: ‘ts.tv_nsec’ may be used uninitialized`.

**Cause.** The CHECK macro reads `ts.tv_nsec` even when
`clock_gettime` returns failure; GCC can't prove the field is
initialized along that path.

**Fix.** Explicit zero-initialization before the call:

```c
struct timespec ts;
ts.tv_sec  = 0;
ts.tv_nsec = 0;
```

You could also write `struct timespec ts = { 0 };` but that
invites the freestanding-C `{ 0 }` memset trap (the 16-byte
struct is under the ~64-byte threshold so it's actually safe
here, but explicit field assignment is the discipline-keeping
move).

### Pitfall — `:=` immediate-expansion (again)

Same as chapter 167. Any OBJS block defined above `CSTRING_OBJ` in
the Makefile must reference the literal path
`$(BUILD)/userspace/libc/cstring.o`, not the variable name.
Worth eventually refactoring all `CSTRING_OBJ`-referencing
blocks to live below the variable definition; left alone here
because it would shuffle every nearby OBJS block in a way
that obscures the chapter-by-chapter additions.

---

## Test coverage

`scripts/test_libc_time.py` runs `timetest` and asserts the
"all checks passed" marker. The CHECK()s inside include:

- Epoch 0 → 1970-01-01 00:00:00 Thursday (tm_wday == 4).
- `t = 1234567890` (the unix-billennium moment) → 2009-02-13
  23:31:30. strftime("%Y-%m-%d %H:%M:%S") byte-exact match.
- `%j` for 2009-02-13 == 044 (31 Jan + 13 Feb).
- `%y/%m/%d %H:%M:%S %p` → "09/02/13 23:31:30 PM".
- `100%% %j` → "100% 044".
- Leap-year boundary 2020-02-29 → tm_wday == 6 (Saturday),
  tm_yday == 59.
- `mktime` round-trip from `t = 1700000000`: gmtime → mktime →
  identity.
- `asctime` shape exact: "Wed Jun 30 21:49:08 1993\n".
- `localtime_r` identity to `gmtime_r` (no TZ).

---

## What this unlocks

- **`userspace/date/date.c`** — switched from `struct
  civil_time` + `strftime_iso` to POSIX `struct tm` +
  `strftime("%Y-%m-%d %H:%M:%S")`.
- **`userspace/taskbar/taskbar.c`** — same swap; deleted the
  hand-rolled `format_clock` helper (~15 lines), replaced with
  `strftime(buf, sizeof buf, "%H:%M:%S", &tm)`.

Future chapters that will pull in the new surface:

- **172 (Doom platform shim)** — `time(NULL)` for the RNG
  seed, `clock()` for frame timing.
- **177 (binutils)** — `mktime`/`gmtime` for archive member
  timestamps.
- **185 (GCC)** — `localtime` + `strftime` for `-time` and
  diagnostic output.
- **Chapter 171** — wires `difftime` and the FP path through
  the same header without source-level changes.

---

## Things to remember

- **When you upgrade a placeholder to the standard, delete the
  placeholder's clients on the same chapter.** Taskbar's
  `format_clock` was 15 lines of hand-rolled HH:MM:SS that
  `strftime("%H:%M:%S")` covers in one. The placeholder
  existed only because the standard didn't. Don't leave both
  around.
- **Test the headers in the locale they'll be consumed in.**
  Every byte-exact `strftime` assertion in `timetest` matches
  the literal format strings Doom and GCC actually use. If
  someone touches the format engine and `%Y-%m-%d %H:%M:%S`
  breaks, the regression points right at the right line.
- **Designated initializers vs the freestanding memset trap.**
  A 16-byte struct is fine to `= { 0 }`; a 100-byte struct
  will emit a memset call you don't have. Discipline:
  explicit field init at every size, so the trap can't even
  surface.
