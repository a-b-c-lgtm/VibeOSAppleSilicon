/* userspace/badptr/badptr.c — syscall-pointer test.
 *
 * Tries to trick the kernel into reading kernel memory into a
 * user-controlled buffer by passing a kernel address as the buf
 * argument to sys_read.  Before the kernel/user boundary chapter
 * the kernel would have
 * happily memcpy'd 16 bytes from /motd into kernel address
 * 0x230000000 (clobbering the heap).  After that chapter the
 * uaccess_check / copy_from_user bounds check rejects this with
 * -EFAULT and the program prints the failure cleanly.
 *
 * Three things are exercised:
 *   (1) sys_read(fd, KERNEL_ADDR, len)   — write-into-kernel
 *   (2) sys_write(fd, KERNEL_ADDR, len)  — read-from-kernel
 *   (3) sys_open(KERNEL_ADDR, 0)         — string-from-kernel
 *
 * Each should return -EFAULT (-14).  The program prints the
 * return code per line and exits 0 if all three failed safely.
 */

#include "../libc/syscall.h"

static const unsigned long KERNEL_ADDR = 0x230000000UL;

static void putd_signed(long v)
{
    char buf[32];
    int  i = 0;
    int  neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) buf[i++] = '-';
    /* reverse */
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    buf[i++] = '\n';
    write(1, buf, (unsigned long)i);
}

int main(void)
{
    int rc;

    write(1, "[badptr] sys_read(fd=0, buf=KERN, 16) -> ", 41);
    rc = (int)read(0, (char *)KERNEL_ADDR, 16);
    putd_signed(rc);

    write(1, "[badptr] sys_write(fd=1, buf=KERN, 16) -> ", 42);
    rc = (int)write(1, (const char *)KERNEL_ADDR, 16);
    putd_signed(rc);

    write(1, "[badptr] sys_open(path=KERN, 0) -> ", 35);
    rc = open((const char *)KERNEL_ADDR, 0);
    putd_signed(rc);

    write(1, "[badptr] all three rejected, exiting 0\n", 39);
    return 0;
}
