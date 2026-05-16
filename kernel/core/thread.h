/*
 * kernel/core/thread.h — kernel-thread public API.
 *
 * A "thread" is a flow of control with its own stack and saved
 * register set.  All threads share the kernel address space; the
 * scheduler is single-CPU, single-runqueue, round-robin.
 *
 * Cooperative semantics:
 *   - thread_init() sets up bookkeeping for the boot CPU's flow
 *     (the one that ran kernel_main from boot.s).
 *   - thread_create(entry, arg, name) spawns a new thread and
 *     enqueues it as READY.
 *   - yield() voluntarily yields to the next READY thread.
 *   - thread_exit() terminates the calling thread; the scheduler
 *     reclaims its stack.
 *
 * In milestone 5 the timer ISR also calls schedule(), giving us
 * preemption "for free" because both code paths use the same
 * cswitch_to primitive and the same exception-frame format.
 */

#ifndef THREAD_H
#define THREAD_H

#include <stddef.h>
#include <stdint.h>
#include "vfs.h"

typedef void (*thread_entry_fn)(void *arg);

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_WAITING,    /* blocked in wait(); not on runqueue */
    THREAD_SLEEPING,   /* blocked in sleep_ms(); woken by timer */
    THREAD_BLOCKED,    /* blocked on I/O (pipe, future socket);
                          woken by another thread via blocked_on */
    THREAD_EXITED,     /* finished; reapable by parent's wait()  */
};

#define THREAD_ARGS_MAX 128
#define THREAD_NAME_MAX 32
/* Per-process current working directory.  Inherited from the
 * spawning thread; mutable via SYS_CHDIR; readable via
 * SYS_GETCWD.  96 bytes is enough for our flat namespace plus
 * generous future-proofing for nested directories. */
#define THREAD_CWD_MAX  96
/* Per-process environment block.  Layout is a packed sequence
 * of NUL-terminated "KEY=VALUE" entries, with a final extra
 * \0 byte marking end-of-list:
 *   "PATH=/bin\0HOME=/\0\0..."
 * Inherited byte-for-byte from spawning thread.  512 bytes
 * comfortably holds 30+ short variables. */
#define THREAD_ENV_MAX  512

struct thread {
    uint64_t           sp;            /* saved SP (points into 'stack')  */
    uint8_t           *stack_base;    /* allocation returned by kmalloc  */
    size_t             stack_size;
    int                id;
    int                parent_id;     /* -1 if no parent */
    int                exit_code;     /* valid once state == THREAD_EXITED */
    enum thread_state  state;
    /* Inline so the buffer is owned by the thread struct.  Was a
     * raw const char * pointer, but spawn() passes a user-mode
     * pointer that becomes invalid the moment the user AS is
     * destroyed; copying into the thread struct is simpler than
     * tracking ownership of a kheap'd duplicate. */
    char               name[THREAD_NAME_MAX];
    struct thread     *next;          /* runqueue link (NULL when off rq) */
    struct thread     *all_next;      /* link in global "all threads" list */
    /* Chapter 93 — refcounted, possibly-shared fd table.  Each
     * thread holds exactly one reference; vfs_close_all on
     * thread exit drops it.  CLONE_FILES (SYS_CLONE3) bumps
     * the refcount instead of allocating a fresh table.  See
     * struct fd_table in vfs.h.  Always non-NULL for live
     * threads (allocated in thread_init / *thread_create*). */
    struct fd_table   *fdt;
    /* Spawn-time argument string.  The parent passes a NUL-terminated
     * string to spawn(), which copies it here; the child reads it via
     * SYS_GETARGS.  Empty when not set.  Truncated (still NUL-term)
     * if the source string >= THREAD_ARGS_MAX bytes. */
    char               args[THREAD_ARGS_MAX];
    /* Per-process address space (page tables).  Owned by user
     * threads (created via user_thread_create).  NULL for kernel
     * threads, which use the boot L1.  Freed when the thread is
     * reaped.  Forward-declared so this header doesn't drag in
     * the page-table internals. */
    struct address_space *as;
    /* Per-process current working directory.  Inherited from the
     * spawning thread.  Always begins with '/' (absolute) and is
     * NUL-terminated.  Initialised to "/" for the boot thread. */
    char               cwd[THREAD_CWD_MAX];
    /* Per-process environment block.  See THREAD_ENV_MAX comment
     * for layout.  Initial state is two zero bytes (empty list).
     * Mutated only via SYS_SETENV / SYS_UNSETENV. */
    char               env[THREAD_ENV_MAX];
    /* When state == THREAD_SLEEPING, the absolute monotonic
     * millisecond timestamp at which the thread should be woken
     * back to THREAD_READY.  Set by sys_sleep_ms; cleared
     * implicitly by the wake-up walk in yield(). */
    uint64_t           wake_at_ms;
    /* When state == THREAD_BLOCKED, an opaque token identifying
     * the resource the thread is blocked on (typically a struct
     * pipe *).  Used by wake-side code to find which threads to
     * unblock — e.g. pipe_write walks g_all_head looking for
     * threads with blocked_on == this pipe and wakes them. */
    void              *blocked_on;
    /* When non-zero, console reads on fd 0 return one byte at a
     * time without echo and without line buffering.  Set via
     * SYS_TTY_RAW.  NOT inherited by spawned children — they
     * always start in cooked mode so cat/grep/etc. behave. */
    int                tty_raw;
    /* Pending-signal bitmask.  Bit (1 << sig) is set when a
     * signal of that number has been raised against this thread
     * but has not yet been delivered.  Checked on every return
     * from a syscall (svc_dispatch tail) and inside cooked-mode
     * console reads, where it short-circuits the read with
     * -EINTR so the dispatcher can see it. */
    uint32_t           sig_pending;
    /* Per-signal disposition table (chapter 77).
     *   sig_handlers[s] == 0  : SIG_DFL — terminate with 128+s.
     *   sig_handlers[s] == 1  : SIG_IGN — drop silently.
     *   any other value       : EL0 user function pointer.
     * SIGKILL (9) is forced SIG_DFL by sys_sigaction; it cannot
     * be caught or ignored.  Inherited across fork (POSIX) and
     * reset to all-SIG_DFL across exec.  Index 0 is unused. */
    uint64_t           sig_handlers[32];
    /* Address of the libc-provided sigreturn trampoline.  When
     * the kernel diverts EL0 into a user handler it sets x30
     * (LR) to this value so the handler's natural `ret` jumps
     * to a tiny stub that issues SYS_SIGRETURN.  Set by the
     * first sys_sigaction call; zero means "no trampoline yet,
     * caller must register one before catching signals." */
    uint64_t           sig_restorer;
    /* Chapter 92 — CPU affinity ("home CPU").  Set at create
     * time to the CPU the thread is destined to run on.  Wakes
     * (thread_wake_blocked, the sleeper-walk in yield) route
     * the thread back to home_cpu's runqueue regardless of
     * which CPU is doing the waking, so a CPU-0 thread that
     * gets woken by a CPU-1 unlock still resumes on CPU 0.
     * Pre-chapter 92, every thread implicitly had home_cpu=0
     * because user threads never ran anywhere else. */
    uint32_t           home_cpu;
    /* Chapter 100 — per-thread syscall-tracer ring.  NULL when
     * not traced (the common case; the dispatcher branch is
     * cheap).  Allocated lazily by sys_trace_me; freed in the
     * two reap sites via strace_release().  Not inherited
     * across fork/clone — each thread that wants to be traced
     * must opt in explicitly.  See kernel/core/strace.h. */
    struct strace_ring *strace;
};

/* Signal numbers (POSIX-compatible subset).  Only SIGINT is
 * currently raised; the others are reserved so userspace headers
 * can use the standard names. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17    /* posted to the parent when a child exits */

/* waitpid options (matches POSIX where it overlaps).  Only
 * WNOHANG is meaningful today; future bits will go here. */
#define WNOHANG  1

/* Set up the boot thread record so the running CPU has a `current`
 * to context-switch *out of*.  Call once after kheap_init. */
void thread_init(void);

/* Spawn a new READY thread.  Returns NULL on OOM. */
struct thread *thread_create(thread_entry_fn entry,
                             void *arg,
                             const char *name);

/* Chapter 89 — spawn a kernel thread on a SPECIFIC CPU.  The new
 * thread is placed on `cpu_id`'s runqueue and an IPI_RESCHED is
 * sent so an idle target wakes from WFI promptly.  `cpu_id` must
 * be < SMP_MAX_CPUS; out-of-range returns NULL.  In chapter 89
 * threads do not migrate after creation: the chosen CPU is the
 * one the thread will run on for life.  Returns NULL on OOM. */
struct thread *thread_create_on(uint32_t cpu_id,
                                thread_entry_fn entry,
                                void *arg,
                                const char *name);

/* Chapter 89 — initialise the calling CPU's idle thread record.
 *
 * Called once from secondary_main on each CPU N >= 1.  Allocates
 * a bare `struct thread` (no heap stack — the idle thread runs on
 * the CPU's already-allocated secondary boot stack), installs it
 * as both `cpu_current()->idle` and `cpu_current()->current`, but
 * does NOT push it onto any runqueue and does NOT add it to the
 * global `all` list.  The idle thread is invisible to
 * thread_lookup / child reap / wait().
 *
 * Idle threads are off-runq by design: yield() switches to the
 * CPU's idle when its runq is empty and switches AWAY from idle
 * the next time anything lands on the runq (e.g. via
 * thread_create_on + IPI_RESCHED).
 *
 * Returns 0 on success, -1 on OOM. */
int thread_secondary_init_idle(const char *name);

/* Spawn a thread that erets to EL0 with PC=user_entry_va and
 * SP_EL0=user_sp_top.  The kernel-side stack returned in t->stack_base
 * is used by SVC/IRQ handlers running on behalf of this user thread.
 * `as` is the per-process address space the thread runs in (must be
 * non-NULL).  Returns NULL on OOM. */
struct thread *user_thread_create(uint64_t user_entry_va,
                                  uint64_t user_sp_top,
                                  const char *name,
                                  struct address_space *as);

/* Chapter 91 — spawn a thread that shares an existing AS.  Bumps
 * the AS's refcount internally (caller does NOT need to bump it
 * first).  The new thread's first user-mode register file has
 * x0 = arg, SP_EL0 = user_sp_top, ELR_EL1 = user_entry_va, and
 * TPIDR_EL0 = tls (or 0 to disable TLS).  Returns NULL on OOM;
 * on success the AS retains an extra reference until the new
 * thread exits and is reaped. */
struct thread *user_thread_create_shared(uint64_t user_entry_va,
                                         uint64_t user_sp_top,
                                         const char *name,
                                         struct address_space *as,
                                         uint64_t arg,
                                         uint64_t tls);

/* Chapter 92 — same as user_thread_create_shared but lets the
 * caller pin the new thread to a specific CPU.  cpu_id is the
 * absolute CPU number (0..SMP_MAX_CPUS-1) the thread will run on
 * for its lifetime; pass -1 to inherit the creating CPU (the
 * chapter-91 default).  Out-of-range cpu_id returns NULL.  When
 * cpu_id != cpu_current_id() the new thread is enqueued on the
 * remote CPU's runqueue and an IPI_RESCHED is sent so the target
 * picks it up promptly. */
struct thread *user_thread_create_shared_on(uint64_t user_entry_va,
                                            uint64_t user_sp_top,
                                            const char *name,
                                            struct address_space *as,
                                            uint64_t arg,
                                            uint64_t tls,
                                            int cpu_id);

/* Chapter 93 — same as user_thread_create_shared_on plus an
 * extra `share_fdt` knob.  When non-zero the new thread adopts
 * the calling thread's fd_table by reference (CLONE_FILES);
 * when zero the new thread gets a fresh private table (same
 * behaviour as the chapter 92 entry point above).  Used by
 * sys_clone3. */
struct thread *user_thread_create_shared_files_on(uint64_t user_entry_va,
                                                   uint64_t user_sp_top,
                                                   const char *name,
                                                   struct address_space *as,
                                                   uint64_t arg,
                                                   uint64_t tls,
                                                   int cpu_id,
                                                   int share_fdt);

/* Yield to the next READY thread.  Re-enqueues the caller as READY
 * unless its state has already been changed (e.g. by thread_exit). */
void yield(void);

/* Voluntarily run the scheduler.  Same as yield() except for the
 * intent: schedule() is also what the IRQ-driven preemption path
 * calls in milestone 5. */
void schedule(void);

/* Terminate the calling thread with the given exit code.  Marks it
 * EXITED, frees its stack on the next reap pass, and wakes the
 * parent if it is currently in wait(). */
void thread_exit(int code) __attribute__((noreturn));

/* Block until any child thread (parent_id == current->id) reaches
 * THREAD_EXITED, then reap it.  Returns the child's id and stores
 * its exit code in `*code_out` (NULL OK).  Returns -1 if the caller
 * has no children. */
int thread_wait(int *code_out);

/* Generalised reaper used by SYS_WAITPID.
 *   target_pid > 0 : wait for that specific child only.
 *   target_pid <=0 : wait for any child (same as thread_wait).
 *   options & WNOHANG: do not block; return 0 if the matching
 *                       child(ren) exist but none have exited yet.
 * Returns the reaped child's id (and stores exit code in
 * *code_out if non-NULL), 0 for the WNOHANG no-exit-yet case, or
 * -1 if no child matches the filter at all. */
int thread_waitpid(int target_pid, int *code_out, int options);

/* Look up a thread by id; returns NULL if not found.  Used by
 * sys_wait when called with a specific tid. */
struct thread *thread_lookup(int id);

/* Total threads in any state — used for shutdown/idle decisions. */
size_t thread_count(void);

/* Currently-running thread (never NULL after thread_init). */
struct thread *thread_current(void);

/* Block the current thread for at least `ms` milliseconds.
 * Sets state = THREAD_SLEEPING, wake_at_ms, and yields.  The
 * scheduler walks sleepers on every yield and re-readies any
 * whose wake time has passed. */
void thread_sleep_ms(uint64_t ms);

/* Block the current thread on resource `token`.  Sets
 * state = THREAD_BLOCKED, blocked_on = token, and yields.
 * Returns when some other thread calls thread_wake_blocked
 * with the same token.  Caller is responsible for re-checking
 * the resource's wakeup condition (spurious wakes are allowed
 * but unlikely with our single-CPU scheduler). */
void thread_block_on(void *token);

/* Wake all threads currently THREAD_BLOCKED with
 * blocked_on == token.  Marks them READY and pushes to the
 * runqueue; clears blocked_on. */
void thread_wake_blocked(void *token);

/* Chapter 92 — cross-CPU-safe block primitive used by the
 * futex slow path.
 *
 * Pattern:
 *   uint64_t f = thread_global_lock();
 *   ... read predicate from user memory ...
 *   if (no need to block) { thread_global_unlock(f); return; }
 *   thread_block_on_held(token, f);   // unlocks + yields
 *
 * The single global lock is the same one all_push/all_remove use,
 * so a futex_wait holding it is mutually-exclusive with a
 * concurrent thread_wake_blocked walk.  That's what closes the
 * lost-wakeup race a cross-CPU unlocker can otherwise create
 * between (predicate check) and (state = BLOCKED). */
uint64_t thread_global_lock(void);
void     thread_global_unlock(uint64_t flags);
void     thread_block_on_held(void *token, uint64_t flags);

/* Signal delivery — minimum-viable POSIX subset.
 *
 * thread_signal_pid(pid, sig): sets bit (1 << sig) in the
 *   target thread's sig_pending mask.  No-op if the pid does
 *   not name a live thread.  Does NOT itself terminate the
 *   target — the signal is observed and acted on at the next
 *   syscall return (or inside a cooked-mode read).
 *
 * Foreground-pid bookkeeping: g_fg_pid names the thread that
 *   should receive a SIGINT when Ctrl-C (0x03) arrives at the
 *   cooked-mode console.  Set by the shell to the spawned
 *   child's pid before wait(), and cleared after.  Zero means
 *   "no foreground process — Ctrl-C is consumed silently".
 */
void thread_signal_pid(int pid, int sig);
int  thread_get_fg_pid(void);
void thread_set_fg_pid(int pid);

/* Forward decl for thread_fork_user.  Real definition in
 * exception.h — we don't drag the whole header in here. */
struct exception_frame;

/* Build a fork-child of `parent`.
 *
 * Allocates a thread struct + 16 KiB kernel stack, copies all
 * inheritable state (cwd, env, args, fd table) from `parent`,
 * sets parent_id = parent->id, attaches `child_as`, and lays
 * out an initial 288-byte cswitch_to-style exception frame at
 * the top of the new kernel stack such that:
 *
 *   - On first cswitch_to into the child, the frame restores
 *     ELR_EL1 = parent_frame->elr (= the user PC right after
 *     the SVC #0 that called fork), SPSR_EL1 = parent_frame->
 *     spsr (= EL0t), and SP_EL0 = parent_sp_el0.
 *   - All GPRs x1..x30 are copied verbatim from parent_frame.
 *   - x0 is set to 0 so the child sees fork() return 0.
 *
 * The dispatcher's tail will then write the parent's frame->x[0]
 * to the parent's child-pid return value.  Child gets 0; parent
 * gets the new tid.
 *
 * `parent_sp_el0` must be the value of SP_EL0 at the moment of
 * the SVC (read by the caller via `mrs x16, sp_el0`); save_context
 * in vectors.S does not capture SP_EL0, so the caller has to
 * snapshot it before any C-side bookkeeping that might switch
 * stacks.
 *
 * Returns the new thread (READY, on the runqueue) or NULL on
 * OOM (caller is responsible for cleaning up child_as in that
 * case). */
struct thread *thread_fork_user(struct thread *parent,
                                struct address_space *child_as,
                                const struct exception_frame *parent_frame,
                                uint64_t parent_sp_el0);

/* Copy `parent`'s entire in-use fd table over `child`'s.  Used
 * by sys_spawn{,_pipe,_redir} so that a child spawned by a
 * shell which itself has fd 0/1/2 wired to a pty (or a pipe, or
 * a tmpfs file) inherits those fds the same way a POSIX
 * fork+exec child would.  Pipe + pty refcounts are bumped
 * symmetrically; any pre-existing FD_PTY_* / FD_PIPE_* slot in
 * `child` is properly dropped before being overwritten, so it's
 * safe to call after `vfs_init_fdtable` has staged the default
 * console fds.  Sockets are deliberately NOT inherited (same
 * convention as thread_fork_user). */
void thread_inherit_fds(struct thread *child, struct thread *parent);

/* Replace the calling thread's display name (the inline `name`
 * buffer).  Used by sys_exec after a successful AS swap so that
 * `ps`-style listings show the new program path.  Bounded copy
 * with truncation to THREAD_NAME_MAX-1 source bytes. */
void thread_rename(struct thread *t, const char *new_name);

/* Chapter 99 — /proc snapshot record.  Designed to be cheap to
 * fill from inside the g_all_lock-held iteration loop in
 * thread_snapshot() so callers can release the lock before any
 * expensive formatting work.  All scalar; no pointers into
 * kernel structures escape. */
struct thread_snap {
    int      id;
    int      parent_id;
    int      state;          /* enum thread_state, cast to int    */
    uint32_t home_cpu;
    int      tty_raw;
    int      exit_code;      /* valid iff state == THREAD_EXITED  */
    uint64_t wake_at_ms;     /* valid iff state == THREAD_SLEEPING*/
    char     name[THREAD_NAME_MAX];
    char     args[THREAD_ARGS_MAX];
    char     cwd[THREAD_CWD_MAX];
};

/* Copy a snapshot of every live thread into `out`, up to `max`
 * records.  Returns the number of records actually written.
 * Holds g_all_lock for the duration of the walk so the list
 * doesn't mutate underneath us; the callback is therefore
 * memcpy-only and must NOT block.  Callers (procfs.c) format
 * the snapshots into text AFTER the call so the lock window
 * stays short. */
int thread_snapshot(struct thread_snap *out, int max);

/* Find one thread's snapshot by pid.  Returns 1 on hit (and
 * fills `*out`), 0 if no such pid.  Same lock discipline as
 * thread_snapshot — caller's formatting work happens after we
 * return. */
int thread_snapshot_pid(int pid, struct thread_snap *out);

/* Chapter 100 — render /proc/<pid>/trace into `out[cap]` and
 * drain the target thread's tracer ring.  Holds g_all_lock for
 * the duration of the render so the target thread cannot be
 * freed underneath the formatter.  Returns the byte length
 * written, or -1 if no such pid exists. */
long thread_strace_render_pid(int pid, char *out, size_t cap);

/* Per-CPU runqueue length, lockless approximation.  Sums every
 * THREAD_READY thread on g_all_head whose home_cpu == cpu_id.
 * Cheap enough for /proc/sched on a couple of cores; if we ever
 * grow O(threads) we'll cache per-cpu_state counters instead. */
int thread_runqueue_len(uint32_t cpu_id);

/* Total number of live threads (everything on g_all_head). */
int thread_live_count(void);

#endif /* THREAD_H */
