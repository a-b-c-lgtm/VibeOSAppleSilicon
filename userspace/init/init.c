/* userspace/init/init.c — milestone-9 PID 1.
 *
 * The first user program the kernel launches.  Spawns each
 * built-in user binary in turn via SYS_SPAWN and waits for it to
 * exit via SYS_WAIT.  This is intentionally a spawn+wait model
 * rather than fork+exec — see book chapter 17 for why.
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

static void run(const char *path)
{
    write(1, "[init] spawn ", 13);
    write(1, path, strlen(path));
    int tid = spawn(path, "");
    if (tid < 0) {
        write(1, " FAILED errno=", 14);
        putd(-tid);
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
    setenv("PATH", "/bin");
    setenv("HOME", "/");
    setenv("SHELL", "/bin/sh");

    /* Self-test: run two non-interactive programs to prove the
     * spawn/wait path is alive before handing control to the
     * (interactive) shell. */
    run("/bin/hello");
    run("/bin/cat");

    /* Hand control to the shell.  When it returns, init shuts
     * down the system (well, exits — there's nobody else).
     *
     * Milestone 46: also auto-spawn the GUI launcher so the system
     * boots straight to a usable desktop.  The launcher is a
     * background child we DON'T wait on specifically — it just
     * happens to also be running.  We block only on the shell so
     * that closing the launcher doesn't terminate init.
     *
     * Milestone 47: also auto-spawn the taskbar.  Order matters
     * only cosmetically — the taskbar uses ALWAYS_ON_TOP and is
     * painted last regardless.
     *
     * Milestone 50: spawn /bin/desktop FIRST so the wallpaper
     * window gets the lowest z, sits underneath everything, and
     * the WM's gradient fallback only flashes for the brief
     * window between fb_init and desktop reading the BGRA blob
     * off OSFS.  Wallpaper ownership is intentionally a
     * userspace concern: the kernel knows nothing about images. */
    puts("[init] launching /bin/desktop (background, GUI)");
    int dtid = spawn("/bin/desktop", "");
    if (dtid < 0) {
        write(1, "[init] spawn /bin/desktop FAILED errno=", 39);
        putd(-dtid);
        write(1, "\n", 1);
    }

    puts("[init] launching /bin/taskbar (background, GUI)");
    int btid = spawn("/bin/taskbar", "");
    if (btid < 0) {
        write(1, "[init] spawn /bin/taskbar FAILED errno=", 39);
        putd(-btid);
        write(1, "\n", 1);
        /* non-fatal */
    }

    puts("[init] launching /bin/launcher (background, GUI)");
    int gtid = spawn("/bin/launcher", "");
    if (gtid < 0) {
        write(1, "[init] spawn /bin/launcher FAILED errno=", 40);
        putd(-gtid);
        write(1, "\n", 1);
        /* non-fatal — keep going */
    }

    /* Chapter 96 — boot chime.  Two short beeps signal "kernel
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
        putd(-tid);
        write(1, "\n", 1);
        return 1;
    }
    /* Reap children until the shell itself is the one that exited.
     * The launcher (and anything it spawned) might exit first; we
     * just keep reaping. */
    int sh_code = 0;
    for (;;) {
        int code = 0;
        int reaped = wait(&code);
        if (reaped < 0) break;
        if (reaped == tid) {
            sh_code = code;
            break;
        }
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
