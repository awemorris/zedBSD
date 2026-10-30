#!/usr/bin/env python3
"""Build the ODROID-C5's SD card image: bootloader area, FAT boot, UFS root."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The card is MBR-partitioned.  The first 8 MiB are the bootloader's: the
# S905X5M's boot ROM reads the bootloader from the sectors after the MBR,
# and Hardkernel's U-Boot keeps its environment at 0x3f0000.  Partition 1
# (FAT32, from 8 MiB) carries boot.scr, which U-Boot runs, the kernel, the
# device tree, data.img and the swapfile; partition 2 (type 0xa5, after it)
# is the UFS root the kernel's autoroot finds.
#
# The bootloader (Amlogic's signed stages and Hardkernel's U-Boot) is not
# built by zedBSD.  With --bootloader, a signed bootloader file (the
# firmware package amlogic-s7d-bootloader's u-boot.bin.signed) is written
# from sector 1, as Hardkernel's Yocto layer places it, and the MBR's boot
# code stays zero; the file must end before the boot partition.  With
# --bootloader-from, instead, the bootloader area of another
# card image (Hardkernel's own ODROID-C5 image, uncompressed) is copied in:
# the MBR's boot code and the sectors from 1 up to that image's first
# partition or 8 MiB, whichever is lower; this image's partition table
# stays.  A donor with a GPT's protective MBR, or whose area is all zero,
# is refused.  A card without a bootloader is made only with
# --no-bootloader, and then needs one written separately.  A card with a
# bootloader carries --notice (the bootloader's terms) on its boot
# partition as BOOTLOADER.txt, followed by the name of the file or donor
# and the SHA-256 of the MBR's boot code and the area written.

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
from pathlib import Path

SECTOR = 512
BOOTLOADER_END_LBA = 16384
BOOT_LBA = BOOTLOADER_END_LBA
BOOT_BLOCKS = 524288
ROOT_LBA = BOOT_LBA + BOOT_BLOCKS
BOOT_TYPE = 0x0C
ROOT_TYPE = 0xA5
MBR_CODE_BYTES = 440
MBR_TABLE = 0x1BE
VOLUME_LABEL = "ZEDC5"


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def require_file(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"make-odroid-c5-hdd-image: missing input: {path}")


def donor_area_end(donor: bytes) -> int:
    """The first sector after the donor's bootloader area."""
    end = BOOTLOADER_END_LBA
    if donor[510:512] != b"\x55\xaa":
        return end
    for index in range(4):
        entry = donor[MBR_TABLE + index * 16:MBR_TABLE + (index + 1) * 16]
        kind = entry[4]
        start = struct.unpack("<I", entry[8:12])[0]
        if kind == 0xEE:
            raise SystemExit("make-odroid-c5-hdd-image: the bootloader donor has a GPT "
                             "(protective MBR); its bootloader area cannot be told apart")
        if kind != 0 and 0 < start < end:
            end = start
    return end


def bootloader_notice(args: argparse.Namespace, source: str, code: bytes,
                      area: bytes, end: int) -> Path:
    """Writes the card's copy of the bootloader's notice beside the output."""
    digest = hashlib.sha256(code + area).hexdigest()
    text = args.notice.read_text(encoding="utf-8")
    text += (f"\n{source}\n"
             f"Area: the MBR's first {MBR_CODE_BYTES} bytes and sectors 1-{end - 1}, "
             f"SHA-256 {digest}\n")
    path = args.output.with_name(args.output.name + ".bootloader.txt")
    path.write_text(text, encoding="utf-8")
    print(f"make-odroid-c5-hdd-image: bootloader notice: {path} (area SHA-256 {digest})")
    return path


def build(args: argparse.Namespace) -> None:
    for path in (args.kernel, args.dtb, args.script, args.data_image,
                 args.swapfile, args.ufs_root):
        require_file(path)
    if args.ufs_root.stat().st_size % SECTOR:
        raise SystemExit("make-odroid-c5-hdd-image: the UFS root is not whole sectors")
    chosen = [args.bootloader is not None, args.bootloader_from is not None, args.no_bootloader]
    if sum(chosen) != 1:
        raise SystemExit("make-odroid-c5-hdd-image: give one of --bootloader (the signed "
                         "bootloader file), --bootloader-from (Hardkernel's ODROID-C5 image) "
                         "or --no-bootloader")
    if args.output.exists() and not args.force:
        raise SystemExit(f"make-odroid-c5-hdd-image: output exists (use --force): {args.output}")

    root_blocks = args.ufs_root.stat().st_size // SECTOR
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as image:
        image.truncate((ROOT_LBA + root_blocks) * SECTOR)
        mbr = bytearray(SECTOR)
        mbr[MBR_TABLE:MBR_TABLE + 16] = struct.pack(
            "<B3sB3sII", 0x80, b"\xfe\xff\xff", BOOT_TYPE, b"\xfe\xff\xff",
            BOOT_LBA, BOOT_BLOCKS)
        mbr[MBR_TABLE + 16:MBR_TABLE + 32] = struct.pack(
            "<B3sB3sII", 0, b"\xfe\xff\xff", ROOT_TYPE, b"\xfe\xff\xff",
            ROOT_LBA, root_blocks)
        mbr[510:512] = b"\x55\xaa"

        if args.bootloader is not None:
            require_file(args.bootloader)
            area = args.bootloader.read_bytes()
            end = 1 + (len(area) + SECTOR - 1) // SECTOR
            if not any(area):
                raise SystemExit(f"make-odroid-c5-hdd-image: the bootloader {args.bootloader} is empty")
            if end > BOOTLOADER_END_LBA:
                raise SystemExit(f"make-odroid-c5-hdd-image: the bootloader {args.bootloader} "
                                 f"({len(area)} bytes) does not end before sector {BOOTLOADER_END_LBA}")
            image.seek(SECTOR)
            image.write(area)
            print(f"make-odroid-c5-hdd-image: bootloader: {len(area)} bytes "
                  f"(sectors 1-{end - 1}) from {args.bootloader}")
            notice = bootloader_notice(args, f"Written from: {args.bootloader.name}",
                                       bytes(MBR_CODE_BYTES), area, end)
        elif args.bootloader_from is not None:
            require_file(args.bootloader_from)
            with args.bootloader_from.open("rb") as donor_file:
                donor = donor_file.read(BOOTLOADER_END_LBA * SECTOR)
            end = donor_area_end(donor)
            area = donor[SECTOR:end * SECTOR]
            if not any(area):
                raise SystemExit(f"make-odroid-c5-hdd-image: the bootloader area of "
                                 f"{args.bootloader_from} is empty (all zero, or the file is compressed)")
            mbr[0:MBR_CODE_BYTES] = donor[0:MBR_CODE_BYTES]
            image.seek(SECTOR)
            image.write(area)
            print(f"make-odroid-c5-hdd-image: bootloader: {len(area)} bytes "
                  f"(sectors 1-{end - 1}) from {args.bootloader_from}")
            notice = bootloader_notice(args, f"Copied from: {args.bootloader_from.name}",
                                       donor[0:MBR_CODE_BYTES], area, end)
        else:
            print("make-odroid-c5-hdd-image: bootloader: none (--no-bootloader); "
                  "the card does not boot until one is written")
            notice = None

        image.seek(0)
        image.write(mbr)
        image.seek(ROOT_LBA * SECTOR)
        image.write(args.ufs_root.read_bytes())

    spec = f"{args.output}@@{BOOT_LBA * SECTOR}"
    run("mformat", "-i", spec, "-F", "-T", str(BOOT_BLOCKS), "-H", str(BOOT_LBA),
        "-v", VOLUME_LABEL, "::")
    run("mcopy", "-i", spec, str(args.script), "::/boot.scr")
    run("mcopy", "-i", spec, str(args.kernel), "::/vmunix")
    run("mcopy", "-i", spec, str(args.dtb), "::/odroid-c5.dtb")
    run("mcopy", "-i", spec, str(args.data_image), "::/data.img")
    run("mcopy", "-i", spec, str(args.swapfile), "::/swapfile")
    if notice is not None:
        run("mcopy", "-i", spec, str(notice), "::/BOOTLOADER.txt")
    checker = Path(__file__).resolve().parents[3] / "tools/build/check-ufs-image.py"
    run("python3", str(checker), str(args.ufs_root))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--data-image", type=Path, required=True)
    parser.add_argument("--swapfile", type=Path, required=True)
    parser.add_argument("--ufs-root", type=Path, required=True)
    parser.add_argument("--bootloader", type=Path)
    parser.add_argument("--bootloader-from", type=Path)
    parser.add_argument("--no-bootloader", action="store_true")
    parser.add_argument("--notice", type=Path,
                        default=Path(__file__).resolve().parents[1] / "odroid-c5-bootloader.notice")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    build(parser.parse_args())


if __name__ == "__main__":
    main()
