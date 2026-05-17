/*
 * userspace/looptest/looptest.c -- chapter 106 / M95 demo program.
 *
 * Exercises the TCP loopback path end to end without any host
 * involvement.  Forks once; the parent acts as a TCP echo server
 * on a fixed port, the child dials 127.0.0.1 on that port, sends
 * a known phrase, reads the echo, and prints "GOT: <phrase>".
 *
 * Without chapter 106's loopback short-circuit, the child's
 * connect() would build a SYN with dst=127.0.0.1, hand it to
 * virtio-net TX, and SLIRP would drop it (no rule matches the
 * 127/8 prefix from the guest).  With loopback in place the SYN
 * is queued straight back into our own RX path and the handshake
 * completes entirely inside the guest kernel.
 *
 * Usage:
 *   looptest                -- default port 9999, single round-trip
 *   looptest <port>         -- specify the listen port
 *
 * Why not just spawn echod and dial it from a second shell?  We
 * want one hermetic process the test harness can wait on -- a
 * single "[looptest] done" line means everything worked, from
 * net_choose_src on the TX side to the 4-tuple match on the RX
 * side.  A two-program version would need extra serial-prompt
 * orchestration to make the timing deterministic.
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Dotted-decimal printer for the per-connection log lines.  The
 * IP arrives in big-endian packed-uint32 form (byte 0 in the
 * high byte), so we shift down with the byte-3 bias. */
static void print_ip(uint32_t ip_be)
{
    printf("%d.%d.%d.%d",
           (int)((ip_be >> 24) & 0xff),
           (int)((ip_be >> 16) & 0xff),
           (int)((ip_be >>  8) & 0xff),
           (int)( ip_be        & 0xff));
}

/* Parse an unsigned decimal port (1..65535).  Returns 0 on
 * success or -1 on malformed / out-of-range input. */
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

/* Server side: accept one connection, read everything until
 * peer FIN, write it back, close.  Returns 0 on success or a
 * negative value on syscall failure (which we log inline). */
static int run_server(int lfd)
{
    uint32_t peer_ip   = 0;
    uint16_t peer_port = 0;
    int cfd = socket_accept(lfd, &peer_ip, &peer_port);
    if (cfd < 0) {
        printf("[loopsrv] accept failed (%d)\n", cfd);
        return -1;
    }
    printf("[loopsrv] accepted from ");
    print_ip(peer_ip);
    printf(":%d (cfd=%d)\n", (int)peer_port, cfd);

    /* Echo whatever the client sends until they half-close. */
    char buf[256];
    long total = 0;
    for (;;) {
        long n = read(cfd, buf, sizeof(buf));
        if (n < 0) {
            printf("[loopsrv] read err %ld\n", n);
            close(cfd);
            return -1;
        }
        if (n == 0) break;          /* peer FIN */
        long off = 0;
        while (off < n) {
            long w = write(cfd, buf + off, (size_t)(n - off));
            if (w < 0) {
                printf("[loopsrv] write err %ld\n", w);
                close(cfd);
                return -1;
            }
            off += w;
        }
        total += n;
    }
    printf("[loopsrv] echoed %ld bytes; closing\n", total);
    close(cfd);
    return 0;
}

/* Client side: connect to 127.0.0.1:<port>, send the test
 * phrase, drain echo until peer FIN, print what we got.
 * 0x7F000001 == 127.0.0.1 in the packed-BE encoding our libc
 * uses (byte 0 = 0x7F = 127). */
static int run_client(uint16_t port)
{
    const char *msg = "loopback-hello\n";
    /* hand-count msg length so we don't pull in strlen. */
    int msg_len = 0;
    while (msg[msg_len]) msg_len++;

    printf("[loopcli] connecting to 127.0.0.1:%d\n", (int)port);
    int fd = socket_connect(0x7F000001u, port);
    if (fd < 0) {
        printf("[loopcli] connect failed (%d)\n", fd);
        return -1;
    }
    printf("[loopcli] connected (fd=%d)\n", fd);

    long w = write(fd, msg, (size_t)msg_len);
    if (w != msg_len) {
        printf("[loopcli] short write (%ld of %d)\n", w, msg_len);
        close(fd);
        return -1;
    }
    /* Half-close so the server's read sees EOF and stops echoing.
     * Otherwise both sides would block forever waiting for the
     * other to say something. */
    socket_shutdown(fd);

    char buf[256];
    long got = 0;
    for (;;) {
        long n = read(fd, buf + got, sizeof(buf) - 1 - (size_t)got);
        if (n < 0) {
            printf("[loopcli] read err %ld\n", n);
            close(fd);
            return -1;
        }
        if (n == 0) break;          /* peer FIN: echo drained */
        got += n;
        if ((size_t)got >= sizeof(buf) - 1) break;
    }
    buf[got] = '\0';
    printf("[loopcli] GOT: %s", buf);
    if (got > 0 && buf[got - 1] != '\n') printf("\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    uint16_t port = 9999;
    if (argc >= 2) {
        if (parse_port(argv[1], &port) < 0) {
            printf("looptest: bad port '%s'\n", argv[1]);
            return 1;
        }
    }

    /* Server first.  We listen BEFORE forking so the child's
     * socket_connect (issued via 127.0.0.1) finds a real
     * listener -- the listen fd lives in the parent's table
     * but the kernel TCP listener is keyed by port, not fd. */
    int lfd = socket_listen(port, 4);
    if (lfd < 0) {
        printf("looptest: listen failed (%d)\n", lfd);
        return 1;
    }
    printf("[looptest] listening on port %d\n", (int)port);

    int pid = fork();
    if (pid < 0) {
        printf("looptest: fork failed (%d)\n", pid);
        close(lfd);
        return 1;
    }
    if (pid == 0) {
        /* Child: don't touch the parent's listen fd (chapter 104
         * arranged for fork() to skip FD_SOCKET_LISTEN inheritance
         * exactly so children can't accidentally accept on a
         * port the parent is serving).  Just dial and exit. */
        int rc = run_client(port);
        exit(rc == 0 ? 0 : 1);
    }

    /* Parent: serve exactly one connection, then reap the child
     * so the harness has a clean "we are done" signal. */
    int srv_rc = run_server(lfd);
    close(lfd);

    int child_status = -1;
    int waited = wait(&child_status);
    if (waited < 0) {
        printf("looptest: wait failed (%d)\n", waited);
        return 1;
    }
    printf("[looptest] child exit=%d srv_rc=%d\n",
           child_status, srv_rc);
    if (srv_rc != 0 || child_status != 0) return 1;
    printf("[looptest] done\n");
    return 0;
}
