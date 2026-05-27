# Chapter 24 — Hardening the kernel/user boundary

Per-process address spaces (chapter 23) gave each user process its
own page tables. That's the structural half of isolation. This
chapter is the *enforcement* half: closing the two real holes that
let a malicious user reach kernel memory anyway.

## Hole #1: DRAM identity blocks were EL0-RW

Look at what the boot L1 actually had after `kernel_main` finished
populating it for our 8 GiB QEMU:

```
slot 0  0x00000000..0x40000000   BLOCK_DEVICE       AP=00 (kernel only)
slot 1  0x40000000..0x80000000   BLOCK_NORMAL       AP=00 (kernel only)
slot 2  0x80000000..0xC0000000   BLOCK_NORMAL_USER  AP=01 (kernel + EL0!)
slot 3  0xC0000000..0x100000000  BLOCK_NORMAL_USER  AP=01
slot 4  0x100000000..0x140000000 BLOCK_NORMAL_USER  AP=01
...
slot 8  0x200000000..0x240000000 BLOCK_NORMAL_USER  AP=01  ← the heap
```

`BLOCK_NORMAL_USER` was added back when user binaries linked at
fixed VAs *inside* one of these blocks (originally `0x80100000`
in slot 2). The user code at EL0 needed to be able to fetch its
own instructions and access its own data, so the slot containing
that VA had to be EL0-accessible. Cheap and easy with 1 GiB
blocks.

The price was that **every byte of DRAM in slots 2..8 was
EL0-writable**. The kernel heap, the kernel's own page tables, the
boot stack, every other process's memory — all visible to any
user thread. The "isolation" of per-process address spaces was a
lie: the per-process L1 *replaced* slot 64 (the new user range
above DRAM, see chapter 23), but every other slot was *inherited*
from the boot L1 with the same `BLOCK_NORMAL_USER` permission.

Once chapter 23 moved the user range to slot 64 (above all DRAM),
the user-RW bit on slots 2..8 was just dead code waiting to be
exploited. This chapter removes it. The change is one line in
`pmap_install_ram_block_1gib`:

```c
- l1_pgtable[idx] = BLOCK_NORMAL_USER(pa);
+ l1_pgtable[idx] = BLOCK_NORMAL(pa);     /* AP=00, kernel only */
```

To prove the change works, write a one-page user program called
`badpoke`:

```c
int main(void) {
    volatile unsigned long *kaddr = (volatile unsigned long *)0x230000000UL;
    *kaddr = 0xDEADBEEFCAFEBABEUL;        /* should fault */
    puts("[badpoke] FAIL: write succeeded — isolation broken");
    return 1;
}
```

Before this chapter this silently succeeded. Now:

```
$ /bin/badpoke
[badpoke] about to write to a kernel address; should fault
[svc] FATAL: non-SVC sync exception from EL0
        ESR_EL1 = 0x9200004d   (Data Abort, ISV=0, WnR=1, DFSC=0x0d)
        EC      = 0x24          Data Abort from a lower EL
        FAR_EL1 = 0x230000000   (the kernel address it tried to write)
        ELR_EL1 = 0x100010007c  (in user code)
        thread  = /bin/badpoke
[sh] exit -1
```

The MMU caught the write, raised a Data Abort from EL0, the
kernel's existing fault handler killed the process, and the shell
moved on. Real isolation in eight bytes of source.

## Hole #2: syscalls trusted user pointers

With `BLOCK_NORMAL_USER` removed, the user can no longer touch
kernel memory *directly*. But it can still ask the kernel to do
the dereferencing on its behalf. Look at the old `sys_read`:

```c
static long sys_read(long fd, long buf_ptr, long len)
{
    void *buf = (void *)(uintptr_t)buf_ptr;
    return vfs_read((int)fd, buf, (size_t)len);
}
```

`vfs_read` will eventually `memcpy` data into `buf`. The kernel
runs at EL1, where AP=00 grants kernel-RW. So
`sys_read(0, 0x230000000, 16)` would happily copy 16 bytes of
file content into the kernel heap. The user can't write to
`0x230000000` directly, but they can ask the kernel to do it for
them.

Three syscalls had this hole:

| call         | hole                                          |
|--------------|-----------------------------------------------|
| `sys_read`   | kernel writes to user-supplied buf            |
| `sys_write`  | kernel reads from user-supplied buf            |
| `sys_open`   | kernel reads a string from user-supplied path  |
| `sys_spawn`  | kernel reads strings from path AND args        |
| `sys_wait`   | kernel writes int to user-supplied code_out    |
| `sys_getargs`| kernel writes string to user-supplied buf      |

The fix is the same family of primitives every kernel ends up
with: `copy_from_user`, `copy_to_user`,
`copy_string_from_user`. The new `kernel/core/uaccess.h` declares
them and the implementation in `uaccess.c` does just one thing
worth talking about — bounds-check the user range:

```c
static int range_in_user(uint64_t uptr, size_t len)
{
    if (uptr < USER_VA_BASE || uptr >= USER_VA_END)
        return 0;
    if (len > USER_VA_END)            /* catch overflow in uptr+len */
        return 0;
    if (uptr + len > USER_VA_END)
        return 0;
    return 1;
}
```

`USER_VA_BASE` is `0x1000000000` and `USER_VA_END` is
`0x1040000000` (slot 64). Kernel addresses live in slots 0..8
(below `0x240000000`), so they fail the very first comparison.
This single check rules out *every* kernel pointer.

Then every syscall that takes a user pointer routes through
these:

```c
static long sys_read(long fd, long buf_ptr, long len)
{
    if (len < 0) return -EINVAL;
    if (len == 0) return 0;
    if (uaccess_check((uint64_t)buf_ptr, (size_t)len) < 0)
        return -EFAULT;
    /* Active TTBR0 IS the caller's AS, so vfs_read can write
     * directly into the user buffer.  This is safe because we
     * just proved the buffer is in user range. */
    return vfs_read((int)fd, (void *)(uintptr_t)buf_ptr, (size_t)len);
}
```

For string-typed arguments (paths) we walk the user buffer to
find a NUL and copy it into a kernel-owned buffer:

```c
static long sys_spawn(long name_ptr, long args_ptr)
{
    char path[128];
    long pn = copy_string_from_user(path, (uint64_t)name_ptr, sizeof(path));
    if (pn < 0) return pn;
    ...
}
```

This has the side benefit that the kernel keeps its own copy of
the path through the rest of the spawn — important now that
destroying the parent's AS at the wrong moment would invalidate
any pointer pointing into it.

A second test program, `badptr`, exercises the new boundary:

```c
int main(void) {
    int rc;
    rc = read(0, (char *)0x230000000UL, 16);    /* expect -EFAULT */
    putd_signed(rc);
    rc = write(1, (const char *)0x230000000UL, 16);
    putd_signed(rc);
    rc = open((const char *)0x230000000UL, 0);
    putd_signed(rc);
    return 0;
}
```

```
$ /bin/badptr
[badptr] sys_read(fd=0, buf=KERN, 16) -> -14
[badptr] sys_write(fd=1, buf=KERN, 16) -> -14
[badptr] sys_open(path=KERN, 0) -> -14
[badptr] all three rejected, exiting 0
```

`-14` is `-EFAULT`. The kernel did not touch `0x230000000` at any
point in any of those calls.

## A subtle bug: the ustrnlen overshoot

The first attempt at `copy_string_from_user` (and its helper
`ustrnlen`) had this check:

```c
if (!range_in_user(uptr, maxlen))
    return -EFAULT;
```

It looked correct — "fail if the *entire* maxlen-sized window
isn't in user range." But it broke every shell command. The shell
keeps a `char line[128]` on its stack:

```c
int main(void) {
    char line[LINE_MAX];          /* LINE_MAX == 128 */
    ...
    int tid = spawn(line, args);  /* line points near top of user stack */
}
```

Sh's user stack is at `USER_STACK_TOP - 16 KiB`, so `line` ends
up only a few hundred bytes below `USER_VA_END`. The strict
"`uptr + 128` must be in range" check rejected it because `uptr +
128` overshot `USER_VA_END` even though we'd find the trailing
NUL within ~10 bytes.

The fix is to treat `maxlen` as a *cap* on how far we'll walk,
not as a requirement that the full window be valid:

```c
long ustrnlen(uint64_t uptr, size_t maxlen) {
    if (uptr < USER_VA_BASE || uptr >= USER_VA_END)
        return -EFAULT;
    size_t avail = (size_t)(USER_VA_END - uptr);
    size_t cap   = maxlen < avail ? maxlen : avail;
    const char *p = (const char *)(uintptr_t)uptr;
    for (size_t i = 0; i < cap; i++)
        if (p[i] == '\0') return (long)i;
    if (cap < maxlen)         /* hit the user-range edge */
        return -EFAULT;
    return -ENAMETOOLONG;     /* hit the caller's cap before NUL */
}
```

This is the kind of bug that's easy to miss in code review and
trivial to find by running every shell command. Test programs
catch what static analysis can't.

## What's still wrong (and what to do about it later)

The bounds check rules out *kernel* addresses. It does not catch
*wild user* addresses — a user program that hands the kernel a
pointer to an unmapped page in its own VA range will cause an
EL1 Data Abort inside `copy_from_user`, and the current handler
treats EL1 faults as fatal. A malicious user can crash the
kernel. Real systems handle this with one of:

- **PAN/UAO.** ARMv8.1 has Privileged Access Never (kernel can't
  touch EL0 memory by default) and User Access Override (the
  copy primitive temporarily lifts PAN inside an inline-asm
  block; an EL1 fault inside that window is treated specially).
- **Fixup tables.** The compiler emits an entry for each
  potentially-faulting load/store instruction inside a "user
  copy" function, with the address of a recovery label. The
  fault handler looks up the faulting PC, jumps to the recovery
  label, which returns -EFAULT to the syscall.

Both are about 100 lines of work and not on the critical path
for what we're building toward (a windowing system, then a
browser). They go on the list.

The other deferreds still apply: ASIDs, fork, exec, real argv on
the user stack, user heap. Each is its own milestone.

## What changed

```
kernel/arch/page_tables.c       BLOCK_NORMAL_USER → BLOCK_NORMAL
kernel/core/uaccess.h           NEW
kernel/core/uaccess.c           NEW
kernel/core/syscall.c           every syscall taking a user
                                pointer routes through uaccess
userspace/badpoke/badpoke.c     NEW — kernel-write isolation test
userspace/badptr/badptr.c       NEW — syscall-pointer test
Makefile                        wires badpoke + badptr into disk
```

Two new test programs that should never need to be modified
again, two new uaccess files, and one line of permissions
change. That's the whole of this chapter.
