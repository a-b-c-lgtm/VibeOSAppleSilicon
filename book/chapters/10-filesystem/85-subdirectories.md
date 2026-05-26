# Chapter 85 — Subdirectories: a path walker, mkdir, and a navigable Save As

OSFS-2 has had directory-shaped inodes since chapter 81.  Look
at `kernel/core/osfs2.h`:

```c
#define OSFS2_TYPE_FREE 0
#define OSFS2_TYPE_FILE 1
#define OSFS2_TYPE_DIR  2

struct osfs2_inode {
    uint32_t type;
    uint32_t size;
    uint32_t nlink;
    uint32_t mode;
    uint32_t direct[OSFS2_DIRECT_PTRS];   /* 16 */
    uint32_t indirect;
    /* …pad to 128 bytes… */
};

struct osfs2_dirent {
    uint32_t ino;
    char     name[60];                    /* total 64 bytes */
};
```

Every inode already carries a `type` field.  Every dirent
already maps a name to an inode number.  The block-allocation
machinery (`alloc_block`, `resolve_block`, the indirect-block
walker) is type-agnostic.  The directory helpers
(`dir_find`, `dir_read_at`, `dir_write_at`) take a generic
`struct osfs2_inode *` — they don't care whether it's the
root or a subdirectory.

What was missing was the *wiring*.  Pre-chapter-85, the public
API hardcoded the root inode into every operation:

```c
uint32_t osfs2_lookup(const char *name)
{
    struct osfs2_inode root;
    if (read_inode(OSFS2_INODE_ROOT, &root) != 0) return 0;
    int idx = dir_find(&root, name, NULL);          // <-- root only
    ...
}
```

This chapter is the small, satisfying patch where we replace
"root" with "wherever the path takes us."  It also ships the
user-visible win that motivates the work: a Save As dialog
that lets you **navigate into subdirectories** and **create new
ones inline**, the way every desktop OS has done since the
mid-1990s.

## Why directories matter for the rest of the book

The notepad in chapter 84 saves to `/data/<name>`.  That works
for a handful of test files.  It does not work for:

- Two notepads open at once with the same filename in mind.
  Today they'd silently overwrite each other on Save As.
- A future `/bin/paint` BMP that wants to live under
  `/data/images/` separately from text.
- Browser cookies (chapter 95-ish), which want
  `/data/cookies/<host>` so a single hostile site can't
  flood the root directory.
- Per-app config under `/data/config/<app>/…`.

All four are solved by the same primitive: a directory.
That's why this chapter is in Part X (Persistence) rather
than punted to a Part XII polish chapter — once we build the
GUI Save As dialog without subdirectory support, every app
that touches it gets wired into the wrong assumption, and
fixing it later means changing the dialog, every consumer,
every test, and every "where does my file live?" mental model
the user has built.

## The path walker

The core change in `kernel/core/osfs2.c` is one new helper:

```c
static uint32_t walk(const char *path)
{
    if (!path || !*path || (path[0] == '/' && path[1] == '\0'))
        return OSFS2_INODE_ROOT;

    uint32_t cur = OSFS2_INODE_ROOT;
    char comp[OSFS2_NAME_MAX];
    const char *p = path;
    while ((p = path_next_component(p, comp, sizeof(comp))) || comp[0]) {
        struct osfs2_inode dir;
        if (read_inode(cur, &dir) != 0) return 0;
        if (dir.type != OSFS2_TYPE_DIR) return 0;
        int idx = dir_find(&dir, comp, NULL);
        if (idx < 0) return 0;
        struct osfs2_dirent ent;
        if (dir_read_at(&dir, (uint32_t)idx, &ent) != 0) return 0;
        cur = ent.ino;
        if (!p) break;
        comp[0] = '\0';
    }
    return cur;
}
```

It splits the path on `/`, starts at the root, looks up each
component in the directory it lands in, and follows the
resulting inode.  If any intermediate component isn't a
directory (you tried to traverse `/data/foo.txt/bar`), we bail
out with 0.  If any component doesn't exist, also 0.

That's the entire walk.  No symbolic links, no `.`, no `..`,
no relative paths (the only "current directory" in the system
lives inside the shell — the kernel always sees absolute
paths).  In ~25 lines we converted OSFS-2 from a flat
namespace to a tree.

### Why no `.` and `..` on disk

POSIX `fs/*` filesystems write `.` and `..` as the first two
dirents in every directory.  We don't.  Two reasons:

1. **Every kernel path that reaches OSFS-2 is already
   absolute.**  The shell expands `cd subdir; cat foo.txt`
   into `/data/subdir/foo.txt` before it issues the
   `open(2)`.  There is no kernel context where we need to
   interpret a relative `..` against the *current
   directory* — that's a shell concept that never makes it
   down here.

2. **Storage cost.**  `.` + `..` in every directory is two
   dirents = 128 bytes per directory.  For the OS we're
   building (a handful of dirs), that's noise.  But it would
   force us to special-case "is this a real entry or a
   self/parent reference?" everywhere we walk a directory —
   in `dir_find`, in `osfs2_listdir_at`, in `osfs2_rmdir`'s
   empty-check.  Skipping them means the on-disk format
   stays as it was in chapter 81: every dirent is a real
   thing.

   The Save As dialog *does* show a `..` row, but that's a UI
   convention — it's synthesised by the dialog at render
   time when we're below the navigation floor.  The on-disk
   directory has no `..`.

### Refactoring `osfs2_create` into a parent-aware helper

Before chapter 85, `osfs2_create` did three things:

```c
uint32_t osfs2_create(const char *name)
{
    /* 1. find_or_make a slot in the root directory       */
    /* 2. allocate a new file inode                       */
    /* 3. link them                                        */
}
```

For directories we need the same dance, just with
`OSFS2_TYPE_DIR` and a different parent.  Extract the
shared body into a helper:

```c
static uint32_t dir_create_in(uint32_t parent_ino, const char *name,
                              uint32_t type);
```

and rebuilt the public API on top of it:

```c
uint32_t osfs2_create(const char *path) /* file */
{
    uint32_t parent;
    char leaf[OSFS2_NAME_MAX];
    if (walk_parent(path, &parent, leaf, sizeof(leaf)) != 0) return 0;
    return dir_create_in(parent, leaf, OSFS2_TYPE_FILE);
}

uint32_t osfs2_mkdir(const char *path)  /* dir */
{
    uint32_t parent;
    char leaf[OSFS2_NAME_MAX];
    if (walk_parent(path, &parent, leaf, sizeof(leaf)) != 0) return 0;
    return dir_create_in(parent, leaf, OSFS2_TYPE_DIR);
}
```

`walk_parent("/data/notes/personal/foo.txt", …)` returns the
inode of `/data/notes/personal/` and copies `"foo.txt"` into
`leaf`.  The signatures match the way the dialog wants to use
them: caller hands us a complete absolute path, we figure
out the parent.

### One semantic difference between create and mkdir

`dir_create_in` has an "if name already exists, return the
existing inode" branch.  This is because `vfs_open` uses
`osfs2_create` as the implementation of `O_CREAT` —
"open-or-create" semantics depend on getting the existing
file back instead of failing.

`mkdir` doesn't want that.  POSIX `mkdir(2)` returns
`EEXIST` on a name collision; every shell script that uses
`mkdir foo && cd foo` depends on that behaviour to fail
loudly when something's already there.  So inside
`dir_create_in`:

```c
if (hit >= 0) {
    /* Name exists.  Semantics differ by requested type:
     *   - FILE: behave as create-or-open.
     *   - DIR:  always fail (POSIX EEXIST). */
    if (type == OSFS2_TYPE_DIR) return 0;
    if (existing.type != type) return 0;
    return ent.ino;
}
```

This subtlety surfaces as one test failure -- `test_directories.py`
catches it on the first run (`mkdir of existing dir fails with
errno`).  The test is good; the behaviour difference between
"create file" and "create dir" is exactly the kind of thing
that's easy to forget if the test isn't already there to
remind you.

## The mkdir syscall

Adding directory creation needed two new syscall numbers:

```c
SYS_MKDIR      = 36,    /* mkdir(path) -> 0 / -errno */
SYS_LISTDIR_AT = 37,    /* listdir_at(path, idx, name, cap,
                         *            &size, &type)         */
```

The handler is uneventful — copy the path into a kernel
buffer, gate on `/data/` prefix, hand off:

```c
static long sys_mkdir(long path_uptr)
{
    char path[128];
    long n = copy_string_from_user(path, (uint64_t)path_uptr, sizeof(path));
    if (n < 0) return n;

    if (!path_starts_with(path, "/data/")) return -EINVAL_VFS;
    if (!osfs2_present()) return -ENOENT_VFS;
    if (osfs2_mkdir(path + 6) == 0) return -ENOENT_VFS;
    return 0;
}
```

`/tmp/` is intentionally not supported — tmpfs is flat (chapter
32) and a per-mount path walker for tmpfs is a separate
feature.  `/mnt/` and `/bin/` are read-only; mkdir there is
nonsensical.

## listdir_at: the type tag matters

The existing `SYS_LISTDIR` (chapter 20) was designed when
every mount was flat.  It returns one big linear list of
*every* leaf in *every* writable mount, prefixed with the
mount path:

```
$ ls
        8  /motd.txt
      113  /mnt/hello.txt
       42  /data/foo.txt
        0  /data/notes
```

That works fine for flat output.  It doesn't work for the
Save As dialog.  The dialog needs three things `SYS_LISTDIR`
doesn't give it:

1. **The contents of one specific directory** (not "every
   leaf in the system collapsed into one list").
2. **A type tag** so it can render `<DIR>` next to
   subdirectories and a byte count next to files.
3. **A way to enumerate `/data/notes/`** without the kernel
   silently treating that as an unknown leaf (the flat
   listdir only walks the root of each mount).

`SYS_LISTDIR_AT` provides all three:

```c
long listdir_at(const char *dirpath, int idx,
                char *name, size_t cap,
                unsigned int *size_out,
                unsigned int *type_out);
```

`dirpath` is `/data` or `/data/notes` (the kernel accepts
either with or without a trailing slash).  `idx` is the
0-based index into the non-empty dirents of that directory.
`type_out` is `LISTDIR_TYPE_FILE` (1) or `LISTDIR_TYPE_DIR`
(2).

The shell `ls` opportunistically uses the new syscall when
asked for a path under `/data`:

```c
int use_at = 0;
if (have_prefix) {
    const char *p = prefix_buf;
    if (p[0] == '/' && p[1] == 'd' && p[2] == 'a' && p[3] == 't' &&
        p[4] == 'a' && (p[5] == '/' || p[5] == '\0'))
        use_at = 1;
}
```

So `ls /data` now shows entries with `<DIR>` markers properly,
and `ls /data/notes/personal` works at all (the flat listdir
couldn't have walked that).  `ls /` and bare `ls` keep the
old behaviour — they walk the flat namespace.

## Six bytes of x4/x5 in the syscall dispatcher

`SYS_LISTDIR_AT` takes six arguments: path, idx, name,
cap, size_out, type_out.  AArch64's syscall ABI passes the
first eight argument-register slots as `x0..x7`, so all six
fit, but the dispatch table in `kernel/core/syscall.c` had
only been wired for four:

```c
long a0 = (long)frame->x[0];
long a1 = (long)frame->x[1];
long a2 = (long)frame->x[2];
long a3 = (long)frame->x[3];
/* a4..a5 unused for now; reserved for future syscalls. */
```

Adding the two missing lines and a corresponding `_svc6`
wrapper in `userspace/libc/syscall.h` was a mechanical
chapter-85 detail that's worth flagging in passing -- it's
the kind of "obvious in hindsight" change that tends to be
invisible until the day a syscall actually wants more than
four arguments.

## The dialog: navigation as a UI affordance

The headline user-facing change is in
`userspace/libgui/save_dialog.c`.  Pre-85 the dialog had a
single `g_prefix` string ("`/data/`") and an unconditional
list of its direct children.  Post-85 it has two:

- `g_root` — immutable floor.  Whatever the caller passed
  in.  We never navigate above this.
- `g_dir` — the directory currently being shown.  Starts
  equal to `g_root`, grows when the user enters a subdir,
  shrinks when they hit `..`.

The list-population loop now goes through `listdir_at(g_dir,
…)` and runs in two passes — directories first, then files.
That single change reshapes the dialog from "flat directory
contents" into "what every Save As dialog you've ever used
looks like":

```
+-- Save As ----------------------+
| Save in: /data/notes/           |
|                                 |
| +---------------------------+   |
| | ..                  <UP>  |   |
| | personal/           <DIR> |   |
| | foo.txt              42 b |   |
| | bar.txt              17 b |   |
| +---------------------------+   |
|                                 |
| Filename: [____________]        |
|                                 |
| Up/Down: pick   Enter: open dir |
| Ctrl-N: new folder   ESC: cancel|
+---------------------------------+
```

### Enter does two different things

The same key — Enter — has to mean both "navigate into this
directory" and "save with this filename."  The dialog
disambiguates with two pieces of state:

1. **The kind of the highlighted row.**  `..` and `<DIR>`
   rows trigger navigation; `<file>` rows trigger save.
2. **Whether the field was typed by the user vs auto-filled
   from the selection.**  We track `g_field_from_selection`
   — it's set by `select_row()` and cleared by any keystroke
   into the field.

The two combine to the rule:

> Enter on a `..` or `<DIR>` row navigates **only if** the
> filename field is either empty or hasn't been touched by
> the user since the row was selected.  Otherwise it saves.

This is the same heuristic Mac and Windows file-pickers use —
"if the user has typed something into the filename field,
they meant Save; if they haven't, they meant Navigate."  It
fits the modal contract without having to add a "Navigate"
button.

### "..", but only sometimes

The dialog synthesises a `..` pseudo-row at the top of the
list whenever `g_dir != g_root`.  Selecting it and pressing
Enter calls `dir_pop()`, which strips the trailing path
component.  When we're already at the floor (the prefix the
caller passed in), there's no `..`: the user can't escape
upward into directories the host app didn't expect.

This is a security-and-UX hybrid choice.  A notepad that
opens the dialog with `dir_prefix = "/data/"` means "let
the user pick anywhere under /data/."  A future per-app
sandbox dialog might pass `dir_prefix = "/data/myapp/"` and
the dialog's `..` button would refuse to escape that
directory.  No new code needed — the floor enforces it for
free.

### Ctrl-N: new folder mode

The "New Folder" affordance is a sub-mode rather than a
separate widget.  When the user presses Ctrl-N (`0x0E`), the
dialog flips its title bar to "Save As — New Folder", clears
the filename field, and rerenders with the field background
in cream (`GUI_BGRA(0xFF, 0xF0, 0xC8)`) so it's visually
obvious the field now means "name of new folder" rather
than "name of file":

```c
case 0x0E:                /* Ctrl-N */
    if (g_mode == MODE_NORMAL) {
        g_mode = MODE_NEW_FOLDER;
        g_field[0] = '\0';
        g_field_len = 0;
        g_field_cur = 0;
        g_sel = -1;
        dirty = 1;
    }
    break;
```

The next Enter calls `mkdir(g_dir + g_field)`.  On success we
auto-`dir_push` into the new directory so the user can
immediately save into it (which is overwhelmingly what they
opened the dialog to do).  On failure (typically EEXIST or a
'/' in the name) we just bounce back to normal mode with the
field cleared.  The escape key, in new-folder mode, cancels
**only the mode** and not the whole dialog — handy if the
user changes their mind.

The cream colour pulls double duty as a test marker: the
chapter 85 navigation test uses pixel-counting on the cream
RGB to verify the dialog is actually in new-folder mode
(rather than just sitting there with an empty field).

### The hint line tells the user what's possible

The bottom hint area is two lines tall, sized to fit:

```
Up/Down: pick   Enter: open dir / save
Ctrl-N: new folder   ESC: cancel
```

In new-folder mode they swap to:

```
Type folder name, then Enter to create.
ESC: cancel new-folder
```

Most of the dialog code is one-pager-of-text C; the hint
lines are the only place where the user discovers these
keybindings without a manual.  It's worth a few extra
characters.

## Tests

Two new tests in this chapter:

### `scripts/test_directories.py` — kernel + shell

13 assertions, all exercising the path-walking syscalls and
the shell builtins:

- `mkdir /data/notes` succeeds, `ls /data` shows it with the
  `<DIR>` marker.
- `mkdir /data/notes/personal` works (nested mkdir).
- `echo hello > /data/notes/personal/hi.txt` writes through
  the new path resolver.
- `cat` reads it back (i.e. `vfs_open(path, O_RDONLY)`
  successfully walks two components deep).
- `/bin/sync`, reboot, then re-`ls` and re-`cat` to verify
  persistence.
- `rm /data/notes/personal/hi.txt` unlinks through the
  path resolver.
- `mkdir /data/no/such/path` correctly fails with errno
  (parent doesn't exist).
- `mkdir /data/notes` correctly fails with errno (EEXIST).

### `scripts/test_notepad_save_as_nav.py` — GUI

A QMP-driven test that:

1. Bare-launches notepad.
2. Opens the dialog with Ctrl-S.
3. Verifies the cream BG isn't visible (normal mode).
4. Presses Ctrl-N, verifies cream pixels appear (~1900 of
   them — enough to tell them apart from any incidental
   colour collision).
5. Types "scratch", Enter — folder gets created.
6. Verifies we're back in normal mode (cream pixels gone).
7. Types a filename, Enter — file is saved.
8. Verifies the dialog closed.
9. Verifies the resulting file is at `/data/scratch/<name>`.
10. Verifies `ls /data` tags `scratch` as a `<DIR>`.

Both pass.  Both are now in the regression sweep alongside
chapter 84's `test_notepad_save_as.py`.

## What's intentionally NOT in this chapter

A few things that were tempting to do, and that are
deliberately left for later:

- **`rmdir` shell builtin.**  The kernel has `osfs2_rmdir`
  with the empty-directory check, but no syscall surface
  yet.  The first real consumer (probably a "trash"
  feature for a future file manager) will pull it in.
- **`mkdir -p`.**  POSIX shells support a `-p` flag that
  creates intermediate parents.  Adding it requires
  whitespace splitting `/data/a/b/c` into three calls and
  ignoring EEXIST on each.  Doable in 20 lines of shell
  but the only consumer right now (Save As dialog) creates
  one directory at a time, so it is deferred.
- **Cross-mount mkdir.**  `mkdir /tmp/foo` returns EINVAL
  today.  Tmpfs is flat by design (chapter 32) and adding
  hierarchy there is a separate feature, not a "while
  we're here" detour.
- **A path component cap > 60.**  OSFS-2's dirent name
  field is 60 bytes (chapter 81); chapter 85 inherits that.
  A future on-disk-format-v2 might widen it, but the
  current limit is plenty for "personal/" and "drafts/"
  style usage.

## What we earned

A 100-ish-line kernel patch (path walker, mkdir, rmdir,
listdir_at, refactor of create/lookup/unlink onto the path
walker), a 20-line userspace patch (mkdir builtin, ls --type
markers), and a ~250-line dialog rewrite (navigation, new
folder mode, two-line hint).  In return:

- **The user can name and organise their files.**  This is
  the table-stakes feature every operating system needs
  before a user will trust it with anything they care about.
- **Future apps inherit it for free.**  Any future
  `gui_save_dialog()` consumer (a paint program's BMP
  exporter, a "save bookmark" affordance in the browser)
  gets directory navigation without writing a line of
  dialog code.
- **OSFS-2 stops being "flat" forever.**  Future chapters
  on cookies, per-app config, browser cache, image
  galleries, and so on can assume hierarchy.  We won't have
  to come back and fit it in later.

The shape of `walk()` is also a useful pedagogical artefact
on its own: in 25 lines of C you can see exactly how Unix-y
filesystems turn `/foo/bar/baz` into a series of disk reads.
Once you've written that walker, the difference between a
toy filesystem and a real one is mostly book-keeping —
indirect blocks, journals, atime tracking, `..` entries,
permission checks — none of which are conceptually new.

Next chapter (86, the second core), we leave Part X and turn
on SMP.  But we leave the filesystem in a state where you can
actually live with it.
