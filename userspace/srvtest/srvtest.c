/*
 * userspace/srvtest/srvtest.c — chapter 112 demo program.
 *
 * Exercises the named-IPC service bus end-to-end without any
 * cooperating second binary.  Forks once; the parent acts as
 * the service (binds /srv/echotest, accepts one client, echoes
 * one message back), the child acts as the client (connects,
 * sends a known phrase, reads the echo, prints "GOT: <phrase>").
 *
 * The whole hermetic flow happens in a single process tree that
 * the test harness can wait on -- a single "[srvtest] done" line
 * means the registry, the per-direction queues, the accept
 * handshake, the framing, and the fd refcount unwinds all
 * worked.
 *
 * Usage:
 *   srvtest                -- default /srv/echotest, one round-trip
 *   srvtest /srv/<name>    -- specify the bound path
 *
 * Why not just spawn srvls and dial it from a second shell?
 * Same reason looptest is one binary: the test harness needs
 * one wait-able process to know "we are done".
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Service side: accept one connection, read one message, write
 * it straight back, close.  Returns 0 on success, negative on
 * any syscall failure (logged inline). */
static int run_service(int lfd)
{
    int cfd = srv_accept(lfd);
    if (cfd < 0) {
        printf("[srvsvc] accept failed (%d)\n", cfd);
        return -1;
    }
    printf("[srvsvc] accepted (cfd=%d)\n", cfd);

    char buf[256];
    long n = read(cfd, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("[srvsvc] read err %ld\n", n);
        close(cfd);
        return -1;
    }
    buf[n] = '\0';
    printf("[srvsvc] got %ld bytes: %s", n, buf);
    if (n > 0 && buf[n - 1] != '\n') printf("\n");

    long w = write(cfd, buf, (size_t)n);
    if (w != n) {
        printf("[srvsvc] short write (%ld of %ld)\n", w, n);
        close(cfd);
        return -1;
    }
    close(cfd);
    return 0;
}

/* Client side: connect, send the test phrase, read the echo,
 * print "GOT: <phrase>". */
static int run_client(const char *path)
{
    const char *msg = "ipc-hello\n";
    int msg_len = 0;
    while (msg[msg_len]) msg_len++;

    printf("[srvcli] connecting to %s\n", path);
    int fd = srv_connect(path);
    if (fd < 0) {
        printf("[srvcli] connect failed (%d)\n", fd);
        return -1;
    }
    printf("[srvcli] connected (fd=%d)\n", fd);

    long w = write(fd, msg, (size_t)msg_len);
    if (w != msg_len) {
        printf("[srvcli] short write (%ld of %d)\n", w, msg_len);
        close(fd);
        return -1;
    }

    char buf[256];
    long n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("[srvcli] read err %ld\n", n);
        close(fd);
        return -1;
    }
    buf[n] = '\0';
    printf("[srvcli] GOT: %s", buf);
    if (n > 0 && buf[n - 1] != '\n') printf("\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = "/srv/echotest";
    if (argc >= 2) path = argv[1];

    /* Bind BEFORE forking so the child's srv_connect finds a
     * registered service (the listen fd lives in the parent's
     * table but the kernel-side registry is keyed by path,
     * not fd). */
    int lfd = srv_bind(path);
    if (lfd < 0) {
        printf("srvtest: bind %s failed (%d)\n", path, lfd);
        return 1;
    }
    printf("[srvtest] bound %s (lfd=%d)\n", path, lfd);

    int pid = fork();
    if (pid < 0) {
        printf("srvtest: fork failed (%d)\n", pid);
        close(lfd);
        return 1;
    }
    if (pid == 0) {
        /* Child: chapter 112 arranges for fork() to skip
         * FD_SRV_LISTEN inheritance for the same reason
         * FD_SOCKET_LISTEN is skipped — the kernel registry
         * is single-owner.  The child just dials. */
        int rc = run_client(path);
        exit(rc == 0 ? 0 : 1);
    }

    /* Parent: serve exactly one connection, then reap. */
    int srv_rc = run_service(lfd);
    close(lfd);

    int child_status = -1;
    int waited = wait(&child_status);
    if (waited < 0) {
        printf("srvtest: wait failed (%d)\n", waited);
        return 1;
    }
    printf("[srvtest] child exit=%d srv_rc=%d\n",
           child_status, srv_rc);
    if (srv_rc != 0 || child_status != 0) return 1;
    printf("[srvtest] done\n");
    return 0;
}
