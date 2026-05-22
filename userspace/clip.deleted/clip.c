/*
 * userspace/clip/clip.c — chapter 108 CLI for the clipboard.
 *
 * Three subcommands, modelled on `xclip` / `pbcopy` / `pbpaste`:
 *
 *   clip set [text...]       store the args (joined with spaces).
 *                            If no args, read stdin until EOF.
 *   clip get                 write the current clipboard to stdout.
 *                            Exit 0 if non-empty, 1 if empty.
 *   clip gen                 print the current generation.  Useful
 *                            for "did anything change since I last
 *                            looked?" tests.
 *   clip clear               wipe the clipboard.
 *
 * Examples (run from /bin/sh):
 *
 *   $ clip set hello world
 *   $ clip get
 *   hello world
 *   $ clip gen
 *   1
 *   $ echo "from a pipe" | clip set
 *   $ clip get
 *   from a pipe
 *
 * The point of this tool is twofold:
 *
 *   1. End-user convenience: copy/paste between shell pipelines
 *      without leaving the terminal.
 *   2. Hermetic regression target: scripts/test_clipboard.py can
 *      drive a full round-trip from a single shell session
 *      without GUI-event injection.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"
#include "../libc/clipboard.h"

static size_t s_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0) && (*b == 0);
}

static void usage(void)
{
    printf("usage: clip set [text...]   store argv (or stdin if none) on the clipboard\n");
    printf("       clip get             write the clipboard to stdout\n");
    printf("       clip gen             print the current generation\n");
    printf("       clip clear           wipe the clipboard\n");
}

/* ---------------- subcommands ---------------- */

static int cmd_set_from_args(int argc, char **argv)
{
    /* Join argv[2..] with single spaces.  Bounded to CLIP_DATA_MAX. */
    static char buf[CLIP_DATA_MAX];
    size_t off = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2 && off < sizeof(buf)) buf[off++] = ' ';
        size_t n = s_len(argv[i]);
        if (off + n > sizeof(buf)) n = sizeof(buf) - off;
        for (size_t j = 0; j < n; j++) buf[off + j] = argv[i][j];
        off += n;
        if (off >= sizeof(buf)) break;
    }
    int truncated = 0;
    int gen = clip_set(CLIP_MIME_TEXT, buf, (uint32_t)off, &truncated);
    if (gen < 0) {
        printf("clip: set failed: %d\n", gen);
        return 1;
    }
    if (truncated) printf("clip: warning: payload truncated to %d bytes\n",
                          CLIP_DATA_MAX);
    return 0;
}

static int cmd_set_from_stdin(void)
{
    static char buf[CLIP_DATA_MAX];
    size_t off = 0;
    /* Read until EOF or buffer full.  read() returns 0 on EOF
     * from a pipe (chapter 39).  No newline magic -- we forward
     * bytes verbatim, including a trailing newline if the caller
     * piped one in (echo's default). */
    for (;;) {
        long n = read(0, buf + off, sizeof(buf) - off);
        if (n <= 0) break;
        off += (size_t)n;
        if (off >= sizeof(buf)) break;
    }
    int truncated = 0;
    int gen = clip_set(CLIP_MIME_TEXT, buf, (uint32_t)off, &truncated);
    if (gen < 0) {
        printf("clip: set failed: %d\n", gen);
        return 1;
    }
    if (truncated) printf("clip: warning: payload truncated to %d bytes\n",
                          CLIP_DATA_MAX);
    return 0;
}

static int cmd_get(void)
{
    static uint8_t buf[CLIP_DATA_MAX];
    uint32_t len = 0;
    char mime[CLIP_MIME_MAX];
    int gen = clip_get(buf, sizeof(buf), &len, mime);
    if (gen < 0) {
        printf("clip: get failed: %d\n", gen);
        return 2;
    }
    if (len == 0) return 1;     /* empty -- distinguishable exit */
    long w = write(1, buf, len);
    (void)w;
    return 0;
}

static int cmd_gen(void)
{
    int gen = clip_generation();
    if (gen < 0) {
        printf("clip: gen failed: %d\n", gen);
        return 1;
    }
    printf("%d\n", gen);
    return 0;
}

static int cmd_clear(void)
{
    int gen = clip_clear();
    if (gen < 0) {
        printf("clip: clear failed: %d\n", gen);
        return 1;
    }
    printf("cleared (gen=%d)\n", gen);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    const char *sub = argv[1];
    if (str_eq(sub, "set")) {
        return (argc >= 3) ? cmd_set_from_args(argc, argv)
                           : cmd_set_from_stdin();
    }
    if (str_eq(sub, "get"))   return cmd_get();
    if (str_eq(sub, "gen"))   return cmd_gen();
    if (str_eq(sub, "clear")) return cmd_clear();
    usage();
    return 1;
}
