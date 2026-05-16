#!/usr/bin/env python3
"""scripts/img_to_bgra.py — convert any image to a raw BGRA blob.

Used by the build to bake `assets/backgrounds/*.jpg` into a raw
BGRA blob that the userspace `desktop` process reads from /mnt/
wallpaper.bgra and blits into a PIN_TO_BOTTOM window covering the
screen.

Output format:
    8-byte header: u32 width, u32 height (both little-endian)
    width * height * 4 bytes: B, G, R, 0 packed pixels (matches
    the kernel framebuffer's B8G8R8X8 layout exactly so the
    desktop process can blit straight in via gui_present).

Embedding the dimensions lets the desktop process tolerate a
build-time/run-time resolution mismatch (it can centre, clip, or
just refuse to paint) instead of writing past the end of its
window's pixel buffer.

Pixel layout matches the framebuffer (B8G8R8X8 packed in a u32 with
the byte order B, G, R, 0 in memory on a little-endian host).

Usage:
    img_to_bgra.py <input> <output> <width> <height>

Resize strategy: cover-fit (preserve aspect, crop overflow).  This
keeps the picture from looking distorted when the source aspect
differs from the framebuffer's actual aspect ratio.
"""
import struct, sys

# PIL (Pillow) is a third-party dependency.  We don't ship a
# requirements.txt because the only thing in the project that
# needs it is this one script, and the user has already had to
# rerun `make clean && make` and hit this import error multiple
# times.  Auto-bootstrap on first miss: try a couple of pip
# install strategies (plain `--user`, then `--user
# --break-system-packages` to satisfy PEP 668 on macOS system
# Python), and only then give up with an actionable hint.
try:
    from PIL import Image  # type: ignore
except ImportError:
    import subprocess

    def _try_install(args):
        try:
            subprocess.check_call(
                [sys.executable, "-m", "pip", "install", "--quiet"] + args,
            )
            return True
        except Exception:
            return False

    print("img_to_bgra: pillow not found, attempting auto-install ...",
          file=sys.stderr)
    installed = (
        _try_install(["--user", "pillow"]) or
        _try_install(["--user", "--break-system-packages", "pillow"]) or
        _try_install(["--break-system-packages", "pillow"])
    )
    if not installed:
        print(
            "img_to_bgra: could not auto-install pillow.  Install it "
            "manually and re-run, e.g.:\n"
            "    python3 -m pip install --user pillow\n"
            "  or, if that fails (PEP 668 on macOS system Python):\n"
            "    python3 -m pip install --user --break-system-packages pillow\n"
            "  or, in a virtualenv:\n"
            "    python3 -m venv .venv && .venv/bin/pip install pillow",
            file=sys.stderr,
        )
        sys.exit(2)
    # Retry the import.  If pip put the package into a --user
    # site-packages dir that this interpreter doesn't already have
    # on sys.path (rare but possible on freshly-bootstrapped boxes),
    # nudge it onto the path before retrying.
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        import site
        for d in site.getusersitepackages() if isinstance(
                site.getusersitepackages(), list) else [site.getusersitepackages()]:
            if d not in sys.path:
                sys.path.insert(0, d)
        from PIL import Image  # type: ignore

def main():
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

    im = Image.open(src).convert("RGB")
    sw, sh = im.size

    # Cover-fit: scale by the LARGER of the two ratios so the image
    # fully covers the target box, then centre-crop.
    sx = W / sw
    sy = H / sh
    s = max(sx, sy)
    nw = int(round(sw * s))
    nh = int(round(sh * s))
    im = im.resize((nw, nh), Image.LANCZOS)
    cx = (nw - W) // 2
    cy = (nh - H) // 2
    im = im.crop((cx, cy, cx + W, cy + H))
    assert im.size == (W, H)

    # Pack as B G R 0 (little-endian B8G8R8X8 = (R<<16)|(G<<8)|B).
    rgb = im.tobytes()                                    # R G B per pixel
    out = bytearray(W * H * 4)
    for i in range(W * H):
        r = rgb[i * 3 + 0]
        g = rgb[i * 3 + 1]
        b = rgb[i * 3 + 2]
        out[i * 4 + 0] = b
        out[i * 4 + 1] = g
        out[i * 4 + 2] = r
        out[i * 4 + 3] = 0

    header = struct.pack("<II", W, H)
    with open(dst, "wb") as f:
        f.write(header)
        f.write(out)
    print(f"wrote {dst}: {W}x{H} BGRA + 8B header = {8 + len(out)} bytes "
          f"(from {sw}x{sh})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
