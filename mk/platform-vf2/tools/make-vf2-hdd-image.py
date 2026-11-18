#!/usr/bin/env python3
"""Build a VisionFive 2 (riscv64) UEFI GPT disk image.

Layout:
  Partition 1: SPL / Reserved (LBA 4096..8191)
  Partition 2: U-Boot / Reserved (LBA 8192..16383)
  Partition 3: EFI System Partition (FAT32, LBA 16384..)
               Holds EFI/BOOT/BOOTRISCV64.EFI, zedbsd.cfg, vmunix
  Partition 4: UFS Root (FreeBSD UFS type, PARTLABEL=zedBSD-root)
  Partition 5: Swap (FreeBSD Swap type, PARTLABEL=zedBSD-swap)
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import uuid
import zlib
from pathlib import Path

SECTOR = 512
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ENTRY_SECTORS = ENTRY_COUNT * ENTRY_SIZE // SECTOR

# VisionFive 2 firmware partitions
SPL_TYPE = uuid.UUID("2e54b353-1271-4842-806f-e436d6af6985")
SPL_LBA = 4096
SPL_LAST = 8191
UBOOT_TYPE = uuid.UUID("bc13c2ff-59e6-4262-a352-b275fd6f7172")
UBOOT_LBA = 8192
UBOOT_LAST = 16383
FIRMWARE_END_LBA = 16384  # 8 MiB

# GPT types
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
ROOT_TYPE = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b")
SWAP_TYPE = uuid.UUID("516e7cb5-6ecf-11d6-8ff8-00022d09712b")
RESERVED_TYPE = uuid.UUID("7a656442-5244-4b76-a172-726573657276")

ROOT_LABEL = "zedBSD-root"
SWAP_LABEL = "zedBSD-swap"

# Fixed UUIDs for reproducibility
DISK_GUID = uuid.UUID("7a656442-5244-4b76-a172-697363763634")
SPL_GUID = uuid.UUID("7a656442-5244-4b76-a172-697363763631")
UBOOT_GUID = uuid.UUID("7a656442-5244-4b76-a172-697363763632")
ESP_GUID = uuid.UUID("7a656442-5244-4b76-a172-697363763645")
ROOT_GUID = uuid.UUID("7a656442-5244-4b76-a172-697363763652")
SWAP_GUID = uuid.UUID("7a656442-5244-4b76-a172-697363763653")
ESP_SERIAL = "5a454253"


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def mib_up(value: int) -> int:
    mib = 1024 * 1024
    return (value + mib - 1) // mib * mib


def entry_bytes(kind: uuid.UUID, unique: uuid.UUID, first: int, last: int,
                name: str) -> bytes:
    label = name.encode("utf-16-le").ljust(72, b"\0")
    return struct.pack("<16s16sQQQ72s", kind.bytes_le, unique.bytes_le,
                       first, last, 0, label)


def header_bytes(current: int, backup: int, first_usable: int, last_usable: int,
                 entries_lba: int, entries_crc: int) -> bytes:
    def pack(crc: int) -> bytes:
        return struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92,
                           crc, 0, current, backup, first_usable, last_usable,
                           DISK_GUID.bytes_le, entries_lba, ENTRY_COUNT,
                           ENTRY_SIZE, entries_crc)

    body = pack(0)
    return pack(zlib.crc32(body) & 0xFFFFFFFF).ljust(SECTOR, b"\0")


def copy_into(image, path: Path, lba: int) -> None:
    image.seek(lba * SECTOR)
    with path.open("rb") as payload:
        while chunk := payload.read(1024 * 1024):
            image.write(chunk)


def build(args: argparse.Namespace) -> None:
    files = [
        ("EFI/BOOT/BOOTRISCV64.EFI", args.efi),
        ("zedbsd.cfg", args.config),
        ("vmunix", args.kernel),
    ]
    for extra in args.esp_file:
        dest, sep, src = extra.partition("=")
        if not sep or not dest or dest.startswith("/") or ".." in dest.split("/"):
            raise SystemExit(f"bad --esp-file: {extra}")
        files.append((dest, Path(src)))

    for _, path in files:
        if not path.is_file():
            raise SystemExit(f"missing input file: {path}")

    if not args.ufs_root.is_file():
        raise SystemExit(f"missing UFS root: {args.ufs_root}")
    if args.ufs_root.stat().st_size == 0 or args.ufs_root.stat().st_size % SECTOR:
        raise SystemExit("UFS root size is not sector-aligned")

    if args.swap is not None:
        if not args.swap.is_file():
            raise SystemExit(f"missing swap image: {args.swap}")
        if args.swap.stat().st_size == 0 or args.swap.stat().st_size % SECTOR:
            raise SystemExit("swap image size is not sector-aligned")

    firmware = []
    if args.spl is not None and args.uboot_itb is not None:
        firmware = [
            (SPL_TYPE, SPL_GUID, SPL_LBA, SPL_LAST, "spl", args.spl),
            (UBOOT_TYPE, UBOOT_GUID, UBOOT_LBA, UBOOT_LAST, "uboot", args.uboot_itb),
        ]
        for _, _, first, last, _, path in firmware:
            if not path.is_file():
                raise SystemExit(f"missing firmware input: {path}")
            if path.stat().st_size > (last - first + 1) * SECTOR:
                raise SystemExit(f"firmware too large for partition: {path}")
    else:
        # Default reserved firmware partitions for SPI flash boot
        firmware = [
            (RESERVED_TYPE, SPL_GUID, SPL_LBA, SPL_LAST, "reserved", None),
            (RESERVED_TYPE, UBOOT_GUID, UBOOT_LBA, UBOOT_LAST, "reserved", None),
        ]

    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    payload = sum(path.stat().st_size for _, path in files)
    esp_bytes = max(64 * 1024 * 1024, mib_up(payload + payload // 8 + 16 * 1024 * 1024))
    esp_sectors = esp_bytes // SECTOR
    esp_lba = FIRMWARE_END_LBA
    esp_last = esp_lba + esp_sectors - 1

    partitions = [(kind, unique, first, last, name)
                  for kind, unique, first, last, name, _ in firmware]
    partitions.append((ESP_TYPE, ESP_GUID, esp_lba, esp_last, "EFI system"))

    root_lba = esp_last + 1
    root_sectors = mib_up(args.ufs_root.stat().st_size) // SECTOR
    partitions.append((ROOT_TYPE, ROOT_GUID, root_lba, root_lba + root_sectors - 1, ROOT_LABEL))

    next_lba = root_lba + root_sectors
    swap_lba = None
    if args.swap is not None:
        swap_lba = next_lba
        swap_sectors = mib_up(args.swap.stat().st_size) // SECTOR
        partitions.append((SWAP_TYPE, SWAP_GUID, swap_lba, swap_lba + swap_sectors - 1, SWAP_LABEL))
        next_lba = swap_lba + swap_sectors

    total = next_lba + 2048
    backup_header = total - 1
    backup_entries = backup_header - ENTRY_SECTORS
    first_usable = 2 + ENTRY_SECTORS
    last_usable = backup_entries - 1

    entries = bytearray(ENTRY_COUNT * ENTRY_SIZE)
    for index, (kind, unique, first, last, name) in enumerate(partitions):
        entries[index * ENTRY_SIZE:(index + 1) * ENTRY_SIZE] = \
            entry_bytes(kind, unique, first, last, name)
    entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF

    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_name(args.output.name + ".partial")
    with partial.open("wb") as image:
        image.truncate(total * SECTOR)
        # Protective MBR
        mbr = bytearray(SECTOR)
        mbr[0x1BE:0x1CE] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE,
                                       b"\xff\xff\xff", 1, min(total - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xaa"
        image.write(mbr)

        # Primary GPT
        image.write(header_bytes(1, backup_header, first_usable, last_usable, 2, entries_crc))
        image.write(entries)

        # Backup GPT
        image.seek(backup_entries * SECTOR)
        image.write(entries)
        image.write(header_bytes(backup_header, 1, first_usable, last_usable,
                                 backup_entries, entries_crc))

        # Copy firmware if present
        for _, _, first, _, _, path in firmware:
            if path is not None:
                copy_into(image, path, first)

        # Copy UFS root
        copy_into(image, args.ufs_root, root_lba)

        # Copy swap if present
        if swap_lba is not None:
            copy_into(image, args.swap, swap_lba)

    # Format ESP FAT32
    spec = f"{partial}@@{esp_lba * SECTOR}"
    run("mformat", "-i", spec, "-F", "-T", str(esp_sectors), "-H", str(esp_lba),
        "-h", "64", "-s", "32", "-N", ESP_SERIAL, "-v", "ZEDBSD", "::")

    directories: list[str] = []
    for destination, _ in files:
        parts = destination.split("/")[:-1]
        for depth in range(1, len(parts) + 1):
            d = "/".join(parts[:depth])
            if d not in directories:
                directories.append(d)

    run("mmd", "-i", spec, *[f"::/{d}" for d in directories])
    for destination, source in files:
        run("mcopy", "-i", spec, str(source), f"::/{destination}")

    os.replace(partial, args.output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--efi", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--ufs-root", type=Path, required=True)
    parser.add_argument("--swap", type=Path)
    parser.add_argument("--spl", type=Path)
    parser.add_argument("--uboot-itb", type=Path)
    parser.add_argument("--reserve-firmware", action="store_true")
    parser.add_argument("--esp-file", action="append", default=[])
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    build(parser.parse_args())


if __name__ == "__main__":
    main()
