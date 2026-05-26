/* sys/wait.h — chapter 132f
 *
 * POSIX-shape <sys/wait.h> for cross-compiling gcc-14.  Our kernel
 * stores a thread's exit_code as a bare int: 0..127 for normal
 * exit(n), and 128+signum for signal-killed processes (see chapter
 * 77 sigaction notes).  These macros decode that encoding into the
 * shape gcc.cc / collect-utils.cc / gcc-ar.cc expect.
 */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H 1

#include "../syscall.h"   /* pid_t-equivalent int, wait(), waitpid(), WNOHANG */

/* Status-decode macros — operate on the int written through
 * waitpid()'s code_out pointer. */
#define WIFEXITED(s)    ((s) >= 0 && (s) < 128)
#define WEXITSTATUS(s)  ((s) & 0xff)
#define WIFSIGNALED(s)  ((s) >= 128 && (s) < 256)
#define WTERMSIG(s)     ((s) - 128)
#define WCOREDUMP(s)    (0)
#define WIFSTOPPED(s)   (0)
#define WSTOPSIG(s)     (0)
#define WIFCONTINUED(s) (0)

#ifndef WUNTRACED
#define WUNTRACED  2
#endif
#ifndef WCONTINUED
#define WCONTINUED 4
#endif

#endif /* _SYS_WAIT_H */
