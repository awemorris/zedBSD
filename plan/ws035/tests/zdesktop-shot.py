#!/usr/bin/env python3
"""Photographs the compositor's output (the Venus console) as PNG.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

QMP screendump has no surface for a GL scanout, so the picture is read the
way WS014 read it: through QEMU's VNC server, whose socket
zdesktop-guest.sh binds to the Venus console.

    zdesktop-shot.py OUT.png [--runtime build/ws035-sq-run]
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
	arguments = parser.parse_args()
	ppm = arguments.output + ".ppm"
	report = capture(str(Path(arguments.runtime) / "vnc.sock"), ppm, 10)
	width, height, pixels = read_ppm(ppm)
	write_png(arguments.output, width, height, pixels)
	os.unlink(ppm)
	print(f"shot: {arguments.output} {width}x{height}")
	return 0 if report is not None else 1


if __name__ == "__main__":
	sys.exit(main())
