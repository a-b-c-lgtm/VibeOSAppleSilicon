#!/usr/bin/env python3
# scripts/test_guest_configure.py — chapters 177 / 131d smoke test.
#
# NOT in scripts/sweep.sh.  Host-side toolchain sanity, run
# manually after editing the wrapper template or after touching
# any file in userspace/libc/ or scripts/aarch64-osdev-configure.cache.
#
# What it pins down:
#
#   1.  binutils-2.44's top-level configure script runs to
#       completion under --host=aarch64-osdev --target=aarch64-osdev
#       with CC=aarch64-osdev-cc.  Failure means the wrapper
#       script regressed: probably an option parse bug, or the
#       link-mode auto-inject stopped finding crt0.
#
#   2.  libiberty/configure (the first subconfigure that triggers
#       a serious link test) gets past the strerror link probe
#       without tripping GCC_NO_EXECUTABLES.  This is the test
#       that proves crt0 auto-injection works for autoconf-style
#       conftests.  Before chapter 177 the conftest link failed
#       with "undefined reference to __errno_value" and autoconf
#       refused all further link tests.
#
#   3.  ALL of libiberty/*.c (minus the small set of
#       host-environment-specific files autoconf chose not to
#       compile) builds cleanly.  This is the 131d assertion.
#       After 131d we have the Class B / C / D libc additions
#       (sleep, _exit, link, execvp, freopen, mktemp, ldexp,
#       frexp, st_dev/st_ino, variadic open) PLUS a pre-populated
#       configure cache that lies to autoconf about our
#       static-inline functions being externally linked — so the
#       libiberty replacement files don't get compiled and don't
#       clash with our headers.  See chapter 178 for the audit.
#
# Build artefacts go under build/binutils-build-guest/ which is
# in .gitignore.  The test wipes and recreates that dir on every
# run so a stale config.cache doesn't hide regressions.

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WRAPPER = os.path.join(ROOT, "build", "toolchain", "bin",
                       "aarch64-osdev-cc")
TOOLCHAIN_BIN = os.path.join(ROOT, "build", "toolchain", "bin")
SRC = os.path.join(ROOT, "vendor", "binutils-2.44")
BUILD = os.path.join(ROOT, "build", "binutils-build-guest")
PREFIX = os.path.join(ROOT, "build", "toolchain-guest")
CACHE = os.path.join(ROOT, "scripts", "aarch64-osdev-configure.cache")

# Files we expect to compile cleanly post-131c (smaller subset —
# kept here as a structural canary; if these regress the wider
# all-libiberty check below is meaningless).
EXPECTED_LIBIBERTY = [
    "alloca.o",
    "argv.o",
    "bsearch_r.o",
    "cplus-dem.o",   # exercises sprintf (chapter 177 addition)
    "regex.o",       # exercises abort via stdlib.h cascade
]

# Chapter 178: libiberty must build *every* object file it
# decides to compile.  We don't enumerate them — we just assert
# the link of `libiberty.a` succeeds.

def fail(msg):
    print(f"guest_configure: FAIL — {msg}", file=sys.stderr)
    sys.exit(1)

def run(cmd, cwd, env=None):
    r = subprocess.run(cmd, cwd=cwd, env=env,
                       capture_output=True, text=True)
    return r

def main():
    if not os.path.exists(WRAPPER):
        print(f"guest_configure: SKIP — {WRAPPER} not installed "
              f"(run `make aarch64-osdev-cc-install`)")
        sys.exit(0)
    if not os.path.exists(SRC):
        print(f"guest_configure: SKIP — {SRC} missing "
              f"(run `make binutils-osdev` first to fetch+patch)")
        sys.exit(0)
    if not os.path.exists(os.path.join(ROOT, "build", "userspace",
                                       "crt", "crt0.o")):
        print(f"guest_configure: SKIP — crt0.o missing "
              f"(run `make` first; the wrapper's link mode "
              f"needs it auto-injected)")
        sys.exit(0)

    # Fresh build dir every run; otherwise config.cache hides
    # changes to the wrapper's link-mode detection.
    if os.path.exists(BUILD):
        shutil.rmtree(BUILD)
    os.makedirs(BUILD)

    env = os.environ.copy()
    env["PATH"] = TOOLCHAIN_BIN + os.pathsep + env.get("PATH", "")
    env["CC"] = "aarch64-osdev-cc"
    env["CFLAGS"] = "-mcpu=cortex-a72"
    # The wrapper auto-adds -nostdlib -nostartfiles and -T script
    # in link mode; leave LDFLAGS empty so we don't duplicate.
    env["LDFLAGS"] = ""
    # CONFIG_SITE pre-loads our chapter-131d cache into every
    # autoconf-generated configure (top-level AND each subdir
    # configure that `make` re-invokes).  Plain --cache-file=
    # only loads at the top level; subdir configures don't
    # inherit it.  Without CONFIG_SITE, libiberty's own configure
    # re-runs all AC_CHECK_FUNCS link tests with our missing
    # mems and concludes (incorrectly) that strerror/vfprintf/
    # etc. are absent → it then compiles libiberty's own copies
    # which collide with our libc.
    env["CONFIG_SITE"] = CACHE

    print("guest_configure: running top-level configure...")
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
            "--disable-binutils",
            "--disable-ld",
            "--disable-gprof",
            "--disable-gprofng",
            "--disable-libdecnumber",
            "--disable-readline",
            "--disable-sim",
            "--disable-libquadmath",
            "--disable-libquadmath-support",
            "--disable-shared",
        ],
        cwd=BUILD, env=env,
    )
    if cfg.returncode != 0:
        fail(f"top-level configure failed (rc={cfg.returncode}):\n"
             f"  stderr tail: {cfg.stderr[-1000:]}")

    # libiberty's configure is normally invoked by `make
    # configure-libiberty` at the top level — but binutils'
    # top-level Makefile feeds CFLAGS through Make variable
    # expansion that introduces trailing whitespace, and that
    # collides with libiberty/config.cache (which gets pre-
    # populated by the top-level configure walk) under
    # autoconf's "env changed since previous run" guard.
    #
    # Bypass the whole drama by running libiberty's configure
    # ourselves with exactly the env we already control.  We
    # explicitly remove any stale config.cache the top-level
    # configure may have left behind, then point libiberty at
    # CONFIG_SITE for the chapter-131d ac_cv_func_* overrides.
    print("guest_configure: configuring libiberty...")
    libi_dir = os.path.join(BUILD, "libiberty")
    if not os.path.exists(libi_dir):
        os.makedirs(libi_dir)
    stale_cache = os.path.join(libi_dir, "config.cache")
    if os.path.exists(stale_cache):
        os.remove(stale_cache)
    cfgl = run(
        [
            os.path.join(SRC, "libiberty", "configure"),
            "--srcdir=" + os.path.join(SRC, "libiberty"),
            "--prefix=" + PREFIX,
            "--build=aarch64-apple-darwin",
            "--host=aarch64-osdev",
            "--target=aarch64-osdev",
            "--disable-shared",
            "--disable-werror",
            "--disable-multilib",
            "--with-system-zlib",
        ],
        cwd=libi_dir, env=env,
    )
    if cfgl.returncode != 0:
        fail(f"libiberty configure failed (rc={cfgl.returncode}):\n"
             f"  stderr tail: {cfgl.stderr[-1500:]}")

    print("guest_configure: building expected libiberty files...")
    mk = run(
        ["make", "-k", "-C", "libiberty"] + EXPECTED_LIBIBERTY,
        cwd=BUILD, env=env,
    )
    # `make -k` reports rc=2 on partial failures but still
    # builds whatever it can.  We don't check rc — we check
    # the artefacts directly.
    missing = []
    for o in EXPECTED_LIBIBERTY:
        op = os.path.join(BUILD, "libiberty", o)
        if not os.path.exists(op) or os.path.getsize(op) == 0:
            missing.append(o)
    if missing:
        fail(f"libiberty objects did not build: {missing}\n"
             f"  make stderr tail: {mk.stderr[-2000:]}")

    # Chapter 178 — wider assertion: ALL of libiberty must build.
    # If autoconf chose to compile a replacement we don't like,
    # the cache file needs another entry; surface that loudly.
    print("guest_configure: building all of libiberty.a...")
    mka = run(
        ["make", "-C", "libiberty", "libiberty.a"],
        cwd=BUILD, env=env,
    )
    if mka.returncode != 0:
        # Extract the first compile error so the failure message
        # is actionable.
        bad = []
        for line in mka.stderr.splitlines():
            if "error:" in line:
                bad.append(line)
                if len(bad) >= 5:
                    break
        fail(f"libiberty.a did not build (rc={mka.returncode}).\n"
             f"  first error lines:\n    "
             + ("\n    ".join(bad) if bad else mka.stderr[-1500:]))
    libiberty_a = os.path.join(BUILD, "libiberty", "libiberty.a")
    if not os.path.exists(libiberty_a):
        fail("libiberty.a missing after successful make")
    sz = os.path.getsize(libiberty_a)

    print(f"guest_configure: PASS — top-level configure clean, "
          f"libiberty {len(EXPECTED_LIBIBERTY)} canary objects "
          f"built, full libiberty.a built ({sz} bytes)")
    sys.exit(0)

if __name__ == "__main__":
    main()
