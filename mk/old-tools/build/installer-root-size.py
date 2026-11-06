#!/usr/bin/env python3
"""installer-root-size.py: how large the installer's image's root is made.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    installer-root-size.py --tree LIVE [--free-mib MIB]

Prints the size in MiB that the UFS of LIVE is made with: what LIVE's files
take in the producer's fragments (1 KiB) and the inode table of the most
inodes it gives (65536), a tenth more for the indirect blocks and the
groups' own blocks, and MIB free (512 by default) for what the running
system writes.  It fails, with what it measured, when the root would be
larger than the producer makes (16 GiB, tools/build/zedimage-host.c) or LIVE
has more entries than the producer's inodes.
"""
from __future__ import annotations

import argparse
import os
import sys

FRAGMENT = 1024
INODE_SIZE = 256
MOST_INODES = 65536
RESERVED_INODES = 3
LARGEST_MIB = 16 * 1024
MIB = 1024 * 1024


def main() -> int:
	"""Prints the root's size, or fails over the producer's limits."""
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument("--tree", required=True)
	parser.add_argument("--free-mib", type=int, default=512)
	arguments = parser.parse_args()

	used = 0
	entries = 1
	for top, directories, files in os.walk(arguments.tree):
		for name in directories + files:
			info = os.lstat(os.path.join(top, name))
			entries += 1
			used += max(FRAGMENT, (info.st_size + FRAGMENT - 1) // FRAGMENT * FRAGMENT)
	used += MOST_INODES * INODE_SIZE
	size_mib = (used + used // 10 + MIB - 1) // MIB + arguments.free_mib
	measured = f"{entries} entries, {used // MIB} MiB used, {size_mib} MiB with {arguments.free_mib} MiB free"
	if entries + RESERVED_INODES > MOST_INODES:
		print(f"installer-root-size: more entries than the root's {MOST_INODES} inodes: {measured}", file=sys.stderr)
		return 1
	if size_mib > LARGEST_MIB:
		print(f"installer-root-size: larger than the {LARGEST_MIB} MiB the producer makes: {measured}", file=sys.stderr)
		return 1
	print(f"installer-root-size: {measured}", file=sys.stderr)
	print(size_mib)
	return 0


if __name__ == "__main__":
	sys.exit(main())
