/* userspace/bf/bf.c — brainfuck interpreter.
 *
 * Chapter 188: the "medium real program" that proves /bin/gcc
 * works on real (i.e. non-trivial, non-hello-world) C source.
 *
 * Brainfuck has exactly eight instructions:
 *   >  move data pointer right
 *   <  move data pointer left
 *   +  increment cell at data pointer
 *   -  decrement cell at data pointer
 *   .  write byte at data pointer to stdout
 *   ,  read one byte from stdin into cell at data pointer
 *   [  if cell == 0, jump to matching ]
 *   ]  if cell != 0, jump back to matching [
 *
 * All other bytes in the program are skipped.  The tape is 30000
 * cells of uint8_t, classic 1993 spec.
 *
 * Why this is the chapter-132h smoke test:
 *
 *   - It is real upstream code (BF interpreters exist in every
 *     language; this one is the standard textbook implementation,
 *     hand-written for the chapter but algorithmically identical
 *     to any other).  No printf-hello-world cheating.
 *   - It exercises argv, file open/read/close, stdin read,
 *     stdout write, heap (the bracket-jump table is malloc'd),
 *     and a tight inner loop the optimizer actually has to look
 *     at.  That's enough surface to break the cross-compile
 *     pipeline in a useful way if anything is bent.
 *   - It produces a deterministic, byte-exact output that the
 *     test harness can diff against a known-good string.
 *
 * Freestanding by choice: bf.c declares the few libc symbols it
 * needs (open, read, write, close, exit, malloc, free, strlen,
 * memset) as `extern`, instead of including any libc headers.
 * Two reasons:
 *
 *   1. In-guest /bin/gcc has no system include directory wired
 *      up (chapter 187's "what we did NOT do").  Shipping the
 *      libc headers on the disk would push the OSFS-1 file
 *      count past its 128-entry directory cap; that ABI bump
 *      is its own chapter, not this one's problem.
 *
 *   2. Every symbol bf needs is already a real (non-inline)
 *      definition in /bin/libosdevc.a — so default-spec linking
 *      resolves them without any header gymnastics.  The whole
 *      chain (gcc driver → cc1 → as → ld → libosdevc.a → ELF)
 *      is exactly what we want to validate end-to-end.
 *
 * Build path: see userspace/bf in the Makefile.  Same crt0 +
 * linker_user.ld + libosdevc.a as every other userspace binary.
 *
 * Test path: scripts/test_gcc_bf.py boots, runs the host-built
 * /bin/bf against a fixed program, then recompiles bf.c with
 * /bin/gcc inside the guest and checks the rebuilt binary
 * produces identical output.
 */

/* --- libc symbols we link against from libosdevc.a -------------- */

typedef unsigned long       size_t;
typedef long                ssize_t;
typedef unsigned char       uint8_t;
typedef unsigned long       uintptr_t;
typedef long                off_t;

extern int   open  (const char *path, int flags, int mode);
extern int   close (int fd);
extern ssize_t read  (int fd, void *buf, size_t n);
extern ssize_t write (int fd, const void *buf, size_t n);
extern void  exit  (int status);
extern void *malloc(size_t n);
extern void  free  (void *p);
extern void *memset(void *p, int c, size_t n);
extern size_t strlen(const char *s);

/* O_RDONLY: classic value, matches userspace/libc/fcntl.h. */
#define O_RDONLY 0

/* --- tiny output helpers (no printf in libosdevc.a) ------------- */

static void puts_raw(const char *s)
{
    write(2, s, strlen(s));
}

static void put_dec(unsigned long v)
{
    char buf[24];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    else {
        char tmp[24];
        int m = 0;
        while (v) { tmp[m++] = '0' + (char)(v % 10); v /= 10; }
        while (m) { buf[n++] = tmp[--m]; }
    }
    write(2, buf, (size_t)n);
}

static void die(const char *msg)
{
    puts_raw("bf: ");
    puts_raw(msg);
    puts_raw("\n");
    exit(1);
}

/* --- the interpreter ------------------------------------------- */

/* Standard 30 KiB tape (Müller 1993). */
#define TAPE_CELLS 30000

/* Programs are loaded into a heap buffer; we cap at 1 MiB which
 * is well above any hand-written BF program and still cheap on
 * a 256 MiB disk. */
#define MAX_PROG   (1 * 1024 * 1024)

static unsigned char *load_program(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { die("cannot open program"); }
    unsigned char *buf = (unsigned char *)malloc(MAX_PROG);
    if (!buf) { die("out of memory loading program"); }
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, MAX_PROG - total);
        if (n < 0) { die("read error"); }
        if (n == 0) break;
        total += (size_t)n;
        if (total >= MAX_PROG) { die("program too large"); }
    }
    close(fd);
    *out_len = total;
    return buf;
}

/* Precompute jump targets so [/] don't have to scan at runtime.
 * jump[i] = index of the matching bracket for the bracket at i,
 * or -1 if position i is not a bracket. */
static long *compute_jumps(const unsigned char *prog, size_t n)
{
    long *jump = (long *)malloc(sizeof(long) * n);
    if (!jump) { die("out of memory for jump table"); }
    for (size_t i = 0; i < n; i++) { jump[i] = -1; }

    /* Stack of unmatched '[' positions. */
    long *stack = (long *)malloc(sizeof(long) * n);
    if (!stack) { die("out of memory for bracket stack"); }
    long top = 0;

    for (size_t i = 0; i < n; i++) {
        if (prog[i] == '[') {
            stack[top++] = (long)i;
        } else if (prog[i] == ']') {
            if (top == 0) { die("unmatched ']'"); }
            long open_pos = stack[--top];
            jump[open_pos] = (long)i;
            jump[i] = open_pos;
        }
    }
    if (top != 0) { die("unmatched '['"); }
    free(stack);
    return jump;
}

static void run(const unsigned char *prog, size_t n, const long *jump)
{
    unsigned char tape[TAPE_CELLS];
    memset(tape, 0, sizeof(tape));
    size_t dp = 0;     /* data pointer */
    size_t ip = 0;     /* instruction pointer */

    while (ip < n) {
        unsigned char op = prog[ip];
        switch (op) {
        case '>':
            dp++;
            if (dp >= TAPE_CELLS) { die("tape pointer over right edge"); }
            break;
        case '<':
            if (dp == 0) { die("tape pointer under left edge"); }
            dp--;
            break;
        case '+':
            tape[dp] = (unsigned char)(tape[dp] + 1);
            break;
        case '-':
            tape[dp] = (unsigned char)(tape[dp] - 1);
            break;
        case '.': {
            unsigned char c = tape[dp];
            write(1, &c, 1);
            break;
        }
        case ',': {
            unsigned char c = 0;
            ssize_t r = read(0, &c, 1);
            if (r <= 0) {
                /* EOF on stdin → leave cell unchanged (one of
                 * the two common BF conventions; matches bff
                 * and dbfi). */
            } else {
                tape[dp] = c;
            }
            break;
        }
        case '[':
            if (tape[dp] == 0) { ip = (size_t)jump[ip]; }
            break;
        case ']':
            if (tape[dp] != 0) { ip = (size_t)jump[ip]; }
            break;
        default:
            /* Comment byte; skip. */
            break;
        }
        ip++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts_raw("usage: bf <program.bf>\n");
        return 1;
    }

    size_t n = 0;
    unsigned char *prog = load_program(argv[1], &n);

    /* Brief startup report on stderr so the test harness can see
     * the loader path is alive even when stdout is consumed by
     * the program's own output. */
    puts_raw("bf: loaded ");
    put_dec((unsigned long)n);
    puts_raw(" bytes from ");
    puts_raw(argv[1]);
    puts_raw("\n");

    long *jump = compute_jumps(prog, n);
    run(prog, n, jump);

    free(jump);
    free(prog);
    return 0;
}
