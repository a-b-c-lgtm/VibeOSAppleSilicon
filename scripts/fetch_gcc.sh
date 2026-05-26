#!/usr/bin/env bash
# scripts/fetch_gcc.sh — vendor gcc-14.2.0 source.
#
# Chapter 132a needs the gcc source tree on disk so we can
# patch the aarch64-osdev triple into it.  We pin to 14.2.0
# because that matches the host's `aarch64-elf-gcc` (Homebrew
# install at chapter 132a writing time) — keeping versions
# matched avoids "host gcc says X about a struct layout, guest
# gcc says Y" debugging that has bitten us before.
#
# Idempotent: if the tarball is already present with the
# expected sha256 and the extracted tree already exists with
# its .patched-osdev marker, this script does nothing.
#
# Bandwidth: ~88 MiB download, ~860 MiB on disk after extract.
# Both paths are .gitignore'd.

set -eu

cd "$(dirname "$0")/.."

URL="https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz"
TARBALL="vendor/gcc-14.2.0.tar.xz"
SHA256="a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9"
SRCDIR="vendor/gcc-14.2.0"
PATCH="vendor/gcc-aarch64-osdev.patch"
MARKER="$SRCDIR/.patched-osdev"

mkdir -p vendor

# 1. download if missing or hash-mismatched.
need_download=1
if [ -f "$TARBALL" ]; then
    actual=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
    if [ "$actual" = "$SHA256" ]; then
        need_download=0
        echo "fetch_gcc: tarball present, sha256 matches"
    else
        echo "fetch_gcc: tarball sha256 mismatch, re-downloading"
        rm -f "$TARBALL"
    fi
fi
if [ "$need_download" = "1" ]; then
    echo "fetch_gcc: downloading $URL"
    curl -fL --progress-bar "$URL" -o "$TARBALL"
    actual=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
    if [ "$actual" != "$SHA256" ]; then
        echo "fetch_gcc: sha256 verification FAILED" >&2
        echo "  expected $SHA256" >&2
        echo "  got      $actual" >&2
        exit 1
    fi
    echo "fetch_gcc: sha256 verified"
fi

# 2. extract if missing.
if [ ! -d "$SRCDIR" ]; then
    echo "fetch_gcc: extracting to $SRCDIR (~860 MiB on disk)"
    tar -xJf "$TARBALL" -C vendor
fi

# 3. apply the aarch64-osdev patch unless the marker says
#    we've already done it.  The marker survives partial
#    builds (we only re-extract if the whole tree is gone).
if [ ! -f "$MARKER" ]; then
    echo "fetch_gcc: applying $PATCH"
    patch -p1 -d "$SRCDIR" < "$PATCH"
    touch "$MARKER"
    echo "fetch_gcc: patched (marker at $MARKER)"
fi

# 4. quick sanity-check: the patched config.sub must accept
#    aarch64-osdev as a canonical triple.  If this fails the
#    patch is broken; bail loudly so a downstream Makefile
#    target doesn't go on to spend an hour building the wrong
#    thing.
got=$(bash "$SRCDIR/config.sub" aarch64-osdev 2>&1)
if [ "$got" != "aarch64-unknown-osdev" ]; then
    echo "fetch_gcc: config.sub aarch64-osdev did not canonicalise" >&2
    echo "  expected aarch64-unknown-osdev" >&2
    echo "  got      $got" >&2
    exit 1
fi
echo "fetch_gcc: ready (aarch64-osdev → aarch64-unknown-osdev)"
