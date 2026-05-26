# Chapter 130a — Doom (the port)

> **Milestone in this chapter:** vendor DoomGeneric and write the
> osdev backend shim that drives it through the chapter-108d
> window manager.
> **Code referenced:**
> - [vendor/doomgeneric/src/](../../../vendor/doomgeneric/src/)
> - [userspace/doom/doomgeneric_osdev.c](../../../userspace/doom/doomgeneric_osdev.c)
> - [userspace/libc/](../../../userspace/libc/) (`strings.h`,
>   `inttypes.h`, `unistd.h`, `sys/types.h`, plus new
>   `strdup` / `calloc` / `realloc` / `atof` / `usleep`)
> - [scripts/test_doom.py](../../../scripts/test_doom.py)
>
> **At the end of this chapter** you will have the first real
> upstream C program running on the guest — a ~240-line shim
> implementing the six `DG_*` callbacks against the kernel WM
> bus, plus the libc holes Doom discovered along the way.
> Prerequisites: chapters 128a–f (the libc surface), 129 (FP /
> SIMD at EL0).

---

## What you'll do in this chapter

1. Vendor DoomGeneric (Cybowski's stripped Chocolate-Doom
   fork) into `vendor/doomgeneric/src/`, then exclude its
   twelve platform/audio backends.
2. Write `userspace/doom/doomgeneric_osdev.c` — a ~240-line
   shim that implements the six `DG_*` callbacks against the
   chapter-108d window manager.
3. Plug the libc holes Doom finds: ship `<strings.h>`,
   `<inttypes.h>`, `<unistd.h>`, `<sys/types.h>`; add
   `strdup` / `calloc` / `realloc` / `atof` / `usleep` /
   `system` / `perror` / `setbuf` / `remove` / `rename`;
   grow `mkdir` to its POSIX two-argument form.
4. Wire the build: enumerate vendor TUs, give them a relaxed
   `DOOM_VENDOR_CFLAGS`, link with `--start-group ...
   --end-group`.
5. Run `scripts/test_doom.py` (boots the OS, runs `doom`
   without a WAD, expects the "Game mode indeterminate"
   banner) and the full regression sweep.

## Why Doom, why now

Section 18 has been adding the bits of POSIX userspace that
real C programs expect — variadic printf without `%f`, then
*with* `%f` (128f); ctype/assert/string (128c); `time_t` and
friends (128d); getopt and `qsort`/`bsearch` (128e); finally
eager FP at EL0 (129). Each chapter ended with a small test
binary (`stdiotest`, `strtest`, `timetest`, `fptest`) that
exercised exactly the surface that chapter built. Those tests
all passed in isolation.

But the real bar is *running someone else's code*. Doom is the
canonical "is this a real OS yet?" port — small enough to
vendor without filling the repo (about 60 kLoC of portable C),
but big enough to find every gap in the libc surface that the
in-house tests missed. Doom uses `strdup`, `calloc`, `realloc`,
`usleep`, `atof`, `mkdir(path, mode)` (POSIX 2-arg!), `perror`,
`setbuf`, `remove`/`rename`, `system`, `getenv`, and headers
that haven't shipped yet (`<strings.h>`, `<inttypes.h>`,
`<unistd.h>`, `<sys/types.h>`). It also touches the
floating-point unit on every renderer frame, so the whole
chapter-129 eager-save machinery gets a real workout instead of
synthetic micro-tests.

The "Doom on this OS" branding is fun, but the *engineering*
purpose is: validate that section 18 actually delivers what
upstream C expects.

---

## Choice of port: DoomGeneric

There are dozens of Doom forks. The two real choices are:

- **Chocolate Doom** (or **Crispy Doom**, **DSDA-Doom**, ...) —
  full SDL-based source ports. Faithful to the original v1.9
  but with a lot of platform glue: configuration, demos,
  network play, multiple game modes. ~150 kLoC, several
  thousand `#ifdef`s for different OSes. Hard to bring up
  without a real SDL.

- **DoomGeneric** (Wojciech Cybowski's fork of vanilla Doom,
  not the doomgeneric/ozkl/lib fork) — Chocolate Doom stripped
  to a six-function platform interface:

  ```c
  void DG_Init(void);
  void DG_DrawFrame(void);
  void DG_SleepMs(uint32_t ms);
  uint32_t DG_GetTicksMs(void);
  int DG_GetKey(int *pressed, unsigned char *key);
  void DG_SetWindowTitle(const char *title);
  ```

  The renderer writes into a fixed 640×400 BGRA framebuffer
  `extern uint32_t DG_ScreenBuffer[]`; the platform shim is
  expected to push that buffer to the display on each
  `DG_DrawFrame`. Input is one byte at a time via the
  `DG_GetKey` callback.

DoomGeneric is the right pick for a hobby OS. The platform
shim is two hundred lines of C; the rest of the code is just
"vanilla Doom that compiles". `vendor/doomgeneric/src/` has
95 `.c` files in it.

Twelve of them get excluded:

- `doomgeneric_{allegro,emscripten,linuxvt,sdl,soso,sosox,win,
  xlib}.c` — eight pre-shipped platform backends. They get
  replaced with `userspace/doom/doomgeneric_osdev.c`.
- `i_sdlsound.c`, `i_sdlmusic.c`, `i_allegrosound.c`,
  `i_allegromusic.c` — sound/music backends that pull in
  SDL_mixer or Allegro headers, neither of which ship here.
- `mus2mid.c` — a standalone MUS→MIDI converter that ships its
  own `main()`. Linking it would give a duplicate `main`
  symbol. The Doom engine itself doesn't reference any symbol
  from `mus2mid.c` so dropping it is safe.

That leaves **83 vendor TUs** to compile. Some of them
depend on each other in genuine cycles (`p_setup.c` calls
`r_main.c` calls `g_game.c` calls back into `p_setup.c`), so
the link line wraps them in `--start-group ... --end-group`
to let the linker iterate. This is the first time the book
has needed that flag — previous binaries got away with
`ar`'s natural ordering because they were small enough to be
acyclic.

---

## The platform shim

`userspace/doom/doomgeneric_osdev.c` is ~240 lines. It
implements the six `DG_*` callbacks against the window manager
(chapter 108d, `userspace/libgui/wmclient.h`), maps the WM's
key events onto Doom's `doomkeys.h` constants, and synthesises
the default argv.

### DG_Init — create one full-size window

```c
static struct wm_window g_win;
static int g_have_win;

void DG_Init(void)
{
    int rc = wm_create_window_input(
        DOOMGENERIC_RESX,    /* 640 */
        DOOMGENERIC_RESY,    /* 400 */
        0,                   /* default flags */
        "DOOM",
        &g_win);
    if (rc != 0 || g_win.fb.pixels == 0
                || g_win.fb.w != DOOMGENERIC_RESX
                || g_win.fb.h != DOOMGENERIC_RESY) {
        return;          /* leave g_have_win = 0; DG_DrawFrame
                          * silently no-ops */
    }
    /* Paint the framebuffer black so the user sees *something*
     * during the IWAD scan, rather than the WM's bg colour
     * showing through. */
    for (uint32_t y = 0; y < g_win.fb.h; y++) {
        uint32_t *row = (uint32_t *)((char *)g_win.fb.pixels
                                     + y * g_win.fb.stride);
        for (uint32_t x = 0; x < g_win.fb.w; x++) row[x] = 0;
    }
    wm_window_dirty(&g_win, 0, 0, g_win.fb.w, g_win.fb.h);
    g_have_win = 1;
}
```

Two things worth flagging:

- **The window resolution is fixed at 640×400** because
  `DG_ScreenBuffer` is a static array, not a pointer the shim
  can re-point on resize. Resize events are dropped in
  `pump_events()`; the shim just keeps painting 640×400 into
  the middle of whatever the WM allocated.

- **`wm_window_dirty` is a hint, not a side effect.** Chapter
  108d's compositor doesn't read the framebuffer until
  something marks it dirty. The black-fill loop has to be
  followed by `wm_window_dirty` or the user sees stale wallpaper
  for the whole IWAD-load delay.

### DG_DrawFrame — BGRA matches BGRA

```c
void DG_DrawFrame(void)
{
    if (!g_have_win) { pump_events(); return; }
    uint32_t *src = DG_ScreenBuffer;        /* DG's BGRA pixels */
    for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
        uint8_t *dst = (uint8_t *)g_win.fb.pixels
                     + (size_t)y * g_win.fb.stride;
        const uint8_t *s = (const uint8_t *)src
                         + (size_t)y * DOOMGENERIC_RESX * 4;
        /* Plain byte copy — see "pixel format" below. */
        for (size_t i = 0; i < DOOMGENERIC_RESX * 4; i++)
            dst[i] = s[i];
    }
    wm_window_dirty(&g_win, 0, 0, g_win.fb.w, g_win.fb.h);
    pump_events();
}
```

**Pixel format.** DoomGeneric defines its pixel union as:

```c
struct color { uint32_t b:8, g:8, r:8, a:8; };
```

On little-endian (which AArch64 is), the bitfield's bit-0 is
the low byte in memory. So the in-memory byte order is
`B, G, R, A` — identical to the window manager's framebuffer
(chapter 108d's `wmclient.h` documents BGRA). No swizzle, just
a row copy.

(If the layouts didn't match you'd have an extra `dst[i*4+0] =
src[i*4+2]` shuffle per pixel × 256 000 pixels × 35 fps = 9 M
extra mem-bytes a second on the hot path. Worth checking
before writing Doom-on-anything.)

The row copy walks bytes one at a time. `DG_DrawFrame` isn't
the bottleneck — the renderer itself does more work, and the
compositor is throttled to 60 Hz — so the simple loop is
fine.

### DG_GetKey and the press+release synthesis

The window manager (chapter 30 input multiplexing, refined in
108d) currently only sends *press* events on the bus.
Release-detection requires keyboard scan-code tracking that
hasn't been wired into the WM yet (it's a chapter for later).

Doom's input model expects matched press/release pairs:
WASD is "hold to move", so without a release Doom thinks you
have a foot welded to the W key.

The workaround is in `pump_events()`:

```c
case GUI_EVENT_KEY: {
    unsigned char dk = gui_to_doom(e.arg0);
    if (dk) {
        kq_push(1, dk);   /* press */
        kq_push(0, dk);   /* synthetic release */
    }
    break;
}
```

Every keypress immediately enqueues its own release. Doom
behaves as if every key is a "tap" — fire, use, weapon-switch,
menu navigation all work fine. The "hold W to walk" case is
broken (you walk one step per tap), but that's a temporary
limitation pending real release events from the WM. Documented;
the test doesn't exercise gameplay.

### DG_SleepMs and DG_GetTicksMs

Trivial:

```c
void DG_SleepMs(uint32_t ms)         { sleep_ms(ms); }
uint32_t DG_GetTicksMs(void)         { return (uint32_t)uptime_ms(); }
```

`sleep_ms` (chapter 29) rounds up to a scheduler tick (100 ms).
Doom asks for `1` ms a lot; it gets 100 ms. Frame rate caps at
~10 Hz in this build. Chapter 130b will introduce a finer
sleep wrapper (cycle counter? hrtimer?); for now the slow tick
is fine for a "does it run" demo.

`uptime_ms` is already 1 ms-resolution (chapter 21).

### Default argv

If the user just types `doom` at the shell, there's no `-iwad`.
The shim notices and synthesises one:

```c
int main(int argc, char **argv)
{
    static char *defaults[] = {
        "doom", "-iwad", "/data/wads/doom1.wad", "-mb", "6", 0
    };
    int seen_iwad = 0;
    for (int i = 1; i < argc; i++)
        if (argv[i] && argv[i][0] == '-'
            && argv[i][1] == 'i' && argv[i][2] == 'w')
            { seen_iwad = 1; break; }
    if (!seen_iwad) { argc = 5; argv = defaults; }
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}
```

`-mb 6` sets the memory zone size to 6 MiB, comfortably above
the default. The OS gives every process 8 GiB of address space
(chapter 15), so there's plenty of headroom.

`doom1.wad` is the id Software shareware WAD. **It is not
bundled** — see "On WAD licensing" below.

---

## What the libc was missing

Compiling the 83 vendor TUs surfaces gap after gap. Each one
is a two-or-three-line addition; none requires a kernel
change.

### `<strings.h>` (POSIX legacy)

`doomtype.h` uses `strcasecmp`. POSIX puts that in `<strings.h>`
(with an `s`), which hasn't shipped yet. New file
`userspace/libc/strings.h`:

```c
#ifndef USERSPACE_LIBC_STRINGS_H
#define USERSPACE_LIBC_STRINGS_H
#include "string.h"                    /* strcasecmp lives here */
#include <stddef.h>
static inline void bzero(void *p, size_t n)      { memset(p, 0, n); }
static inline void bcopy(const void *s, void *d, size_t n)
                                                 { memmove(d, s, n); }
#endif
```

`strcasecmp` / `strncasecmp` themselves are added to
`userspace/libc/string.h` as static inlines.

### `<inttypes.h>` (PRI*/SCN* macros)

`doomtype.h` uses `PRIu32` etc. New file
`userspace/libc/inttypes.h` defining the standard set
(`PRId8`/`PRId16`/.../`PRIx64`, `PRIdPTR` / `PRIxPTR`, the
fast/least variants which are identical to the base types on
aarch64-elf, etc.). All trivial; pure preprocessor.

**Pitfall worth flagging up front.** The file's opening doc
comment originally reads:

```c
/* inttypes.h — PRI*/SCN* width specifiers ... */
```

The `*/` inside the descriptive text *closes* the comment.
Everything after that line gets parsed as C code, the
preprocessor sees garbage, and you get 144 errors out of a
single file. The rule:
never put `*/` inside a C comment body, even in human prose.
Use spaces or parentheses (`PRI / SCN`, `(PRI*)(SCN*)`).

### `<unistd.h>` and `<sys/types.h>`

`i_system.c` includes `<unistd.h>` (for `getpid`, `usleep`,
`isatty`); `i_video.c` includes `<sys/types.h>` (for `ssize_t`,
`time_t`, `pid_t`). The *functions* exist already — they live
in `syscall.h` — but the umbrella headers don't.

`unistd.h` is a one-line forward:

```c
#ifndef USERSPACE_LIBC_UNISTD_H
#define USERSPACE_LIBC_UNISTD_H
#include "syscall.h"
#include "sys/stat.h"
#endif
```

`sys/types.h` adds the typedef set. One pitfall to flag:
`syscall.h` already defines `off_t` and `time_t` as `int64_t`
elsewhere. If `sys/types.h` types those as `long long`, even
though `long long` and `int64_t` happen to be the same width
on aarch64-elf, GCC's type identity is *name-based*, not
size-based — you get "conflicting types for 'off_t'". Fix:
`sys/types.h` uses `int64_t` too, matching `syscall.h`.

### `strdup`, `calloc`, `realloc`, `atof`, `usleep`, `system`

All new. `strdup` and `atof` are simple loops in `cstring.c` /
`stdlib.h`. `calloc` and `realloc` go in `malloc.h` next to the
existing `malloc`/`free`. `usleep` rounds up to a `sleep_ms`
tick (the only sub-tick option until 130b). `system` is stubbed
to always return -1; the only callers are `i_system.c`'s
zenity-error-popup probe and an external-error-dialog launch,
both of which handle the -1 gracefully (no popup).

### `mkdir` grows a mode argument

POSIX is `int mkdir(const char *path, mode_t mode)`. The
existing wrapper was `int mkdir(const char *path)` — a
kernel-side simplification. DoomGeneric calls
`mkdir(path, 0777)`.

The fix is two characters in `syscall.h`:

```c
static inline int mkdir(const char *path, unsigned int mode)
{
    (void)mode;                                /* no perms yet */
    return (int)_svc1(SYS_MKDIR, (long)(uintptr_t)path);
}
```

…plus updating the two existing one-arg callers (`sh.c`,
`save_dialog.c`, `cookies.h`) to pass `0755` / `0700`. The
mode is ignored at the syscall layer until per-file permissions
land (no chapter scheduled for that yet).

### `perror`, `setbuf`, `remove`, `rename`

Added to `stdio.h` as static inlines. `remove` forwards to
`unlink`. `rename` doesn't have a kernel syscall (no
`SYS_RENAME` exists) — the stub sets `errno = ENOSYS` and
returns -1. Doom only uses `rename` for the savegame-write-and-
rename atomicity dance; with -1 it falls back to a direct write,
which is slightly less crash-safe but works.

---

## Build-system integration

`Makefile` additions, in three blocks.

### Block 1: source enumeration

```makefile
DOOM_VENDOR_EXCLUDES := \
  vendor/doomgeneric/src/doomgeneric_allegro.c \
  vendor/doomgeneric/src/doomgeneric_emscripten.c \
  vendor/doomgeneric/src/doomgeneric_linuxvt.c \
  vendor/doomgeneric/src/doomgeneric_sdl.c \
  vendor/doomgeneric/src/doomgeneric_soso.c \
  vendor/doomgeneric/src/doomgeneric_sosox.c \
  vendor/doomgeneric/src/doomgeneric_win.c \
  vendor/doomgeneric/src/doomgeneric_xlib.c \
  vendor/doomgeneric/src/i_sdlsound.c \
  vendor/doomgeneric/src/i_sdlmusic.c \
  vendor/doomgeneric/src/i_allegrosound.c \
  vendor/doomgeneric/src/i_allegromusic.c \
  vendor/doomgeneric/src/mus2mid.c

DOOM_VENDOR_SRCS := $(filter-out $(DOOM_VENDOR_EXCLUDES), \
                      $(wildcard vendor/doomgeneric/src/*.c))
DOOM_VENDOR_OBJS := $(patsubst vendor/doomgeneric/src/%.c, \
                      $(BUILD)/vendor/doomgeneric/%.o, $(DOOM_VENDOR_SRCS))

DOOM_OBJS := $(BUILD)/userspace/crt0.o \
             $(BUILD)/userspace/doom/doomgeneric_osdev.o \
             $(BUILD)/userspace/libc/setjmp.o \
             $(BUILD)/userspace/libc/cstring.o \
             $(BUILD)/userspace/libgui/wmclient.o \
             $(DOOM_VENDOR_OBJS)

DOOM_ELF      := $(BUILD)/userspace/doom/doom.elf
DOOM_STRIPPED := $(BUILD)/userspace/doom/doom.stripped.elf
```

### Block 2: relaxed warnings for vendor

The vendor sources don't pass the normal `-Wall -Wextra
-Werror` regime. (DoomGeneric was written in 1993 and has
been ported by many people who each disabled a different
warning set.) Use a custom `DOOM_VENDOR_CFLAGS` that keeps
the freestanding flags but adds `-Wno-` for every category
that fires:

```makefile
DOOM_VENDOR_CFLAGS := \
  -ffreestanding -nostdlib -nostartfiles \
  -mcpu=cortex-a72 \
  -fno-stack-protector -fno-pie -fno-pic \
  -fno-asynchronous-unwind-tables \
  -DNORMALUNIX \
  -I vendor/doomgeneric/src -I userspace/libc \
  -Wno-unused -Wno-sign-compare -Wno-missing-braces \
  -Wno-format -Wno-implicit-fallthrough \
  -Wno-misleading-indentation -Wno-array-bounds \
  -Wno-stringop-overflow -Wno-stringop-truncation \
  -Wno-discarded-qualifiers -Wno-pointer-sign \
  -Wno-int-conversion -Wno-incompatible-pointer-types \
  -Os -g -MMD -MP

$(BUILD)/vendor/doomgeneric/%.o: vendor/doomgeneric/src/%.c
	@mkdir -p $(@D)
	$(CC) $(DOOM_VENDOR_CFLAGS) -c $< -o $@
```

Do **not** add `-Wno-implicit-function-declaration`. That
warning catches every missing libc piece on first compile —
exactly the signal you want when porting upstream code. Each
implicit-declaration is a libc gap to close, not a warning to
suppress.

The shim itself (`userspace/doom/doomgeneric_osdev.o`) compiles
with the strict `USER_CFLAGS` plus a per-file `-I userspace/libc`,
because it's first-party code and deserves the full warning
set:

```makefile
$(BUILD)/userspace/doom/doomgeneric_osdev.o: \
        userspace/doom/doomgeneric_osdev.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -I userspace/libc -c $< -o $@
```

### Block 3: link line

```makefile
$(DOOM_ELF): $(DOOM_OBJS)
	@mkdir -p $(@D)
	$(LD) $(USER_LDFLAGS) -o $@ \
	      --start-group $(DOOM_OBJS) --end-group
```

The `--start-group ... --end-group` wrapping is **required**.
Doom has genuine circular dependencies between TUs:
`r_main.c → g_game.c → p_setup.c → r_main.c`. Without the
group, the linker walks the object list once; the second
mention of `R_RenderPlayerView` (called from `g_game.c` but
defined in `r_main.c`) goes unresolved because by then
`r_main.o` has been "consumed". With the group, the linker
iterates until either everything resolves or no progress is
made on a pass.

A nice property of `--start-group` is that it's free when there
are no cycles — the linker just goes around once and stops.
Every previous binary in the book could have used it safely;
the earlier chapters didn't because it wasn't needed.

---

## Pitfalls

### Pitfall — `*/` inside a comment body

**Symptom.** A single file produces 144 spurious errors
because everything past the doc comment is parsed as C.

**Cause.** The descriptive text inside `/* ... */` literally
contains the sequence `*/` (here, `PRI*/SCN*`).

**Fix.** Never write `*/` in prose. Use spaces or parens:
`PRI / SCN`, `(PRI*)(SCN*)`.

### Pitfall — `strlen` / `time` cross-guard between headers

**Symptom.** GCC errors with "static declaration follows
non-static" when a TU pulls in both `string.h` and
`syscall.h` (transitively via `z_zone.h` → `stdlib.h` →
`syscall.h`).

**Cause.** `string.h` declares `extern size_t strlen(...)`
(with the body in `cstring.c`). `syscall.h` defines
`static inline size_t strlen(...)` for its own internal use.
Same trap for `time`.

**Fix.** One-shot `#define OSDEV_<NAME>_PROVIDED` guards.
Each `static inline` in `syscall.h` checks the macro and
silences itself when defined. `cstring.c` sets both macros at
the top before any include, so its extern definitions win.

### Pitfall — multi-TU strong symbols in header-only libc files

**Symptom.** Doom's many TUs each want to define
`__cxa_finalize` and `environ` — multiple-definition errors
for every vendor `.o`.

**Cause.** `atexit.h` defines `__cxa_finalize` as a plain
function; `env.h` defines `char **environ` similarly. They
worked fine for apps that include `<stdlib.h>` from *one*
TU. Doom includes `<stdlib.h>` from dozens of TUs.

The obvious-but-wrong fix is `__attribute__((weak))` on the
two definitions. The linker happily picks one copy and the
doom build goes green — but `__cxa_finalize` reads a `static`
per-TU `g_atexit_fns[]` table, and the linker is now free to
keep a `__cxa_finalize` whose table was never written to by
any `atexit()` call. The regression sweep catches it
immediately: `test_atexit.py` drops from 11/11 to 5/11, with
every "handler ran" check failing.

**Fix.** Compile-time gate. Wrap the strong definitions of
`__cxa_finalize` (in `atexit.h`) and `environ` (in `env.h`)
in:

```c
#ifndef OSDEV_LIBC_NO_GLOBAL_DEFS
__attribute__((used))
void __cxa_finalize(void *dso_handle) { ... }
#endif
```

Single-TU apps (every binary in the book before this chapter)
don't define the macro and compile unchanged — exactly one
TU emits the strong copy. The Doom Makefile block adds
`-DOSDEV_LIBC_NO_GLOBAL_DEFS` to `DOOM_VENDOR_CFLAGS` so
vendor TUs *suppress* the strong definitions, and the shim
TU (`doomgeneric_osdev.c`, compiled *without* the macro) is
the single source of the real `__cxa_finalize` and `environ`.
Vendor TUs still get their per-TU `g_env_envv[]` arena and
`g_atexit_fns[]` table (unused; few KB each, accepted).

**Rule going forward.** Any future ported library that
compiles across many TUs MUST be built with
`-DOSDEV_LIBC_NO_GLOBAL_DEFS`, and the port MUST include a
single shim TU built without it.

### Pitfall — `mkdir` arity change ripples

**Symptom.** After widening `mkdir` to two arguments, the
build fails in `sh.c`, `save_dialog.c`, and `cookies.h`.

**Cause.** Those three callers were the only existing
in-tree users of `mkdir`. The new signature breaks them.

**Fix.** Update all three in the same commit to pass `0755`
(directories created by the shell) or `0700` (private state
directories like the cookie jar). The mode is ignored at the
syscall layer until per-file permissions land.

### Pitfall — `LOAD segment with RWX permissions` linker warning

**Symptom.** Linking `doom.elf` prints
`LOAD segment with RWX permissions`.

**Cause.** The linker script puts text+data+bss into one
segment to keep the page table simple. Every guest binary
since chapter 13 has produced this warning.

**Fix.** Nothing to do. Not a regression; not a security
issue (the guest has no separate code-vs-data permission
enforcement yet).

---

## On WAD licensing

The doom1.wad shareware file is freely *playable* — id
Software's 1993 distribution license explicitly grants that —
but redistributing it is a gray area that varies by
jurisdiction. It is not in the repo.

To play, drop your own copy at `assets/wads/doom1.wad` (or any
other Doom-engine WAD: `doom2.wad`, `tnt.wad`, `plutonia.wad`,
`heretic.wad`, `hexen.wad`) and rebuild. A future chapter
(130b) will stage the WAD into `/data/wads/` on the runtime
disk image via `mkosfs2.py`.

Without a WAD, `doom` runs, paints its window black, scans for
IWADs, and exits with:

```
Game mode indeterminate.  No IWAD file was found.  Try
specifying one with the '-iwad' parameter.
```

That's the regression test signature.

---

## What this unlocks

This chapter doesn't modify any existing app. Doom is itself
the new app, and demonstrates the union of:

- **Chapter 129 (FP/SIMD at EL0).** The renderer's
  `R_PointToAngle` table builder and `R_PointToDist` use
  `double` arithmetic at startup; the lighting tables in
  `r_things.c` use FP for shade-fade math. Without
  chapter 129's eager save/restore, `doom` would die with
  `EC=0x07` (SIMD/FP access trap) on the second context switch
  after `D_DoomMain`.

- **Chapter 128a (setjmp/longjmp).** Doom uses `setjmp` for
  the long-distance error escape from `I_Error`. With FP at
  EL0 enabled, `setjmp` correctly saves `d8..d15` per AAPCS64
  — code touched in 128a, observable for the first time here.

- **Chapter 128c (string/ctype).** Half of DoomGeneric is
  string parsing for the IWAD scanner and the config-file
  reader.

- **Chapter 128d (time).** The renderer ties its tick counter
  to `uptime_ms`; the demo recorder uses wall-clock `time(NULL)`
  for timestamps.

- **Chapter 128e (qsort/bsearch/getopt).** `qsort` is used by
  the WAD-lump table; `getopt` is internally re-implemented in
  `m_argv.c`, but it links against the libc version when a TU
  happens to reference the libc symbol via stdlib.h.

- **Chapter 108d (userspace WM).** Doom is the largest single
  consumer of the WM's `wm_create_window_input` /
  `wm_window_dirty` / `wm_poll_event` API. It exercises the
  full-window-dirty path 35 times a second.

### What gets exercised in tests

- **New:** `scripts/test_doom.py`. Boots the OS, runs `doom`
  with no args, expects to see the "Game mode indeterminate"
  banner (or any DOOM version banner, in case a WAD is present
  in a fork). Asserts no kernel panic, no EL0 sync abort, no
  hang. Picked up automatically by `scripts/sweep.sh` —
  total regression test count goes from 126 to 127.

- **Existing tests unchanged.** No regressions; the libc
  additions are all new surfaces. The two `mkdir` callers
  (`sh.c`, `save_dialog.c`, `cookies.h`) have been updated to
  pass a mode argument; their existing tests
  (`test_chapter85_subdirs.py`, `test_save_dialog.py`,
  `test_browser_cookies.py`) all still pass.

---

## What's next

- **130b — WAD staging.** Wire `mkosfs2.py` to install
  `assets/wads/*.wad` into `/data/wads/` so a user-provided
  shareware WAD lands on the runtime disk and `doom` reaches
  the title screen.
- **130c — gameplay smoke test.** Boot, type `doom`, wait for
  the title screen, screenshot via `qemu-img convert`, verify
  the red `DOOM` pixels are present.
- **131 — `libgcc` runtime.** Once Doom runs, move on to the
  *compile-Doom-from-source-inside-the-guest* arc, which
  needs the libgcc helpers (soft-float, 128-bit divides,
  `__cxa_*` C++ glue that future C++ ports would want).

---

## Things to remember

- **Real upstream code finds every gap.** The 128-series
  chapters all had their own tests and all passed. Doom
  surfaces ten more libc pieces in a single port. There is no
  substitute for running real software.

- **`*/` in a comment is silent until it's catastrophic.** And
  `__attribute__((weak))` is sneaky in the other direction —
  multiple-definition errors at link time are loud, but
  "linker silently picks the wrong copy of a function that
  reads per-TU static state" only shows up when behaviour
  diverges. The `weak` keyword is a tool for situations where
  every copy is interchangeable; it is *not* a generic
  workaround for multi-TU definition collisions. Use a
  compile-time gate (`#ifndef OSDEV_LIBC_NO_GLOBAL_DEFS`) when
  you need exactly-one-strong-definition semantics.

- **Type identity is by name.** The silent `long long` vs
  `int64_t` errors from two headers typing the same logical
  type differently are easy to miss. Always match types by
  name, not by hand-wave equivalence.

- **`--start-group` is free.** Use it on every link from now on
  if there's any chance of cycles; the cost is one extra linker
  pass that almost always terminates immediately.

- **The platform-shim pattern works.** DoomGeneric reduces
  Doom to six callbacks. The shim is 240 lines; everything else
  is portable. If a future chapter wants to port another big
  upstream program (sqlite? lua? a music player?), a similar
  "vendor + thin shim" structure is the right shape.
