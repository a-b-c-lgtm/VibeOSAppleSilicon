/* userspace/beep/beep.c — chapter 97: synthesise a square wave.
 *
 * Usage:
 *   beep                   # 800 Hz for 200 ms
 *   beep <freq>            # <freq> Hz for 200 ms
 *   beep <freq> <duration> # <freq> Hz for <duration> ms
 *
 * Calls SYS_BEEP, which routes through the kernel's virtio-snd
 * driver.  Returns 0 on success, prints a one-line error and
 * returns 1 if the device is absent (run with QEMU's
 * `-device virtio-sound-device` to enable it).
 */

#include "../libc/syscall.h"
#include "../libc/printf.h"

/* Mirrors kernel/core/syscall.h ENODEV.  Local to beep so we
 * don't drag a full errno.h into userspace just for one constant. */
#define BEEP_ENODEV 19

static unsigned int parse_uint(const char *s, unsigned int dflt)
{
    if (!s || !s[0]) return dflt;
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned int)(*s - '0');
        s++;
    }
    return v;
}

int main(int argc, char **argv)
{
    unsigned int freq_hz     = (argc >= 2) ? parse_uint(argv[1], 800) : 800;
    unsigned int duration_ms = (argc >= 3) ? parse_uint(argv[2], 200) : 200;

    int rc = beep(freq_hz, duration_ms);
    if (rc == -BEEP_ENODEV) {
        printf("beep: no virtio-sound device "
               "(re-run QEMU with -device virtio-sound-device)\n");
        return 1;
    }
    if (rc != 0) {
        printf("beep: kernel returned %d\n", rc);
        return 1;
    }
    return 0;
}
