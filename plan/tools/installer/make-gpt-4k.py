#!/usr/bin/env python3
"""Writes a protective MBR and both GPT copies with 4096-byte sectors into an image file (a disk of 4096-byte sectors for the installer's walks).

    make-gpt-4k.py [--sector 512] IMAGE START,COUNT,TYPE,PARTUUID,NAME ...

START and COUNT are sectors (4096 bytes unless --sector says 512, p010's disk of another system).  The image must
already have its size.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import os
import struct
import sys
import uuid
import zlib

SECTOR = 4096


def guid_bytes(text):
	return uuid.UUID(text).bytes_le


def main():
	global SECTOR
	if sys.argv[1] == "--sector":
		SECTOR = int(sys.argv[2])
		del sys.argv[1:3]
	path = sys.argv[1]
	size = os.path.getsize(path)
	sectors = size // SECTOR
	entries = bytearray(128 * 128)
	for index, spec in enumerate(sys.argv[2:]):
		start, count, kind, partuuid, name = spec.split(",", 4)
		start, count = int(start), int(count)
		entry = struct.pack("<16s16sQQQ72s", guid_bytes(kind), guid_bytes(partuuid), start, start + count - 1, 0,
			name.encode("utf-16-le").ljust(72, b"\0"))
		entries[index * 128:(index + 1) * 128] = entry
	table_blocks = len(entries) // SECTOR
	first, last = 2 + table_blocks, sectors - 2 - table_blocks
	disk = uuid.uuid4().bytes_le
	entries_crc = zlib.crc32(entries)

	def header(lba, alternate, table):
		raw = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, lba, alternate, first, last,
			disk, table, 128, 128, entries_crc))
		struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]))
		return bytes(raw).ljust(SECTOR, b"\0")

	mbr = bytearray(SECTOR)
	struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(sectors - 1, 0xFFFFFFFF))
	mbr[510:512] = b"\x55\xaa"
	with open(path, "r+b") as image:
		image.seek(0)
		image.write(mbr)
		image.seek(SECTOR)
		image.write(header(1, sectors - 1, 2))
		image.seek(2 * SECTOR)
		image.write(entries)
		image.seek((sectors - 1 - table_blocks) * SECTOR)
		image.write(entries)
		image.seek((sectors - 1) * SECTOR)
		image.write(header(sectors - 1, 1, sectors - 1 - table_blocks))


if __name__ == "__main__":
	main()
