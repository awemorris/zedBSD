#!/usr/bin/env python3
"""Fetch the ODROID-C5's device tree sources into a verified cache."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Each file of the manifest is fetched from its repository at a pinned
# revision into a new sibling directory, checked by size and SHA-256, and
# the directory is renamed to the cache's name only when every file
# passed.  An existing cache is checked the same way and never refetched;
# a corrupt one is an error.  A failed fetch leaves its partial directory
# for inspection (nothing is deleted).

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tempfile
import urllib.request
from pathlib import Path

# The repositories the manifest names, at the revisions the sources were
# taken from: Hardkernel's kernel_common_drivers (the SoC's device trees
# and binding headers) and Hardkernel's Linux (the generic headers).
REPOSITORIES = {
    "common": "https://raw.githubusercontent.com/hardkernel/kernel_common_drivers/"
              "34daba993d84e4aa09afbe3e2678dd19978eaace",
    "linux": "https://raw.githubusercontent.com/hardkernel/linux/"
             "9ea2aa831a585955dcb0e38a0bc3c63d2c690adc",
}


def read_manifest(path: Path) -> list[tuple[str, str, int, str]]:
    entries = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        repository, name, size, digest = line.split("|")
        if repository not in REPOSITORIES or name.startswith("/") or ".." in name.split("/"):
            raise SystemExit(f"fetch-odroid-c5-dts: bad manifest line: {line}")
        entries.append((repository, name, int(size), digest))
    return entries


def verify(directory: Path, entries: list[tuple[str, str, int, str]]) -> None:
    for repository, name, size, digest in entries:
        path = directory / repository / name
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"fetch-odroid-c5-dts: missing or unsafe file: {path}")
        data = path.read_bytes()
        if len(data) != size:
            raise SystemExit(f"fetch-odroid-c5-dts: size mismatch: {path}")
        if hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit(f"fetch-odroid-c5-dts: SHA-256 mismatch: {path}")


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("cache", type=Path)
    args = parser.parse_args()
    entries = read_manifest(args.manifest)

    if args.cache.exists() or args.cache.is_symlink():
        if not args.cache.is_dir() or args.cache.is_symlink():
            raise SystemExit(f"fetch-odroid-c5-dts: unsafe cache path: {args.cache}")
        verify(args.cache, entries)
        return

    args.cache.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="." + args.cache.name + ".",
                                    dir=args.cache.parent))
    for repository, name, _size, _digest in entries:
        target = staging / repository / name
        target.parent.mkdir(parents=True, exist_ok=True)
        url = REPOSITORIES[repository] + "/" + name
        print(f"fetch-odroid-c5-dts: {url}", file=sys.stderr)
        target.write_bytes(fetch(url))
    verify(staging, entries)
    os.rename(staging, args.cache)


if __name__ == "__main__":
    main()
