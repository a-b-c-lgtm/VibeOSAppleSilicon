/*
 * kernel/core/wsd_fb.h — chapter 117.
 *
 * Window-Server Daemon framebuffer-mapping interface.  See
 * wsd_fb.c for the full design rationale.  Two entry points:
 *
 *   sys_fb_map_scanout(long args_uptr)
 *       Backs SYS_FB_MAP_SCANOUT.  Called from syscall.c
 *       dispatch.
 *
 *   wsd_fb_release_owner(uint64_t pid)
 *       Called from the thread exit path when ANY thread
 *       exits.  No-op unless `pid` is the current owner of
 *       the FB mapping; otherwise tears it down and releases
 *       the single-owner slot.
 */
#ifndef KERNEL_CORE_WSD_FB_H
#define KERNEL_CORE_WSD_FB_H

#include <stdint.h>

long sys_fb_map_scanout(long args_uptr);
void wsd_fb_release_owner(uint64_t pid);

/* Chapter 117 \u2014 userspace-driven GPU flush.
 * fb_present(x, y, w, h) wrapper.  See SYS_FB_PRESENT in
 * kernel/core/syscall.h for the contract. */
long sys_fb_present(long x_arg, long y_arg,
                    long w_arg, long h_arg);

#endif /* KERNEL_CORE_WSD_FB_H */
