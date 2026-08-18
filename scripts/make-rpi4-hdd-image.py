#!/usr/bin/env python3
"""Build an MBR/FAT16 SD image bootable by Raspberry Pi 4 firmware."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR = 512
PARTITION_LBA = 2048
PARTITION_BLOCKS = 262144
IMAGE_BLOCKS = 524288
FIRMWARE_FILES = ("start4.elf", "fixup4.dat", "bcm2711-rpi-4-b.dtb",
                  "LICENCE.broadcom")


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def require_file(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"missing input: {path}")


def create(args: argparse.Namespace) -> None:
    for path in (args.kernel, args.arch_image, args.config):
        require_file(path)
    for name in FIRMWARE_FILES:
        require_file(args.firmware_dir / name)
    overlay = args.firmware_dir / "overlays" / "disable-bt.dtbo"
    require_file(overlay)
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=args.output.name + ".",
                                           dir=args.output.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        total_blocks = IMAGE_BLOCKS
        with temporary.open("r+b") as image:
            image.truncate(total_blocks * SECTOR)
            mbr = bytearray(SECTOR)
            mbr[0x1BE:0x1CE] = struct.pack(
                "<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x06,
                b"\xfe\xff\xff", PARTITION_LBA, PARTITION_BLOCKS)
            mbr[510:512] = b"\x55\xaa"
            image.write(mbr)

        spec = f"{temporary}@@{PARTITION_LBA * SECTOR}"
        run("mformat", "-i", spec, "-T", str(PARTITION_BLOCKS),
            "-v", "ZEDRPI4", "::")
        run("mmd", "-i", spec, "::/overlays")
        for name in FIRMWARE_FILES:
            run("mcopy", "-i", spec, str(args.firmware_dir / name),
                f"::/{name}")
        run("mcopy", "-i", spec, str(overlay),
            "::/overlays/disable-bt.dtbo")
        run("mcopy", "-i", spec, str(args.config), "::/config.txt")
        run("mcopy", "-i", spec, str(args.kernel), "::/VMUNIX.A64")
        run("mcopy", "-i", spec, str(args.arch_image),
            "::/rootfs.img")

        checker = Path(__file__).with_name("check-rpi4-hdd-image.py")
        run("python3", str(checker), "--kernel", str(args.kernel),
            "--arch-image", str(args.arch_image), "--config", str(args.config),
            str(temporary))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--arch-image", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--firmware-dir", type=Path, required=True)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()
