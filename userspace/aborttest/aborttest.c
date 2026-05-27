/* userspace/aborttest/aborttest.c — chapter 166 regression for
 * abort().  Must terminate with exit code 134 (128 + SIGABRT).
 *
 * The test harness runs us via the shell and then checks $?,
 * so the assertion is "the shell prints 134".  We don't print
 * anything on the success path because abort() takes us out
 * before exit() with a return value would.  Any output means
 * something went wrong.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/signal.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[aborttest] about to abort\n");
    abort();
    /* Unreachable per C99 7.20.4.1 -- abort() must not return. */
    printf("[aborttest] FAIL: abort() returned\n");
    return 1;
}
