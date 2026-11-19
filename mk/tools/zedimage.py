#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""Runs the host image tool (zedimage-host) for the build scripts.

zedimage-host is the C program that writes the UFS images; these helpers
give it its arguments and measure the trees it is handed.
"""

from __future__ import annotations

import os
import subprocess

from zedbuild import fail


def ufs_build(backend: str, size_text: str, root: str, output: str,
              profile: str = "ordinary", inodes: int = 0) -> None:
    """Builds OUTPUT, a UFS image of the tree ROOT.

    SIZE_TEXT is the image's size in bytes, or in MiB when it ends in M.
    PROFILE is "ordinary" or "journal-snapshot" (a tail journal and
    snapshot area after the file system).
    """
    command = [backend, "ufs", size_text, root, output]
    if profile == "journal-snapshot":
        command.append("--profile=journal-snapshot")
    elif profile != "ordinary":
        fail("unsupported UFS profile")
    # Inodes are provisioned from the cylinder groups, which the byte size
    # alone decides.  A root of many small files needs more of them than
    # that, so the count is asked for when the caller knows it.
    if inodes > 0:
        command.append("--inodes=%d" % inodes)
    if subprocess.run(command).returncode != 0:
        fail("UFS backend failed")


def ufs_build_mib(backend: str, size_mib: int, root: str, output: str,
                  profile: str = "ordinary", inodes: int = 0) -> None:
    """Builds a UFS image whose size is given in MiB."""
    ufs_build(backend, "%dM" % size_mib, root, output, profile, inodes)


def count_entries(root: str) -> int:
    """Counts what the staged root will need one inode for: every
    directory, file and symbolic link under it, and the root directory
    itself.  Symbolic links to directories are counted, not followed."""
    count = 1
    for _, directories, files in os.walk(root):
        count += len(directories) + len(files)
    return count


def measure_mib(root: str) -> int:
    """Sizes an image for the tree ROOT from its disk usage.

    On top of the file bytes UFS needs inodes, cylinder-group metadata
    and fragment rounding, and nothing here rewards being frugal: the
    measured size is doubled and given a fixed margin.
    """
    result = subprocess.run(["du", "-sk", root], stdout=subprocess.PIPE,
                            text=True)
    if result.returncode != 0:
        fail("cannot measure the staging root")
    field = result.stdout.split("\t", 1)[0]
    if field == "" or "\t" not in result.stdout:
        fail("cannot read the staging root size")
    return int(field) * 2 // 1024 + 32
