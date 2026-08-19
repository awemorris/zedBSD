#!/usr/bin/env python3
"""Create an architecture-specific raw FAT16 root filesystem image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import re
import stat
import subprocess
import tempfile
from pathlib import Path

PROFILES = {
    "i386": ("ZEDI386", 1, 3),
    "amd64": ("ZEDAMD64", 2, 62),
    "aarch64": ("ZEDAARCH64", 2, 183),
}
FAT_COMPONENT = r"[a-z0-9_]{1,8}(?:\.[a-z0-9_]{1,3})?"
DESTINATION = re.compile(rf"/(bin|lib|etc|var|usr)(?:/{FAT_COMPONENT}){{1,4}}")


def run(*arguments: str) -> None:
    subprocess.run(arguments, check=True)


def parse_files(specifications: list[str]) -> dict[str, Path]:
    files: dict[str, Path] = {}
    folded: set[str] = set()
    for specification in specifications:
        if "=" not in specification:
            raise SystemExit("--file requires /bin/NAME=SOURCE or /lib/NAME=SOURCE")
        destination, source_text = specification.split("=", 1)
        source = Path(source_text)
        if not DESTINATION.fullmatch(destination):
            raise SystemExit(f"invalid FAT16 destination: {destination}")
        key = destination.casefold()
        if key in folded:
            raise SystemExit(f"case-insensitive destination collision: {destination}")
        if not source.is_file():
            raise SystemExit(f"missing manifest input: {source}")
        if not stat.S_ISREG(source.stat().st_mode):
            raise SystemExit(f"manifest input is not a regular file: {source}")
        files[destination] = source
        folded.add(key)
    if "/bin/sh" not in files:
        raise SystemExit("profile manifest must contain /bin/sh")
    return files


def parse_modes(specifications: list[str], files: dict[str, Path]) -> dict[str, int]:
    modes: dict[str, int] = {}
    for specification in specifications:
        if "=" not in specification:
            raise SystemExit("--mode requires /PATH=OCTAL")
        destination, mode_text = specification.split("=", 1)
        if destination not in files:
            raise SystemExit(f"mode destination is not a manifest file: {destination}")
        try:
            mode = int(mode_text, 8)
        except ValueError as error:
            raise SystemExit(f"invalid mode: {specification}") from error
        if mode < 0 or mode > 0o7777:
            raise SystemExit(f"invalid mode: {specification}")
        modes[destination] = mode
    return modes


def create(args: argparse.Namespace) -> None:
    label, _, _ = PROFILES[args.profile]
    files = parse_files(args.file)
    modes = parse_modes(args.mode, files)
    if args.size_mib < 16:
        raise SystemExit("architecture image must be at least 16 MiB")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=args.output.name + ".", dir=args.output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("r+b") as stream:
            stream.truncate(args.size_mib * 1024 * 1024)
        # -h/-s select a geometry whose 16-MiB default formats as FAT16.
        run("mformat", "-i", str(temporary), "-h", "16", "-s", "32",
            "-v", label, "::")
        for directory in ("bin", "lib", "etc", "var", "var/run", "root",
                          "home", "usr", "usr/bin", "dev", "boot"):
            run("mmd", "-i", str(temporary), f"::/{directory}")
        parents = set()
        for destination in files:
            parent = str(Path(destination).parent).replace("\\", "/")
            while parent not in ("/", "/bin", "/lib", "/etc", "/var", "/usr"):
                parents.add(parent)
                parent = str(Path(parent).parent).replace("\\", "/")
        for directory in sorted(parents, key=lambda value: value.count("/")):
            run("mmd", "-i", str(temporary), f"::{directory}")
        with tempfile.TemporaryDirectory(prefix="zedbsd-arch-files-") as work_text:
            work = Path(work_text)
            marker = work / "arch.id"
            marker.write_text(args.profile + "\n", encoding="ascii")
            run("mcopy", "-i", str(temporary), str(marker), "::/lib/arch.id")
        for destination, source in sorted(files.items()):
            run("mcopy", "-i", str(temporary), str(source), f"::{destination}")
        if modes:
            with tempfile.TemporaryDirectory(prefix="zedbsd-metadata-") as metadata_text:
                metadata = Path(metadata_text) / "unixmode"
                metadata.write_text("".join(
                    f"{destination.lstrip('/')}:{mode:04o}:0:0\n"
                    for destination, mode in sorted(modes.items())) +
                    "etc/unixmode:0400:0:0\n", encoding="ascii")
                run("mcopy", "-i", str(temporary), str(metadata), "::/etc/unixmode")
        checker = Path(__file__).with_name("check-arch-overlay-image.py")
        command = ["python3", str(checker), "--profile", args.profile,
                   "--image", str(temporary), "--size-mib", str(args.size_mib),
                   "--min-free-bytes", str(args.min_free_bytes)]
        for destination, source in sorted(files.items()):
            command += ["--file", f"{destination}={source}"]
        for destination, mode in sorted(modes.items()):
            command += ["--mode", f"{destination}={mode:04o}"]
        run(*command)
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--min-free-bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--file", action="append", default=[])
    parser.add_argument("--mode", action="append", default=[])
    parser.add_argument("--force", action="store_true")
    create(parser.parse_args())


if __name__ == "__main__":
    main()
