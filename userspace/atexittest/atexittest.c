/*
 * userspace/atexittest/atexittest.c — chapter-120 demo.
 *
 * Exercises every piece of the new bootstrap glue:
 *
 *   1. A global constructor (__attribute__((constructor)))
 *      → printed BEFORE main runs.  Proves crt0's
 *        __init_array walk works.
 *   2. main() registers two atexit handlers in order
 *      [exit1, exit2].
 *   3. main() returns 7.
 *   4. crt0 calls __cxa_finalize(NULL); the strong
 *      override from atexit.h runs the chain in LIFO
 *      order → prints "exit2" then "exit1".
 *   5. A global destructor (__attribute__((destructor)))
 *      runs from .fini_array LAST (after the atexit
 *      LIFO).  Prints "dtor".
 *   6. crt0 syscalls SYS_EXIT(7).
 *
 * Expected stdout:
 *
 *     ctor1
 *     ctor2
 *     main
 *     exit2
 *     exit1
 *     dtor
 *
 * scripts/test_atexit.py checks that exact sequence and
 * the exit code 7.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/atexit.h"

/* Two constructors prove that init_priority sort (or at
 * least source-order traversal) is stable — both must run,
 * and both must run before main.  We don't strictly require
 * a particular order between them; the test asserts both
 * lines are present before "main". */
static void __attribute__((constructor)) ctor1(void)
{
    printf("ctor1\n");
}

static void __attribute__((constructor)) ctor2(void)
{
    printf("ctor2\n");
}

static void exit1(void) { printf("exit1\n"); }
static void exit2(void) { printf("exit2\n"); }

static void __attribute__((destructor)) dtor(void)
{
    printf("dtor\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("main\n");
    atexit(exit1);
    atexit(exit2);
    return 7;
}
