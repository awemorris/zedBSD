#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""Makes the data image: the UFS image an overlay writes to.

  make-data-image.py --backend ZEDIMAGE-HOST --output IMAGE [--size-mib N]
                     [--profile=journal-snapshot]

It holds the overlay journal's two slots, .zovl0 (empty, epoch 1) and
.zovl1 (zeros).  The image is written by zedimage-host.
"""

from __future__ import annotations

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from overlay_journal_format import JOURNAL_BYTES, empty_journal  # noqa: E402
from zedbuild import fail, parse_number  # noqa: E402
from zedimage import ufs_build  # noqa: E402


def remove_tree(path: str) -> bool:
    try:
        if os.path.lexists(path):
            shutil.rmtree(path)
    except OSError:
        return False
    return True


def main(args: list[str]) -> None:
    profile = "ordinary"
    output = ""
    backend = ""
    size_mib = 32
    i = 0
    while i < len(args):
        if args[i] == "--output" and i + 1 < len(args):
            output = args[i + 1]
            i += 2
        elif args[i] == "--backend" and i + 1 < len(args):
            backend = args[i + 1]
            i += 2
        elif args[i] == "--size-mib" and i + 1 < len(args):
            size_mib = parse_number(args[i + 1])
            i += 2
        elif args[i] == "--profile=journal-snapshot":
            profile = "journal-snapshot"
            i += 1
        else:
            fail("invalid make-data-image argument: " + args[i])
    if output == "" or backend == "" or size_mib < 16:
        fail("usage: make-data-image.py --backend PATH --output PATH [--size-mib N]")

    root = output + ".root.tmp"
    if not remove_tree(root):
        fail("cannot prepare data staging root")
    try:
        os.makedirs(root)
        with open(os.path.join(root, ".zovl0"), "wb") as stream:
            stream.write(empty_journal())
        with open(os.path.join(root, ".zovl1"), "wb") as stream:
            stream.write(bytes(JOURNAL_BYTES))
    except OSError:
        fail("cannot prepare data staging root")
    ufs_build(backend, str(size_mib * 1024 * 1024), root, output + ".tmp",
              profile)
    try:
        os.replace(output + ".tmp", output)
    except OSError:
        fail("cannot publish data image")
    if not remove_tree(root):
        fail("cannot publish data image")


if __name__ == "__main__":
    main(sys.argv[1:])
