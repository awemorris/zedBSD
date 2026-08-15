#!/usr/bin/env python3
"""Create the canonical zedBSD UFS1 root filesystem image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from ufs1_format import create


def build(args: argparse.Namespace) -> None:
    native = args.arch_profile == "sparcv9"
    if native:
        if args.native_shell is None or not args.native_shell.is_file():
            raise SystemExit("sparcv9 root requires --native-shell")
    elif args.arch_image is None or not args.arch_image.is_file():
        raise SystemExit(f"missing architecture image: {args.arch_image}")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")
    if args.size_mib < 16:
        raise SystemExit("UFS1 root image must be at least 16 MiB")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zedbsd-ufs-root-") as work_text:
        root = Path(work_text)
        for directory in ("bin", "lib", "etc", "home", "apps", "arch",
                          "dev", "boot", "disk1", "disk2"):
            (root / directory).mkdir()
        if native:
            shutil.copy2(args.native_shell, root / "bin" / "sh")
        else:
            shutil.copyfile(args.arch_image,
                            root / "arch" / f"{args.arch_profile}.ufs")
        data = create(args.size_mib * 1024 * 1024, root)
        temporary = args.output.with_name(args.output.name + ".tmp")
        try:
            temporary.write_bytes(data)
            temporary.replace(args.output)
        finally:
            if temporary.exists():
                temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch-profile",
                        choices=("i386", "amd64", "aarch64", "sparcv9"),
                        required=True)
    parser.add_argument("--arch-image", type=Path)
    parser.add_argument("--native-shell", type=Path)
    # The canonical writer intentionally uses one cylinder group.  With 1 KiB
    # fragments its free-fragment bitmap fits in the 8 KiB CG block through
    # 60 MiB, leaving a small margin for the fixed CG header.
    parser.add_argument("--size-mib", type=int, default=60)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    build(parser.parse_args())


if __name__ == "__main__":
    main()
