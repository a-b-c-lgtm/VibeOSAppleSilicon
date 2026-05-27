/*
 * userspace/htmltok/htmltok.c — HTML5 tokenizer driver.
 *
 * Reads a file path from argv[1] (or "/mnt/test.html" if missing),
 * slurps it whole into a heap buffer, runs `html.h`'s tokenizer over
 * it, and prints each token in a stable parseable form for the
 * test harness to grep on:
 *
 *   [DOCTYPE] "html"
 *   [START]   "html" lang="en"
 *   [START]   "meta" charset="utf-8" /
 *   [CHARS]   "Hello, world!"
 *   [END]     "p"
 *   [COMMENT] "a comment"
 *   [EOF]
 *
 * Multi-line CHARS values are emitted with literal newlines escaped
 * to "\n" inside the quoted string so each token stays on one line
 * — easy to grep, easy to diff against fixtures.
 */
#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/html.h"

/* Read an entire file into a malloc'd buffer.  Returns the buffer
 * (caller frees) and writes byte count to *out_len.  Returns NULL
 * on error (and prints a message). */
static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("htmltok: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("htmltok: oom\n"); close(fd); return 0; }
    char tmp[1024];
    long n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)n) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) { printf("htmltok: oom (grow)\n"); free(buf); close(fd); return 0; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf);
            buf = nb; cap = newcap;
        }
        for (long i = 0; i < n; i++) buf[len + (size_t)i] = tmp[i];
        len += (size_t)n;
    }
    close(fd);
    if (n < 0) { printf("htmltok: read error %ld\n", n); free(buf); return 0; }
    *out_len = len;
    return buf;
}

/* Print a quoted string with any control bytes escaped.  We emit
 *   "\n" for newline,
 *   "\t" for tab,
 *   "\\" for backslash,
 *   "\"" for double-quote.
 * Everything else (incl. high bytes) goes through verbatim — our
 * tokenizer already collapsed entities to bytes. */
static void print_quoted(const char *s, size_t n)
{
    write(1, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
            case '\n':  write(1, "\\n", 2); break;
            case '\t':  write(1, "\\t", 2); break;
            case '\r':  write(1, "\\r", 2); break;
            case '\\':  write(1, "\\\\", 2); break;
            case '"':   write(1, "\\\"", 2); break;
            default:
                if ((unsigned char)c < 0x20) {
                    /* unprintable — show as ?  */
                    write(1, "?", 1);
                } else {
                    write(1, &c, 1);
                }
                break;
        }
    }
    write(1, "\"", 1);
}

static void print_attr(const struct html_attr *a)
{
    write(1, " ", 1);
    write(1, a->name, a->name_len);
    write(1, "=", 1);
    print_quoted(a->value, a->value_len);
}

int main(int argc, char **argv)
{
    const char *path = "/mnt/test.html";
    if (argc >= 2 && argv[1] && argv[1][0]) path = argv[1];

    size_t n = 0;
    char  *buf = slurp(path, &n);
    if (!buf) return 1;

    struct html_tokenizer tz;
    html_tok_init(&tz, buf, n);

    /* Heap-allocate the token; ~12 KiB is too big for the user stack. */
    struct html_token *t = (struct html_token *)malloc(sizeof(*t));
    if (!t) { printf("htmltok: oom (token)\n"); free(buf); return 1; }

    int total = 0;
    for (;;) {
        int r = html_tok_next(&tz, t);
        if (r == 0) {
            printf("[EOF]\n");
            break;
        }
        if (r < 0) {
            printf("[ERROR]\n");
            break;
        }
        total++;
        switch (t->type) {
            case HTML_TOK_DOCTYPE:
                printf("[DOCTYPE] ");
                print_quoted(t->data, t->data_len);
                write(1, "\n", 1);
                break;
            case HTML_TOK_START:
                printf("[START]   ");
                print_quoted(t->tag_name, t->tag_name_len);
                for (int i = 0; i < t->n_attrs; i++) print_attr(&t->attrs[i]);
                if (t->self_closing) write(1, " /", 2);
                write(1, "\n", 1);
                break;
            case HTML_TOK_END:
                printf("[END]     ");
                print_quoted(t->tag_name, t->tag_name_len);
                write(1, "\n", 1);
                break;
            case HTML_TOK_CHARS:
                printf("[CHARS]   ");
                print_quoted(t->data, t->data_len);
                write(1, "\n", 1);
                break;
            case HTML_TOK_COMMENT:
                printf("[COMMENT] ");
                print_quoted(t->data, t->data_len);
                write(1, "\n", 1);
                break;
            default:
                printf("[?TYPE=%d]\n", t->type);
                break;
        }
    }
    printf("[TOTAL] %d tokens, %lu bytes input\n",
           total, (unsigned long)n);
    free(t);
    free(buf);
    return 0;
}
