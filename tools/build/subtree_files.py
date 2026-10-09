"""The files of a package's --subtree DESTINATION=DIRECTORY (ws125-p002)."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import os
from pathlib import Path


def subtree_entries(items: list[str]) -> tuple[dict[str, Path], dict[str, Path]]:
    """Returns the regular files and the symbolic links of each DESTINATION=DIRECTORY, by their place in the root.

    The walk does not follow a link to a directory: the link itself is what the root holds.
    """
    regular: dict[str, Path] = {}
    links: dict[str, Path] = {}
    for item in items:
        if "=" not in item:
            raise SystemExit("--subtree requires /DESTINATION=DIRECTORY")
        destination, source_text = item.split("=", 1)
        source = Path(source_text)
        if not source.is_dir():
            raise SystemExit(f"--subtree names no directory: {source}")
        for top, directories, names in os.walk(source):
            for name in sorted(directories + names):
                path = Path(top) / name
                place = destination.rstrip("/") + "/" + path.relative_to(source).as_posix()
                if path.is_symlink():
                    links[place] = path
                elif path.is_file():
                    regular[place] = path
    return regular, links


def subtree_file_specifications(items: list[str]) -> list[str]:
    """Returns each directory's regular files as --file DESTINATION=SOURCE, for a root that holds no link (FAT)."""
    regular, links = subtree_entries(items)
    if links:
        first = sorted(links)[0]
        raise SystemExit(f"a FAT root holds no symbolic link: {first}")
    return [f"{destination}={source}" for destination, source in sorted(regular.items())]
