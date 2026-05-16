/* userspace/cowtest/cowtest.c \u2014 chapter 75 copy-on-write smoke test.
 *
 * Verifies the lazy-clone path that replaced address_space_clone's
 * eager memcpy in sys_fork:
 *
 *   1. HEAP COW.  Pre-fork allocation, both halves see the
 *      sentinel.  Child writes a different value; parent's copy
 *      is unaffected.  Same end-state guarantee as forktest's
 *      check 3, but exercises a *larger* pre-fork heap (4 MiB)
 *      so we know it's the COW path doing the work, not eager
 *      copy.  If an eager copy ran here it would still pass
 *      semantically, but we'd burn 4 MiB of pmem on every fork.
 *
 *   2. STACK COW.  Sentinel on the parent's stack survives the
 *      fork; child writes a different value to its (private)
 *      stack page; parent's stack still holds the original after
 *      wait().  Catches the most subtle COW bug: missing the
 *      stack pages because they live in a different L2 slot from
 *      the heap.
 *
 *   3. KERNEL UACCESS COW.  After the fork, the parent calls
 *      waitpid() with a stack-allocated `code_out`; the kernel
 *      copy_to_user must pre-fault the COW page rather than
 *      trapping at EL1.  This is the single most subtle bug we
 *      hit during chapter 75 bring-up: AArch64 RO permissions
 *      apply to EL1 too, so a kernel write through a user VA
 *      that happens to point at a still-shared COW page faults
 *      in EL1 with nowhere good to land.  uaccess.c's
 *      copy_to_user now walks pages and forces a COW resolve
 *      via address_space_make_writable on each one.
 *
 *   4. PERFORMANCE PROXY.  fork()ing a process whose heap is
 *      8 MiB takes well under 200 ms (the pre-COW eager-copy
 *      path took 8s+ and would blow OOM on small RAM configs).
 *      We assert <200ms; on HVF it's typically ~5ms.
 *
 * On success prints `[cowtest] all checks passed`.  Any failure
 * prints `[cowtest] FAIL ...` and exits non-zero.
 */

#include "../libc/syscall.h"
#include "../libc/malloc.h"

static void say(const char *s)
{
    write(1, s, strlen(s));
}

static void say_ln(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

static void say_d(const char *prefix, long v)
{
    say(prefix);
    putd(v);
    write(1, "\n", 1);
}

int main(void)
{
    say_ln("[cowtest] starting");

    /* ------------------------------------------------------------
     * Check 1: heap COW.
     * ---------------------------------------------------------- */
    const size_t HEAP_BYTES = 4 * 1024 * 1024;     /* 4 MiB */
    unsigned char *heap = (unsigned char *)malloc(HEAP_BYTES);
    if (!heap) {
        say_ln("[cowtest] FAIL malloc(4 MiB) returned NULL");
        return 1;
    }
    /* Touch every page so the parent has resident pages to share.
     * Pattern is "0xAA" \u2014 the child's write will use 0x55 so
     * mismatches are visible. */
    for (size_t i = 0; i < HEAP_BYTES; i++) heap[i] = 0xAA;

    int pid = fork();
    if (pid < 0) {
        say_d("[cowtest] FAIL heap-cow fork -errno=", -pid);
        return 1;
    }
    if (pid == 0) {
        /* Child: confirm the sentinel survived the COW share. */
        for (size_t i = 0; i < HEAP_BYTES; i++) {
            if (heap[i] != 0xAA) {
                say_ln("[cowtest] FAIL child heap missing sentinel");
                exit(11);
            }
        }
        /* Now overwrite \u2014 each page write triggers a COW
         * resolve in the kernel.  Touch every page (write 0x55
         * to byte 0) so we know the COW handler ran 1024 times. */
        for (size_t i = 0; i < HEAP_BYTES; i += 4096) {
            heap[i] = 0x55;
        }
        /* Final byte too, to make sure last page is unshared. */
        heap[HEAP_BYTES - 1] = 0x55;
        exit(0);
    }
    int code = 0;
    int reaped = wait(&code);
    if (reaped != pid || code != 0) {
        say_d("[cowtest] FAIL heap-cow child reaped/code; reaped=", reaped);
        say_d("                                            code=", code);
        return 1;
    }
    /* Parent: every byte must STILL be 0xAA. */
    for (size_t i = 0; i < HEAP_BYTES; i++) {
        if (heap[i] != 0xAA) {
            say_d("[cowtest] FAIL parent heap clobbered at offset ", (long)i);
            return 1;
        }
    }
    free(heap);
    say_ln("[cowtest] check 1 (4 MiB heap COW) ok");

    /* ------------------------------------------------------------
     * Check 2: stack COW.
     * ---------------------------------------------------------- */
    volatile unsigned char stack_sentinel[128];
    for (int i = 0; i < 128; i++) stack_sentinel[i] = 0xC3;

    int pid2 = fork();
    if (pid2 < 0) {
        say_d("[cowtest] FAIL stack-cow fork -errno=", -pid2);
        return 1;
    }
    if (pid2 == 0) {
        /* Child: confirm + trample. */
        for (int i = 0; i < 128; i++) {
            if (stack_sentinel[i] != 0xC3) {
                say_ln("[cowtest] FAIL child stack missing sentinel");
                exit(21);
            }
        }
        for (int i = 0; i < 128; i++) stack_sentinel[i] = 0x3C;
        exit(0);
    }
    int code2 = 0;
    int reaped2 = wait(&code2);
    if (reaped2 != pid2 || code2 != 0) {
        say_d("[cowtest] FAIL stack-cow child reaped/code: ", reaped2);
        return 1;
    }
    /* Parent: original sentinel must still be there. */
    for (int i = 0; i < 128; i++) {
        if (stack_sentinel[i] != 0xC3) {
            say_ln("[cowtest] FAIL parent stack clobbered");
            return 1;
        }
    }
    say_ln("[cowtest] check 2 (stack COW) ok");

    /* ------------------------------------------------------------
     * Check 3: kernel uaccess into a COW stack page.
     *
     * waitpid()'s code_out argument is a pointer into the parent's
     * stack.  After fork, the parent's stack page that contains
     * the local `int code3` is COW-shared until the parent
     * actually writes to it (and a "spill local across a syscall"
     * write may not happen depending on register pressure).  When
     * the kernel resolves the wait and tries to write the exit
     * code via copy_to_user, the destination page must be COW-
     * unshared first \u2014 otherwise the kernel itself takes an
     * EL1 data abort and panics.  This check forces that path:
     * we deliberately do NOT touch any stack local between the
     * fork and the waitpid call.
     * ---------------------------------------------------------- */
    int pid3 = fork();
    if (pid3 < 0) {
        say_d("[cowtest] FAIL uaccess-cow fork -errno=", -pid3);
        return 1;
    }
    if (pid3 == 0) {
        /* Child exits with a recognisable code.  The kernel must
         * deposit this into the parent's `code3` local via
         * copy_to_user. */
        exit(42);
    }
    int code3;
    int reaped3 = waitpid(pid3, &code3, 0);
    if (reaped3 != pid3) {
        say_d("[cowtest] FAIL uaccess-cow reaped wrong pid: ", reaped3);
        return 1;
    }
    if (code3 != 42) {
        say_d("[cowtest] FAIL uaccess-cow wrong code: ", code3);
        return 1;
    }
    say_ln("[cowtest] check 3 (kernel uaccess into COW page) ok");

    /* ------------------------------------------------------------
     * Check 4: large-heap fork performance.
     *
     * Pre-COW (eager copy) this fork() would memcpy ~8 MiB and
     * take seconds on HVF, sometimes ENOMEM on small RAM configs.
     * With COW it's just page-table walking + descriptor twiddles
     * \u2014 well under 200ms.  We don't measure precise speedup,
     * just assert that fork() of a big-heap process completes
     * inside a generous wall-clock budget. */
    const size_t BIG = 8 * 1024 * 1024;          /* 8 MiB */
    unsigned char *big = (unsigned char *)malloc(BIG);
    if (!big) { say_ln("[cowtest] FAIL malloc(8 MiB)"); return 1; }
    /* Touch every page once. */
    for (size_t i = 0; i < BIG; i += 4096) big[i] = 0xEE;

    unsigned long t0 = uptime_ms();
    int pid4 = fork();
    unsigned long t1 = uptime_ms();
    if (pid4 < 0) {
        say_d("[cowtest] FAIL perf fork -errno=", -pid4);
        return 1;
    }
    if (pid4 == 0) {
        /* Child exits immediately \u2014 we only care about the
         * parent's return path latency. */
        exit(0);
    }
    int code4 = 0;
    waitpid(pid4, &code4, 0);

    unsigned long elapsed = t1 - t0;
    say_d("[cowtest] fork(8 MiB heap) took ms = ", (long)elapsed);
    if (elapsed > 200) {
        say_ln("[cowtest] FAIL fork too slow (>200 ms) \u2014 eager copy?");
        return 1;
    }
    free(big);
    say_ln("[cowtest] check 4 (large-heap fork latency) ok");

    say_ln("[cowtest] all checks passed");
    return 0;
}
