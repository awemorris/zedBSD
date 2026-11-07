#!/usr/bin/env python3
"""Validate an x86_64 disk image for BIOS, UEFI, or Hybrid boot."""
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
BASIC_DATA_TYPE = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
UFS_ROOT_TYPE = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b")
SWAP_TYPE = uuid.UUID("516e7cb5-6ecf-11d6-8ff8-00022d09712b")
BIOS_BOOT_TYPE = uuid.UUID("21686148-6449-6e6f-744e-656564454649")


def fail(message: str) -> None:
    raise SystemExit(f"check-x86_64-hdd-image: {message}")


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
        fail(f"/{name} on FAT partition differs from {source_path}")


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


def check_fat_serials_distinct(f, esp_offset: int, boot_offset: int) -> None:
    f.seek(esp_offset)
    esp_bpb = f.read(512)
    f.seek(boot_offset)
    boot_bpb = f.read(512)
    esp_fat32 = (struct.unpack_from("<H", esp_bpb, 22)[0] == 0)
    boot_fat32 = (struct.unpack_from("<H", boot_bpb, 22)[0] == 0)
    esp_serial = struct.unpack_from("<I", esp_bpb, 0x43 if esp_fat32 else 0x27)[0]
    boot_serial = struct.unpack_from("<I", boot_bpb, 0x43 if boot_fat32 else 0x27)[0]
    if esp_serial == boot_serial:
        fail(f"ESP and BOOT partitions have duplicate FAT volume serial 0x{esp_serial:08x}")


def check(args: argparse.Namespace) -> None:
    variant = "uefi" if args.variant == "native" else args.variant
    size = args.image.stat().st_size
    if size % SECTOR:
        fail("image size is not sector-aligned")
    total_sectors = size // SECTOR

    with args.image.open("rb") as f:
        mbr = f.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR boot signature 0x55AA")

        if variant == "bios":
            # Partition 1: active (0x80), type 0x0C, LBA 2048
            p1 = struct.unpack_from("<B3sB3sII", mbr, 0x1BE)
            if p1[0] != 0x80 or p1[2] != 0x0C or p1[4] != 2048:
                fail("BIOS MBR partition 1 is not active FAT32 at LBA 2048")
            boot_start, boot_blocks = p1[4], p1[5]

            # Partition 2: UFS (0xA5)
            p2 = struct.unpack_from("<B3sB3sII", mbr, 0x1CE)
            root_blocks = args.ufs_root.stat().st_size // SECTOR
            if p2[2] != 0xA5 or p2[5] != root_blocks:
                fail("BIOS MBR partition 2 is not UFS root")
            root_start = p2[4]

            # Partition 3: Swap (0x82)
            p3 = struct.unpack_from("<B3sB3sII", mbr, 0x1DE)
            swap_blocks = args.swap_image.stat().st_size // SECTOR
            if p3[2] != 0x82 or p3[5] != swap_blocks:
                fail("BIOS MBR partition 3 is not swap")
            swap_start = p3[4]

            # Partition 4 must be empty
            if any(mbr[0x1EE:0x1FE]):
                fail("BIOS MBR partition 4 is not empty")

            # Check stage2 at LBA 1
            f.seek(1 * SECTOR)
            stage2_hdr = f.read(16)
            magic, ver, hdr_sz, machine = struct.unpack_from("<IHHH", stage2_hdr, 0)
            if magic != 0x324C425A or ver != 1:
                fail("invalid ZBL2 header at LBA 1 for BIOS stage2")

            # Check UFS root & swap contents
            compare_extent(f, root_start * SECTOR, args.ufs_root)
            compare_extent(f, swap_start * SECTOR, args.swap_image)

            # Check FAT32 BOOT partition
            boot_offset = boot_start * SECTOR
            same_fat_file(args.image, boot_offset, "BOOTZBSD.EXE", args.bootzbsd)
            same_fat_file(args.image, boot_offset, "vmunix", args.kernel)
            same_fat_file(args.image, boot_offset, "zedbsd.cfg", args.zedbsd_config)
            if args.logo is not None and args.logo.is_file():
                same_fat_file(args.image, boot_offset, "logo.ppm", args.logo)

        elif variant == "uefi":
            # Pure protective MBR: entry 1 is 0xEE, entries 2..4 must be 0
            p1 = struct.unpack_from("<B3sB3sII", mbr, 0x1BE)
            if p1[2] != 0xEE or p1[4] != 1:
                fail("UEFI MBR entry 1 is not protective 0xEE")
            if any(mbr[0x1CE:0x1FE]):
                fail("UEFI MBR must NOT contain additional MBR partitions (Intel Mac requirement)")

            # Check primary GPT header & entries
            f.seek(1 * SECTOR)
            pri_hdr = f.read(SECTOR)
            entries_crc = check_gpt_header(pri_hdr, 1, total_sectors - 1, 2, total_sectors)

            f.seek(2 * SECTOR)
            entries = f.read(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
            if zlib.crc32(entries) & 0xFFFFFFFF != entries_crc:
                fail("primary GPT entries CRC mismatch")

            # Check backup GPT header & entries
            backup_entries_lba = total_sectors - 33
            f.seek(backup_entries_lba * SECTOR)
            bak_entries = f.read(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
            if bak_entries != entries:
                fail("backup GPT entries differ from primary")

            f.seek((total_sectors - 1) * SECTOR)
            bak_hdr = f.read(SECTOR)
            check_gpt_header(bak_hdr, total_sectors - 1, 1, backup_entries_lba, total_sectors)

            # Validate GPT partitions
            # Partition 1: ESP
            e1 = entries[:GPT_ENTRY_SIZE]
            t1 = uuid.UUID(bytes_le=e1[:16])
            s1, l1 = struct.unpack_from("<QQ", e1, 32)
            if t1 != ESP_TYPE:
                fail("GPT partition 1 is not ESP")

            # Partition 2: BOOT
            e2 = entries[GPT_ENTRY_SIZE:2 * GPT_ENTRY_SIZE]
            t2 = uuid.UUID(bytes_le=e2[:16])
            s2, l2 = struct.unpack_from("<QQ", e2, 32)
            name2 = e2[56:128].decode("utf-16le").rstrip("\0")
            if t2 != BASIC_DATA_TYPE or name2 != "BOOT":
                fail("GPT partition 2 is not Basic Data named 'BOOT'")

            # Partition 3: UFS ROOT
            e3 = entries[2 * GPT_ENTRY_SIZE:3 * GPT_ENTRY_SIZE]
            t3 = uuid.UUID(bytes_le=e3[:16])
            s3, l3 = struct.unpack_from("<QQ", e3, 32)
            name3 = e3[56:128].decode("utf-16le").rstrip("\0")
            if t3 != UFS_ROOT_TYPE or name3 != "zedBSD-root":
                fail("GPT partition 3 is not UFS root named 'zedBSD-root'")

            # Partition 4: Swap
            e4 = entries[3 * GPT_ENTRY_SIZE:4 * GPT_ENTRY_SIZE]
            t4 = uuid.UUID(bytes_le=e4[:16])
            s4, l4 = struct.unpack_from("<QQ", e4, 32)
            name4 = e4[56:128].decode("utf-16le").rstrip("\0")
            if t4 != SWAP_TYPE or name4 != "zedBSD-swap":
                fail("GPT partition 4 is not Swap named 'zedBSD-swap'")

            # Partitions 5..128 must be empty
            if any(entries[4 * GPT_ENTRY_SIZE:]):
                fail("unexpected extra GPT partitions in UEFI image")

            # Check contents
            compare_extent(f, s3 * SECTOR, args.ufs_root)
            compare_extent(f, s4 * SECTOR, args.swap_image)

            # Check ESP and BOOT FAT volume serials are distinct
            check_fat_serials_distinct(f, s1 * SECTOR, s2 * SECTOR)

            # Check ESP files
            same_fat_file(args.image, s1 * SECTOR, "EFI/BOOT/BOOTX64.EFI", args.bootx64)

            # Check BOOT files
            same_fat_file(args.image, s2 * SECTOR, "vmunix", args.kernel)
            same_fat_file(args.image, s2 * SECTOR, "zedbsd.cfg", args.zedbsd_config)
            if args.logo is not None and args.logo.is_file():
                same_fat_file(args.image, s2 * SECTOR, "logo.ppm", args.logo)

        elif variant == "hybrid":
            # Hybrid MBR:
            # Entry 1: FAT32 BOOT (active 0x80, type 0x0C)
            p1 = struct.unpack_from("<B3sB3sII", mbr, 0x1BE)
            if p1[0] != 0x80 or p1[2] != 0x0C:
                fail("Hybrid MBR entry 1 is not active FAT32 BOOT")
            # Entry 2: Protective GPT (0xEE, start 1)
            p2 = struct.unpack_from("<B3sB3sII", mbr, 0x1CE)
            if p2[2] != 0xEE or p2[4] != 1:
                fail("Hybrid MBR entry 2 is not protective 0xEE")
            # Entry 3: UFS (0xA5)
            p3 = struct.unpack_from("<B3sB3sII", mbr, 0x1DE)
            if p3[2] != 0xA5:
                fail("Hybrid MBR entry 3 is not UFS root")
            # Entry 4: Swap (0x82)
            p4 = struct.unpack_from("<B3sB3sII", mbr, 0x1EE)
            if p4[2] != 0x82:
                fail("Hybrid MBR entry 4 is not swap")

            # Check stage2 at LBA 34
            f.seek(34 * SECTOR)
            stage2_hdr = f.read(16)
            magic, ver, hdr_sz, machine = struct.unpack_from("<IHHH", stage2_hdr, 0)
            if magic != 0x324C425A or ver != 1:
                fail("invalid ZBL2 header at LBA 34 for hybrid stage2")

            # Check primary GPT header & entries
            f.seek(1 * SECTOR)
            pri_hdr = f.read(SECTOR)
            entries_crc = check_gpt_header(pri_hdr, 1, total_sectors - 1, 2, total_sectors)

            f.seek(2 * SECTOR)
            entries = f.read(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
            if zlib.crc32(entries) & 0xFFFFFFFF != entries_crc:
                fail("primary GPT entries CRC mismatch")

            # Check GPT partition 1: ESP
            e1 = entries[:GPT_ENTRY_SIZE]
            t1 = uuid.UUID(bytes_le=e1[:16])
            s1, l1 = struct.unpack_from("<QQ", e1, 32)
            if t1 != ESP_TYPE:
                fail("GPT partition 1 is not ESP")

            # Check GPT partition 2: BOOT (name "BOOT")
            e2 = entries[GPT_ENTRY_SIZE:2 * GPT_ENTRY_SIZE]
            t2 = uuid.UUID(bytes_le=e2[:16])
            s2, l2 = struct.unpack_from("<QQ", e2, 32)
            name2 = e2[56:128].decode("utf-16le").rstrip("\0")
            if t2 != BASIC_DATA_TYPE or name2 != "BOOT":
                fail("GPT partition 2 is not Basic Data named 'BOOT'")

            # Check GPT partition 3: UFS
            e3 = entries[2 * GPT_ENTRY_SIZE:3 * GPT_ENTRY_SIZE]
            t3 = uuid.UUID(bytes_le=e3[:16])
            s3, l3 = struct.unpack_from("<QQ", e3, 32)
            name3 = e3[56:128].decode("utf-16le").rstrip("\0")
            if t3 != UFS_ROOT_TYPE or name3 != "zedBSD-root":
                fail("GPT partition 3 is not UFS root named 'zedBSD-root'")

            # Check GPT partition 4: Swap
            e4 = entries[3 * GPT_ENTRY_SIZE:4 * GPT_ENTRY_SIZE]
            t4 = uuid.UUID(bytes_le=e4[:16])
            s4, l4 = struct.unpack_from("<QQ", e4, 32)
            name4 = e4[56:128].decode("utf-16le").rstrip("\0")
            if t4 != SWAP_TYPE or name4 != "zedBSD-swap":
                fail("GPT partition 4 is not Swap named 'zedBSD-swap'")

            # Check raw extents
            compare_extent(f, s3 * SECTOR, args.ufs_root)
            compare_extent(f, s4 * SECTOR, args.swap_image)

            # Check ESP and BOOT FAT volume serials are distinct
            check_fat_serials_distinct(f, s1 * SECTOR, s2 * SECTOR)

            # Check ESP files
            same_fat_file(args.image, s1 * SECTOR, "EFI/BOOT/BOOTX64.EFI", args.bootx64)

            # Check BOOT files (both BIOS & UEFI files)
            same_fat_file(args.image, s2 * SECTOR, "BOOTZBSD.EXE", args.bootzbsd)
            same_fat_file(args.image, s2 * SECTOR, "vmunix", args.kernel)
            same_fat_file(args.image, s2 * SECTOR, "zedbsd.cfg", args.zedbsd_config)
            if args.logo is not None and args.logo.is_file():
                same_fat_file(args.image, s2 * SECTOR, "logo.ppm", args.logo)

    print(f"check-x86_64-hdd-image: {args.image} ({variant}): PASS")


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate x86_64 HDD image")
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
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()
