#!/usr/bin/env python3
"""Draws the desktop's bundled wallpapers (ws089-p009).

    userland/desktop/wallpapers/generate.py OUTDIR [--width=1920] [--height=1080] [--preview=PNG]

Each picture is a quiet gradient with a few soft, blurred shapes in the desktop's look (pale sky blues, young greens,
warm light), written as a PNG named after it, e.g. OUTDIR/Aurora.png (ws138-p002: 8-bit RGB, each row filtered by
the best of the five filters, zlib level 9, through ppm-to-png.py's write_png); the Wallpaper page of Settings shows
the file's name without .png.  The pictures are not kept in git: the image builds make them.

Only the Python standard library is used.  A picture is drawn at a quarter of its size, where the shapes are
smooth anyway, then enlarged bilinearly, with a faint fixed dither against banding, so the same command always
writes the same bytes.  --preview also writes a PNG sheet of all the pictures side by side (for looking at them).
Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import concurrent.futures
import importlib.util
import math
import os
import random
import struct
import sys
import zlib

# How much smaller the pictures are drawn before they are enlarged.
SCALE = 4


def mix(a, b, t):
    """Mixes two colours (0..1 channels) by t."""
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def smooth(t):
    """Eases 0..1 in and out."""
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def hexcolour(value):
    """Turns 0xRRGGBB into a 0..1 colour."""
    return ((value >> 16 & 255) / 255.0, (value >> 8 & 255) / 255.0, (value & 255) / 255.0)


# The pictures: a vertical gradient (top, middle, bottom), a light glow, and blurred shapes, each a colour, a place
# (fractions of the width and height), a radius (a fraction of the height), a strength, and a stretch across.
PICTURES = [
    {
        'name': 'Aurora',
        'sky': (0x0c1a3a, 0x163e6a, 0x1f5f78),
        'glow': (0.70, 0.95, 0.50, 0x3a8fb0, 0.45),
        'shapes': [
            (0x3fe0b0, 0.18, 0.42, 0.13, 0.80, 4.5),
            (0x55e6c8, 0.48, 0.32, 0.10, 0.70, 5.0),
            (0x7fb8ff, 0.78, 0.24, 0.12, 0.60, 3.5),
            (0xa98cf5, 0.90, 0.46, 0.14, 0.45, 2.5),
            (0x2fc6d8, 0.40, 0.70, 0.22, 0.30, 4.0),
        ],
    },
    {
        'name': 'Dawn',
        'sky': (0xf7d9c4, 0xf3c6cf, 0xc9c4ec),
        'glow': (0.30, 0.62, 0.45, 0xfff1d6, 0.75),
        'shapes': [
            (0xffb48a, 0.22, 0.66, 0.26, 0.45, 1.6),
            (0xf49ac1, 0.62, 0.40, 0.30, 0.35, 1.4),
            (0xa9b8f5, 0.88, 0.22, 0.28, 0.40, 1.2),
            (0xfff5e6, 0.45, 0.85, 0.35, 0.40, 2.4),
        ],
    },
    {
        'name': 'Lagoon',
        'sky': (0xdff4f7, 0xa8e0e6, 0x5fb8c9),
        'glow': (0.75, 0.20, 0.40, 0xffffff, 0.55),
        'shapes': [
            (0x3fa7c4, 0.18, 0.78, 0.30, 0.45, 1.3),
            (0x7fd8c8, 0.50, 0.55, 0.22, 0.40, 1.0),
            (0x2f7cf6, 0.86, 0.72, 0.26, 0.35, 1.1),
            (0xc6f0ea, 0.32, 0.28, 0.18, 0.35, 1.0),
            (0x9fe3f0, 0.68, 0.32, 0.12, 0.40, 1.0),
        ],
    },
    {
        'name': 'Meadow',
        'sky': (0xeef6e8, 0xd6ecc6, 0xa9d49a),
        'glow': (0.78, 0.18, 0.50, 0xfffbe0, 0.70),
        'shapes': [
            (0x8cc97a, 0.12, 0.88, 0.34, 0.55, 2.2),
            (0xb9e08e, 0.52, 0.92, 0.30, 0.50, 2.8),
            (0x6fb88a, 0.90, 0.84, 0.28, 0.50, 2.0),
            (0xf6e7a8, 0.30, 0.40, 0.20, 0.30, 1.6),
            (0xdff2c8, 0.66, 0.58, 0.22, 0.35, 2.4),
        ],
    },
    {
        'name': 'Twilight',
        'sky': (0x2a2350, 0x5a3f7a, 0xd8866e),
        'glow': (0.50, 0.95, 0.55, 0xffc48a, 0.65),
        'shapes': [
            (0x7a5cc9, 0.20, 0.30, 0.30, 0.40, 2.0),
            (0xe07a9a, 0.72, 0.62, 0.26, 0.40, 2.6),
            (0xffb070, 0.35, 0.86, 0.30, 0.45, 3.0),
            (0x4d7ad8, 0.88, 0.22, 0.22, 0.35, 1.6),
        ],
    },
]


def draw_small(picture, width, height):
    """Draws a picture's colours at a small size; returns rows of (r, g, b) in 0..1."""
    top, middle, bottom = (hexcolour(c) for c in picture['sky'])
    gx, gy, gradius, gcolour, gstrength = picture['glow']
    gcolour = hexcolour(gcolour)
    shapes = [(hexcolour(c), x, y, r, s, stretch) for (c, x, y, r, s, stretch) in picture['shapes']]
    aspect = width / float(height)
    rows = []
    for y in range(height):
        v = (y + 0.5) / height
        # The sky: top to middle, then middle to bottom, eased.
        if v < 0.5:
            base = mix(top, middle, smooth(v * 2.0))
        else:
            base = mix(middle, bottom, smooth((v - 0.5) * 2.0))
        row = []
        for x in range(width):
            u = (x + 0.5) / width
            colour = base
            # The glow: a wide soft light.
            dx = (u - gx) * aspect
            dy = v - gy
            glow = math.exp(-(dx * dx + dy * dy) / (gradius * gradius))
            colour = mix(colour, gcolour, glow * gstrength)
            # The shapes: blurred ellipses, stretched across.
            for (scolour, sx, sy, sradius, sstrength, stretch) in shapes:
                dx = (u - sx) * aspect / stretch
                dy = v - sy
                weight = math.exp(-(dx * dx + dy * dy) / (sradius * sradius))
                colour = mix(colour, scolour, weight * sstrength)
            row.append(colour)
        rows.append(row)
    return rows


def enlarge(rows, width, height):
    """Enlarges small rows bilinearly to width x height; returns the picture as bytes (RGB) with a faint dither."""
    small_height = len(rows)
    small_width = len(rows[0])
    noise = random.Random(8900)
    out = bytearray(width * height * 3)
    at = 0
    for y in range(height):
        fy = (y + 0.5) * small_height / height - 0.5
        y0 = min(max(int(math.floor(fy)), 0), small_height - 1)
        y1 = min(y0 + 1, small_height - 1)
        ty = min(max(fy - y0, 0.0), 1.0)
        upper = rows[y0]
        lower = rows[y1]
        for x in range(width):
            fx = (x + 0.5) * small_width / width - 0.5
            x0 = min(max(int(math.floor(fx)), 0), small_width - 1)
            x1 = min(x0 + 1, small_width - 1)
            tx = min(max(fx - x0, 0.0), 1.0)
            a = upper[x0]
            b = upper[x1]
            c = lower[x0]
            d = lower[x1]
            dither = noise.random() - 0.5
            for channel in range(3):
                top = a[channel] + (b[channel] - a[channel]) * tx
                bottom = c[channel] + (d[channel] - c[channel]) * tx
                value = (top + (bottom - top) * ty) * 255.0 + dither
                out[at] = 0 if value < 0.0 else (255 if value > 255.0 else int(value + 0.5))
                at += 1
    return bytes(out)


def load_converter():
    """Loads ppm-to-png.py (its name has dashes, so it is loaded by path), which writes the PNG."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ppm-to-png.py')
    # The build writes only into its build directory, not a __pycache__ beside the source.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('ppm_to_png', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_picture(converter, path, width, height, pixels):
    """Writes a picture as a PNG with the best filter of each row, through a new file renamed over the old."""
    temporary = path + '.new'
    converter.write_png(temporary, width, height, bytes(pixels), 'best')
    os.replace(temporary, path)


def make_picture(job):
    """Draws one picture and writes its PNG (a job of its own, so the five run in parallel); returns its pixels."""
    picture, out, width, height = job
    rows = draw_small(picture, width // SCALE, height // SCALE)
    pixels = enlarge(rows, width, height)
    write_picture(load_converter(), os.path.join(out, picture['name'] + '.png'), width, height, pixels)
    return pixels


def make_pictures(out, width, height):
    """Makes every picture: in parallel processes (zlib's level 9 takes most of the time), else one by one."""
    jobs = [(picture, out, width, height) for picture in PICTURES]
    try:
        with concurrent.futures.ProcessPoolExecutor(max_workers=len(jobs)) as pool:
            return list(pool.map(make_picture, jobs))
    except (OSError, NotImplementedError, concurrent.futures.process.BrokenProcessPool):
        return [make_picture(job) for job in jobs]


def write_preview(path, pictures, width, height):
    """Writes a PNG sheet of the pictures (each a quarter of its size, side by side, two rows)."""
    thumb_width = width // 4
    thumb_height = height // 4
    columns = 3
    count = len(pictures)
    sheet_rows = (count + columns - 1) // columns
    sheet_width = columns * thumb_width + (columns + 1) * 8
    sheet_height = sheet_rows * thumb_height + (sheet_rows + 1) * 8
    sheet = bytearray([255, 255, 255] * sheet_width * sheet_height)
    for index, (name, pixels) in enumerate(pictures):
        left = 8 + (index % columns) * (thumb_width + 8)
        top = 8 + (index // columns) * (thumb_height + 8)
        for y in range(thumb_height):
            source = (y * 4) * width * 3
            target = ((top + y) * sheet_width + left) * 3
            for x in range(thumb_width):
                sheet[target + x * 3:target + x * 3 + 3] = pixels[source + x * 12:source + x * 12 + 3]
    raw = b''.join(b'\x00' + bytes(sheet[y * sheet_width * 3:(y + 1) * sheet_width * 3]) for y in range(sheet_height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', sheet_width, sheet_height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    with open(path, 'wb') as file:
        file.write(png)


def main():
    arguments = sys.argv[1:]
    if not arguments or arguments[0].startswith('--'):
        sys.stderr.write('usage: generate.py OUTDIR [--width=1920] [--height=1080] [--preview=PNG]\n')
        return 2
    out = arguments[0]
    width = 1920
    height = 1080
    preview = None
    for argument in arguments[1:]:
        if argument.startswith('--width='):
            width = int(argument[8:])
        elif argument.startswith('--height='):
            height = int(argument[9:])
        elif argument.startswith('--preview='):
            preview = argument[10:]
        else:
            sys.stderr.write('generate.py: unknown option %s\n' % argument)
            return 2
    os.makedirs(out, exist_ok=True)
    made = []
    for picture, pixels in zip(PICTURES, make_pictures(out, width, height)):
        made.append((picture['name'], pixels))
        print('wallpaper: %s/%s.png %dx%d' % (out, picture['name'], width, height))
    if preview is not None:
        write_preview(preview, made, width, height)
        print('wallpaper preview: %s' % preview)
    return 0


if __name__ == '__main__':
    sys.exit(main())
