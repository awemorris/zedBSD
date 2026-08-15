#!/usr/bin/env python3
"""Build the dedicated deterministic zedBSD X68000 SCSI disk image."""

import argparse
import binascii
import json
import os
import struct
import tempfile
from pathlib import Path

SECTOR = 512
IMAGE_SECTORS = 131072          # 64 MiB
ROOT_LBA = 4096                # 2 MiB
MANIFEST_LBA = 8
STAGE2_LBA = 9
STAGE2_LOAD = 0x00020000
MANIFEST_MAGIC = 0x5A36384D    # Z68M
MANIFEST_VERSION = 1
MANIFEST_SIZE = 64
RAM_BYTES = 12 * 1024 * 1024


def require(path: Path) -> bytes:
    if not path.is_file():
        raise SystemExit(f"missing input: {path}")
    return path.read_bytes()


def align(value: int, amount: int) -> int:
    return (value + amount - 1) // amount * amount


def fat16_partition(sectors: int, hidden: int) -> bytes:
    sectors_per_cluster = 4
    reserved = 1
    fats = 2
    root_entries = 512
    root_sectors = root_entries * 32 // SECTOR
    sectors_per_fat = 1
    while True:
        data_sectors = sectors - reserved - fats * sectors_per_fat - root_sectors
        clusters = data_sectors // sectors_per_cluster
        required = (clusters + 2) * 2
        updated = align(required, SECTOR) // SECTOR
        if updated == sectors_per_fat:
            break
        sectors_per_fat = updated
    if not 4085 <= clusters < 65525:
        raise SystemExit(f"root extent is not FAT16 ({clusters} clusters)")

    image = bytearray(sectors * SECTOR)
    boot = memoryview(image)[:SECTOR]
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"ZEDX68K "
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = sectors_per_cluster
    struct.pack_into("<H", boot, 14, reserved)
    boot[16] = fats
    struct.pack_into("<H", boot, 17, root_entries)
    struct.pack_into("<H", boot, 19, sectors if sectors < 65536 else 0)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, sectors_per_fat)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 16)
    struct.pack_into("<I", boot, 28, hidden)
    struct.pack_into("<I", boot, 32, sectors if sectors >= 65536 else 0)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x5A683001)
    boot[43:54] = b"ZEDX68K    "
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xaa"

    fat_bytes = sectors_per_fat * SECTOR
    first_fat = reserved * SECTOR
    for index in range(fats):
        base = first_fat + index * fat_bytes
        image[base:base + 4] = b"\xf8\xff\xff\xff"
    root = (reserved + fats * sectors_per_fat) * SECTOR
    image[root:root + 11] = b"ZEDX68K    "
    image[root + 11] = 0x08
    return bytes(image)


def disk_mark(total_sectors: int) -> bytes:
    mark = bytearray(1024)
    mark[:8] = b"X68SCSI1"
    mark[8] = 2
    mark[9] = 0
    struct.pack_into(">I", mark, 10, total_sectors)
    mark[14] = 1
    mark[15] = 0
    text = b"zedBSD/x68k SCSI Primary Boot."
    mark[16:16 + len(text)] = text
    return bytes(mark)


def partition_table(total_blocks: int, root_blocks: int) -> bytes:
    table = bytearray(2048)
    table[0:8] = b"X68K\0\0\0 "
    struct.pack_into(">I", table, 8, total_blocks)
    struct.pack_into(">I", table, 12, total_blocks)
    root = 16
    table[root:root + 8] = b"ZEDBSD  "
    root_start = ROOT_LBA // 2
    table[root + 8] = 0             # bootable
    table[root + 9:root + 12] = root_start.to_bytes(3, "big")
    struct.pack_into(">I", table, root + 12, root_blocks)
    for index in range(2, 8):
        table[index * 16 + 8] = 1   # unused
    return bytes(table)


def create(args: argparse.Namespace) -> None:
    stage1 = require(args.stage1)
    stage2 = require(args.stage2)
    kernel = require(args.kernel)
    if len(stage1) > 1024:
        raise SystemExit(f"stage 1 is {len(stage1)} bytes, maximum is 1024")
    if not stage2 or len(stage2) > 0x20000:
        raise SystemExit("stage 2 is empty or exceeds its 128 KiB RAM window")
    if len(kernel) < 52 or kernel[:7] != b"\x7fELF\x01\x02\x01" or \
            struct.unpack_from(">H", kernel, 18)[0] != 4:
        raise SystemExit("kernel is not current big-endian ELF32/EM_68K")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    stage2_sectors = align(len(stage2), SECTOR) // SECTOR
    kernel_lba = align(STAGE2_LBA + stage2_sectors, 8)
    kernel_sectors = align(len(kernel), SECTOR) // SECTOR
    if kernel_lba + kernel_sectors > ROOT_LBA:
        raise SystemExit("raw stage 2/kernel extents overlap the root partition")
    root_sectors = IMAGE_SECTORS - ROOT_LBA
    root_blocks = root_sectors // 2
    manifest = struct.pack(
        ">IHH14I", MANIFEST_MAGIC, MANIFEST_VERSION, MANIFEST_SIZE,
        STAGE2_LBA, len(stage2), STAGE2_LOAD, STAGE2_LOAD,
        binascii.crc32(stage2) & 0xFFFFFFFF,
        kernel_lba, len(kernel), binascii.crc32(kernel) & 0xFFFFFFFF,
        ROOT_LBA, root_sectors, args.ram_bytes, 0, 0, 0,
    )
    assert len(manifest) == MANIFEST_SIZE

    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=args.output.name + ".", dir=args.output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("w+b") as stream:
            stream.truncate(IMAGE_SECTORS * SECTOR)
            stream.seek(0)
            stream.write(disk_mark(IMAGE_SECTORS))
            stream.seek(1024)
            stream.write(stage1)
            stream.seek(2048)
            stream.write(partition_table(IMAGE_SECTORS // 2, root_blocks))
            stream.seek(MANIFEST_LBA * SECTOR)
            stream.write(manifest)
            stream.seek(STAGE2_LBA * SECTOR)
            stream.write(stage2)
            stream.seek(kernel_lba * SECTOR)
            stream.write(kernel)
            stream.seek(ROOT_LBA * SECTOR)
            stream.write(fat16_partition(root_sectors, ROOT_LBA))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()

    metadata = {
        "format": "zedbsd-x68k-v1",
        "image_sectors": IMAGE_SECTORS,
        "stage1_bytes": len(stage1),
        "stage2_lba": STAGE2_LBA,
        "stage2_bytes": len(stage2),
        "stage2_crc32": f"{binascii.crc32(stage2) & 0xffffffff:08x}",
        "kernel_lba": kernel_lba,
        "kernel_bytes": len(kernel),
        "kernel_crc32": f"{binascii.crc32(kernel) & 0xffffffff:08x}",
        "root_lba": ROOT_LBA,
        "root_sectors": root_sectors,
        "ram_bytes": args.ram_bytes,
    }
    if args.manifest_json:
        args.manifest_json.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_json.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    if args.manifest_bin:
        args.manifest_bin.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_bin.write_bytes(manifest)
    print(f"X68k image: {args.output} ({IMAGE_SECTORS * SECTOR} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", required=True, type=Path)
    parser.add_argument("--stage2", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--manifest-json", type=Path)
    parser.add_argument("--manifest-bin", type=Path,
                        help="also write the 64-byte boot manifest")
    parser.add_argument("--ram-bytes", type=int, default=RAM_BYTES)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()
