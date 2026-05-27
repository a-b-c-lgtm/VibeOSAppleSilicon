/*
 * userspace/libc/proc_stat.h — chapter 145 public ABI for the
 * three "kernel state" syscalls that procd uses to render
 * /proc files in user space.
 *
 * These three structs (plus the constants beneath them) are the
 * contract between the kernel and the userspace procd daemon.
 * They live here so both sides can include the same definitions.
 * Touch the kernel-side definitions in kernel/core/syscall.c if
 * you change them.
 */
#ifndef LIBC_PROC_STAT_H
#define LIBC_PROC_STAT_H

#include <stdint.h>

/* Maximum thread fields, same as kernel's THREAD_NAME_MAX /
 * THREAD_ARGS_MAX / THREAD_CWD_MAX.  Hard-coded here so the
 * ABI is independent of any kernel header. */
#define PROC_THREAD_NAME_MAX 32
#define PROC_THREAD_ARGS_MAX 128
#define PROC_THREAD_CWD_MAX  96

/* Per-CPU runqueue slots returned by SYS_KSTAT.  Matches
 * SMP_MAX_CPUS in kernel/arch/cpu.h.  Bump together if the
 * kernel grows more CPUs. */
#define PROC_MAX_CPUS 4

/* Kernel-wide state snapshot, filled by SYS_KSTAT.  Single
 * call returns everything needed for /proc/uptime,
 * /proc/meminfo, /proc/cpuinfo and /proc/sched.  Pads are
 * explicit so the struct has the same layout on every build. */
struct kstat_pub {
    uint64_t uptime_ms;
    uint64_t pmem_total_pages;
    uint64_t pmem_free_pages;
    uint64_t kheap_used_bytes;
    uint32_t cpu_count;
    uint32_t live_threads;
    uint32_t runq_len[PROC_MAX_CPUS];
};

/* One thread's state, filled by SYS_THREAD_SNAPSHOT.  Mirrors
 * kernel/core/thread.h::struct thread_snap one-to-one. */
struct thread_snap_pub {
    int32_t  id;
    int32_t  parent_id;
    int32_t  state;
    uint32_t home_cpu;
    int32_t  tty_raw;
    int32_t  exit_code;
    uint64_t wake_at_ms;
    char     name[PROC_THREAD_NAME_MAX];
    char     args[PROC_THREAD_ARGS_MAX];
    char     cwd[PROC_THREAD_CWD_MAX];
};

#endif /* LIBC_PROC_STAT_H */
