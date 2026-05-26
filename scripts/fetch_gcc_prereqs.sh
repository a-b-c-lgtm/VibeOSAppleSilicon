#!/usr/bin/env bash
# scripts/fetch_gcc_prereqs.sh — vendor GMP / MPFR / MPC.
#
# Chapter 132b.  These three libraries are GCC's link-time
# dependencies for arbitrary-precision arithmetic (used by
# the constant folder, the front-end for `__builtin_…` math,
# and the diagnostic for overflow in integer constants).
# GCC 14.2.0 won't link without all three.
#
# We take the easy path GCC's own `contrib/download_prerequisites`
# uses: download each tarball into vendor/, then symlink the
# extracted source directories into vendor/gcc-14.2.0/{gmp,mpfr,mpc}.
# When configure sees those subdirs it switches into "in-tree"
# build mode — gmp/mpfr/mpc get built before gcc itself as part
# of the same `make all-gcc` invocation, against the same host
# compiler, statically linked into the final xgcc binary. No
# separate `--with-gmp=…` / `--with-mpfr=…` plumbing required,
# no version skew with whatever Homebrew happens to ship.
#
# Versions are pinned to whatever gcc-14.2.0's own
# `contrib/prerequisites.sha512` lists.  Cross-checked here
# via sha256 too (independent computation, our own pin):
#   gmp-6.2.1.tar.bz2  eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c
#   mpfr-4.1.0.tar.bz2 feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926
#   mpc-1.2.1.tar.gz   17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459
#
# Idempotent: if everything is in place, this script is a no-op.
#
# Bandwidth: ~5 MiB total download, ~50 MiB on disk after extract.
# Tarballs and extracted dirs are .gitignore'd.
#
# Prerequisite: vendor/gcc-14.2.0/.patched-osdev must exist
# (i.e. run scripts/fetch_gcc.sh first, or `make gcc-osdev-src`).
# We refuse to symlink into a source tree that isn't there.

set -eu

cd "$(dirname "$0")/.."

GCC_SRC="vendor/gcc-14.2.0"
GCC_MARKER="$GCC_SRC/.patched-osdev"
MARKER="$GCC_SRC/.prereqs-osdev"

# (name | tarball | sha256 | extracted dir | gcc-source symlink name)
PKGS=(
  "gmp|gmp-6.2.1.tar.bz2|eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c|gmp-6.2.1|gmp"
  "mpfr|mpfr-4.1.0.tar.bz2|feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926|mpfr-4.1.0|mpfr"
  "mpc|mpc-1.2.1.tar.gz|17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459|mpc-1.2.1|mpc"
)

BASE_URL="http://gcc.gnu.org/pub/gcc/infrastructure"

if [ ! -f "$GCC_MARKER" ]; then
    echo "fetch_gcc_prereqs: $GCC_MARKER missing — run scripts/fetch_gcc.sh first" >&2
    exit 1
fi

mkdir -p vendor

for pkg in "${PKGS[@]}"; do
    IFS='|' read -r name tarball sha256 extdir linkname <<< "$pkg"
    tar_path="vendor/$tarball"
    ext_path="vendor/$extdir"
    link_path="$GCC_SRC/$linkname"

    # 1. download if missing or hash-mismatched.
    need_download=1
    if [ -f "$tar_path" ]; then
        actual=$(shasum -a 256 "$tar_path" | awk '{print $1}')
        if [ "$actual" = "$sha256" ]; then
            need_download=0
        else
            echo "fetch_gcc_prereqs: $name sha256 mismatch, re-downloading"
            rm -f "$tar_path"
        fi
    fi
    if [ "$need_download" = "1" ]; then
        echo "fetch_gcc_prereqs: downloading $tarball"
        curl -fL --progress-bar "$BASE_URL/$tarball" -o "$tar_path"
        actual=$(shasum -a 256 "$tar_path" | awk '{print $1}')
        if [ "$actual" != "$sha256" ]; then
            echo "fetch_gcc_prereqs: $name sha256 FAILED" >&2
            echo "  expected $sha256" >&2
            echo "  got      $actual" >&2
            exit 1
        fi
    fi

    # 2. extract if missing.
    if [ ! -d "$ext_path" ]; then
        echo "fetch_gcc_prereqs: extracting $tarball"
        case "$tarball" in
            *.tar.bz2) tar -xjf "$tar_path" -C vendor ;;
            *.tar.gz)  tar -xzf "$tar_path" -C vendor ;;
            *)         echo "unknown extension: $tarball" >&2; exit 1 ;;
        esac
    fi

    # 3. apply the chapter-132e config.sub osdev-suffix patch
    #    if not yet applied.  Each tarball ships an autoconf-era
    #    config.sub that rejects unknown OS suffixes; the patch
    #    adds `osdev*` so cross-builds with --host=aarch64-osdev
    #    pass the very first sanity check inside configure.
    #
    #    Idempotency strategy: the marker file `.patched-osdev`
    #    records a successful apply.  If the marker is missing
    #    but the patch is already present (e.g. someone edited
    #    the source by hand during bring-up, the way chapter
    #    132e was developed), `patch --dry-run -R` succeeds —
    #    we treat that as "already applied", just write the
    #    marker, and move on.  Otherwise we apply forward.
    patch_path="vendor/${name}-aarch64-osdev.patch"
    pkg_marker="$ext_path/.patched-osdev"
    if [ -f "$patch_path" ] && [ ! -f "$pkg_marker" ]; then
        if patch --dry-run -R -s -f -p1 -d "$ext_path" < "$patch_path" \
                >/dev/null 2>&1; then
            echo "fetch_gcc_prereqs: $patch_path already applied to $ext_path"
            touch "$pkg_marker"
        else
            echo "fetch_gcc_prereqs: applying $patch_path"
            patch -p1 -d "$ext_path" < "$patch_path"
            touch "$pkg_marker"
        fi
    fi

    # 4. symlink into the gcc source tree as an in-tree subdir.
    #    Use a relative symlink so the gcc source tree stays
    #    movable (configure happily follows ../$extdir).
    if [ ! -e "$link_path" ]; then
        echo "fetch_gcc_prereqs: linking $linkname -> ../$extdir"
        ln -s "../$extdir" "$link_path"
    elif [ ! -L "$link_path" ]; then
        echo "fetch_gcc_prereqs: $link_path exists but isn't a symlink — refusing to clobber" >&2
        exit 1
    fi
done

touch "$MARKER"
echo "fetch_gcc_prereqs: ready (gmp, mpfr, mpc in-tree under $GCC_SRC/)"
