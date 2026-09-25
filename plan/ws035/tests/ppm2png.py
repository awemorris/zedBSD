#!/usr/bin/env python3
"""Converts a binary PPM to PNG, with no library beyond zlib.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import re
import struct
import sys
import zlib


def read_ppm(path):
	"""Returns the width, the height and the RGB bytes of a P6 file."""
	data = open(path, "rb").read()
	match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
	if match is None:
		raise SystemExit(f"{path}: not a P6 PPM")
	return int(match.group(1)), int(match.group(2)), data[match.end():]


def write_png(path, width, height, pixels):
	"""Writes 8-bit RGB rows as one PNG."""
	def chunk(kind, body):
		return (struct.pack(">I", len(body)) + kind + body +
			struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))
	stride = width * 3
	raw = b"".join(b"\0" + pixels[row * stride:(row + 1) * stride]
		       for row in range(height))
	with open(path, "wb") as out:
		out.write(b"\x89PNG\r\n\x1a\n")
		out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
		out.write(chunk(b"IDAT", zlib.compress(raw, 6)))
		out.write(chunk(b"IEND", b""))


if __name__ == "__main__":
	w, h, p = read_ppm(sys.argv[1])
	write_png(sys.argv[2], w, h, p)
