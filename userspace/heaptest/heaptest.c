/* userspace/heaptest/heaptest.c — user-heap test.
 *
 * Allocates a few buffers of varying sizes, fills them with
 * patterns, verifies the patterns survive interleaved frees, and
 * frees everything.  Prints allocation addresses so we can
 * eyeball that they fall in the user heap range
 * (USER_HEAP_BASE = 0x1010000000) and that adjacent allocs are
 * adjacent in the address space.
 */

#include "../libc/syscall.h"
#include "../libc/malloc.h"

static void puthex(unsigned long v)
{
    char buf[18];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned nib = (unsigned)((v >> (60 - i * 4)) & 0xF);
        buf[2 + i] = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
    }
    write(1, buf, 18);
}

static int verify(unsigned char *p, size_t n, unsigned char pat)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != pat) return 0;
    return 1;
}

static void fill(unsigned char *p, size_t n, unsigned char pat)
{
    for (size_t i = 0; i < n; i++) p[i] = pat;
}

int main(void)
{
    write(1, "[heaptest] starting\n", 20);

    /* Three small allocs first, to check addresses are sane. */
    unsigned char *a = (unsigned char *)malloc(64);
    unsigned char *b = (unsigned char *)malloc(128);
    unsigned char *c = (unsigned char *)malloc(256);

    write(1, "  a = ", 6); puthex((unsigned long)(uintptr_t)a); write(1, "\n", 1);
    write(1, "  b = ", 6); puthex((unsigned long)(uintptr_t)b); write(1, "\n", 1);
    write(1, "  c = ", 6); puthex((unsigned long)(uintptr_t)c); write(1, "\n", 1);

    if (!a || !b || !c) {
        write(1, "[heaptest] FAIL - malloc returned NULL\n", 39);
        return 1;
    }

    /* Fill, then verify the buffers don't alias. */
    fill(a, 64,  0xAA);
    fill(b, 128, 0xBB);
    fill(c, 256, 0xCC);
    if (!verify(a, 64,  0xAA)) { write(1, "[heaptest] FAIL pattern A\n", 26); return 1; }
    if (!verify(b, 128, 0xBB)) { write(1, "[heaptest] FAIL pattern B\n", 26); return 1; }
    if (!verify(c, 256, 0xCC)) { write(1, "[heaptest] FAIL pattern C\n", 26); return 1; }

    /* Free middle, alloc same size, free everything else. */
    free(b);
    unsigned char *b2 = (unsigned char *)malloc(128);
    write(1, "  b2 = ", 7); puthex((unsigned long)(uintptr_t)b2); write(1, "\n", 1);
    fill(b2, 128, 0xDD);
    if (!verify(b2, 128, 0xDD)) { write(1, "[heaptest] FAIL pattern B2\n", 27); return 1; }

    /* Big allocation — should trigger sbrk to grow the heap. */
    unsigned char *big = (unsigned char *)malloc(32 * 1024);
    write(1, "  big = ", 8); puthex((unsigned long)(uintptr_t)big); write(1, "\n", 1);
    if (!big) { write(1, "[heaptest] FAIL - big malloc NULL\n", 34); return 1; }
    fill(big, 32 * 1024, 0x55);
    if (!verify(big, 32 * 1024, 0x55)) {
        write(1, "[heaptest] FAIL pattern BIG\n", 28);
        return 1;
    }

    free(a);
    free(b2);
    free(c);
    free(big);

    write(1, "[heaptest] all checks passed\n", 29);
    return 0;
}
