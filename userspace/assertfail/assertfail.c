/* userspace/assertfail/assertfail.c — chapter 128c regression
 * for the assert(0) -> __assert_fail -> exit(134) path.
 *
 * Test harness checks $? and asserts 134.  We also expect a
 * specific diagnostic line on stderr; the harness greps for it.
 */
#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/assert.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[assertfail] about to assert(0)\n");
    assert(2 + 2 == 5);
    printf("[assertfail] FAIL: assert returned\n");
    return 1;
}
