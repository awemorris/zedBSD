#!/usr/bin/env python3
"""Build a PC-98 HDD disk image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512
HEADS = 8
SECTORS_PER_TRACK = 17
CYLINDER_SECTORS = HEADS * SECTORS_PER_TRACK  # 136 sectors = 69632 bytes
CYLINDER_BYTES = CYLINDER_SECTORS * SECTOR


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def pc98_partition_entry(mid: int, sid: int, cyl_start: int, cyl_end: int, name: str) -> bytes:
    start_chs = struct.pack("<BBH", 0, 0, cyl_start)
    data_chs = struct.pack("<BBH", 0, 0, cyl_start)
    end_chs = struct.pack("<BBH", 16, 7, cyl_end - 1)
    name_bytes = name.encode("ascii").ljust(16, b" ")[:16]
    return struct.pack("<BB2s4s4s4s16s", mid, sid, b"\x00\x00", start_chs, data_chs, end_chs, name_bytes)


def copy_sparse_file(image_file, offset_bytes: int, source_path: Path) -> None:
    image_file.seek(offset_bytes)
    with source_path.open("rb") as src:
        while chunk := src.read(1024 * 1024):
            image_file.write(chunk)


def format_fat16(image_path: Path, offset_bytes: int, start_lba: int,
                 blocks: int, label: str) -> None:
    spec = f"{image_path}@@{offset_bytes}"
    cluster = 1
    while blocks // cluster >= 65525:
        cluster *= 2
    run("mformat", "-i", spec, "-h", str(HEADS), "-s", str(SECTORS_PER_TRACK),
        "-H", str(start_lba), "-R", "4", "-c", str(cluster), "-T", str(blocks),
        "-v", label, "::")


def install_fat16_pbr(image_path: Path, partition_offset: int,
                      pbr_bytes: bytes) -> None:
    with image_path.open("r+b") as f:
        f.seek(partition_offset)
        bpb_sector = f.read(512)
        if len(bpb_sector) < 512:
            raise SystemExit("short read of FAT16 boot sector")
        sec_per_fat = struct.unpack_from("<H", bpb_sector, 22)[0]
        if sec_per_fat == 0:
            raise SystemExit("expected FAT16 partition, got FAT32")
        pbr = bytearray(pbr_bytes)
        # Preserve BPB: offset 3 to 0x3E (62)
        pbr[3:0x3E] = bpb_sector[3:0x3E]
        f.seek(partition_offset)
        f.write(pbr)


def copy_to_fat(image_path: Path, offset_bytes: int, src_file: Path,
                dest_name: str) -> None:
    spec = f"{image_path}@@{offset_bytes}"
    run("mcopy", "-o", "-i", spec, str(src_file), f"::{dest_name}")


def create(args: argparse.Namespace) -> None:
    if not args.stage1.is_file():
        raise SystemExit(f"missing stage1: {args.stage1}")
    if not args.stage2.is_file():
        raise SystemExit(f"missing stage2: {args.stage2}")
    if not args.partition_pbr.is_file():
        raise SystemExit(f"missing partition PBR: {args.partition_pbr}")
    if not args.bootzbsd.is_file():
        raise SystemExit(f"missing BOOTZBSD.EXE: {args.bootzbsd}")
    if not args.kernel.is_file():
        raise SystemExit(f"missing kernel: {args.kernel}")
    if not args.zedbsd_config.is_file():
        raise SystemExit(f"missing zedbsd.cfg: {args.zedbsd_config}")
    if not args.ufs_root.is_file() or args.ufs_root.stat().st_size % SECTOR:
        raise SystemExit("invalid UFS root image")
    if not args.swap_image.is_file() or args.swap_image.stat().st_size % SECTOR:
        raise SystemExit("invalid swap image")

    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=args.output.name + ".",
                                     dir=args.output.parent)
    os.close(fd)
    temporary = Path(temp_name)

    try:
        # Boot partition starts at cylinder 16 (LBA 2176, aligned to 136 and 8)
        cyl_boot_start = 16
        boot_start = cyl_boot_start * CYLINDER_SECTORS  # 2176

        raw_boot_blocks = args.fat_size_mib * 2048
        boot_cyls = (raw_boot_blocks + CYLINDER_SECTORS - 1) // CYLINDER_SECTORS
        cyl_boot_end = cyl_boot_start + boot_cyls
        boot_blocks = boot_cyls * CYLINDER_SECTORS

        # Root partition (UFS)
        root_blocks = args.ufs_root.stat().st_size // SECTOR
        cyl_root_start = cyl_boot_end
        root_cyls = (root_blocks + CYLINDER_SECTORS - 1) // CYLINDER_SECTORS
        cyl_root_end = cyl_root_start + root_cyls
        root_start = cyl_root_start * CYLINDER_SECTORS

        # Swap partition
        swap_blocks = args.swap_image.stat().st_size // SECTOR
        cyl_swap_start = cyl_root_end
        swap_cyls = (swap_blocks + CYLINDER_SECTORS - 1) // CYLINDER_SECTORS
        cyl_swap_end = cyl_swap_start + swap_cyls
        swap_start = cyl_swap_start * CYLINDER_SECTORS

        total_sectors = cyl_swap_end * CYLINDER_SECTORS

        with temporary.open("wb") as stream:
            stream.truncate(total_sectors * SECTOR)

            stage1 = bytearray(args.stage1.read_bytes())
            if len(stage1) != 512 or stage1[510:512] != b"\x55\xaa":
                raise SystemExit("invalid stage1")
            stage2 = args.stage2.read_bytes()
            if len(stage2) % SECTOR != 0 or stage2[510:512] != b"\x55\xaa":
                raise SystemExit("invalid stage2")

            # Sector 0: IPL (Stage 1)
            stream.seek(0)
            stream.write(stage1)

            # Sector 1: PC-98 Partition Table
            part_table = bytearray(512)
            part_table[0:32] = pc98_partition_entry(0xa1, 0x91, cyl_boot_start, cyl_boot_end, "BOOT")
            part_table[32:64] = pc98_partition_entry(0x21, 0x01, cyl_root_start, cyl_root_end, "ROOT")
            part_table[64:96] = pc98_partition_entry(0x21, 0x02, cyl_swap_start, cyl_swap_end, "SWAP")
            stream.seek(1 * SECTOR)
            stream.write(part_table)

            # Sector 2..: Stage 2
            stream.seek(2 * SECTOR)
            stream.write(stage2)

        # Copy UFS root and Swap
        copy_sparse_file(temporary.open("r+b"), root_start * SECTOR, args.ufs_root)
        copy_sparse_file(temporary.open("r+b"), swap_start * SECTOR, args.swap_image)

        # Format FAT16 BOOT partition
        boot_offset = boot_start * SECTOR
        format_fat16(temporary, boot_offset, boot_start, boot_blocks, "BOOT")
        install_fat16_pbr(temporary, boot_offset, args.partition_pbr.read_bytes())

        # Copy files to FAT16 partition
        copy_to_fat(temporary, boot_offset, args.bootzbsd, "/BOOTZBSD.EXE")
        copy_to_fat(temporary, boot_offset, args.kernel, "/vmunix")
        copy_to_fat(temporary, boot_offset, args.zedbsd_config, "/BOOTZBSD.CFG")
        copy_to_fat(temporary, boot_offset, args.zedbsd_config, "/zedbsd.cfg")
        if args.logo is not None and args.logo.is_file():
            copy_to_fat(temporary, boot_offset, args.logo, "/logo.ppm")

        # Run validation
        checker = Path(__file__).with_name("check-pc98-hdd-image.py")
        check_args = [sys.executable, str(checker),
                      "--kernel", str(args.kernel),
                      "--zedbsd-config", str(args.zedbsd_config),
                      "--ufs-root", str(args.ufs_root),
                      "--swap-image", str(args.swap_image),
                      "--stage1", str(args.stage1),
                      "--stage2", str(args.stage2),
                      "--partition-pbr", str(args.partition_pbr),
                      "--bootzbsd", str(args.bootzbsd)]
        if args.logo is not None and args.logo.is_file():
            check_args += ["--logo", str(args.logo)]
        check_args.append(str(temporary))

        run(*check_args)
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description="Create PC-98 HDD image")
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--zedbsd-config", type=Path, required=True)
    parser.add_argument("--logo", type=Path)
    parser.add_argument("--ufs-root", type=Path, required=True)
    parser.add_argument("--swap-image", "--swapfile", dest="swap_image",
                        type=Path, required=True)
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--stage2", type=Path, required=True)
    parser.add_argument("--partition-pbr", type=Path, required=True)
    parser.add_argument("--bootzbsd", type=Path, required=True)
    parser.add_argument("--fat-size-mib", dest="fat_size_mib",
                        type=int, default=256)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)

    args = parser.parse_args()
    create(args)


if __name__ == "__main__":
    main()
