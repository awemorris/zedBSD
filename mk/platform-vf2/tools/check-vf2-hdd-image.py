#!/usr/bin/env python3
"""Validate a VisionFive 2 (riscv64) UEFI GPT disk image and boot payloads."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import tempfile
import uuid
import zlib
from pathlib import Path

SECTOR = 512
GPT_HEADER_SIZE = 92
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128

ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
ROOT_TYPE = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b")
SWAP_TYPE = uuid.UUID("516e7cb5-6ecf-11d6-8ff8-00022d09712b")
SPL_TYPE = uuid.UUID("2e54b353-1271-4842-806f-e436d6af6985")
UBOOT_TYPE = uuid.UUID("bc13c2ff-59e6-4262-a352-b275fd6f7172")
RESERVED_TYPE = uuid.UUID("7a656442-5244-4b76-a172-726573657276")


def fail(message: str) -> None:
    raise SystemExit(f"check-vf2-hdd-image: {message}")


def extract_fat_file(image_path: Path, offset_bytes: int, name: str) -> bytes:
    with tempfile.TemporaryDirectory() as work:
        out = Path(work) / "extracted"
        subprocess.run(["mcopy", "-n", "-i", f"{image_path}@@{offset_bytes}",
                        f"::{name}", str(out)], check=True,
                       stdout=subprocess.DEVNULL)
        return out.read_bytes()


def same_fat_file(image_path: Path, offset_bytes: int, name: str,
                  source_path: Path) -> None:
    extracted = extract_fat_file(image_path, offset_bytes, name)
    if hashlib.sha256(extracted).digest() != hashlib.sha256(source_path.read_bytes()).digest():
        fail(f"/{name} on ESP partition differs from {source_path}")


def compare_extent(image_file, offset_bytes: int, source_path: Path) -> None:
    image_file.seek(offset_bytes)
    with source_path.open("rb") as src:
        while chunk := src.read(1024 * 1024):
            data = image_file.read(len(chunk))
            if data != chunk:
                fail(f"image extent at {offset_bytes} differs from {source_path}")


def check_gpt_header(raw: bytes, current_lba: int, backup_lba: int,
                     entries_lba: int, total_sectors: int) -> int:
    if raw[:8] != b"EFI PART":
        fail(f"invalid GPT signature at LBA {current_lba}")
    rev, size, stored_crc = struct.unpack_from("<III", raw, 8)
    if rev != 0x00010000 or size != GPT_HEADER_SIZE:
        fail(f"invalid GPT header revision or size at LBA {current_lba}")
    copy = bytearray(raw[:size])
    struct.pack_into("<I", copy, 16, 0)
    if zlib.crc32(copy) & 0xFFFFFFFF != stored_crc:
        fail(f"GPT header CRC mismatch at LBA {current_lba}")
    my_lba, alt_lba, first_u, last_u = struct.unpack_from("<QQQQ", raw, 24)
    if my_lba != current_lba or alt_lba != backup_lba:
        fail(f"GPT header links mismatch at LBA {current_lba}")
    ent_lba, count, entry_size, entries_crc = struct.unpack_from("<QIII", raw, 72)
    if ent_lba != entries_lba or count != GPT_ENTRY_COUNT or entry_size != GPT_ENTRY_SIZE:
        fail(f"GPT entry table geometry mismatch at LBA {current_lba}")
    return entries_crc


def check(args: argparse.Namespace) -> None:
    size = args.image.stat().st_size
    if size % SECTOR:
        fail("image size is not sector-aligned")
    total_sectors = size // SECTOR

    with args.image.open("rb") as f:
        mbr = f.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR boot signature 0x55AA")

        # Protective MBR: entry 1 is 0xEE, entries 2..4 must be 0
        p1 = struct.unpack_from("<B3sB3sII", mbr, 0x1BE)
        if p1[2] != 0xEE or p1[4] != 1:
            fail("MBR entry 1 is not protective 0xEE")
        if any(mbr[0x1CE:0x1FE]):
            fail("MBR must not contain additional partitions")

        # Primary GPT header & entries
        f.seek(1 * SECTOR)
        pri_hdr = f.read(SECTOR)
        entries_crc = check_gpt_header(pri_hdr, 1, total_sectors - 1, 2, total_sectors)

        f.seek(2 * SECTOR)
        entries = f.read(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
        if zlib.crc32(entries) & 0xFFFFFFFF != entries_crc:
            fail("primary GPT entries CRC mismatch")

        # Backup GPT header & entries
        backup_entries_lba = total_sectors - 1 - (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE // SECTOR)
        f.seek(backup_entries_lba * SECTOR)
        b_entries = f.read(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
        if b_entries != entries:
            fail("backup GPT entries do not match primary")

        f.seek((total_sectors - 1) * SECTOR)
        b_hdr = f.read(SECTOR)
        check_gpt_header(b_hdr, total_sectors - 1, 1, backup_entries_lba, total_sectors)

        # Parse partitions
        def get_entry(idx: int):
            off = idx * GPT_ENTRY_SIZE
            raw = entries[off:off + GPT_ENTRY_SIZE]
            type_guid = uuid.UUID(bytes_le=raw[:16])
            part_guid = uuid.UUID(bytes_le=raw[16:32])
            first_lba, last_lba, flags = struct.unpack_from("<QQQ", raw, 32)
            name = raw[56:128].decode("utf-16-le").rstrip("\0")
            return type_guid, part_guid, first_lba, last_lba, flags, name

        # Partitions:
        # 1: SPL / Reserved
        t1, _, start1, last1, _, _ = get_entry(0)
        if t1 not in (SPL_TYPE, RESERVED_TYPE) or start1 != 4096 or last1 != 8191:
            fail(f"partition 1 is not SPL/Reserved at LBA 4096..8191 (got type={t1} start={start1})")

        # 2: U-Boot / Reserved
        t2, _, start2, last2, _, _ = get_entry(1)
        if t2 not in (UBOOT_TYPE, RESERVED_TYPE) or start2 != 8192 or last2 != 16383:
            fail(f"partition 2 is not U-Boot/Reserved at LBA 8192..16383 (got type={t2} start={start2})")

        # 3: ESP
        t3, _, esp_start, esp_last, _, _ = get_entry(2)
        if t3 != ESP_TYPE or esp_start != 16384:
            fail(f"partition 3 is not ESP at LBA 16384 (got type={t3} start={esp_start})")

        # 4: UFS root
        t4, _, root_start, root_last, _, name4 = get_entry(3)
        if t4 != ROOT_TYPE or name4 != "zedBSD-root":
            fail(f"partition 4 is not UFS root (got type={t4} name={name4})")

        # Compare UFS root
        compare_extent(f, root_start * SECTOR, args.ufs_root)

        # 5: Swap
        if args.swap is not None:
            t5, _, swap_start, swap_last, _, name5 = get_entry(4)
            if t5 != SWAP_TYPE or name5 != "zedBSD-swap":
                fail(f"partition 5 is not Swap (got type={t5} name={name5})")
            compare_extent(f, swap_start * SECTOR, args.swap)

        # Check ESP files
        esp_offset = esp_start * SECTOR
        same_fat_file(args.image, esp_offset, "EFI/BOOT/BOOTRISCV64.EFI", args.efi)
        same_fat_file(args.image, esp_offset, "vmunix", args.kernel)
        same_fat_file(args.image, esp_offset, "zedbsd.cfg", args.config)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--efi", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--ufs-root", type=Path, required=True)
    parser.add_argument("--swap", type=Path)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())
    print(f"{parser.parse_args().image}: VisionFive 2 disk image OK")


if __name__ == "__main__":
    main()
