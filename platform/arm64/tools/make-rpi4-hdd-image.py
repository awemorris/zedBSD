#!/usr/bin/env python3
"""Build an MBR/FAT32 SD image bootable by Raspberry Pi 4 firmware."""
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
# The boot partition is FAT32 (the user's decision of 2026-09-24), the type
# the firmware and every SD card tool expect of it.  0x0C is FAT32 addressed
# by LBA.
PARTITION_TYPE = 0x0C
FIRMWARE_FILES = ("start4.elf", "fixup4.dat", "bcm2711-rpi-4-b.dtb",
                  "LICENCE.broadcom")


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def require_file(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"missing input: {path}")


def create(args: argparse.Namespace) -> None:
    for path in (args.kernel, args.data_image, args.swapfile, args.config):
        require_file(path)
    if args.cmdline is not None:
        require_file(args.cmdline)
    if args.arch_image is not None:
        require_file(args.arch_image)
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
                "<B3sB3sII", 0x80, b"\xfe\xff\xff", PARTITION_TYPE,
                b"\xfe\xff\xff", PARTITION_LBA, PARTITION_BLOCKS)
            mbr[510:512] = b"\x55\xaa"
            image.write(mbr)

        spec = f"{temporary}@@{PARTITION_LBA * SECTOR}"
        run("mformat", "-i", spec, "-F", "-T", str(PARTITION_BLOCKS),
            "-H", str(PARTITION_LBA),
            "-v", "ZEDRPI4", "::")
        run("mmd", "-i", spec, "::/overlays")
        for name in FIRMWARE_FILES:
            run("mcopy", "-i", spec, str(args.firmware_dir / name),
                f"::/{name}")
        run("mcopy", "-i", spec, str(overlay),
            "::/overlays/disable-bt.dtbo")
        run("mcopy", "-i", spec, str(args.config), "::/config.txt")
        if args.cmdline is not None:
            run("mcopy", "-i", spec, str(args.cmdline), "::/cmdline.txt")
        run("mcopy", "-i", spec, str(args.kernel), "::/vmunix")
        if args.arch_image is not None:
            run("mcopy", "-i", spec, str(args.arch_image), "::/rootfs.img")
        run("mcopy", "-i", spec, str(args.data_image), "::/data.img")
        run("mcopy", "-i", spec, str(args.swapfile), "::/swapfile")

        checker = Path(__file__).with_name("check-rpi4-hdd-image.py")
        arch = [] if args.arch_image is None else \
            ["--arch-image", str(args.arch_image)]
        cmdline = [] if args.cmdline is None else ["--cmdline", str(args.cmdline)]
        run("python3", str(checker), *cmdline, "--kernel", str(args.kernel), *arch,
            "--data-image", str(args.data_image),
            "--swapfile", str(args.swapfile), "--config", str(args.config),
            str(temporary))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    # The FAT overlay image is optional: a UFS root partition replaces it.
    parser.add_argument("--arch-image", type=Path)
    parser.add_argument("--data-image", type=Path, required=True)
    parser.add_argument("--swapfile", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--cmdline", type=Path)
    parser.add_argument("--firmware-dir", type=Path, required=True)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()
