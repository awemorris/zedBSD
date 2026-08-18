#!/usr/bin/env python3
"""Create a deterministic rootfs.tar.gz from a zedBSD file manifest."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import gzip
import io
import os
import tarfile
from pathlib import Path, PurePosixPath

DIRECTORIES = ("bin", "lib", "etc", "var", "var/run", "root", "home",
               "apps", "dev", "boot")


def parse_pairs(specifications: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for specification in specifications:
        if "=" not in specification:
            raise SystemExit(f"invalid manifest entry: {specification}")
        destination, value = specification.split("=", 1)
        path = PurePosixPath(destination)
        if not destination.startswith("/") or ".." in path.parts:
            raise SystemExit(f"invalid rootfs path: {destination}")
        result[destination] = value
    return result


def tar_info(name: str, mode: int, size: int = 0) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.mode = mode
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = 0
    info.size = size
    return info


def create(args: argparse.Namespace) -> None:
    files = parse_pairs(args.file)
    modes = {path: int(value, 8) for path, value in parse_pairs(args.mode).items()}
    if "/bin/sh" not in files:
        raise SystemExit("rootfs manifest must contain /bin/sh")
    for source in files.values():
        if not Path(source).is_file():
            raise SystemExit(f"missing rootfs input: {source}")
    directories = set(DIRECTORIES)
    for destination in files:
        parent = PurePosixPath(destination).parent
        while str(parent) not in ("/", "."):
            directories.add(str(parent).lstrip("/"))
            parent = parent.parent
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + f".tmp.{os.getpid()}")
    try:
        with temporary.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
                with tarfile.open(fileobj=zipped, mode="w") as archive:
                    for directory in sorted(directories,
                                            key=lambda item: (item.count("/"), item)):
                        info = tar_info(directory, 0o755)
                        info.type = tarfile.DIRTYPE
                        archive.addfile(info)
                    for destination, source_text in sorted(files.items()):
                        source = Path(source_text)
                        data = source.read_bytes()
                        default_mode = 0o755 if destination.startswith(("/bin/", "/lib/")) else 0o644
                        info = tar_info(destination.lstrip("/"),
                                        modes.get(destination, default_mode), len(data))
                        archive.addfile(info, io.BytesIO(data))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--file", action="append", default=[])
    parser.add_argument("--mode", action="append", default=[])
    create(parser.parse_args())


if __name__ == "__main__":
    main()
