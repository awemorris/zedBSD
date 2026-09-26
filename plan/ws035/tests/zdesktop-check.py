#!/usr/bin/env python3
"""Photographs the compositor's output and checks pixels against colors.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The picture is taken as zdesktop-shot.py takes it (QEMU's VNC server on the
Venus console) and kept as PNG.  Each --expect X,Y,RRGGBB names a pixel and
the color it must have (each channel within 2).  Prints one line per pixel
and exits 1 when any differs.

    zdesktop-check.py OUT.png --runtime DIR --expect 10,10,203040 ...
"""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "plan/ws014/tests"))
sys.path.insert(0, str(HERE))

from venus_rfb import capture  # noqa: E402
from ppm2png import read_ppm, write_png  # noqa: E402


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("output")
	parser.add_argument("--runtime", default=os.environ.get(
		"GUEST_RUNTIME", str(ROOT / "build/ws035-sq-run")))
	parser.add_argument("--expect", action="append", default=[])
	arguments = parser.parse_args()
	ppm = arguments.output + ".ppm"
	report = capture(str(Path(arguments.runtime).resolve() / "vnc.sock"), ppm, 10)
	if report is None:
		print("check: no picture")
		return 1
	width, height, pixels = read_ppm(ppm)
	write_png(arguments.output, width, height, pixels)
	os.unlink(ppm)
	failed = 0
	for expect in arguments.expect:
		x_text, y_text, color = expect.split(",")
		x = int(x_text)
		y = int(y_text)
		want = (int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16))
		at = (y * width + x) * 3
		got = tuple(pixels[at:at + 3])
		same = all(abs(got[i] - want[i]) <= 2 for i in range(3))
		print("check: (%d,%d) want %s got %02x%02x%02x %s" % (
			x, y, color, got[0], got[1], got[2], "ok" if same else "DIFFERS"))
		if not same:
			failed = 1
	print("check: %s %dx%d %s" % (arguments.output, width, height,
				      "PASS" if failed == 0 else "FAIL"))
	return failed


if __name__ == "__main__":
	sys.exit(main())
