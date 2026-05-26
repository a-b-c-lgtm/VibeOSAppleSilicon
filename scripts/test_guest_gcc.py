#!/usr/bin/env python3
# scripts/test_guest_gcc.py - chapter 132e smoke test.
#
# NOT in scripts/sweep.sh.  Host-side toolchain sanity, run
# manually after editing the wrapper, libc, or the gcc patch /
# configure cache.  Build artefacts under
# build/gcc-build-guest/  (separate from the chapter-132c
# build/gcc-build-host/ so the two compilers cannot poison
# each other).
#
# What it pins down (iterative; this file grows phase-by-phase
# the same way chapter 131d -> 131e did):
#
#   Phase 1 (this commit): gmp / mpfr / mpc cross-configure
#                          under --host=aarch64-osdev with our
#                          aarch64-osdev-cc wrapper as CC.
#   Phase 2:               gmp / mpfr / mpc cross-BUILD,
#                          producing libgmp.a / libmpfr.a /
#                          libmpc.a as aarch64 archives.
#   Phase 3:               gcc top-level configure +
#                          all-gcc sub-build.
#   Phase 4:               xgcc / cpp / cc1 emerge as aarch64
#                          ELF binaries.
#
# This file currently implements through Phase 1 only;
# subsequent phases extend the SUBDIRS list and add per-subdir
# Makefile fixups in the same style as test_guest_ld.py.

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WRAPPER = os.path.join(ROOT, "build", "toolchain", "bin",
                       "aarch64-osdev-cc")
XGCC = os.path.join(ROOT, "build", "toolchain", "bin",
                    "aarch64-osdev-gcc")
TOOLCHAIN_BIN = os.path.join(ROOT, "build", "toolchain", "bin")

# Vendor source layout.
GMP_SRC = os.path.join(ROOT, "vendor", "gmp-6.2.1")
MPFR_SRC = os.path.join(ROOT, "vendor", "mpfr-4.1.0")
MPC_SRC = os.path.join(ROOT, "vendor", "mpc-1.2.1")
GCC_SRC = os.path.join(ROOT, "vendor", "gcc-14.2.0")

# Guest cross build tree.
BUILD = os.path.join(ROOT, "build", "gcc-build-guest")
PREFIX = os.path.join(ROOT, "build", "toolchain-guest")
CACHE = os.path.join(ROOT, "scripts", "aarch64-osdev-configure.cache")

# Phase 1 subdirs: math prereqs only.  Order matters - mpfr
# depends on gmp, mpc depends on both gmp and mpfr.
PHASE1_SUBDIRS = [
    ("gmp",  GMP_SRC,  []),
    ("mpfr", MPFR_SRC, ["--with-gmp=" + os.path.join(BUILD, "gmp",
                                                     "install")]),
    ("mpc",  MPC_SRC,  ["--with-gmp=" + os.path.join(BUILD, "gmp",
                                                     "install"),
                        "--with-mpfr=" + os.path.join(BUILD, "mpfr",
                                                      "install")]),
]


def fail(msg):
    print(f"guest_gcc: FAIL - {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, cwd, env=None):
    return subprocess.run(cmd, cwd=cwd, env=env,
                          capture_output=True, text=True)


def first_errors(stderr, n=8):
    bad = []
    for line in stderr.splitlines():
        ls = line.lstrip()
        if ls.startswith("|") or (ls[:1].isdigit() and "|" in ls[:8]):
            continue
        if ": error:" in line or "Error " in line or \
                "configure: error" in line:
            bad.append(line)
            if len(bad) >= n:
                break
    return "\n    ".join(bad) if bad else stderr[-2000:]


def main():
    # Skip cleanly if the toolchain inputs aren't built.
    if not os.path.exists(WRAPPER):
        print(f"guest_gcc: SKIP - {WRAPPER} not installed "
              f"(run `make aarch64-osdev-cc-install`)")
        sys.exit(0)
    if not os.path.exists(XGCC):
        print(f"guest_gcc: SKIP - {XGCC} not present "
              f"(run `make gcc-osdev`)")
        sys.exit(0)
    for src in (GMP_SRC, MPFR_SRC, MPC_SRC, GCC_SRC):
        if not os.path.exists(src):
            print(f"guest_gcc: SKIP - {src} missing "
                  f"(run `make gcc-prereqs` first)")
            sys.exit(0)
    if not os.path.exists(os.path.join(ROOT, "build", "userspace",
                                       "crt", "crt0.o")):
        print(f"guest_gcc: SKIP - crt0.o missing (run `make` first)")
        sys.exit(0)

    if os.path.exists(BUILD):
        if os.environ.get("KEEP") == "1":
            # Iterative mode: reuse the existing tree (skips already-
            # configured subdirs, lets make pick up incremental
            # progress).  Useful when iterating Phase 4 traps.
            pass
        else:
            shutil.rmtree(BUILD)
    os.makedirs(BUILD, exist_ok=True)

    env = os.environ.copy()
    env["PATH"] = TOOLCHAIN_BIN + os.pathsep + env.get("PATH", "")
    env["CC"] = "aarch64-osdev-cc"
    # CFLAGS chosen to match the chapter-131e env exactly: NDEBUG
    # to neuter assert(), OSDEV_LIBC_NO_GLOBAL_DEFS to suppress
    # per-TU `environ` / `__cxa_finalize`, OSDEV_LIBC_NO_GETOPT
    # so libiberty's getopt.c owns the single shared optarg/optind.
    env["CFLAGS"] = ("-mcpu=cortex-a72 -DNDEBUG "
                     "-DOSDEV_LIBC_NO_GLOBAL_DEFS "
                     "-DOSDEV_LIBC_NO_GETOPT")
    env["CXXFLAGS"] = env["CFLAGS"]
    env["LDFLAGS"] = ""
    env["CONFIG_SITE"] = CACHE

    # ---- Phase 1: configure gmp / mpfr / mpc one at a time ----
    for name, src, extra_args in PHASE1_SUBDIRS:
        sd = os.path.join(BUILD, name)
        install = os.path.join(sd, "install")
        if os.environ.get("KEEP") == "1" and os.path.exists(
                os.path.join(install, "lib", f"lib{name}.a")):
            print(f"guest_gcc: skip {name} (KEEP=1, already built)")
            continue
        print(f"guest_gcc: configuring {name}...")
        os.makedirs(sd, exist_ok=True)
        args = [
            os.path.join(src, "configure"),
            "--srcdir=" + src,
            "--prefix=" + install,
            "--build=aarch64-apple-darwin",
            "--host=aarch64-osdev",
            "--disable-shared",
            "--disable-nls",
        ]
        if name == "gmp":
            # GMP picks an assembly backend per ABI; force the
            # plain C path so it works with our minimal assembler
            # support out of the box.  Same flag chapter 132b used
            # for the host build.
            args += ["--disable-assembly", "ABI=64"]
        if name == "mpfr":
            # MPFR's configure runs a tsearch() link probe that
            # tries to invoke the cross compiler.  Our libc has
            # tsearch as a header-only static inline; pre-set the
            # cache var so configure doesn't try.
            env_sub = dict(env)
            env_sub["ac_cv_func_tsearch"] = "no"
        else:
            env_sub = env
        args += extra_args
        r = run(args, cwd=sd, env=env_sub)
        if r.returncode != 0:
            fail(f"{name} configure failed (rc={r.returncode}):\n"
                 f"    {first_errors(r.stderr)}\n"
                 f"--- last 80 lines of config.log ---\n" +
                 _tail_log(os.path.join(sd, "config.log"), 80))

        # mpfr/mpc need gmp installed (and mpc needs mpfr).  Build
        # and install right after each configure so the next
        # subdir's --with-gmp / --with-mpfr resolves to a real
        # install tree.  Phase 1 thus collapses the configure +
        # build cycle for the math prereqs into one pass.
        print(f"guest_gcc: building {name}...")
        rb = run(["make", "-j" + str(os.cpu_count() or 4)],
                 cwd=sd, env=env_sub)
        if rb.returncode != 0:
            fail(f"{name} build failed (rc={rb.returncode}):\n"
                 f"    {first_errors(rb.stderr)}")
        print(f"guest_gcc: installing {name}...")
        ri = run(["make", "install"], cwd=sd, env=env_sub)
        if ri.returncode != 0:
            fail(f"{name} install failed (rc={ri.returncode}):\n"
                 f"    {first_errors(ri.stderr)}")

    print(f"guest_gcc: phase 1 OK - gmp/mpfr/mpc configured, "
          f"cross-built, and installed under --host=aarch64-osdev")

    # ---- Phase 3: configure gcc itself under --host=aarch64-osdev ----
    # We DO NOT yet attempt `make all-gcc`; this phase pins down the
    # configure surface first.  Same iterative model as 131e/132e.
    print("guest_gcc: configuring gcc (phase 3)...")
    gcc_build = os.path.join(BUILD, "gcc")
    os.makedirs(gcc_build, exist_ok=True)
    gmp_install = os.path.join(BUILD, "gmp", "install")
    mpfr_install = os.path.join(BUILD, "mpfr", "install")
    mpc_install = os.path.join(BUILD, "mpc", "install")

    gcc_args = [
        os.path.join(GCC_SRC, "configure"),
        "--srcdir=" + GCC_SRC,
        "--prefix=" + os.path.join(gcc_build, "install"),
        "--build=aarch64-apple-darwin",
        "--host=aarch64-osdev",
        "--target=aarch64-osdev",
        # No libstdc++, no libgcc target libs - those need a working
        # libc/headers we haven't shipped to the guest yet.  Phase 4
        # is host xgcc only: cc1, cpp, the driver, collect2.
        "--disable-bootstrap",
        "--disable-libstdcxx",
        "--disable-libssp",
        "--disable-libquadmath",
        "--disable-libatomic",
        "--disable-libgomp",
        "--disable-libitm",
        "--disable-libsanitizer",
        "--disable-libvtv",
        "--disable-multilib",
        "--disable-nls",
        "--disable-shared",
        "--enable-languages=c",
        "--without-headers",
        "--with-gmp=" + gmp_install,
        "--with-mpfr=" + mpfr_install,
        "--with-mpc=" + mpc_install,
    ]
    if os.environ.get("KEEP") == "1" and os.path.exists(
            os.path.join(gcc_build, "Makefile")):
        print("guest_gcc: skip gcc configure (KEEP=1, "
              "Makefile already present)")
    else:
        r = run(gcc_args, cwd=gcc_build, env=env)
        if r.returncode != 0:
            fail("gcc configure failed (rc={}):\n    {}\n"
                 "--- last 80 lines of config.log ---\n{}".format(
                     r.returncode, first_errors(r.stderr),
                     _tail_log(os.path.join(gcc_build, "config.log"),
                               80)))

    print("guest_gcc: phase 3 OK - gcc configured under "
          "--host=aarch64-osdev")

    # ---- Phase 4: cross-build all-gcc (xgcc / cc1 / cpp etc.) ----
    # `make all-gcc` builds every host-side gcc binary as
    # aarch64-osdev ELF.  Skips libgcc/libstdc++/libssp/libgomp
    # target libs (disabled in phase 3).  This is the big build:
    # ~hundreds of TUs, will surface most of the remaining libc
    # gaps in the wrapper + cstring.o.
    print("guest_gcc: building gcc (phase 4 - make all-gcc)...")
    _stub_libcody(gcc_build)
    cpu = os.cpu_count() or 4
    r = run(["make", f"-j{cpu}", "all-gcc",
             "MAKEINFO=true"],
            cwd=gcc_build, env=env)
    if r.returncode != 0:
        # Build log is huge; surface the last ~80 lines so the
        # next trap is visible without paging.
        log_tail = (r.stdout or "")[-6000:] + (r.stderr or "")[-6000:]
        fail("gcc all-gcc failed (rc={}):\n--- tail ---\n{}".format(
            r.returncode, log_tail))

    xgcc = os.path.join(gcc_build, "gcc", "xgcc")
    if not os.path.isfile(xgcc):
        fail(f"all-gcc reported success but {xgcc} missing")

    print("guest_gcc: PASS (phase 4) - xgcc built at " + xgcc)
    sys.exit(0)


def _tail_log(path, n):
    try:
        with open(path, "r") as f:
            lines = f.readlines()
        return "".join(lines[-n:])
    except OSError:
        return "(no config.log)"


def _stub_libcody(gcc_build):
    """Chapter 132f trap A: libcody is C++ code that needs the
    full STL (<memory>, <string>, <vector>, ...).  We ship a
    C-only libc; cross-building libcody is a multi-chapter
    rabbit hole on its own.  But cc1 (the C frontend) doesn't
    reference any cody:: symbol -- only cc1plus does, and we
    disabled C++ via --enable-languages=c.  So we override the
    top-level Makefile's libcody rules with no-ops by appending
    overriding recipes.  make warns about the duplicate
    definition but uses the last one, which is exactly what we
    want.  Adds one stamp so the second test run doesn't append
    again.
    """
    mk = os.path.join(gcc_build, "Makefile")
    marker = "# chapter-132f: libcody stubbed (C-only build)"
    with open(mk) as f:
        src = f.read()
    if marker in src:
        return
    with open(mk, "a") as f:
        f.write(
            "\n" + marker + "\n"
            "configure-libcody:\n"
            "\t@true\n"
            "all-libcody:\n"
            "\t@true\n"
            "install-libcody:\n"
            "\t@true\n"
            "check-libcody:\n"
            "\t@true\n"
        )
    print("guest_gcc: stubbed libcody in top-level Makefile")


if __name__ == "__main__":
    main()
