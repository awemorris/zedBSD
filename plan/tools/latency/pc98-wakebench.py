#!/usr/bin/env python3
"""Runs wakebench on a PC-98 guest (i386, 100 Hz timer) and prints its lines.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

WS041.  PC-98 has no serial console, so the guest is driven through the
QEMU monitor and read from text VRAM with the WS040 guest driver.  The
486 is emulated, so the counts are small.

    pc98-wakebench.py QEMU IMAGE
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
	"pc98_sleep", HERE.parent / "clock/pc98-sleep.py")
pc98 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc98)

RUNS = (("quiet 200", "QUIET"), ("pingpong 200", "PINGPONG"),
	("sleep 100", "SLEEP"), ("fair 5", "FAIR"), ("starve 5", "STARVE"))


def main() -> int:
	qemu, image = sys.argv[1], Path(sys.argv[2])
	with tempfile.TemporaryDirectory(prefix="latency-pc98-") as name:
		directory = Path(name)
		disk = directory / "guest.img"
		subprocess.run(["cp", "--sparse=always", str(image), str(disk)], check=True)
		guest = pc98.Guest(qemu, disk, directory)
		try:
			guest.wait(r"login: *$", 300)
			guest.send("root")
			guest.wait(r"Password:", 30)
			guest.send("")
			guest.wait(r"[#$] *$", 60)
			for command, label in RUNS:
				guest.send("clear; wakebench " + command)
				guest.wait(r"^WAKEBENCH " + label + " .*$", 600)
				for line in guest.screen().splitlines():
					if line.startswith("WAKEBENCH"):
						print(line.strip(), flush=True)
		finally:
			guest.close()
	return 0


if __name__ == "__main__":
	sys.exit(main())
