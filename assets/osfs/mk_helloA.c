/* mk_helloA.c -- chapter 133b make test fixture: one of two
 * source files that together produce a tiny binary the
 * expanded /bin/make is asked to build via pattern rule. */
#include <syscall.h>
#include <printf.h>

int hello_from_B(void);

int main(void)
{
    int n = hello_from_B();
    printf("hello A=%d\n", n);
    return 0;
}
