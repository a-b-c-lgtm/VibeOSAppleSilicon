#!/usr/bin/env python3
"""
mkcabundle.py -- Chapter 112e/112f: build a framed CA bundle for
osdev's in-guest TLS trust store.

Reads CA certificates from one or more inputs and writes a
framed binary bundle that the in-guest TLS client
(userspace/libc/tls_socket.c, tls_socket_init_chain_from_bundle)
can parse without dragging in a base64 decoder or an ASN.1
wrapper parser.

Two input formats are accepted, freely intermixable:

  * BearSSL chain-style header (default):
        path/to/chain-foo.h
    The file is scanned for `static const unsigned char CERT1[] =
    { ... };` and the CERT1[] bytes are appended.  (CERT1 is the
    CA that signed CERT0 in a chain-*.h pair, NOT the leaf --
    see chapter 112e for the rationale.)

  * PEM (chapter 112f):
        --pem path/to/roots.pem
    The file is scanned for one or more
    `-----BEGIN CERTIFICATE-----` / `-----END CERTIFICATE-----`
    blocks; each block is base64-decoded and appended.  This is
    the format that the Mozilla NSS root list and every public
    CA distribution use, so plugging in a real-world root set is
    just `--pem mozilla-roots.pem` with no recompile.

Bundle format ("CAB1"):

    [4]  magic  = b"CAB1"
    [4]  count  = u32 little-endian
    for each cert (count times):
        [4]  der_len = u32 LE
        [der_len]    DER bytes (raw X.509 certificate)

Why a custom format instead of PEM-at-rest:

  - The guest already has a working DER-blob entry point
    (tls_socket_init_chain_from_anchor takes raw DER).  Keeping
    the on-disk shape close to that minimizes new code in the
    guest -- the bundle parser is ~30 lines.
  - PEM at rest would force a base64 decoder into userspace just
    to bootstrap the trust store.
  - PKCS#7/PKCS#12 would add ASN.1 wrapper parsing for no gain
    over an array of bare DER blobs.

Why CERT1 and not CERT0 for the .h path:

  - CERT0 is the end-entity certificate.  Trusting that as an
    anchor amounts to public-key pinning -- chapter 112b shape,
    not the goal.
  - CERT1 is the CA that signed CERT0.  Trusting it means the
    validator actually verifies CERT0's signature against
    CERT1's public key.  In a 3-tier chain (root -> intermediate
    -> leaf) the intermediate is what gets baked into CHAIN[];
    the root sits separately in cert-root-*.pem and is what
    chapter 112f bakes into the bundle via --pem so the
    validator does the full recursive walk.

Usage:
    mkcabundle.py OUT.bundle  [--pem FILE | HEADER.h]  ...

Inputs are processed in order; the bundle's anchor list is the
concatenation of every cert extracted from every input.
"""

import base64
import os
import re
import struct
import sys


CERT1_RE = re.compile(
    rb"static\s+const\s+unsigned\s+char\s+CERT1\s*\[\s*\]\s*=\s*\{"
    rb"([^}]*)\}\s*;",
    re.DOTALL,
)
BYTE_RE = re.compile(rb"0x([0-9A-Fa-f]{1,2})")

PEM_BLOCK_RE = re.compile(
    rb"-----BEGIN CERTIFICATE-----\s*"
    rb"([A-Za-z0-9+/=\s]+?)"
    rb"-----END CERTIFICATE-----",
    re.DOTALL,
)


def parse_cert1_from_header(path: str) -> list:
    """Return [DER bytes of CERT1[]] from a BearSSL chain header."""
    with open(path, "rb") as f:
        src = f.read()
    m = CERT1_RE.search(src)
    if not m:
        raise RuntimeError(f"{path}: no `CERT1[]` definition found")
    body = m.group(1)
    bytes_out = bytearray()
    for hexm in BYTE_RE.finditer(body):
        bytes_out.append(int(hexm.group(1), 16))
    if not bytes_out:
        raise RuntimeError(f"{path}: CERT1[] is empty")
    return [bytes(bytes_out)]


def parse_pem_certs(path: str) -> list:
    """Return [DER, DER, ...] for every CERTIFICATE block in PEM `path`.

    Tolerates surrounding whitespace, comment lines, and embedded
    other PEM types (PRIVATE KEY, etc.) -- only -----BEGIN
    CERTIFICATE----- blocks are extracted.
    """
    with open(path, "rb") as f:
        src = f.read()
    out = []
    for m in PEM_BLOCK_RE.finditer(src):
        b64 = b"".join(m.group(1).split())     # strip every whitespace byte
        try:
            der = base64.b64decode(b64, validate=True)
        except Exception as exc:
            raise RuntimeError(
                f"{path}: bad base64 in CERTIFICATE block: {exc}"
            ) from exc
        if not der:
            raise RuntimeError(f"{path}: empty CERTIFICATE block")
        out.append(der)
    if not out:
        raise RuntimeError(
            f"{path}: no `-----BEGIN CERTIFICATE-----` blocks found"
        )
    return out


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(
            "usage: mkcabundle.py OUT.bundle  [--pem FILE | HEADER.h]  ...\n"
        )
        return 2
    out_path = argv[1]

    ders = []
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--pem":
            i += 1
            if i >= len(argv):
                sys.stderr.write("mkcabundle: --pem requires a path\n")
                return 2
            ds = parse_pem_certs(argv[i])
            ders.extend(ds)
            sys.stdout.write(
                f"mkcabundle: --pem {argv[i]}: "
                f"{len(ds)} cert(s) extracted "
                f"({sum(len(d) for d in ds)} DER bytes total)\n"
            )
        else:
            ds = parse_cert1_from_header(a)
            ders.extend(ds)
            sys.stdout.write(
                f"mkcabundle: {a}: CERT1 = {len(ds[0])} DER bytes\n"
            )
        i += 1

    if not ders:
        sys.stderr.write("mkcabundle: no certificates collected\n")
        return 2

    # Frame.
    buf = bytearray()
    buf += b"CAB1"
    buf += struct.pack("<I", len(ders))
    for d in ders:
        buf += struct.pack("<I", len(d))
        buf += d

    # Write atomically so a build interrupted mid-write can't
    # leave a half-baked bundle in OSFS sources.
    tmp_path = out_path + ".tmp"
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(tmp_path, "wb") as f:
        f.write(buf)
    os.replace(tmp_path, out_path)

    total = sum(len(d) for d in ders)
    sys.stdout.write(
        f"mkcabundle: wrote {out_path} "
        f"({len(buf)} bytes, {len(ders)} anchors, {total} DER bytes total)\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
