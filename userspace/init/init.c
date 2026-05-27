/* userspace/init/init.c — PID 1.
 *
 * The first user program the kernel launches.  Spawns each
 * built-in user binary in turn via SYS_SPAWN and waits for it to
 * exit via SYS_WAIT.  This is intentionally a spawn+wait model
 * rather than fork+exec — see book chapter 16 for why.
 *
 * Chapter 113 added a tiny per-pid supervisor: a handful of
 * "this binary must always be running" entries are tracked in
 * g_supervised[].  When the main reap loop reaps a tid that
 * matches a supervised entry, the entry is respawned and the
 * new tid recorded.  This is the userspace-side equivalent of
 * systemd's `Restart=on-failure` -- minus the dependency graph,
 * minus the backoff, minus the journal -- but enough to keep
 * /bin/clipboardd and /bin/fontd alive across crashes, which
 * is the whole reason it's there.
 *
 * Expected output (kernel chatter elided):
 *   [init] hello -> tid=...
 *   ... hello prints ...
 *   [init] hello exited code=0
 *   [init] cat   -> tid=...
 *   ... cat prints ...
 *   [init] cat exited code=0
 *   [init] no programs left, exiting 0
 */

#include "../libc/syscall.h"
#include "../libc/errno.h"
#include "../libc/malloc.h"
#include "../libc/env.h"

/* ---------------- chapter 113 supervisor ----------------
 *
 * Fixed-size table of "keep this alive" entries.  Each entry is
 * a path + the tid of the most recently spawned instance.  When
 * an entry's tid is reaped (in the main wait loop), restart() is
 * called and the new tid recorded.  Entries are added by
 * supervise(path) and never removed -- chapter 113 has no
 * unload story, and won't have one until we add a service
 * manager UI.
 *
 * No backoff: the supervised entries today are clipboardd,
 * fontd, and (chapter 117) wsd; all small daemons.  If any
 * is crashing in a tight loop the right answer is to fix it,
 * not to back off.  When ch113 adds the audio mixer we'll
 * revisit. */
#define SUPERVISED_MAX 6
struct supervised {
    const char *path;
    const char *args;
    int         tid;     /* -1 if not currently running */
};
static struct supervised g_supervised[SUPERVISED_MAX];
static int g_supervised_count = 0;

static int supervise(const char *path, const char *args)
{
    if (g_supervised_count >= SUPERVISED_MAX) return -1;
    int tid = spawn(path, args);
    if (tid < 0) {
        write(1, "[init] supervise spawn FAILED ", 30);
        write(1, path, strlen(path));
        write(1, " errno=", 7);
        putd(errno);
        write(1, "\n", 1);
        return tid;
    }
    g_supervised[g_supervised_count].path = path;
    g_supervised[g_supervised_count].args = args;
    g_supervised[g_supervised_count].tid  = tid;
    g_supervised_count++;
    write(1, "[init] supervise ", 17);
    write(1, path, strlen(path));
    write(1, " tid=", 5);
    putd(tid);
    write(1, "\n", 1);
    return tid;
}

/* If `reaped` matches a supervised tid, respawn its binary and
 * update the tid in place.  Returns 1 if respawned (caller
 * should treat this as a service event, not an ordinary child
 * exit), 0 if `reaped` was not a supervised tid. */
static int supervise_check(int reaped, int code)
{
    for (int i = 0; i < g_supervised_count; i++) {
        if (g_supervised[i].tid != reaped) continue;
        write(1, "[init] supervised ", 18);
        write(1, g_supervised[i].path, strlen(g_supervised[i].path));
        write(1, " died code=", 11);
        putd(code);
        write(1, " -- respawning\n", 15);
        int tid = spawn(g_supervised[i].path, g_supervised[i].args);
        if (tid < 0) {
            write(1, "[init] respawn FAILED errno=", 28);
            putd(errno);
            write(1, "\n", 1);
            g_supervised[i].tid = -1;
        } else {
            g_supervised[i].tid = tid;
            write(1, "[init] respawn tid=", 19);
            putd(tid);
            write(1, "\n", 1);
        }
        return 1;
    }
    return 0;
}

static void run(const char *path)
{
    write(1, "[init] spawn ", 13);
    write(1, path, strlen(path));
    int tid = spawn(path, "");
    if (tid < 0) {
        write(1, " FAILED errno=", 14);
        putd(errno);
        write(1, "\n", 1);
        return;
    }
    write(1, " -> tid=", 8);
    putd(tid);
    write(1, "\n", 1);

    int code = 0;
    int reaped = wait(&code);
    write(1, "[init] reaped tid=", 18);
    putd(reaped);
    write(1, " code=", 6);
    putd(code);
    write(1, "\n", 1);
}

int main(void)
{
    puts("[init] starting (pid 1)");

    /* Seed the environment.  Children inherit this byte-for-byte
     * via SYS_SPAWN, so the shell and every program it launches
     * see the same defaults. */
    setenv("PATH", "/bin", 1);
    setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/sh", 1);

    /* Self-test: run two non-interactive programs to prove the
     * spawn/wait path is alive before handing control to the
     * (interactive) shell. */
    run("/bin/hello");
    run("/bin/cat");

    /* Hand control to the shell.  When it returns, init shuts
     * down the system (well, exits — there's nobody else).
     *
     * Also auto-spawn the GUI launcher so the system
     * boots straight to a usable desktop.  The launcher is a
     * background child we DON'T wait on specifically — it just
     * happens to also be running.  We block only on the shell so
     * that closing the launcher doesn't terminate init.
     *
     * Also auto-spawn the taskbar.  Order matters
     * only cosmetically — the taskbar uses ALWAYS_ON_TOP and is
     * painted last regardless.
     *
     * Spawn /bin/desktop FIRST so the wallpaper
     * window gets the lowest z, sits underneath everything, and
     * the WM's gradient fallback only flashes for the brief
     * window between fb_init and desktop reading the BGRA blob
     * off OSFS.  Wallpaper ownership is intentionally a
     * userspace concern: the kernel knows nothing about images. */

    /* Chapter 115 -- font service.
     *
     * The TrueType rasteriser used to live in the kernel
     * (chapter 104), eating ~1.5K lines of EL1 code plus the
     * ~700KB DejaVuSans.ttf blob.  Chapter 115 moved it to
     * /bin/fontd, a userspace daemon bound to /srv/font.  The
     * kernel-side WM (kernel/core/wm_font.c) is its only client
     * today; on a cache miss it sends a one-shot RPC to fontd
     * and gets back an AA bitmap.
     *
     * Supervised because the WM falls back to the kernel's
     * always-available bitmap font when fontd is unreachable --
     * which means a fontd crash is recoverable: the user sees
     * one frame of bitmap text, init respawns fontd, and the
     * next cache miss re-establishes the conn.
     *
     * Started BEFORE /bin/desktop so the very first TTF glyph
     * a userspace window asks for has a non-trivial chance of
     * being TTF rather than the bitmap fallback. */
    puts("[init] launching /bin/fontd (supervised)");
    supervise("/bin/fontd", "");

    /* Chapter 117 — window-server daemon.  Owns the
     * scanout framebuffer and the /srv/wm compositor bus;
     * all GUI apps paint through it.  A wsd crash visibly
     * freezes the desktop — hence the supervisor wrapper
     * from day one, so the restart machinery is the same
     * the day it matters as it was the day it didn't.
     *
     * Started AFTER fontd so wsd has a working font server
     * when it starts drawing title-bar text. */
    puts("[init] launching /bin/wsd (supervised)");
    supervise("/bin/wsd", "");

    puts("[init] launching /bin/desktop (background, GUI)");
    int dtid = spawn("/bin/desktop", "");
    if (dtid < 0) {
        write(1, "[init] spawn /bin/desktop FAILED errno=", 39);
        putd(errno);
        write(1, "\n", 1);
    }

    puts("[init] launching /bin/taskbar (background, GUI)");
    int btid = spawn("/bin/taskbar", "");
    if (btid < 0) {
        write(1, "[init] spawn /bin/taskbar FAILED errno=", 39);
        putd(errno);
        write(1, "\n", 1);
        /* non-fatal */
    }

    puts("[init] launching /bin/launcher (background, GUI)");
    int gtid = spawn("/bin/launcher", "");
    if (gtid < 0) {
        write(1, "[init] spawn /bin/launcher FAILED errno=", 40);
        putd(errno);
        write(1, "\n", 1);
        /* non-fatal — keep going */
    }

    /* Chapter 111 — boot-time httpd.
     *
     * The browser/httpd loop closes when there's an HTTP server
     * waiting for it on the loopback interface from the moment
     * the desktop is usable.  Without this, every "go open
     * gui_term and type `browser http://127.0.0.1/...`" recipe
     * starts with a manual `httpd N &` first, which is busywork
     * and easy to forget.
     *
     * We bind port 80 (the conventional HTTP port) rather than
     * 8080 specifically so the existing chapter-105/106a/106b
     * regression tests can keep spawning their own httpd on 8080
     * without colliding (tcp_listen returns -2 on duplicate
     * binds; chapter 111 memory has the audit).  The two
     * coexist: init's port-80 instance serves /mnt/ files over
     * loopback; the test-spawned port-8080 instance is the
     * forwarding proxy used by HTTPS-bridge tests.
     *
     * HTTPD_UPSTREAM is deliberately NOT set in init's env, so
     * the port-80 instance has no upstream — non-VFS GETs land
     * in `serve_forward` and 502.  That's the right behaviour
     * for a local-only fileserver; configuring an upstream is a
     * user choice, not a default. */
    puts("[init] launching /bin/httpd 80 (background, loopback)");
    int httid = spawn("/bin/httpd", "80");
    if (httid < 0) {
        write(1, "[init] spawn /bin/httpd FAILED errno=", 37);
        putd(errno);
        write(1, "\n", 1);
        /* non-fatal — desktop is still usable without it */
    }

    /* Chapter 125 — boot-time httpsd.
     *
     * Same rationale as the chapter-106c httpd above, but for
     * TLS.  Without an in-guest HTTPS server we'd need either
     * a host-side proxy (scripts/https_proxy.py, deprecated by
     * chapter 127) or working outbound HTTPS through the QEMU
     * SLIRP stack -- both add wall-clock latency and external
     * dependencies to regression runs.  A loopback httpsd lets
     * the tls_socket handshake and the browser's https:// path
     * be tested without ever crossing the host network.
     *
     * Port 8443 (the conventional "alternate HTTPS" port)
     * keeps us off port 443 which would clash with anything
     * the host might be running on the SLIRP guest side.  The
     * sample CN=localhost cert chain in test_chain.c is fine
     * for loopback; tlstest's --handshake mode pins on the
     * leaf public key, so name/expiry validation isn't in play
     * yet (that's chapter 126).
     *
     * As with httpd we tolerate spawn failure: a missing
     * httpsd means TLS regression tests will fail at the
     * connect() step with a clear "tlstest: FAIL" line. */
    puts("[init] launching /bin/httpsd 8443 (background, loopback TLS)");
    int httsid = spawn("/bin/httpsd", "8443");
    if (httsid < 0) {
        write(1, "[init] spawn /bin/httpsd FAILED errno=", 38);
        putd(errno);
        write(1, "\n", 1);
        /* non-fatal — TLS tests will surface this */
    }

    /* Chapter 128 — second httpsd on port 8444, presenting the
     * EC / ECDSA sample chain instead of the RSA one.  Same
     * loopback-test rationale as the RSA instance above; the
     * point is to give the browser two distinct CAs to validate
     * against simultaneously so the multi-anchor trust store
     * isn't just a one-entry list with extra ceremony.  Failure
     * is non-fatal for the same reason. */
    puts("[init] launching /bin/httpsd --ec 8444 (background, loopback TLS, ECDSA)");
    int httpsid_ec = spawn("/bin/httpsd", "--ec 8444");
    if (httpsid_ec < 0) {
        write(1, "[init] spawn /bin/httpsd --ec FAILED errno=", 43);
        putd(errno);
        write(1, "\n", 1);
    }

    /* Chapter 140 — clipboard service.
     *
     * The clipboard isn't a kernel feature; it lives in a
     * userspace daemon that, since chapter 140, mounts
     * /clipboard via the userfs interface (SYS_MOUNT +
     * libfs).  Spawned through supervise() so the main reap
     * loop respawns it if it dies -- the "keep my daemons
     * alive" responsibility that on Linux lives in systemd /
     * sysvinit and on macOS lives in launchd.  Ours fits in
     * ~50 lines of init.c.
     *
     * The supervisor must come AFTER the GUI services so the
     * keystroke wiring in notepad/browser etc. finds
     * /clipboard/text already opened on first paste.  The
     * race is benign: a clip_set() that lands before
     * clipboardd has mounted returns -ENOENT, which notepad
     * treats as "no clipboard available right now" and
     * silently drops. */
    puts("[init] launching /bin/clipboardd (supervised)");
    supervise("/bin/clipboardd", "");

    /* Chapter 145 — /proc service.
     *
     * The chapter-99 in-kernel /proc lived under kernel/core/
     * procfs.c.  Chapter 145 evicts it: /bin/procd mounts /proc
     * via the same userfs channel clipboardd uses, and serves
     * the same files (uptime, meminfo, cpuinfo, sched, plus
     * per-pid status/cmdline/trace).  Supervised so a crash
     * restarts the mount; /proc readers (ps, top, strace, cat)
     * are unchanged. */
    puts("[init] launching /bin/procd (supervised)");
    supervise("/bin/procd", "");

    /* Chapter 97 — boot chime.  Two short beeps signal "kernel
     * is up, GUI is up, you can start typing".  -ENODEV is
     * silently ignored: not every QEMU invocation attaches a
     * virtio-sound device, and a missing chime is cosmetic. */
    puts("[init] playing boot chime");
    beep(440, 100);   /* A4 */
    beep(659, 150);   /* E5 */

    puts("[init] launching /bin/sh");
    int tid = spawn("/bin/sh", "");
    if (tid < 0) {
        write(1, "[init] spawn /bin/sh FAILED errno=", 34);
        putd(errno);
        write(1, "\n", 1);
        return 1;
    }
    /* Reap children until the shell itself is the one that exited.
     * The launcher (and anything it spawned) might exit first; we
     * just keep reaping.
     *
     * Chapter 113: per-reap supervisor hook.  If the reaped tid
     * matches a supervised entry, supervise_check() respawns the
     * binary and updates the entry's tid in place.  The reap loop
     * carries on as before -- the supervisor is intentionally
     * passive (it only acts on events that would have happened
     * anyway), which keeps the boot path the chapter-9 shape
     * everyone is used to. */
    int sh_code = 0;
    for (;;) {
        int code = 0;
        int reaped = wait(&code);
        if (reaped < 0) break;
        if (reaped == tid) {
            sh_code = code;
            break;
        }
        if (supervise_check(reaped, code)) continue;
        write(1, "[init] reaped background tid=", 29);
        putd(reaped);
        write(1, " code=", 6);
        putd(code);
        write(1, "\n", 1);
    }
    write(1, "[init] /bin/sh exited code=", 27);
    putd(sh_code);
    write(1, "\n", 1);

    puts("[init] all done, exiting 0");
    return 0;
}
