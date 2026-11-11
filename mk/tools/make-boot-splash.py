#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""Makes the boot splash the loaders draw full-screen.

The splash is userland/desktop/artwork/zedbsd-boot-splash.png (the
project owner's picture with the system's name, artwork/README.md):
the glass mark, the word zedBSD, "operating system" and a dotted
spinner over a bright blurred lake.  This script turns it into what
the UEFI and the BIOS loaders read (logo=):

- the spinner is taken out of the picture (its place is filled from
  the sky and water around it), because the kernel draws a spinner
  that moves there (src/drivers/platform/pcat/graphics/splash.c);

- the picture is scaled to WIDTHxHEIGHT (1920x1080 by default, the GOP
  mode the UEFI loader asks for, 16:9 like the picture) with bilinear
  sampling;

- the result is a binary PPM whose header carries the comment
  "fit=contain": a loader then draws it whole in the middle of a black
  screen, at its own size when the screen holds it, else shrunk with
  its proportions kept; the sides it does not reach are black bars.
  The kernel's splash.c knows the 1920x1080.

The spinner's place is a contract with the kernel's splash.c: in the
picture's coordinates its centre is at the middle of the width and at
SPINNER_Y of the height, its ring's radius SPINNER_RING of the height.

Only the Python standard library is used (zlib for the PNG).  The
output is the same for the same input.

    make-boot-splash.py INPUT.png OUTPUT.ppm [WIDTHxHEIGHT]

"""
import struct
import sys
import zlib

# The spinner in the picture, as fractions of its height (x is the middle); see splash.c.
SPINNER_Y = 0.7720
SPINNER_RING = 0.0308
SPINNER_DOT = 0.0064

# How far around the spinner the picture is filled again, as a fraction of the height.
SPINNER_CLEAR = SPINNER_RING + SPINNER_DOT + 0.012


def read_png(path):
	"""Reads an 8-bit RGB or RGBA, non-interlaced PNG; returns (width, height, rows of RGB bytearrays)."""
	data = open(path, 'rb').read()
	if data[:8] != b'\x89PNG\r\n\x1a\n':
		raise SystemExit('make-boot-splash: not a PNG: ' + path)
	at = 8
	width = height = depth = kind = interlace = None
	chunks = []
	while at < len(data):
		length, name = struct.unpack('>I4s', data[at:at + 8])
		body = data[at + 8:at + 8 + length]
		at += 12 + length
		if name == b'IHDR':
			width, height, depth, kind, _, _, interlace = struct.unpack('>IIBBBBB', body)
		elif name == b'IDAT':
			chunks.append(body)
		elif name == b'IEND':
			break
	if depth != 8 or kind not in (2, 6) or interlace != 0:
		raise SystemExit('make-boot-splash: only 8-bit RGB or RGBA non-interlaced PNGs are read')
	channels = 3 if kind == 2 else 4
	raw = zlib.decompress(b''.join(chunks))
	stride = width * channels
	rows = []
	previous = bytearray(stride)
	at = 0
	for _ in range(height):
		kind_of_filter = raw[at]
		line = bytearray(raw[at + 1:at + 1 + stride])
		at += 1 + stride
		unfilter(kind_of_filter, line, previous, channels)
		previous = line
		if channels == 4:
			rgb = bytearray(width * 3)
			rgb[0::3] = line[0::4]
			rgb[1::3] = line[1::4]
			rgb[2::3] = line[2::4]
			rows.append(rgb)
		else:
			rows.append(line)
	return width, height, rows


def unfilter(kind, line, previous, bpp):
	"""Undoes one PNG row filter in place."""
	count = len(line)
	if kind == 0:
		return
	if kind == 1:
		for i in range(bpp, count):
			line[i] = (line[i] + line[i - bpp]) & 255
	elif kind == 2:
		for i in range(count):
			line[i] = (line[i] + previous[i]) & 255
	elif kind == 3:
		for i in range(count):
			left = line[i - bpp] if i >= bpp else 0
			line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
	elif kind == 4:
		for i in range(count):
			a = line[i - bpp] if i >= bpp else 0
			b = previous[i]
			c = previous[i - bpp] if i >= bpp else 0
			p = a + b - c
			pa = abs(p - a)
			pb = abs(p - b)
			pc = abs(p - c)
			if pa <= pb and pa <= pc:
				predictor = a
			elif pb <= pc:
				predictor = b
			else:
				predictor = c
			line[i] = (line[i] + predictor) & 255
	else:
		raise SystemExit('make-boot-splash: bad PNG filter %d' % kind)


def clear_spinner(width, height, rows, is_vertical=False):
	"""Fills the spinner's square from the four sides around it (the sky and water there are smooth)."""
	cx = width / 2.0
	spinner_y = 0.6465 if is_vertical else 0.7720
	spinner_ring = 0.0189 if is_vertical else 0.0308
	spinner_dot = 0.0039 if is_vertical else 0.0064
	spinner_clear = spinner_ring + spinner_dot + 0.012

	cy = spinner_y * height
	half = spinner_clear * height
	left = int(cx - half)
	right = int(cx + half) + 1
	top = int(cy - half)
	bottom = int(cy + half) + 1
	for y in range(top + 1, bottom):
		v = (y - top) / float(bottom - top)
		for x in range(left + 1, right):
			u = (x - left) / float(right - left)
			for c in range(3):
				across = rows[y][left * 3 + c] * (1 - u) + rows[y][right * 3 + c] * u
				down = rows[top][x * 3 + c] * (1 - v) + rows[bottom][x * 3 + c] * v
				# Nearer sides count more: each direction by how close the pixel is to its two sides.
				wa = 1.0 / (min(u, 1 - u) + 0.05)
				wd = 1.0 / (min(v, 1 - v) + 0.05)
				value = (across * wa + down * wd) / (wa + wd)
				rows[y][x * 3 + c] = int(value + 0.5)


def scale(width, height, rows, out_width, out_height):
	"""Scales the picture bilinearly (a row pass, then a column pass)."""
	# Along the rows: each output column samples two input columns.
	xs = []
	for x in range(out_width):
		source = (x + 0.5) * width / out_width - 0.5
		if source < 0:
			source = 0.0
		base = int(source)
		if base >= width - 1:
			base = width - 2
		xs.append((base * 3, source - base))
	narrow = []
	for row in rows:
		line = bytearray(out_width * 3)
		for x, (base, frac) in enumerate(xs):
			for c in range(3):
				a = row[base + c]
				b = row[base + 3 + c]
				line[x * 3 + c] = int(a + (b - a) * frac + 0.5)
		narrow.append(line)
	# Down the columns: each output row mixes two narrowed rows.
	out = []
	for y in range(out_height):
		source = (y + 0.5) * height / out_height - 0.5
		if source < 0:
			source = 0.0
		base = int(source)
		if base >= height - 1:
			base = height - 2
		frac = source - base
		upper = narrow[base]
		lower = narrow[base + 1]
		out.append(bytes(int(a + (b - a) * frac + 0.5) for a, b in zip(upper, lower)))
	return out


def main():
	args = [arg for arg in sys.argv[1:] if not arg.startswith('--')]
	flags = [arg for arg in sys.argv[1:] if arg.startswith('--')]
	no_clear = ('--no-clear' in flags)
	if len(args) not in (2, 3):
		raise SystemExit('usage: make-boot-splash.py INPUT.png OUTPUT.ppm [WIDTHxHEIGHT] [--no-clear]')
	input_png, output_ppm = args[0], args[1]
	width, height, rows = read_png(input_png)
	is_vertical = (height > width)
	if len(args) == 3:
		out_width, out_height = (int(value) for value in args[2].split('x'))
	else:
		out_width, out_height = (1080, 1920) if is_vertical else (1920, 1080)
	if not no_clear:
		clear_spinner(width, height, rows, is_vertical=is_vertical)
	out = scale(width, height, rows, out_width, out_height)
	with open(output_ppm, 'wb') as output:
		output.write(b'P6\n# fit=contain\n%d %d\n255\n' % (out_width, out_height))
		for line in out:
			output.write(line)


if __name__ == '__main__':
	main()
