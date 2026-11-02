#!/usr/bin/env python3
"""Summarizes the guest's frames in the pcap QEMU's filter-dump wrote.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    pcap-sizes.py PCAP MAC

A frame is in the dump only when the USB network function sent it on to
QEMU's network, so a frame the guest gave the adapter but the function held
back is not there.  Prints the number of frames from MAC, how many of them
are a whole number of 64-byte packets, and those lengths.
"""
from __future__ import annotations

import struct
import sys


def main() -> int:
	path, mac = sys.argv[1], bytes.fromhex(sys.argv[2].replace(":", ""))
	with open(path, "rb") as stream:
		data = stream.read()
	if len(data) < 24:
		print("pcap: empty")
		return 1
	magic = struct.unpack("<I", data[:4])[0]
	order = "<" if magic in (0xa1b2c3d4, 0xa1b23c4d) else ">"
	offset = 24
	total = 0
	whole = []
	while offset + 16 <= len(data):
		_, _, captured, length = struct.unpack(order + "IIII", data[offset:offset + 16])
		frame = data[offset + 16:offset + 16 + captured]
		offset += 16 + captured
		if frame[6:12] != mac:
			continue
		total += 1
		if length % 64 == 0:
			whole.append(length)
	print(f"pcap: {total} frames from the guest, {len(whole)} of a whole number of 64-byte packets")
	sizes = {}
	for length in whole:
		sizes[length] = sizes.get(length, 0) + 1
	print("pcap: whole-packet lengths " + (" ".join(f"{k}x{v}" for k, v in sorted(sizes.items())) or "none"))
	return 0


if __name__ == "__main__":
	sys.exit(main())
