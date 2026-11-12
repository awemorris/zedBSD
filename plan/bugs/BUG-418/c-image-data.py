#!/usr/bin/env python3
"""The packages' data files in a root UFS image.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    c-image-data.py UFS-IMAGE TREE PATH...

For each PATH, reads the file from the UFS image (with the reader of
mk/old-tools/build/check-ufs-image.py) and prints its mode, owner, size and
SHA-256, and whether its bytes are those of TREE/PATH (the tree the image
was made from).  The last line is PASS when every PATH is in the image with
those bytes and is readable by everyone, FAIL otherwise.
"""
import hashlib
import importlib.util
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
	"check_ufs_image", ROOT / "mk/old-tools/build/check-ufs-image.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def main() -> int:
	image, tree, paths = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3:]
	fs = checker.UFS(pathlib.Path(image).read_bytes())
	verdict = "PASS"
	for path in paths:
		try:
			number = fs.lookup(path)
		except ValueError:
			print(f"{path}: missing")
			verdict = "FAIL"
			continue
		raw = fs.inode(number)
		mode = struct.unpack_from("<H", raw, 0)[0]
		uid, gid = struct.unpack_from("<II", raw, 4)
		data = fs.read_file(number)
		digest = hashlib.sha256(data).hexdigest()
		source = tree / path.lstrip("/")
		same = source.is_file() and source.read_bytes() == data
		print(f"{path}: mode {mode & 0o7777:04o} uid {uid} gid {gid} "
			f"size {len(data)} sha256 {digest} {'same as tree' if same else 'DIFFERS from tree'}")
		if not same or (mode & 0o444) != 0o444:
			verdict = "FAIL"
	print(verdict)
	return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
	sys.exit(main())
