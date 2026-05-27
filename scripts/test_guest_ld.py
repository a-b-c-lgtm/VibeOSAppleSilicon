#!/usr/bin/env python3
# scripts/test_guest_ld.py — chapter 179 smoke test.
#
# NOT in scripts/sweep.sh.  Host-side toolchain sanity, run
# manually after editing the wrapper, libc, or the libiberty
# patch / configure cache.
#
# What it pins down:
#
#   1.  binutils-2.44's top-level configure runs cleanly with
#       bfd + libsframe + libctf + opcodes + ld ENABLED (chapter
#       131d only enabled libiberty).  This catches new env /
#       autoconf-cache surprises when more subdirs are in play.
#
#   2.  Each of those subdir configures runs cleanly when invoked
#       directly with our controlled env (we bypass `make
#       configure-XXX` for the same CFLAGS-whitespace reason as
#       chapter 178).
#
#   3.  `make all-ld` completes and produces the cross-built
#       `ld/ld-new` binary as an aarch64 ELF executable.
#
# Build artefacts under build/binutils-build-guest-ld/ (separate
# from chapter 178's build/binutils-build-guest/ so the two
# tests don't poison each other).  Wiped on every run.

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WRAPPER = os.path.join(ROOT, "build", "toolchain", "bin",
                       "aarch64-osdev-cc")
TOOLCHAIN_BIN = os.path.join(ROOT, "build", "toolchain", "bin")
SRC = os.path.join(ROOT, "vendor", "binutils-2.44")
BUILD = os.path.join(ROOT, "build", "binutils-build-guest-ld")
PREFIX = os.path.join(ROOT, "build", "toolchain-guest")
CACHE = os.path.join(ROOT, "scripts", "aarch64-osdev-configure.cache")

# Order matters: bfd links against libsframe.la; libctf and
# opcodes both build on bfd; gas needs bfd + opcodes;
# ld is the umbrella.  We build BOTH ld-new and gas/as-new
# (chapter 180 wires them into /bin/ld and /bin/as on the OSFS
# image).
SUBDIRS = [
    "libiberty",
    "libsframe",
    "bfd",
    "opcodes",
    "libctf",
    "gas",
    "ld",
]

def fail(msg):
    print(f"guest_ld: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)

def run(cmd, cwd, env=None):
    return subprocess.run(cmd, cwd=cwd, env=env,
                          capture_output=True, text=True)

def first_errors(stderr, n=8):
    bad = []
    for line in stderr.splitlines():
        # Real compiler errors are `<path>:<line>:<col>: error:`.
        # Skip caret-quoted source lines (start with whitespace +
        # digits + `|`) and printf format strings that contain
        # the literal word "error:".
        ls = line.lstrip()
        if ls.startswith("|") or (ls[:1].isdigit() and "|" in ls[:8]):
            continue
        if ": error:" in line or "Error " in line:
            bad.append(line)
            if len(bad) >= n:
                break
    return "\n    ".join(bad) if bad else stderr[-2000:]

def main():
    if not os.path.exists(WRAPPER):
        print(f"guest_ld: SKIP — {WRAPPER} not installed "
              f"(run `make aarch64-osdev-cc-install`)")
        sys.exit(0)
    if not os.path.exists(SRC):
        print(f"guest_ld: SKIP — {SRC} missing "
              f"(run `make binutils-osdev` first)")
        sys.exit(0)
    if not os.path.exists(os.path.join(ROOT, "build", "userspace",
                                       "crt", "crt0.o")):
        print(f"guest_ld: SKIP — crt0.o missing (run `make` first)")
        sys.exit(0)

    # Chapter 179 — vendor archives (libiberty/strdup.c,
    # cplus-dem.c, etc.) reference `extern malloc`, `extern
    # strlen`, `extern strcmp`, etc.  Our libc keeps those as
    # `static inline` in headers, so the externs are unresolved at
    # ld's final link.  cstring.o (chapter 179 block) supplies
    # asm-renamed strong symbols for all of them; we pass it via
    # LIBS= to the ld make invocation below.  Pre-build it here so
    # the binutils Makefile sees an up-to-date .o.
    cstring_o = os.path.join(ROOT, "build", "userspace", "libc",
                             "cstring.o")
    print("guest_ld: building cstring.o (libc extern wrappers)...")
    r = subprocess.run(["make", cstring_o], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"cstring.o rebuild failed (rc={r.returncode}):\n"
             f"    {first_errors(r.stderr)}")
    if not os.path.exists(cstring_o):
        fail(f"cstring.o still missing at {cstring_o}")

    if os.path.exists(BUILD):
        # macOS shutil.rmtree races on busy `.deps` autodep dirs
        # (ENOTEMPTY).  Shell `rm -rf` retries internally and is
        # robust to mid-directory churn from prior runs.  Chapter
        # 132f hit this when KEEP=1 was off and the script was
        # invoked back-to-back; the fallback to `rm -rf` makes the
        # wipe deterministic.
        rc = subprocess.run(["rm", "-rf", BUILD]).returncode
        if rc != 0 or os.path.exists(BUILD):
            fail(f"could not wipe {BUILD} (rm -rf rc={rc})")
    os.makedirs(BUILD)

    env = os.environ.copy()
    env["PATH"] = TOOLCHAIN_BIN + os.pathsep + env.get("PATH", "")
    env["CC"] = "aarch64-osdev-cc"
    # -DNDEBUG: vendor TUs include <assert.h> and would otherwise
    # emit references to __assert_fail.  Our libc only provides
    # __assert_fail as a non-static-inline body in cstring.c, which
    # we don't link into the binutils build.  -DNDEBUG turns every
    # assert() into ((void)0), matching binutils' own release
    # convention.
    #
    # -DOSDEV_LIBC_NO_GLOBAL_DEFS: same guard chapter 172's
    # Doom port uses.  Suppresses atexit.h's __cxa_finalize body
    # and env.h's `environ` definition per-TU.  Without this,
    # every one of binutils' ~150 vendor TUs that includes
    # <stdlib.h> would emit a strong __cxa_finalize and a strong
    # `environ`, producing multi-def errors at the ld-new link.
    # crt0.o's weak __cxa_finalize no-op satisfies the call;
    # cstring.o provides a weak `environ` slot for lexsup.c's
    # extern reference.
    #
    # -DOSDEV_LIBC_NO_GETOPT (chapter 180): suppresses our libc's
    # `static char *optarg` / `static int optind` and the static
    # `getopt` body in every binutils TU.  gas/as.c references
    # `optarg` via libiberty's bundled include/getopt.h (extern
    # decl) and expects it to bind to libiberty/getopt.c's strong
    # def.  Without this flag our static optarg shadows the
    # extern per-TU — getopt_long_only() updates libiberty's copy
    # while parse_args() reads its own zero-initialised one,
    # producing strcmp(NULL,"-") at gas/as.c:658 the first time a
    # non-option argv element is seen.  Flagging this globally
    # makes optarg/optind/optopt the single shared variable
    # libiberty's getopt.c already defines (with chapter 179's
    # in-file OSDEV_LIBC_NO_GETOPT guard).
    env["CFLAGS"] = ("-mcpu=cortex-a72 -DNDEBUG "
                     "-DOSDEV_LIBC_NO_GLOBAL_DEFS "
                     "-DOSDEV_LIBC_NO_GETOPT")
    env["LDFLAGS"] = ""
    env["CONFIG_SITE"] = CACHE

    print("guest_ld: running top-level configure...")
    cfg = run(
        [
            os.path.join(SRC, "configure"),
            "--host=aarch64-osdev",
            "--target=aarch64-osdev",
            "--prefix=" + PREFIX,
            "--disable-nls",
            "--disable-gdb",
            "--disable-werror",
            "--disable-multilib",
            "--with-system-zlib",
            "--disable-gprof",
            "--disable-gprofng",
            "--disable-libdecnumber",
            "--disable-readline",
            "--disable-sim",
            "--disable-libquadmath",
            "--disable-libquadmath-support",
            "--disable-shared",
            "--disable-binutils",  # gas + ar + objdump etc. — 131f's problem
            "--without-zstd",       # we don't have libzstd for aarch64-osdev
            # ld is intentionally NOT disabled this chapter.
        ],
        cwd=BUILD, env=env,
    )
    if cfg.returncode != 0:
        fail(f"top-level configure failed (rc={cfg.returncode}):\n"
             f"    {first_errors(cfg.stderr)}")

    # Direct sub-configures with controlled env (bypassing
    # `make configure-XXX` to dodge the CFLAGS-whitespace bug
    # documented in chapter 178).
    for sub in SUBDIRS:
        print(f"guest_ld: configuring {sub}...")
        sd = os.path.join(BUILD, sub)
        if not os.path.exists(sd):
            os.makedirs(sd)
        stale = os.path.join(sd, "config.cache")
        if os.path.exists(stale):
            os.remove(stale)
        src_sub = os.path.join(SRC, sub)
        args = [
            os.path.join(src_sub, "configure"),
            "--srcdir=" + src_sub,
            "--prefix=" + PREFIX,
            "--build=aarch64-apple-darwin",
            "--host=aarch64-osdev",
            "--target=aarch64-osdev",
            "--disable-shared",
            "--disable-werror",
            "--disable-multilib",
            "--disable-nls",
            "--with-system-zlib",
            "--without-zstd",
        ]
        if sub == "bfd":
            args += ["--disable-plugins"]
        if sub == "ld":
            args += ["--disable-plugins"]
        if sub == "gas":
            # gas reads target via --target=aarch64-osdev; no extra
            # flags needed beyond the shared set.  TARG_CPU is
            # derived from target triple by configure.tgt.
            pass
        r = run(args, cwd=sd, env=env)
        if r.returncode != 0:
            fail(f"{sub} configure failed (rc={r.returncode}):\n"
                 f"    {first_errors(r.stderr)}")

        # Strip `-lz` from generated Makefile.  `--with-system-zlib`
        # bakes `ZLIB = -lz` into bfd/ld; we don't ship a libz.a
        # for aarch64-osdev (zlib.h is header-only static-inline,
        # see chapter 179).  The actual zlib calls are unreachable
        # in the normal link path, so dropping the library is safe.
        mf = os.path.join(sd, "Makefile")
        if os.path.exists(mf):
            with open(mf, "r") as f:
                txt = f.read()
            txt2 = txt.replace("ZLIB = -lz", "ZLIB =")
            txt2 = txt2.replace("ZLIB =  -lz", "ZLIB =")
            # For ld and gas specifically, append cstring.o to the
            # final-link LDADD variable (ld_new_LDADD / as_new_LDADD)
            # so the link picks up extern malloc / strlen / strcmp /
            # etc. from our libc extern wrappers.  Same trap as ld:
            # we use Makefile injection rather than `make LIBS=`
            # because LIBS leaks into the libtool libdep.la build,
            # which rejects non-libtool objects.  These _LDADD
            # variables are only consumed by the as-new / ld-new
            # link lines.
            if sub == "ld":
                old_ldadd = "ld_new_LDADD = "
                new_ldadd = f"ld_new_LDADD = {cstring_o} "
                if old_ldadd in txt2 and new_ldadd not in txt2:
                    txt2 = txt2.replace(old_ldadd, new_ldadd, 1)
            if sub == "gas":
                old_ldadd = "as_new_LDADD = "
                new_ldadd = f"as_new_LDADD = {cstring_o} "
                if old_ldadd in txt2 and new_ldadd not in txt2:
                    txt2 = txt2.replace(old_ldadd, new_ldadd, 1)
            if txt2 != txt:
                with open(mf, "w") as f:
                    f.write(txt2)

    # Build sequence: each subdir in dependency order.
    for sub in SUBDIRS:
        print(f"guest_ld: building {sub}...")
        if sub in ("bfd", "ld"):
            # bfd needs `make headers` first for some generated .h
            # like bfd.h, bfd-in3.h, etc.
            if sub == "bfd":
                rh = run(["make", "headers"], cwd=os.path.join(BUILD, sub),
                         env=env)
                if rh.returncode != 0:
                    fail(f"bfd headers failed (rc={rh.returncode}):\n"
                         f"    {first_errors(rh.stderr)}")
        # cstring.o is injected into ld_new_LDADD via Makefile
        # rewrite above (NOT via `make LIBS=`, which leaks into
        # libtool's libdep.la link line and gets rejected).
        cmd = ["make"]
        r = run(cmd, cwd=os.path.join(BUILD, sub), env=env)
        if r.returncode != 0:
            fail(f"{sub} build failed (rc={r.returncode}):\n"
                 f"    {first_errors(r.stderr)}")

    # Final artefacts: ld/ld-new and gas/as-new (binutils
    # convention for the freshly-built tools before `make install`
    # renames them).
    ld_new = os.path.join(BUILD, "ld", "ld-new")
    if not os.path.exists(ld_new):
        fail(f"ld-new not produced at {ld_new}")
    ld_sz = os.path.getsize(ld_new)

    as_new = os.path.join(BUILD, "gas", "as-new")
    if not os.path.exists(as_new):
        fail(f"as-new not produced at {as_new}")
    as_sz = os.path.getsize(as_new)

    # Verify both are aarch64 ELF executables (not Mach-O host
    # binaries somehow).
    for label, path in (("ld-new", ld_new), ("as-new", as_new)):
        r = run(["aarch64-elf-readelf", "-h", path], cwd=BUILD,
                env=env)
        if r.returncode != 0:
            fail(f"aarch64-elf-readelf failed on {path}: {r.stderr}")
        if "AArch64" not in r.stdout:
            fail(f"{label} is not AArch64 ELF:\n{r.stdout[:500]}")

    print(f"guest_ld: PASS \u2014 ld-new ({ld_sz} bytes) + "
          f"as-new ({as_sz} bytes) built for aarch64")
    sys.exit(0)

if __name__ == "__main__":
    main()
