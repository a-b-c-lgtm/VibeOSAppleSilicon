#!/usr/bin/env python3
"""scripts/make_test_png.py — bake a small known-pixels PNG.

Usage:
    make_test_png.py [--kind=KIND] <output.png>

KINDs (all 16x16 except large_palette which is 64x64):

  rgba     (default) — colour type 6 with transparent corners +
                       solid-red bg + green diagonal + blue 4x4
                       landmark in bottom-right.  Used by
                       test_png.py and test_browser_image.py.

  palette  —           colour type 3 (8-bit indexed) with a
                       4-colour palette: red / green / blue /
                       white in four 8x8 quadrants.  Used by
                       test_png_palette.py.

  gray     —           colour type 0 (8-bit grayscale) with a
                       diagonal gradient from black to white.
                       Used by test_png_palette.py.

  large_palette —      64x64, identical four-quadrant pattern to
                       `palette` but with 32x32 quadrants.  Used
                       by test_browser_intrinsic_size.py to prove
                       the layout-pass intrinsic-size hook is
                       actually using the cache's dimensions
                       rather than the 16x16 fallback.  Total
                       4 * 32 * 32 = 4096 pixels (1024 per
                       quadrant).

Why bake at build time rather than commit a binary?  Two reasons:

  1. The PNG byte stream isn't reproducible — Pillow's deflate
     output depends on the host's zlib version.  If we committed
     a binary, an `git apply` round-trip might silently mutate
     the file.  Baking at build time means the bytes match
     whatever zlib our host has, and the test asserts on the
     decoded *pixels* (which ARE reproducible) rather than the
     encoded bytes.

  2. We can hand-verify the test corpus by reading this Python
     instead of reaching for a hex editor.
"""
import sys

# Same auto-bootstrap pattern as scripts/img_to_bgra.py — see
# /memories/python-host-tools-pillow.md for the rationale.
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

    print("make_test_png: pillow not found, attempting auto-install ...",
          file=sys.stderr)
    installed = (
        _try_install(["--user", "pillow"]) or
        _try_install(["--user", "--break-system-packages", "pillow"]) or
        _try_install(["--break-system-packages", "pillow"])
    )
    if not installed:
        print(
            "make_test_png: could not auto-install pillow.  Install it "
            "manually and re-run, e.g.:\n"
            "    python3 -m pip install --user pillow\n"
            "  or, if that fails (PEP 668 on macOS system Python):\n"
            "    python3 -m pip install --user --break-system-packages pillow",
            file=sys.stderr,
        )
        sys.exit(2)
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        import site
        for d in (site.getusersitepackages() if isinstance(
                site.getusersitepackages(), list) else [site.getusersitepackages()]):
            if d not in sys.path:
                sys.path.insert(0, d)
        from PIL import Image  # type: ignore


def make_icon():
    W = H = 16
    im = Image.new("RGBA", (W, H), (255, 0, 0, 255))   # solid red
    px = im.load()
    # Green diagonal
    for i in range(W):
        px[i, i] = (0, 255, 0, 255)
    # Transparent corners
    px[0, 0]         = (0, 0, 0, 0)
    px[W - 1, 0]     = (0, 0, 0, 0)
    px[0, H - 1]     = (0, 0, 0, 0)
    px[W - 1, H - 1] = (0, 0, 0, 0)
    # Opaque-blue 4x4 landmark in bottom-right
    for y in range(H - 4, H):
        for x in range(W - 4, W):
            px[x, y] = (0, 0, 255, 255)
    return im


def make_palette_icon():
    """16x16 four-quadrant solid-colour palette PNG (colour type 3).

    Quadrants:                Decoded pixel (BGRA bytes):
      top-left:    red          B=0   G=0   R=255 A=255  sum 510
      top-right:   green        B=0   G=255 R=0   A=255  sum 510
      bot-left:    blue         B=255 G=0   R=0   A=255  sum 510
      bot-right:   white        B=255 G=255 R=255 A=255  sum 1020

    Total decoded BGRA sum: 64 * (510 + 510 + 510 + 1020) = 163200.
    All 256 pixels are fully opaque (no tRNS chunk).
    """
    return _make_quad_palette(16)


def make_large_palette_icon():
    """64x64 four-quadrant solid-colour palette PNG (colour type 3).

    Identical pattern to make_palette_icon() scaled 4x.  Used
    by test_browser_intrinsic_size.py — 32x32 = 1024 pixels of
    each pure colour means a pre-fix browser (16x16 placeholder)
    would render at most 256 pixels of one colour (clipped to
    top-left), and a post-fix browser (intrinsic size from cache)
    would render 1024 pixels of each.  Wide enough delta to make
    a robust pixel-count assertion.
    """
    return _make_quad_palette(64)


def _make_quad_palette(side):
    W = H = side
    im = Image.new("RGB", (W, H), (0, 0, 0))
    px = im.load()
    half = W // 2
    for y in range(H):
        for x in range(W):
            tl = (x < half) and (y < half)
            tr = (x >= half) and (y < half)
            bl = (x < half) and (y >= half)
            br = (x >= half) and (y >= half)
            if tl:   px[x, y] = (255, 0, 0)
            elif tr: px[x, y] = (0, 255, 0)
            elif bl: px[x, y] = (0, 0, 255)
            elif br: px[x, y] = (255, 255, 255)
    return im.convert("P", palette=Image.ADAPTIVE, colors=4)


def make_gray_icon():
    """16x16 8-bit grayscale PNG (colour type 0).

    A diagonal gradient: pixel(x, y) = (x + y) * 8, clamped to
    255.  All pixels opaque (no tRNS).

    Decoded as BGRA each pixel contributes 3 * grey + 255 to the
    byte sum.  Total computed at bake time and echoed back via
    the script's stdout so the test can hard-code it.
    """
    W = H = 16
    im = Image.new("L", (W, H), 0)
    px = im.load()
    for y in range(H):
        for x in range(W):
            v = (x + y) * 8
            if v > 255: v = 255
            px[x, y] = v
    return im


def main():
    kind = "rgba"
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    for a in sys.argv[1:]:
        if a.startswith("--kind="):
            kind = a.split("=", 1)[1]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    out = args[0]
    if kind == "rgba":
        im = make_icon()
        # Force RGBA (color type 6), no interlace, default zlib level.
        # Pillow / libpng will pick dynamic huffman for anything non-trivial.
        im.save(out, format="PNG", optimize=False)
        sum_bgra = _sum_bgra_rgba(im)
        op = _count_opaque(im)
        print(f"wrote {out}: {im.size[0]}x{im.size[1]} RGBA "
              f"({op} opaque pixels, decoded BGRA sum {sum_bgra})")
    elif kind == "palette":
        im = make_palette_icon()
        im.save(out, format="PNG", optimize=False)
        sum_bgra = _sum_bgra_palette(im)
        # Palette images without tRNS are fully opaque.
        op = im.size[0] * im.size[1]
        print(f"wrote {out}: {im.size[0]}x{im.size[1]} palette "
              f"({op} opaque pixels, decoded BGRA sum {sum_bgra})")
    elif kind == "large_palette":
        im = make_large_palette_icon()
        im.save(out, format="PNG", optimize=False)
        sum_bgra = _sum_bgra_palette(im)
        op = im.size[0] * im.size[1]
        print(f"wrote {out}: {im.size[0]}x{im.size[1]} large_palette "
              f"({op} opaque pixels, decoded BGRA sum {sum_bgra})")
    elif kind == "gray":
        im = make_gray_icon()
        im.save(out, format="PNG", optimize=False)
        sum_bgra = _sum_bgra_gray(im)
        op = im.size[0] * im.size[1]
        print(f"wrote {out}: {im.size[0]}x{im.size[1]} gray "
              f"({op} opaque pixels, decoded BGRA sum {sum_bgra})")
    else:
        print(f"make_test_png: unknown --kind={kind}", file=sys.stderr)
        return 2
    return 0


def _count_opaque(im):
    c = 0
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            if im.getpixel((x, y))[3] == 0xFF:
                c += 1
    return c


def _sum_bgra_rgba(im):
    """Sum of decoded BGRA bytes when our decoder reads `im`.

    For RGBA source: BGRA pixel == (B, G, R, A) which has the
    same byte-sum as the RGBA tuple (channels swapped, sum
    unchanged).  Just sum all 4 channels per pixel.
    """
    s = 0
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b, a = im.getpixel((x, y))
            s += r + g + b + a
    return s


def _sum_bgra_palette(im):
    """BGRA sum for a palette (mode 'P') image, no tRNS.

    Every pixel decodes opaque (A=255), and the RGB values come
    from the palette indexed by the pixel.  Sum = sum_over_pixels(
    pal_R + pal_G + pal_B + 255).
    """
    pal = im.getpalette()
    s = 0
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            idx = im.getpixel((x, y))
            r = pal[idx * 3 + 0]
            g = pal[idx * 3 + 1]
            b = pal[idx * 3 + 2]
            s += r + g + b + 255
    return s


def _sum_bgra_gray(im):
    """BGRA sum for an 8-bit grayscale (mode 'L') image.

    Decoded BGRA = (grey, grey, grey, 255) per pixel.  Sum =
    3 * grey + 255 per pixel.
    """
    s = 0
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            v = im.getpixel((x, y))
            s += 3 * v + 255
    return s


if __name__ == "__main__":
    sys.exit(main())
