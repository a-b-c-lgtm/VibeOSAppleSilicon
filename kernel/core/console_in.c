/*
 * kernel/core/console_in.c — unified console input source.
 *
 * Polls the virtio-input keyboard first (so a held-down key
 * doesn't get starved by a fast serial typist), then falls back
 * to the PL011 RX FIFO.
 */

#include "console_in.h"
#include "serial.h"
#include "wm.h"
#include "../device/virtio_input.h"

int console_try_getc(char *out)
{
    /* Always poll the virtio-input ring even if we don't end up
     * returning a keyboard byte this tick — otherwise events
     * accumulate forever in the device's pending queue and the
     * shift-state shadow falls out of sync. */
    if (virtio_input_present()) {
        char c;
        if (virtio_input_try_getc(&c)) {
            /* If a window has focus, the GUI session consumes
             * this byte and stdin sees nothing.  Otherwise the
             * byte falls through to the cooked/raw line
             * discipline as if it had come from the serial RX. */
            if (wm_has_windows() && wm_keyboard_byte(c))
                return serial_try_getc(out);
            if (out) *out = c;
            return 1;
        }
    }
    return serial_try_getc(out);
}
