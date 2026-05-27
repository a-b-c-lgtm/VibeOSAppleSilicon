/*
 * userspace/htmldom/htmldom.c — DOM-builder driver.
 *
 * Reads a file path from argv[1] (or "/mnt/test.html" if missing),
 * slurps it whole into a heap buffer, runs `html.h`'s tokenizer +
 * `dom.h`'s tree builder over it, and walks the resulting tree
 * depth-first printing each node on its own line.  The output is
 * a stable indented form that the test harness greps over for
 * landmarks, e.g.:
 *
 *   [DOC]
 *     [DOCTYPE] "html"
 *     [ELEM] "html" lang="en"
 *       [ELEM] "head"
 *         [ELEM] "meta" charset="utf-8"
 *         [ELEM] "title"
 *           [TEXT] "tokenizer fixture"
 *         [ELEM] "style"
 *           [TEXT] "\n  body { color: red; }\n"
 *       [COMMENT] " a comment in the middle "
 *       [ELEM] "body"
 *         [ELEM] "h1"
 *           [TEXT] "Hello, & goodbye"
 *         ...
 *   [TOTAL] N nodes, M bytes input
 *
 * Indent is 2 spaces per level.  Strings are printed with control
 * bytes escaped (newlines as \n etc) so each node stays on one line.
 */
#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/printf.h"
#include "../libc/malloc.h"
#include "../libc/html.h"
#include "../libc/dom.h"

/* ---------- file slurp ---------- */

static char *slurp(const char *path, size_t *out_len)
{
    int fd = open(path, 0);
    if (fd < 0) {
        printf("htmldom: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) { printf("htmldom: oom\n"); close(fd); return 0; }
    char tmp[1024];
    long n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + (size_t)n) newcap *= 2;
            char *nb = (char *)malloc(newcap);
            if (!nb) { printf("htmldom: oom (grow)\n"); free(buf); close(fd); return 0; }
            for (size_t i = 0; i < len; i++) nb[i] = buf[i];
            free(buf);
            buf = nb; cap = newcap;
        }
        for (long i = 0; i < n; i++) buf[len + (size_t)i] = tmp[i];
        len += (size_t)n;
    }
    close(fd);
    if (n < 0) { printf("htmldom: read error %ld\n", n); free(buf); return 0; }
    *out_len = len;
    return buf;
}

/* ---------- pretty printer ---------- */

static void print_indent(int depth)
{
    for (int i = 0; i < depth; i++) write(1, "  ", 2);
}

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
                    write(1, "?", 1);
                } else {
                    write(1, &c, 1);
                }
                break;
        }
    }
    write(1, "\"", 1);
}

static void print_quoted_z(const char *s)
{
    size_t n = 0;
    if (s) while (s[n]) n++;
    print_quoted(s ? s : "", n);
}

static void print_node(const struct dom_node *n, int depth)
{
    if (!n) return;
    print_indent(depth);
    switch (n->type) {
    case DOM_NODE_DOCUMENT:
        write(1, "[DOC]\n", 6);
        break;
    case DOM_NODE_DOCTYPE:
        write(1, "[DOCTYPE] ", 10);
        print_quoted(n->text, n->text_len);
        write(1, "\n", 1);
        break;
    case DOM_NODE_ELEMENT:
        write(1, "[ELEM] ", 7);
        print_quoted_z(n->tag);
        for (struct dom_attr *a = n->attrs; a; a = a->next) {
            write(1, " ", 1);
            if (a->name) {
                size_t k = 0; while (a->name[k]) k++;
                write(1, a->name, k);
            }
            write(1, "=", 1);
            print_quoted_z(a->value);
        }
        write(1, "\n", 1);
        break;
    case DOM_NODE_TEXT:
        write(1, "[TEXT] ", 7);
        print_quoted(n->text, n->text_len);
        write(1, "\n", 1);
        break;
    case DOM_NODE_COMMENT:
        write(1, "[COMMENT] ", 10);
        print_quoted(n->text, n->text_len);
        write(1, "\n", 1);
        break;
    default:
        printf("[?TYPE=%d]\n", n->type);
        break;
    }
    /* Children, in source order. */
    for (struct dom_node *c = n->first_child; c; c = c->next_sibling)
        print_node(c, depth + 1);
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

    /* Heap-allocate the token scratch (~12 KiB). */
    struct html_token *scratch = (struct html_token *)malloc(sizeof(*scratch));
    if (!scratch) { printf("htmldom: oom (scratch)\n"); free(buf); return 1; }

    struct dom dom;
    if (dom_init(&dom) < 0) {
        printf("htmldom: oom (dom_init)\n");
        free(scratch); free(buf); return 1;
    }
    int rc = dom_build(&dom, &tz, scratch);
    if (rc < 0) {
        printf("htmldom: build error rc=%d\n", rc);
        /* still print whatever we got — useful for debugging */
    }

    print_node(dom_root(&dom), 0);
    printf("[TOTAL] %lu nodes, %lu bytes input\n",
           (unsigned long)dom.n_nodes, (unsigned long)n);

    dom_destroy(&dom);
    free(scratch);
    free(buf);
    return rc < 0 ? 1 : 0;
}
