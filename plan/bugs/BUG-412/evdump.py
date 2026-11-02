"""Prints a raw evdev capture (24-byte aarch64 input_event records) one event a line."""
# Usage: python3 -I evdump.py CAPTURE (a dd of /dev/input/eventN with bs=24 on the Pi 5).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import struct
import sys

NAMES = {(0, 0): "SYN", (1, 0x110): "BTN_LEFT", (1, 0x111): "BTN_RIGHT", (2, 0): "REL_X", (2, 1): "REL_Y", (2, 8): "WHEEL", (2, 6): "HWHEEL"}
data = open(sys.argv[1], "rb").read()
first = None
for offset in range(0, len(data) - 23, 24):
	sec, usec, kind, code, value = struct.unpack_from("<qqHHi", data, offset)
	t = sec * 1000.0 + usec / 1000.0
	if first is None:
		first = t
	print("%10.1f %-9s %d" % (t - first, NAMES.get((kind, code), "%d/%d" % (kind, code)), value))
