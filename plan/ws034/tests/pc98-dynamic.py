#!/usr/bin/env python3
"""ws034-p057: logs in on a PC-98 guest whose programs are all dynamic and
runs a few of them.  Prints the screen after the commands.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    pc98-dynamic.py QEMU IMAGE
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
	"pc98_sleep", HERE.parent.parent / "ws040/tests/pc98-sleep.py")
pc98 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc98)


def main() -> int:
	qemu, image = sys.argv[1], Path(sys.argv[2])
	with tempfile.TemporaryDirectory(prefix="ws034-pc98-") as name:
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
			guest.send("clear; uname -m; date; ls /lib; echo DYNAMIC-DONE")
			guest.wait(r"^DYNAMIC-DONE", 300)
			print(guest.screen(), flush=True)
		finally:
			guest.close()
	return 0


if __name__ == "__main__":
	sys.exit(main())
