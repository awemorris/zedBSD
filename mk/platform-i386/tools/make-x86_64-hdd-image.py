#!/usr/bin/env python3
"""Build an x86_64 disk image for BIOS, UEFI, or Hybrid boot."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

SECTOR = 512
GPT_HEADER_SIZE = 92
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128

# GUIDs (little-endian byte representation)
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
BASIC_DATA_TYPE = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
UFS_ROOT_TYPE = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b")
SWAP_TYPE = uuid.UUID("516e7cb5-6ecf-11d6-8ff8-00022d09712b")
BIOS_BOOT_TYPE = uuid.UUID("21686148-6449-6e6f-744e-656564454649")


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def align_up(val: int, alignment: int = 2048) -> int:
    return (val + alignment - 1) // alignment * alignment


def gpt_entry(type_guid: uuid.UUID, unique_guid: uuid.UUID, first: int,
              last: int, name: str) -> bytes:
    return (type_guid.bytes_le + unique_guid.bytes_le +
            struct.pack("<QQQ", first, last, 0) +
            name.encode("utf-16le")[:72].ljust(72, b"\0"))


def gpt_header(current: int, backup: int, entries_lba: int, first_usable: int,
               last_usable: int, disk_guid: uuid.UUID,
               entries_crc: int) -> bytes:
    header = bytearray(GPT_HEADER_SIZE)
    struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0, b"EFI PART",
                     0x00010000, GPT_HEADER_SIZE, 0, 0, current, backup,
                     first_usable, last_usable, disk_guid.bytes_le,
                     entries_lba, GPT_ENTRY_COUNT, GPT_ENTRY_SIZE,
                     entries_crc)
    crc = zlib.crc32(header) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, crc)
    return bytes(header).ljust(SECTOR, b"\0")


def copy_sparse_file(image_file, offset_bytes: int, source_path: Path) -> None:
    image_file.seek(offset_bytes)
    with source_path.open("rb") as src:
        while chunk := src.read(1024 * 1024):
            image_file.write(chunk)


def format_fat(image_path: Path, offset_bytes: int, start_lba: int,
               blocks: int, label: str) -> None:
    spec = f"{image_path}@@{offset_bytes}"
    run("mformat", "-i", spec, "-F", "-T", str(blocks), "-H", str(start_lba),
        "-v", label, "::")


def install_fat32_pbr(image_path: Path, partition_offset: int,
                      pbr_bytes: bytes) -> None:
    with image_path.open("r+b") as f:
        f.seek(partition_offset)
        bpb = f.read(2048)
        if len(bpb) < 2048:
            raise SystemExit("short read of BPB")
        is_fat32 = (struct.unpack_from("<H", bpb, 22)[0] == 0)
        pbr = bytearray(pbr_bytes)
        if is_fat32:
            pbr[3:0x5A] = bpb[3:0x5A]
            reserved_sectors = struct.unpack_from("<H", bpb, 14)[0]
            backup_sector = struct.unpack_from("<H", bpb, 50)[0]
            if backup_sector == 0 or backup_sector >= reserved_sectors:
                raise SystemExit("invalid FAT32 backup boot sector")
            f.seek(partition_offset)
            f.write(pbr[:512])
            f.seek(partition_offset + backup_sector * 512)
            f.write(pbr[:512])
            f.seek(partition_offset + 1024)
            f.write(pbr[512:2048])
        else:
            pbr[3:0x3E] = bpb[3:0x3E]
            f.seek(partition_offset)
            f.write(pbr[:2048])


def copy_to_fat(image_path: Path, partition_offset: int, source_path: Path,
                dest_name: str) -> None:
    spec = f"{image_path}@@{partition_offset}"
    run("mcopy", "-i", spec, str(source_path), f"::{dest_name}")


def mkdir_fat(image_path: Path, partition_offset: int, dir_name: str) -> None:
    spec = f"{image_path}@@{partition_offset}"
    run("mmd", "-i", spec, f"::{dir_name}")


def create(args: argparse.Namespace) -> None:
    variant = "uefi" if args.variant == "native" else args.variant

    if not args.ufs_root.is_file() or args.ufs_root.stat().st_size % SECTOR:
        raise SystemExit("invalid UFS root image")
    if not args.swap_image.is_file() or args.swap_image.stat().st_size % SECTOR:
        raise SystemExit("invalid swap image")
    if not args.kernel.is_file():
        raise SystemExit(f"missing kernel: {args.kernel}")
    if not args.zedbsd_config.is_file():
        raise SystemExit(f"missing zedbsd.cfg: {args.zedbsd_config}")

    if variant in ("uefi", "hybrid"):
        if args.bootx64 is None or not args.bootx64.is_file():
            raise SystemExit(f"{variant} requires --bootx64")
    if variant in ("bios", "hybrid"):
        for req, name in ((args.stage1, "--stage1"),
                          (args.stage2, "--stage2"),
                          (args.partition_pbr, "--partition-pbr"),
                          (args.bootzbsd, "--bootzbsd")):
            if req is None or not req.is_file():
                raise SystemExit(f"{variant} requires {name}")

    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=args.output.name + ".",
                                     dir=args.output.parent)
    os.close(fd)
    temporary = Path(temp_name)

    try:
        root_blocks = args.ufs_root.stat().st_size // SECTOR
        swap_blocks = args.swap_image.stat().st_size // SECTOR
        boot_blocks = args.fat_size_mib * 2048

        if variant == "bios":
            boot_start = 2048
            root_start = align_up(boot_start + boot_blocks)
            swap_start = align_up(root_start + root_blocks)
            total_sectors = swap_start + swap_blocks

            with temporary.open("wb") as stream:
                stream.truncate(total_sectors * SECTOR)

                stage1 = bytearray(args.stage1.read_bytes())
                stage2 = args.stage2.read_bytes()
                if len(stage1) != 512 or stage1[510:512] != b"\x55\xaa":
                    raise SystemExit("invalid stage1")
                if len(stage2) % 512:
                    raise SystemExit("invalid stage2")

                sig = (zlib.crc32(stage2) ^
                       zlib.crc32(args.kernel.read_bytes())) & 0xFFFFFFFF
                if sig == 0:
                    sig = 1
                struct.pack_into("<I", stage1, 0x1B8, sig)

                # Partition 1: FAT32 BOOT (active)
                stage1[0x1BE:0x1CE] = struct.pack(
                    "<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x0C,
                    b"\xfe\xff\xff", boot_start, boot_blocks)
                # Partition 2: UFS ROOT
                stage1[0x1CE:0x1DE] = struct.pack(
                    "<B3sB3sII", 0x00, b"\xfe\xff\xff", 0xA5,
                    b"\xfe\xff\xff", root_start, root_blocks)
                # Partition 3: Swap
                stage1[0x1DE:0x1EE] = struct.pack(
                    "<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x82,
                    b"\xfe\xff\xff", swap_start, swap_blocks)
                stage1[0x1EE:0x1FE] = b"\x00" * 16
                stage1[510:512] = b"\x55\xaa"

                stream.seek(0)
                stream.write(stage1)
                stream.seek(1 * SECTOR)
                stream.write(stage2)

            copy_sparse_file(temporary.open("r+b"), root_start * SECTOR, args.ufs_root)
            copy_sparse_file(temporary.open("r+b"), swap_start * SECTOR, args.swap_image)

            # Format FAT32 BOOT partition
            boot_offset = boot_start * SECTOR
            format_fat(temporary, boot_offset, boot_start, boot_blocks, "BOOT")
            install_fat32_pbr(temporary, boot_offset, args.partition_pbr.read_bytes())
            copy_to_fat(temporary, boot_offset, args.bootzbsd, "/BOOTZBSD.EXE")
            copy_to_fat(temporary, boot_offset, args.kernel, "/vmunix")
            copy_to_fat(temporary, boot_offset, args.zedbsd_config, "/zedbsd.cfg")
            if args.logo is not None and args.logo.is_file():
                copy_to_fat(temporary, boot_offset, args.logo, "/logo.ppm")

        else:
            # UEFI or Hybrid
            esp_start = 2048
            esp_blocks = args.esp_size_mib * 2048
            boot_start = align_up(esp_start + esp_blocks)
            root_start = align_up(boot_start + boot_blocks)
            swap_start = align_up(root_start + root_blocks)
            total_sectors = align_up(swap_start + swap_blocks + 34)
            last_usable = total_sectors - 34

            disk_guid = uuid.uuid4()
            esp_part_guid = uuid.uuid4()
            boot_part_guid = uuid.uuid4()
            root_part_guid = uuid.uuid4()
            swap_part_guid = uuid.uuid4()
            bios_part_guid = uuid.uuid4()

            with temporary.open("wb") as stream:
                stream.truncate(total_sectors * SECTOR)

                if variant == "uefi":
                    # Pure protective MBR: only entry 1 is 0xEE, entries 2..4 are 0
                    mbr = bytearray(SECTOR)
                    mbr[0x1BE:0x1CE] = struct.pack(
                        "<B3sB3sII", 0x00, b"\x00\x02\x00", 0xEE,
                        b"\xfe\xff\xff", 1, min(total_sectors - 1, 0xFFFFFFFF))
                    mbr[510:512] = b"\x55\xaa"
                    stream.seek(0)
                    stream.write(mbr)
                else:
                    # Hybrid MBR: stage1 code + FAT32 (active), 0xEE, UFS, Swap
                    stage1 = bytearray(args.stage1.read_bytes())
                    stage2 = args.stage2.read_bytes()
                    if len(stage1) != 512 or stage1[510:512] != b"\x55\xaa":
                        raise SystemExit("invalid stage1")
                    sig = (zlib.crc32(stage2) ^
                           zlib.crc32(args.kernel.read_bytes())) & 0xFFFFFFFF
                    if sig == 0:
                        sig = 1
                    struct.pack_into("<I", stage1, 0x1B8, sig)
                    # Entry 1: FAT32 BOOT (active)
                    stage1[0x1BE:0x1CE] = struct.pack(
                        "<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x0C,
                        b"\xfe\xff\xff", boot_start, boot_blocks)
                    # Entry 2: Protective GPT
                    stage1[0x1CE:0x1DE] = struct.pack(
                        "<B3sB3sII", 0x00, b"\xfe\xff\xff", 0xEE,
                        b"\xfe\xff\xff", 1, min(total_sectors - 1, 0xFFFFFFFF))
                    # Entry 3: UFS ROOT
                    stage1[0x1DE:0x1EE] = struct.pack(
                        "<B3sB3sII", 0x00, b"\xfe\xff\xff", 0xA5,
                        b"\xfe\xff\xff", root_start, root_blocks)
                    # Entry 4: Swap
                    stage1[0x1EE:0x1FE] = struct.pack(
                        "<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x82,
                        b"\xfe\xff\xff", swap_start, swap_blocks)
                    stage1[510:512] = b"\x55\xaa"
                    stream.seek(0)
                    stream.write(stage1)
                    # Stage 2 at LBA 34
                    stream.seek(34 * SECTOR)
                    stream.write(stage2)

                # GPT entries
                entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
                entries[:GPT_ENTRY_SIZE] = gpt_entry(
                    ESP_TYPE, esp_part_guid, esp_start,
                    esp_start + esp_blocks - 1, "zedBSD EFI System")
                entries[GPT_ENTRY_SIZE:2 * GPT_ENTRY_SIZE] = gpt_entry(
                    BASIC_DATA_TYPE, boot_part_guid, boot_start,
                    boot_start + boot_blocks - 1, "BOOT")
                entries[2 * GPT_ENTRY_SIZE:3 * GPT_ENTRY_SIZE] = gpt_entry(
                    UFS_ROOT_TYPE, root_part_guid, root_start,
                    root_start + root_blocks - 1, "zedBSD-root")
                entries[3 * GPT_ENTRY_SIZE:4 * GPT_ENTRY_SIZE] = gpt_entry(
                    SWAP_TYPE, swap_part_guid, swap_start,
                    swap_start + swap_blocks - 1, "zedBSD-swap")
                if variant == "hybrid":
                    entries[4 * GPT_ENTRY_SIZE:5 * GPT_ENTRY_SIZE] = gpt_entry(
                        BIOS_BOOT_TYPE, bios_part_guid, 34,
                        esp_start - 1, "zedBSD BIOS loader")

                entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

                # Primary GPT header
                pri_header = gpt_header(1, total_sectors - 1, 2, 34,
                                        last_usable, disk_guid, entries_crc)
                stream.seek(1 * SECTOR)
                stream.write(pri_header)
                stream.seek(2 * SECTOR)
                stream.write(entries)

                # Backup GPT
                backup_entries_lba = total_sectors - 33
                bak_header = gpt_header(total_sectors - 1, 1,
                                        backup_entries_lba, 34, last_usable,
                                        disk_guid, entries_crc)
                stream.seek(backup_entries_lba * SECTOR)
                stream.write(entries)
                stream.seek((total_sectors - 1) * SECTOR)
                stream.write(bak_header)

            copy_sparse_file(temporary.open("r+b"), root_start * SECTOR, args.ufs_root)
            copy_sparse_file(temporary.open("r+b"), swap_start * SECTOR, args.swap_image)

            # Format ESP partition
            esp_offset = esp_start * SECTOR
            format_fat(temporary, esp_offset, esp_start, esp_blocks, "ESP")
            mkdir_fat(temporary, esp_offset, "/EFI")
            mkdir_fat(temporary, esp_offset, "/EFI/BOOT")
            copy_to_fat(temporary, esp_offset, args.bootx64, "/EFI/BOOT/BOOTX64.EFI")

            # Format FAT32 BOOT partition
            boot_offset = boot_start * SECTOR
            format_fat(temporary, boot_offset, boot_start, boot_blocks, "BOOT")
            if variant == "hybrid":
                install_fat32_pbr(temporary, boot_offset,
                                  args.partition_pbr.read_bytes())
                copy_to_fat(temporary, boot_offset, args.bootzbsd, "/BOOTZBSD.EXE")
            copy_to_fat(temporary, boot_offset, args.kernel, "/vmunix")
            copy_to_fat(temporary, boot_offset, args.zedbsd_config, "/zedbsd.cfg")
            if args.logo is not None and args.logo.is_file():
                copy_to_fat(temporary, boot_offset, args.logo, "/logo.ppm")

        # Run validation
        checker = Path(__file__).with_name("check-x86_64-hdd-image.py")
        check_args = [sys.executable, str(checker), "--variant", variant,
                      "--kernel", str(args.kernel),
                      "--zedbsd-config", str(args.zedbsd_config),
                      "--ufs-root", str(args.ufs_root),
                      "--swap-image", str(args.swap_image)]
        if args.bootx64 is not None:
            check_args += ["--bootx64", str(args.bootx64)]
        if args.stage1 is not None:
            check_args += ["--stage1", str(args.stage1)]
        if args.stage2 is not None:
            check_args += ["--stage2", str(args.stage2)]
        if args.partition_pbr is not None:
            check_args += ["--partition-pbr", str(args.partition_pbr)]
        if args.bootzbsd is not None:
            check_args += ["--bootzbsd", str(args.bootzbsd)]
        if args.logo is not None and args.logo.is_file():
            check_args += ["--logo", str(args.logo)]
        check_args.append(str(temporary))
        run(*check_args)

        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description="Create x86_64 HDD image")
    parser.add_argument("--variant", choices=("bios", "uefi", "hybrid", "native"),
                        default="uefi")
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--zedbsd-config", type=Path, required=True)
    parser.add_argument("--logo", type=Path)
    parser.add_argument("--ufs-root", type=Path, required=True)
    parser.add_argument("--swap-image", "--swapfile", dest="swap_image",
                        type=Path, required=True)
    parser.add_argument("--bootx64", type=Path)
    parser.add_argument("--stage1", type=Path)
    parser.add_argument("--stage2", type=Path)
    parser.add_argument("--partition-pbr", type=Path)
    parser.add_argument("--bootzbsd", type=Path)
    parser.add_argument("--fat-size-mib", "--boot-size-mib", dest="fat_size_mib",
                        type=int, default=256)
    parser.add_argument("--esp-size-mib", type=int, default=64)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()
