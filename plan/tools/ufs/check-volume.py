#!/usr/bin/env python3
"""Checks a UFS volume image the way fsck would, and lists its directories.

ws054.  Uses the validator of tools/build/check-ufs-image.py (reachability,
block ownership, bitmaps, link counts, summaries, directory records) on a
volume that is not a root image, then prints each directory's size.

    check-volume.py IMAGE
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
	"check_ufs_image", ROOT / "tools/build/check-ufs-image.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

# runtime: a mounted volume may have changed the clean byte and the counters.
fs = checker.UFS(pathlib.Path(sys.argv[1]).read_bytes(), runtime=True)
fs.validate()
for name, number, _ in fs.entries(checker.ROOT):
	if name in (".", ".."):
		continue
	raw = fs.inode(number)
	if fs.u16(raw, 0) & checker.IFMT != checker.IFDIR:
		continue
	size = len(fs.read_file(number))
	print(f"/{name}: {len(fs.entries(number)) - 2} names, {size} bytes, "
	      f"{size // fs.bsize + (1 if size % fs.bsize else 0)} blocks")
print(f"{sys.argv[1]}: UFS OK")
