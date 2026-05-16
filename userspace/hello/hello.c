/* userspace/hello/hello.c — first user program (printf-clean).
 *
 * The whole thing is a few lines of C against the inline-asm
 * syscall wrappers in libc/syscall.h plus the header-only printf.
 * No globals beyond what the heap brings in transitively.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

int main(void)
{
    printf("hello from EL0!\n");
    printf("pid=0x%08x\n", getpid());

    /* Cooperative yield to demonstrate the round-trip works. */
    yield();
    printf("after yield, still alive\n");

    return 0;
}

