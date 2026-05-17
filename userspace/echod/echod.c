/*
 * userspace/echod/echod.c -- chapter 104 / M93 demo daemon.
 *
 * The smallest interesting program that exercises the new
 * server-side socket syscalls (`socket_listen` + `socket_accept`).
 * Listens on a TCP port, accepts one peer at a time, echoes
 * every byte it reads back to that peer, and closes the
 * connection cleanly when the peer half-closes.
 *
 * Usage:
 *   echod                  -- listen on default port 7777
 *   echod <port>           -- listen on a specific port
 *   echod <port> --once    -- accept exactly one connection
 *                              then exit (used by tests so
 *                              the kernel can drain log output
 *                              without a daemon left running)
 *
 * Why 7777 and not 8088?  The boot self-test in main.c uses
 * 8088 during phase-7 to verify passive open before init even
 * runs; reusing the port would race the test harness.  7777
 * is unallocated in our environment and easy to remember.
 *
 * Why one connection at a time?  We don't have non-blocking
 * accept or select() yet (chapter 105 territory).  An echo
 * daemon serialised on `socket_accept` is sufficient to prove
 * the new syscalls work end-to-end and keeps the source tiny.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Decimal parser that fits in a few lines.  We don't pull in
 * libc/url.h here because URL parsing is overkill for a single
 * port arg and we'd rather keep the binary small. */
static int parse_port(const char *s, uint16_t *out)
{
    uint32_t v = 0;
    int seen = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 65535u) return -1;
        seen = 1;
        s++;
    }
    if (!seen || v == 0) return -1;
    *out = (uint16_t)v;
    return 0;
}

/* Pretty-print a packed BE IPv4 address (network byte order)
 * as a.b.c.d.  Used purely for the accept-log line. */
static void print_ip(uint32_t ip_be)
{
    printf("%d.%d.%d.%d",
           (int)((ip_be >> 24) & 0xff),
           (int)((ip_be >> 16) & 0xff),
           (int)((ip_be >>  8) & 0xff),
           (int)( ip_be        & 0xff));
}

/* Echo loop for one already-accepted connection.  Reads up to
 * BUF bytes at a time, writes them straight back.  Returns the
 * total number of bytes echoed (for the post-close log line). */
static long echo_one(int cfd)
{
    char buf[512];
    long total = 0;
    for (;;) {
        long n = read(cfd, buf, sizeof(buf));
        if (n < 0) {
            printf("echod: read error %ld\n", n);
            return total;
        }
        if (n == 0) {
            /* Peer half-closed (FIN) and our RX buffer is
             * drained.  Mirror the close so the kernel can
             * tear the conn down. */
            return total;
        }
        /* Echo verbatim.  `write` may short-write if the kernel
         * TX buffer is full; loop until the slice is gone. */
        long off = 0;
        while (off < n) {
            long w = write(cfd, buf + off, (size_t)(n - off));
            if (w < 0) {
                printf("echod: write error %ld\n", w);
                return total;
            }
            off += w;
        }
        total += n;
    }
}

int main(int argc, char **argv)
{
    uint16_t port = 7777;
    int once = 0;

    if (argc >= 2) {
        if (parse_port(argv[1], &port) < 0) {
            printf("echod: bad port \"%s\"\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        /* The only flag we recognise.  Treating unknown args as
         * fatal keeps the surface tiny -- this is a demo. */
        const char *flag = argv[2];
        if (flag[0] == '-' && flag[1] == '-' &&
            flag[2] == 'o' && flag[3] == 'n' &&
            flag[4] == 'c' && flag[5] == 'e' && flag[6] == '\0') {
            once = 1;
        } else {
            printf("echod: unknown arg \"%s\"\n", flag);
            return 1;
        }
    }

    /* backlog of 4 is well under TCP_ACCEPT_QCAP (8); the kernel
     * still uses its compile-time cap but we pass a sensible
     * value so the call site is forward-compatible. */
    int lfd = socket_listen(port, 4);
    if (lfd < 0) {
        printf("echod: listen failed: %d\n", lfd);
        return 1;
    }
    printf("echod: listening on port %d (fd=%d, once=%d)\n",
           (int)port, lfd, once);

    for (;;) {
        uint32_t peer_ip = 0;
        uint16_t peer_port = 0;
        int cfd = socket_accept(lfd, &peer_ip, &peer_port);
        if (cfd < 0) {
            printf("echod: accept failed: %d\n", cfd);
            close(lfd);
            return 1;
        }
        printf("echod: accepted from ");
        print_ip(peer_ip);
        printf(":%d (fd=%d)\n", (int)peer_port, cfd);

        long n = echo_one(cfd);
        /* Send our FIN -- the kernel keeps the cid alive for
         * the FIN-WAIT teardown but our fd is done.  close()
         * sends FIN as part of cleanup. */
        close(cfd);
        printf("echod: closed peer; echoed %ld byte(s)\n", n);

        if (once) break;
    }

    close(lfd);
    printf("echod: done\n");
    return 0;
}
