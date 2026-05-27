/*
 * kernel/core/strace.c — chapter 102 syscall tracer ring.
 *
 * See strace.h for the design.  This file is small on purpose:
 * the entire "tracer" is a ring buffer + a textual formatter,
 * with the actual tracepoints living one-and-a-half levels up
 * in svc_dispatch (kernel/core/syscall.c).  Decoding (turning
 * "syscall 5 = ('/proc/uptime', 0)" into the human-readable
 * `open("/proc/uptime", 0)`) is split across the two layers:
 *   - this file maps syscall_no -> name + arity.
 *   - userspace /bin/strace just relays what we render.
 * The kernel never reads user-mode strings for the trace; we
 * print pointer args as 0x%lx and let userspace render them
 * if it ever grows a -s option.
 */

#include "strace.h"
#include "thread.h"
#include "heap.h"
#include "timer.h"
#include "syscall.h"
#include "../arch/cpu.h"
#include <stddef.h>
#include <stdint.h>

/* TICK_INTERVAL_MS lives in timer.h. */

/* ------------------------------------------------------------------
 * Allocation + lifecycle.
 * ------------------------------------------------------------------ */

int strace_enable(struct thread *t)
{
    if (!t) return -1;
    if (t->strace) return 0;
    struct strace_ring *r = (struct strace_ring *)kmalloc(sizeof(*r));
    if (!r) return -1;
    /* Explicit field init — `= {0}` on a 2 KiB struct trips the
     * implicit-memset trap from freestanding-c-memset-trap. */
    r->head = 0;
    r->tail = 0;
    r->lost = 0;
    for (int i = 0; i < STRACE_RING_CAP; i++) {
        r->entries[i].completed = 0;
        r->entries[i].syscall_no = 0;
        r->entries[i].ret = 0;
        r->entries[i].ts_ms = 0;
        for (int j = 0; j < 6; j++) r->entries[i].args[j] = 0;
    }
    t->strace = r;
    return 0;
}

void strace_release(struct thread *t)
{
    if (!t || !t->strace) return;
    kfree(t->strace);
    t->strace = NULL;
}

/* ------------------------------------------------------------------
 * Hot path: called from svc_dispatch before every syscall.
 * ------------------------------------------------------------------ */

struct strace_entry *strace_enter(struct thread *t,
                                  uint32_t syscall_no,
                                  uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5)
{
    if (!t || !t->strace) return NULL;
    struct strace_ring *r = t->strace;
    uint32_t idx = r->head & (STRACE_RING_CAP - 1);
    struct strace_entry *e = &r->entries[idx];

    /* Overwrite-on-full: if the next write would lap the
     * reader, drop the oldest entry by advancing tail and
     * counting the loss.  Because the traced thread is the
     * only writer and is mid-dispatch (cannot re-enter), the
     * head/tail bump is safe without a lock for single-CPU
     * traces.  /proc/<pid>/trace readers run on other CPUs;
     * they only ever advance `tail`, never `head`, so the
     * worst they cause is a missed entry. */
    if (r->head - r->tail >= STRACE_RING_CAP) {
        r->lost++;
        r->tail++;
    }

    e->ts_ms      = timer_ticks() * (uint64_t)TICK_INTERVAL_MS;
    e->syscall_no = syscall_no;
    e->args[0]    = a0;
    e->args[1]    = a1;
    e->args[2]    = a2;
    e->args[3]    = a3;
    e->args[4]    = a4;
    e->args[5]    = a5;
    e->ret        = 0;
    e->completed  = 0;
    r->head++;
    return e;
}

/* ------------------------------------------------------------------
 * Per-syscall metadata — name + arity (number of args we render).
 *
 * Anything not listed here renders as `syscall_<n>(...)` with all
 * 6 args; that path stays useful when we add new syscall numbers
 * without immediately updating this table.  Keep the table sorted
 * by syscall number for ease of audit; the lookup is a linear
 * scan (~50 entries, called only from the renderer which is
 * already O(ring size)).
 * ------------------------------------------------------------------ */

struct syscall_meta {
    uint32_t    no;
    int         arity;        /* 0..6 */
    const char *name;
};

static const struct syscall_meta SYSCALL_META[] = {
    {  1, 3, "write"        },
    {  2, 1, "exit"         },
    {  3, 0, "getpid"       },
    {  4, 0, "yield"        },
    {  5, 2, "open"         },
    {  6, 3, "read"         },
    {  7, 1, "close"        },
    {  8, 2, "spawn"        },
    {  9, 1, "wait"         },
    { 10, 2, "getargs"      },
    { 11, 1, "sbrk"         },
    { 12, 4, "listdir"      },
    { 13, 0, "uptime_ms"    },
    { 14, 1, "chdir"        },
    { 15, 2, "getcwd"       },
    { 16, 3, "getenv"       },
    { 17, 2, "setenv"       },
    { 18, 1, "unsetenv"     },
    { 19, 2, "getenv_all"   },
    { 20, 3, "spawn_redir"  },
    { 21, 1, "sleep_ms"     },
    { 22, 1, "pipe"         },
    { 23, 2, "dup2"         },
    { 24, 4, "spawn_pipe"   },
    { 25, 1, "unlink"       },
    { 26, 1, "tty_raw"      },
    { 27, 2, "kill"         },
    { 28, 1, "set_fg_pid"   },
    { 29, 0, "fork"         },
    { 30, 2, "execv"        },
    { 31, 3, "sigaction"    },
    { 32, 1, "sigreturn"    },
    { 33, 3, "waitpid"      },
    { 34, 2, "openpty"      },
    { 35, 1, "fsync"        },
    { 36, 1, "mkdir"        },
    { 37, 6, "listdir_at"   },
    { 40, 3, "gui_create_window"     },
    { 41, 1, "gui_destroy_window"    },
    { 42, 1, "gui_present"           },
    { 43, 1, "gui_fill_rect"         },
    { 44, 1, "gui_draw_text"         },
    { 45, 1, "gui_flush"             },
    { 46, 1, "gui_poll_event"        },
    { 47, 1, "gui_create_window_ex"  },
    { 48, 2, "gui_list_windows"      },
    { 49, 1, "gui_raise_window"      },
    { 50, 2, "gui_get_screen_size"   },
    { 51, 2, "gui_set_minimized"     },
    { 60, 2, "socket_connect"        },
    { 61, 1, "socket_state"          },
    { 62, 1, "socket_shutdown"       },
    { 63, 2, "resolve"               },
    { 70, 6, "mmap"                  },
    { 71, 2, "munmap"                },
    { 72, 4, "clone"                 },
    { 73, 2, "futex_wait"            },
    { 74, 2, "futex_wake"            },
    { 75, 5, "clone2"                },
    { 76, 0, "getcpu"                },
    { 77, 1, "clone3"                },
    { 78, 1, "gettimeofday"          },
    { 79, 2, "beep"                  },
    { 80, 0, "trace_me"              },
};
#define SYSCALL_META_COUNT \
    ((int)(sizeof(SYSCALL_META) / sizeof(SYSCALL_META[0])))

static const struct syscall_meta *meta_for(uint32_t no)
{
    for (int i = 0; i < SYSCALL_META_COUNT; i++) {
        if (SYSCALL_META[i].no == no) return &SYSCALL_META[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * Tiny formatter.  Mirrors the pf_* helpers in procfs.c — we
 * deliberately do not share them via a header to keep procfs
 * standalone, but the contract is identical (`buf[cap]` writer
 * with a cursor that saturates at cap-1).
 * ------------------------------------------------------------------ */

static void sf_putc(char *buf, size_t cap, size_t *pos, char c)
{
    if (*pos + 1 < cap) buf[(*pos)++] = c;
}

static void sf_puts(char *buf, size_t cap, size_t *pos, const char *s)
{
    while (*s) sf_putc(buf, cap, pos, *s++);
}

static void sf_putu(char *buf, size_t cap, size_t *pos, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) { sf_putc(buf, cap, pos, '0'); return; }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0) sf_putc(buf, cap, pos, tmp[n]);
}

static void sf_puti(char *buf, size_t cap, size_t *pos, int64_t v)
{
    if (v < 0) {
        sf_putc(buf, cap, pos, '-');
        /* Cast via uint64_t covers INT64_MIN without UB. */
        sf_putu(buf, cap, pos, (uint64_t)(-v));
    } else {
        sf_putu(buf, cap, pos, (uint64_t)v);
    }
}

static void sf_puthex(char *buf, size_t cap, size_t *pos, uint64_t v)
{
    sf_putc(buf, cap, pos, '0');
    sf_putc(buf, cap, pos, 'x');
    /* Print all 16 nibbles for ease of grep — strace output isn't
     * fmt-aligned the way ps is, but having every pointer the same
     * width makes vertical reading easier. */
    static const char HEX[] = "0123456789abcdef";
    for (int s = 60; s >= 0; s -= 4)
        sf_putc(buf, cap, pos, HEX[(v >> s) & 0xF]);
}

/* Choose hex vs decimal for an argument value.  Small values
 * (fds, signals, small lengths) are easier to read in decimal;
 * pointer-shaped values get hex.  Heuristic: anything that
 * looks like a user VA (>= 0x10000) renders as hex. */
static void sf_putarg(char *buf, size_t cap, size_t *pos, uint64_t v)
{
    if (v >= 0x10000ULL) sf_puthex(buf, cap, pos, v);
    else                 sf_putu (buf, cap, pos, v);
}

static void sf_put_secs_cs(char *buf, size_t cap, size_t *pos, uint64_t ms)
{
    sf_putu(buf, cap, pos, ms / 1000);
    sf_putc(buf, cap, pos, '.');
    uint64_t cs = (ms % 1000) / 10;
    if (cs < 10) sf_putc(buf, cap, pos, '0');
    sf_putu(buf, cap, pos, cs);
}

/* Render one entry as a textual line:
 *   "<sec.cs> <name>(<a0>, <a1>, ...) = <ret>\n"
 * If the entry is incomplete (the syscall hasn't returned yet,
 * the typical case for the read() that triggered the render),
 * the suffix is "= ?" instead of an integer. */
static void render_entry(char *out, size_t cap, size_t *pos,
                         const struct strace_entry *e)
{
    sf_put_secs_cs(out, cap, pos, e->ts_ms);
    sf_putc(out, cap, pos, ' ');

    const struct syscall_meta *m = meta_for(e->syscall_no);
    int arity;
    if (m) {
        sf_puts(out, cap, pos, m->name);
        arity = m->arity;
    } else {
        sf_puts(out, cap, pos, "syscall_");
        sf_putu(out, cap, pos, (uint64_t)e->syscall_no);
        arity = 6;
    }
    sf_putc(out, cap, pos, '(');
    for (int i = 0; i < arity; i++) {
        if (i) { sf_putc(out, cap, pos, ','); sf_putc(out, cap, pos, ' '); }
        sf_putarg(out, cap, pos, e->args[i]);
    }
    sf_putc(out, cap, pos, ')');
    sf_puts(out, cap, pos, " = ");
    if (e->completed) {
        sf_puti(out, cap, pos, e->ret);
    } else {
        sf_putc(out, cap, pos, '?');
    }
    sf_putc(out, cap, pos, '\n');
}

long strace_render_and_drain(struct thread *t, char *out, size_t cap)
{
    size_t pos = 0;
    if (!t || !t->strace) {
        sf_puts(out, cap, &pos, "(not traced)\n");
        if (cap > 0) out[pos < cap ? pos : cap - 1] = '\0';
        return (long)pos;
    }
    struct strace_ring *r = t->strace;

    /* Snapshot head once; we'll drain up to this point.  Anything
     * the traced thread writes after we sampled `head` survives
     * for the next render. */
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    uint32_t lost = r->lost;

    if (lost) {
        sf_puts(out, cap, &pos, "(");
        sf_putu(out, cap, &pos, (uint64_t)lost);
        sf_puts(out, cap, &pos, " entries lost)\n");
        r->lost = 0;
    }

    while (tail != head) {
        const struct strace_entry *e =
            &r->entries[tail & (STRACE_RING_CAP - 1)];
        /* Stop if we'd overflow the output buffer mid-entry —
         * leave the remaining entries in the ring for the next
         * render.  Worst case the user's `cat` loop just takes
         * one more iteration to drain. */
        if (cap > 256 && pos + 256 > cap) break;
        render_entry(out, cap, &pos, e);
        tail++;
    }
    /* Drain whatever we actually rendered. */
    r->tail = tail;

    if (cap > 0) out[pos < cap ? pos : cap - 1] = '\0';
    return (long)pos;
}
