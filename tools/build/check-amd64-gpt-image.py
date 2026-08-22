#!/usr/bin/env python3
"""Validate the standalone amd64 hybrid-MBR/GPT BIOS+UEFI image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

SECTOR = 512
ESP_LBA = 2048
STAGE2_LBA = 34


def fail(message: str) -> None:
    raise SystemExit("amd64 GPT image check: " + message)


def extracted_hash(image: Path, name: str) -> bytes:
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / name.replace("/", "-")
        subprocess.run(["mcopy", "-n", "-i", f"{image}@@{ESP_LBA * SECTOR}",
                        f"::{name}", str(output)], check=True,
                       stdout=subprocess.DEVNULL)
        return hashlib.sha256(output.read_bytes()).digest()


def check_header(raw: bytes, current: int, backup: int, entries_lba: int,
                 total: int) -> tuple[int, int, int]:
    if raw[:8] != b"EFI PART" or struct.unpack_from("<I", raw, 8)[0] != 0x10000:
        fail(f"invalid GPT header at LBA {current}")
    size, stored_crc = struct.unpack_from("<II", raw, 12)
    copy = bytearray(raw[:size])
    struct.pack_into("<I", copy, 16, 0)
    if size != 92 or zlib.crc32(copy) & 0xffffffff != stored_crc:
        fail(f"GPT header CRC mismatch at LBA {current}")
    values = struct.unpack_from("<QQQQ", raw, 24)
    if values[0] != current or values[1] != backup:
        fail(f"GPT header links are invalid at LBA {current}")
    table_lba, count, entry_size, table_crc = struct.unpack_from("<QIII", raw, 72)
    if table_lba != entries_lba or count != 128 or entry_size != 128:
        fail(f"GPT entry geometry is invalid at LBA {current}")
    return count, entry_size, table_crc


def check(args: argparse.Namespace) -> None:
    size = args.image.stat().st_size
    if size == 0 or size % SECTOR:
        fail("image size is not sector aligned")
    total = size // SECTOR
    with args.image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR signature")
        first = struct.unpack_from("<B3sB3sII", mbr, 0x1be)
        second = struct.unpack_from("<B3sB3sII", mbr, 0x1ce)
        if first[0] != 0x80 or first[2] != 0x0e or first[4] != ESP_LBA:
            fail("first hybrid-MBR entry is not the active FAT16 ESP")
        if second[2] != 0xee or second[4] != 1:
            fail("missing protective-MBR entry")
        stream.seek(SECTOR)
        primary = stream.read(SECTOR)
        count, entry_size, table_crc = check_header(
            primary, 1, total - 1, 2, total)
        stream.seek(2 * SECTOR)
        entries = stream.read(count * entry_size)
        if zlib.crc32(entries) & 0xffffffff != table_crc:
            fail("primary GPT entry-array CRC mismatch")
        first_lba, last_lba = struct.unpack_from("<QQ", entries, 32)
        if first_lba != ESP_LBA or last_lba + 1 != ESP_LBA + first[5]:
            fail("GPT ESP and hybrid-MBR FAT extents differ")
        bios_first, bios_last = struct.unpack_from("<QQ", entries, 128 + 32)
        if bios_first != STAGE2_LBA or bios_last != ESP_LBA - 1:
            fail("BIOS Boot partition has the wrong extent")
        stream.seek(STAGE2_LBA * SECTOR)
        stage2 = stream.read(SECTOR)
        magic, version, header_size = struct.unpack_from("<IHH", stage2)
        sectors, machine = struct.unpack_from("<HH", stage2, 12)
        if (magic, version, header_size, machine) != (0x324c425a, 1, 32, 1):
            fail("invalid BIOS ZBL2 header at LBA 34")
        stream.seek(STAGE2_LBA * SECTOR)
        body = stream.read(sectors * SECTOR)
        if sum(struct.unpack_from("<I", body, n)[0]
               for n in range(0, len(body), 4)) & 0xffffffff:
            fail("BIOS ZBL2 checksum mismatch")
        backup_entries_lba = total - 33
        stream.seek(backup_entries_lba * SECTOR)
        if stream.read(len(entries)) != entries:
            fail("backup GPT entry array differs")
        stream.seek((total - 1) * SECTOR)
        check_header(stream.read(SECTOR), total - 1, 1,
                     backup_entries_lba, total)
    for name, source in (("BOOTZBSD.EXE", args.bootzbsd),
                         ("VMUNIX", args.kernel),
                         ("VMUNIX.X64", args.kernel),
                         ("EFI/BOOT/BOOTX64.EFI", args.bootx64),
                         ("rootfs.img", args.arch_image),
                         ("data.img", args.data_image),
                         ("swapfile", args.swapfile)):
        if source is not None and extracted_hash(args.image, name) != \
                hashlib.sha256(source.read_bytes()).digest():
            fail(f"{name} differs from its input")
    print("amd64 GPT image check: PASS")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=("pcat",), required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--bootzbsd", type=Path, required=True)
    parser.add_argument("--fat-type", choices=("auto", "fat12", "fat16", "fat32"),
                        default="auto")
    parser.add_argument("--bootx64", type=Path, required=True)
    parser.add_argument("--arch-profile")
    parser.add_argument("--arch-image", type=Path)
    parser.add_argument("--arch-format")
    parser.add_argument("--data-image", type=Path)
    parser.add_argument("--swapfile", type=Path)
    parser.add_argument("--noct")
    parser.add_argument("--holoris")
    parser.add_argument("--bin-file", action="append", default=[])
    parser.add_argument("--ufs-root")
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()
