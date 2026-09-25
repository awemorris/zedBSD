#!/usr/bin/env python3
"""Boots a PC-98 image to the login prompt, logs in and runs one command.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The PC-98 counterpart of boot-test.sh, which has no PC-98 mode (ws053-p004).
It follows the master's PC-98 procedure: the QEMU PC-98 fork, key events
through its monitor, and the text VRAM read with pmemsave.  The screen at the
login prompt and after `uname -a` is written as text and as a PNG drawn from
that text.  The exit status is 0 only when both were seen.

    pc98-boot.py QEMU IMAGE OUTPUT

    QEMU     the PC-98 fork (~/qemu-pc98/build/qemu-system-i386)
    IMAGE    a pc98 hdd-image.img (copied; the original is not written)
    OUTPUT   directory for login.png, command.png and their .txt
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HELPER = Path(__file__).resolve().parent / "clock" / "pc98-sleep.py"


def load_guest():
	"""Imports the Guest class of the PC-98 clock tool."""
	spec = importlib.util.spec_from_file_location("pc98_sleep", HELPER)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module.Guest


def save(screen: str, path: Path) -> None:
	"""Writes the screen as text and as a PNG."""
	path.with_suffix(".txt").write_text(screen + "\n")
	image = Image.new("RGB", (80 * 8 + 16, 25 * 16 + 16), "black")
	draw = ImageDraw.Draw(image)
	for row, line in enumerate(screen.split("\n")):
		draw.text((8, 8 + row * 16), line, fill="white")
	image.save(path)


def main() -> int:
	qemu, image, output = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
	output.mkdir(parents=True, exist_ok=True)
	guest_class = load_guest()
	with tempfile.TemporaryDirectory(prefix="ws053-pc98-") as name:
		directory = Path(name)
		disk = directory / "guest.img"
		subprocess.run(["cp", "--sparse=always", str(image), str(disk)], check=True)
		guest = guest_class(qemu, disk, directory)
		try:
			guest.wait(r"login: *$", 300)
			save(guest.screen(), output / "login.png")
			guest.send("root")
			guest.wait(r"Password:", 30)
			guest.send("")
			guest.wait(r"[#$] *$", 60)
			guest.send("uname -a; echo WSDONE")
			guest.wait(r"^WSDONE *$", 60)
			save(guest.screen(), output / "command.png")
		finally:
			guest.close()
	print(output / "login.png")
	return 0


if __name__ == "__main__":
	sys.exit(main())
