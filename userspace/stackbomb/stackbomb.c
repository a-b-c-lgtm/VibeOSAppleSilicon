/* userspace/stackbomb/stackbomb.c — chapter 103 guard-page test.
 *
 * Deliberately recurses with a fat local until the user stack
 * runs out.  Used by scripts/test_stackbomb.py to confirm the
 * kernel's guard-page detection produces the friendly
 * "[svc] user stack overflow" diagnostic rather than the
 * generic "non-SVC sync exception" register dump.
 *
 * Math at chapter-101 defaults:
 *   USER_STACK_PAGES = 16   -> 64 KiB usable stack
 *   per frame: ~256 B local + ~32 B saved x29/x30/etc -> ~288 B
 *   frames to exhaust:      ~228
 *
 * recurse() must not be tail-call-optimised — we want a real
 * frame per call so the SP marches monotonically downward.
 * The `volatile` on `fat` and the `(void)fat[0]` after the
 * recursive call defeat TCO at -O0/-O2 alike.
 *
 * Output: a single one-liner printed before the dive, so the
 * test harness can find the test's slot in the boot log.
 * After the recursion runs out, the kernel kills us; we never
 * reach the "UNEXPECTED" line. */

#include "../libc/syscall.h"

static unsigned long g_depth = 0;

/* Deep recursion with a 256-byte volatile local.  The local is
 * touched at both ends so the optimiser cannot elide it.
 *
 * GCC's -Winfinite-recursion correctly spots that recurse()
 * never terminates.  That's the whole point of this test
 * program — we want the kernel guard page, not the C compiler,
 * to be the one that stops us.  Suppress the warning locally. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static void recurse(void)
{
    volatile char fat[256];
    fat[0]   = (char)g_depth;
    fat[255] = (char)(g_depth ^ 0xA5UL);
    g_depth++;
    recurse();
    /* Touch fat AFTER the recursive call so TCO cannot fire. */
    (void)fat[0];
}
#pragma GCC diagnostic pop

int main(void)
{
    write(1, "[stackbomb] about to overflow user stack\n", 41);
    recurse();
    /* Unreachable: the kernel killed us when recurse() poked
     * the guard page. */
    write(1, "[stackbomb] UNEXPECTED return from recurse()\n", 45);
    return 1;
}
