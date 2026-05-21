#!/usr/bin/env bash
# scripts/fetch_public_roots.sh -- Chapter 112g.
#
# Copy the host system's public CA root list to OUTPUT so that
# scripts/mkcabundle.py can fold it into /mnt/ca.bundle.  This is
# what lets the in-guest browser talk to https://news.ycombinator.com,
# https://example.com etc. without going through the host-side
# TLS-stripping proxy (chapter 106a) -- the in-guest validator now
# trusts the same roots your host browser does.
#
# We deliberately do NOT commit the resulting PEM:
#
#   - It is ~150-300 KiB of churn-prone bytes (roots get added,
#     removed, distrusted as CAs misbehave).
#   - Every developer machine already has a curated, up-to-date
#     copy maintained by the OS or by `certifi` / `ca-certificates`.
#   - Pinning the bundle in-repo would let it drift silently from
#     what real browsers trust, defeating the whole point of
#     "the OS speaks to the same internet the host does".
#
# If no system bundle is found, the script prints actionable
# guidance and exits non-zero -- the developer is expected to drop
# a PEM at OUTPUT manually (e.g. from https://curl.se/ca/cacert.pem)
# rather than have the build silently ship an empty trust store.
#
# Usage:
#   bash scripts/fetch_public_roots.sh vendor/testcerts/public-roots.pem
set -euo pipefail

OUTPUT="${1:-vendor/testcerts/public-roots.pem}"
mkdir -p "$(dirname "$OUTPUT")"

# Candidate system bundle locations, in order of preference.
# macOS: /etc/ssl/cert.pem is the LibreSSL-shipped bundle, updated
# by Apple's security updates.  Linux: pick the first present.
CANDIDATES=(
    /etc/ssl/cert.pem                       # macOS, Alpine, FreeBSD
    /etc/ssl/certs/ca-certificates.crt      # Debian, Ubuntu
    /etc/pki/tls/certs/ca-bundle.crt        # RHEL, CentOS, Fedora
    /etc/ca-certificates/extracted/tls-ca-bundle.pem  # Arch
    /usr/local/etc/openssl@3/cert.pem       # Homebrew openssl@3
    /usr/local/etc/openssl/cert.pem         # Homebrew openssl@1.1
    /opt/homebrew/etc/openssl@3/cert.pem    # Apple Silicon Homebrew
)

SRC=""
for cand in "${CANDIDATES[@]}"; do
    if [[ -f "$cand" && -r "$cand" ]]; then
        SRC="$cand"
        break
    fi
done

if [[ -z "$SRC" ]]; then
    echo "fetch_public_roots: no system CA bundle found." >&2
    echo "" >&2
    echo "Searched (in order):" >&2
    for cand in "${CANDIDATES[@]}"; do
        echo "    $cand" >&2
    done
    echo "" >&2
    echo "To proceed, drop a PEM bundle at:" >&2
    echo "    $OUTPUT" >&2
    echo "" >&2
    echo "A common choice is curl's mirror of the Mozilla NSS roots:" >&2
    echo "    curl -o $OUTPUT https://curl.se/ca/cacert.pem" >&2
    exit 1
fi

COUNT="$(grep -c -- '-----BEGIN CERTIFICATE-----' "$SRC" || true)"

# Atomic write so an interrupted copy doesn't leave a half-written
# bundle that mkcabundle.py would silently accept.
TMP="${OUTPUT}.tmp.$$"
cp "$SRC" "$TMP"
mv "$TMP" "$OUTPUT"

SIZE="$(wc -c < "$OUTPUT" | tr -d ' ')"
echo "fetch_public_roots: copied $SRC -> $OUTPUT ($COUNT certs, $SIZE bytes)"
