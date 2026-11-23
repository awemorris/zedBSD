#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Makes mikuterm's icon from its original picture: the PNGs and the .ico the build embeds.

    python3 -I apps/terminal-win32/resources/make-icon.py ORIGINAL.jpg [COMPARISON.png]

ORIGINAL.jpg is the drawing (a dark rounded square with a white ">" and a glowing green leek).  The large
sizes (64, 256 and 512 pixels) are the drawing itself, cut to its rounded square: the corners outside it are
made transparent with a mask of the square's shape, measured from the picture (its rim lies 2 pixels inside
the picture's edge and its corners have a radius of 170 pixels of the 989).  The small sizes (16, 24, 32 and
48) are drawn again in the same shapes and colours, simpler, so that they still read at that size: a weaker
glow, a thicker ">" and leek, and two leaves instead of three.

The files are written beside this script (icon/mikuterm-SIZE.png and mikuterm.ico: 16, 24, 32, 48, 64 and 256
pixels, each a PNG of its own in the .ico) and are kept in the tree, so the build needs no picture library.
COMPARISON.png, when named, shows each small size as the drawing scaled down and as drawn again, side by side
and enlarged.  It needs Pillow.
"""

import io
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))

# The rounded square in the original picture: its box (left, top, right, bottom) and its corners' radius.
SQUARE_BOX = (1, 1, 988, 1002)
SQUARE_RADIUS = 170

# The sizes of the icon: drawn again (small) and cut from the drawing (large).
SMALL = (16, 24, 32, 48)
LARGE = (64, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 256)

# How many times larger the shapes are drawn before they are scaled down (the smoothing of their edges).
SUPERSAMPLE = 8


def rounded_mask(size, radius, inset=0.0):
    """A mask of a rounded square of size pixels, smoothed at its edge."""
    big = size * SUPERSAMPLE
    mask = Image.new("L", (big, big), 0)
    draw = ImageDraw.Draw(mask)
    edge = inset * SUPERSAMPLE
    draw.rounded_rectangle((edge, edge, big - 1 - edge, big - 1 - edge), radius=radius * SUPERSAMPLE, fill=255)
    return mask.resize((size, size), Image.LANCZOS)


def cut(original, size):
    """The drawing at size pixels, the corners outside its rounded square transparent."""
    square = original.crop(SQUARE_BOX).convert("RGB")
    radius = SQUARE_RADIUS * size / (SQUARE_BOX[2] - SQUARE_BOX[0])
    picture = square.resize((size, size), Image.LANCZOS).convert("RGBA")
    picture.putalpha(rounded_mask(size, radius))
    return picture


def line(draw, points, width, colour):
    """A stroke through points with round ends and joins."""
    draw.line(points, fill=colour, width=int(round(width)), joint="curve")
    radius = width / 2
    for x, y in (points[0], points[-1]):
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=colour)


def redraw(size):
    """The icon drawn again at a small size: the same shapes, simpler, bolder, with a weak glow."""
    big = size * SUPERSAMPLE

    def at(x, y):
        return (x * big, y * big)

    # The thickness of the strokes: bolder the smaller the icon.
    chevron_width = {16: 0.13, 24: 0.115, 32: 0.10, 48: 0.085}[size] * big
    stem_width = {16: 0.13, 24: 0.12, 32: 0.105, 48: 0.09}[size] * big
    leaf_width = stem_width * 0.72

    # The ground: the drawing's dark grey, a faint rim.
    ground = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(ground)
    radius = 0.17 * big
    draw.rounded_rectangle((0, 0, big - 1, big - 1), radius=radius, fill=(78, 82, 100, 255))
    rim = max(1.0, 0.03 * size) * SUPERSAMPLE
    draw.rounded_rectangle((rim, rim, big - 1 - rim, big - 1 - rim), radius=radius - rim, fill=(26, 28, 36, 255))

    # The leek's shapes: the stem from its white root to the green, and two leaves.
    stem = [at(0.40, 0.665), at(0.56, 0.655), at(0.70, 0.635)]
    leaf_up = [at(0.68, 0.635), at(0.85, 0.53)]
    leaf_down = [at(0.69, 0.645), at(0.87, 0.69)]

    # The glow under the leek, weak.
    glow = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    for points, width in ((stem, stem_width), (leaf_up, leaf_width), (leaf_down, leaf_width)):
        line(glow_draw, points, width * 1.6, (90, 220, 90, 70))
    glow = glow.filter(ImageFilter.GaussianBlur(0.035 * big))
    ground = Image.alpha_composite(ground, glow)

    # The ">" in the drawing's white.
    draw = ImageDraw.Draw(ground)
    line(draw, [at(0.17, 0.335), at(0.37, 0.50), at(0.17, 0.665)], chevron_width, (226, 226, 230, 255))

    # The leek: leaves first, the stem over them; the stem pale at its root, green towards the leaves.
    line(draw, leaf_up, leaf_width, (88, 200, 80, 255))
    line(draw, leaf_down, leaf_width, (80, 190, 74, 255))
    line(draw, [stem[1], stem[2]], stem_width, (130, 222, 110, 255))
    line(draw, [stem[0], stem[1]], stem_width, (214, 246, 214, 255))

    # Scaled down, the outside of the rounded square transparent.
    picture = ground.resize((size, size), Image.LANCZOS)
    alpha = Image.composite(picture.getchannel("A"), Image.new("L", (size, size), 0), rounded_mask(size, 0.17 * size))
    picture.putalpha(alpha)
    return picture


def png_bytes(picture):
    """A picture as PNG bytes, the same bytes for the same picture."""
    out = io.BytesIO()
    picture.save(out, format="PNG", optimize=True)
    return out.getvalue()


def write_ico(path, pictures):
    """An .ico of PNG images, one for each size (Windows Vista and later read PNG in an .ico)."""
    images = [png_bytes(pictures[size]) for size in ICO_SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    for size, image in zip(ICO_SIZES, images):
        side = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    with open(path + ".new", "wb") as out:
        out.write(header + entries + b"".join(images))
    os.replace(path + ".new", path)


def comparison(original, pictures, path):
    """The small sizes, scaled down and drawn again, side by side, each enlarged 8 times (and at its size)."""
    scale = 8
    gap = 16
    width = gap + 2 * (48 * scale + gap) + 2 * (48 + gap)
    height = gap + sum(size * scale + gap for size in SMALL)
    sheet = Image.new("RGBA", (width, height), (240, 240, 240, 255))
    y = gap
    for size in SMALL:
        scaled = cut(original, size)
        drawn = pictures[size]
        x = gap
        for picture in (scaled, drawn):
            big = picture.resize((size * scale, size * scale), Image.NEAREST)
            sheet.alpha_composite(big, (x, y))
            x += 48 * scale + gap
        for picture in (scaled, drawn):
            sheet.alpha_composite(picture, (x, y))
            x += 48 + gap
        y += size * scale + gap
    sheet.save(path)


def main():
    if len(sys.argv) not in (2, 3):
        sys.stderr.write(__doc__)
        return 2
    original = Image.open(sys.argv[1])
    pictures = {}
    for size in SMALL:
        pictures[size] = redraw(size)
    for size in LARGE:
        pictures[size] = cut(original, size)

    # The PNGs and the .ico beside this script.
    os.makedirs(os.path.join(HERE, "icon"), exist_ok=True)
    for size, picture in sorted(pictures.items()):
        with open(os.path.join(HERE, "icon", "mikuterm-%d.png" % size), "wb") as out:
            out.write(png_bytes(picture))
    write_ico(os.path.join(HERE, "mikuterm.ico"), pictures)
    if len(sys.argv) == 3:
        comparison(original, pictures, sys.argv[2])
    print("make-icon: %d sizes, mikuterm.ico of %s" % (len(pictures), ", ".join(str(size) for size in ICO_SIZES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
