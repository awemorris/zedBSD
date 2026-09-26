#!/usr/bin/env python3
"""Prints files of the root UFS volume of a zedBSD disk image (GPT, partition zedBSD-root).

WS035 p066: the guest of a passthrough run writes its programs' output to files
(plan/ws031/tests/zdesktop/*.sh); after the run they are read here from the
disk image instead of from the serial log.  Files still only in the journal of
an unclean stop may be missing.

    ufs-cat.py IMAGE PATH...
Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import importlib.util
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_ufs_image", HERE / "check-ufs-image.py")
if not (HERE / "check-ufs-image.py").exists():
	spec = importlib.util.spec_from_file_location(
		"check_ufs_image", HERE.parents[2] / "tools/build/check-ufs-image.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def root_partition(image):
	"""The byte range of the GPT partition named zedBSD-root."""
	header = image[512:1024]
	entries_lba, count, size = struct.unpack_from("<QII", header, 72)
	for index in range(count):
		entry = image[entries_lba * 512 + index * size:entries_lba * 512 + (index + 1) * size]
		name = entry[56:128].decode("utf-16-le").rstrip("\0")
		if name == "zedBSD-root":
			first, last = struct.unpack_from("<QQ", entry, 32)
			return first * 512, (last + 1) * 512
	raise SystemExit("ufs-cat: no zedBSD-root partition")


def main():
	image = pathlib.Path(sys.argv[1]).read_bytes()
	start, end = root_partition(image)
	fs = checker.UFS(image[start:end], runtime=True)
	for path in sys.argv[2:]:
		number = checker.ROOT
		for part in [p for p in path.split("/") if p]:
			found = [n for name, n, _ in fs.entries(number) if name == part]
			if not found:
				print(f"ufs-cat: {path}: not found")
				number = None
				break
			number = found[0]
		if number is not None:
			print(f"===== {path}")
			sys.stdout.write(fs.read_file(number).decode(errors="replace"))
	return 0


if __name__ == "__main__":
	sys.exit(main())
