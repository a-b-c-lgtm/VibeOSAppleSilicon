#!/usr/bin/env bash
# scripts/fetch_binutils.sh — vendor binutils-2.44 source.
#
# Chapter 131a needs the binutils source tree on disk so we can
# patch the aarch64-osdev triple into it and host-build the
# cross-binutils.  We pin to 2.44 because that's the version
# Homebrew shipped at chapter-131a writing time (the host's
# aarch64-elf-as is also 2.44 — keeping versions matched
# avoids weird "host binutils produces X, guest binutils
# produces Y" symptom-debugging when something goes wrong).
#
# Idempotent: if the tarball is already present with the
# expected sha256 and the extracted tree already exists with
# its .patched-osdev marker, this script does nothing.
#
# Bandwidth: ~27 MiB download, ~370 MiB on disk after extract.
# Both paths are .gitignore'd.

set -eu

cd "$(dirname "$0")/.."

URL="https://ftp.gnu.org/gnu/binutils/binutils-2.44.tar.xz"
TARBALL="vendor/binutils-2.44.tar.xz"
SHA256="ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237"
SRCDIR="vendor/binutils-2.44"
PATCH="vendor/binutils-aarch64-osdev.patch"
MARKER="$SRCDIR/.patched-osdev"

mkdir -p vendor

# 1. download if missing or hash-mismatched.
need_download=1
if [ -f "$TARBALL" ]; then
    actual=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
    if [ "$actual" = "$SHA256" ]; then
        need_download=0
        echo "fetch_binutils: tarball present, sha256 matches"
    else
        echo "fetch_binutils: tarball sha256 mismatch, re-downloading"
        rm -f "$TARBALL"
    fi
fi
if [ "$need_download" = "1" ]; then
    echo "fetch_binutils: downloading $URL"
    curl -fL --progress-bar "$URL" -o "$TARBALL"
    actual=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
    if [ "$actual" != "$SHA256" ]; then
        echo "fetch_binutils: sha256 verification FAILED" >&2
        echo "  expected $SHA256" >&2
        echo "  got      $actual" >&2
        exit 1
    fi
    echo "fetch_binutils: sha256 verified"
fi

# 2. extract if missing.
if [ ! -d "$SRCDIR" ]; then
    echo "fetch_binutils: extracting to $SRCDIR"
    tar -xJf "$TARBALL" -C vendor
fi

# 3. apply the aarch64-osdev patch unless the marker says
#    we've already done it.  The marker survives partial
#    builds (we only re-extract if the whole tree is gone).
if [ ! -f "$MARKER" ]; then
    echo "fetch_binutils: applying $PATCH"
    patch -p1 -d "$SRCDIR" < "$PATCH"
    touch "$MARKER"
    echo "fetch_binutils: patched (marker at $MARKER)"
fi

# 4. quick sanity-check: the patched config.sub must accept
#    aarch64-osdev as a canonical triple.  If this fails the
#    patch is broken; bail loudly so the Makefile doesn't go
#    on to spend ten minutes building the wrong thing.
got=$(bash "$SRCDIR/config.sub" aarch64-osdev 2>&1)
if [ "$got" != "aarch64-unknown-osdev" ]; then
    echo "fetch_binutils: config.sub aarch64-osdev did not canonicalise" >&2
    echo "  expected aarch64-unknown-osdev" >&2
    echo "  got      $got" >&2
    exit 1
fi
echo "fetch_binutils: ready (aarch64-osdev → aarch64-unknown-osdev)"
