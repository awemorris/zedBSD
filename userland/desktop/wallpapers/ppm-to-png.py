#!/usr/bin/env python3
"""Turns a binary PPM into a PNG with the same pixels (ws138-p001).

    userland/desktop/wallpapers/ppm-to-png.py IN.ppm OUT.png [--filter=best|none|up]

The PNG is 8-bit RGB without interlace, compressed at zlib's level 9.  With --filter=best (the default) every row
takes the filter (None, Sub, Up, Average or Paeth) whose filtered bytes, read as signed, add up to the least
absolute sum, the way libpng chooses by default.  After writing, the PNG is decoded again here and its pixels are
compared with the PPM's byte for byte; the exit status is 1 when they differ.

write_png() and read_png() are imported by generate.py (the bundled wallpapers) and by the WS138 tests.  Only the
Python standard library is used.
Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import struct
import sys
import zlib

# The PNG signature.
SIGNATURE = b'\x89PNG\r\n\x1a\n'


def read_ppm(path):
    """Reads a P6 PPM with a maximum of 255; returns (width, height, rgb bytes)."""
    with open(path, 'rb') as stream:
        data = stream.read()
    fields = []
    at = 2
    if data[:2] != b'P6':
        raise ValueError('%s: not a binary PPM' % path)
    while len(fields) < 3:
        # Whitespace and comments between the header's numbers.
        while at < len(data) and data[at:at + 1] in b' \t\r\n':
            at += 1
        if data[at:at + 1] == b'#':
            while at < len(data) and data[at:at + 1] != b'\n':
                at += 1
            continue
        start = at
        while at < len(data) and data[at:at + 1].isdigit():
            at += 1
        fields.append(int(data[start:at]))
    width, height, maximum = fields
    if maximum != 255:
        raise ValueError('%s: maximum %d is not 255' % (path, maximum))
    at += 1
    rgb = data[at:at + width * height * 3]
    if len(rgb) != width * height * 3:
        raise ValueError('%s: short pixels' % path)
    return width, height, rgb


def paeth(a, b, c):
    """The Paeth predictor of PNG."""
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def filter_row(kind, row, previous):
    """Filters one row of RGB bytes with a PNG filter type."""
    out = bytearray(len(row))
    for i in range(len(row)):
        left = row[i - 3] if i >= 3 else 0
        up = previous[i]
        corner = previous[i - 3] if i >= 3 else 0
        if kind == 0:
            value = row[i]
        elif kind == 1:
            value = row[i] - left
        elif kind == 2:
            value = row[i] - up
        elif kind == 3:
            value = row[i] - ((left + up) >> 1)
        else:
            value = row[i] - paeth(left, up, corner)
        out[i] = value & 255
    return bytes(out)


def unfilter_row(kind, data, previous):
    """Undoes one row's PNG filter."""
    row = bytearray(len(data))
    for i in range(len(data)):
        left = row[i - 3] if i >= 3 else 0
        up = previous[i]
        corner = previous[i - 3] if i >= 3 else 0
        if kind == 0:
            value = data[i]
        elif kind == 1:
            value = data[i] + left
        elif kind == 2:
            value = data[i] + up
        elif kind == 3:
            value = data[i] + ((left + up) >> 1)
        else:
            value = data[i] + paeth(left, up, corner)
        row[i] = value & 255
    return bytes(row)


# Each byte's absolute value read as signed (0..128, so it fits a byte again), for signed_sum.
SIGNED_ABS = bytes(value if value < 128 else 256 - value for value in range(256))


def signed_sum(data):
    """The sum of the bytes' absolute values read as signed, libpng's measure of a filtered row."""
    return sum(data.translate(SIGNED_ABS))


def filter_rows(row, previous):
    """The five filtered forms of a row (None, Sub, Up, Average, Paeth), the same bytes filter_row makes.

    zip over the row and its shifted neighbours keeps the loops in C as far as Python allows; write_png's best choice
    calls this for every row.
    """
    left = bytes(3) + row[:-3]
    corner = bytes(3) + previous[:-3]
    sub = bytes((value - before) & 255 for value, before in zip(row, left))
    up = bytes((value - above) & 255 for value, above in zip(row, previous))
    average = bytes((value - ((before + above) >> 1)) & 255 for value, before, above in zip(row, left, previous))
    # paeth() written out: p - a = b - c, p - b = a - c, p - c = a + b - 2c (a call per byte would double the time).
    paeth_row = bytes((value - (before if (near_a := abs(above - diagonal)) <= (near_b := abs(before - diagonal)) and
                                near_a <= abs(before + above - diagonal - diagonal)
                                else (above if near_b <= abs(before + above - diagonal - diagonal) else diagonal))) & 255
                      for value, before, above, diagonal in zip(row, left, previous, corner))
    return (bytes(row), sub, up, average, paeth_row)


def chunk(kind, data):
    """One PNG chunk with its length and CRC."""
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


def write_png(path, width, height, rgb, filter='best'):
    """Writes 8-bit RGB pixels as a PNG; filter is 'best' (chosen per row), 'none' or 'up'."""
    stride = width * 3
    previous = bytes(stride)
    raw = bytearray()
    for y in range(height):
        row = rgb[y * stride:(y + 1) * stride]
        if filter == 'none':
            kind, data = 0, row
        elif filter == 'up':
            kind, data = 2, filter_row(2, row, previous)
        else:
            candidates = [(signed_sum(filtered), kind, filtered)
                          for kind, filtered in enumerate(filter_rows(row, previous))]
            candidates.sort(key=lambda entry: (entry[0], entry[1]))
            _, kind, data = candidates[0]
        raw.append(kind)
        raw += data
        previous = row
    png = SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as stream:
        stream.write(png)


def read_png(path):
    """Reads an 8-bit RGB, non-interlaced PNG (what write_png writes); returns (width, height, rgb bytes)."""
    with open(path, 'rb') as stream:
        data = stream.read()
    if data[:8] != SIGNATURE:
        raise ValueError('%s: not a PNG' % path)
    at = 8
    idat = bytearray()
    width = height = 0
    while at < len(data):
        length = struct.unpack('>I', data[at:at + 4])[0]
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or colour != 2 or interlace != 0:
                raise ValueError('%s: not 8-bit RGB without interlace' % path)
        elif kind == b'IDAT':
            idat += body
        at += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = width * 3
    previous = bytes(stride)
    rgb = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        row = unfilter_row(raw[start], raw[start + 1:start + 1 + stride], previous)
        rgb += row
        previous = row
    return width, height, bytes(rgb)


def main():
    """Converts one PPM and checks the result."""
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith('--')]
    options = [argument for argument in sys.argv[1:] if argument.startswith('--')]
    if len(arguments) != 2:
        sys.stderr.write('usage: ppm-to-png.py IN.ppm OUT.png [--filter=best|none|up]\n')
        return 2
    chosen = 'best'
    for option in options:
        if option.startswith('--filter='):
            chosen = option.split('=', 1)[1]
    width, height, rgb = read_ppm(arguments[0])
    write_png(arguments[1], width, height, rgb, chosen)
    back_width, back_height, back = read_png(arguments[1])
    if (back_width, back_height, back) != (width, height, rgb):
        sys.stderr.write('ppm-to-png: %s does not hold the same pixels as %s\n' % (arguments[1], arguments[0]))
        return 1
    print('%s: %dx%d, same pixels' % (arguments[1], width, height))
    return 0


if __name__ == '__main__':
    sys.exit(main())
