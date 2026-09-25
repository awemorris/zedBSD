#!/usr/bin/env python3
"""Check an amd64 UEFI native disk image (WS062).

The image is a GPT disk with three partitions: the EFI system partition
holding the UEFI loader, the kernel and zedbsd.cfg; the UFS root partition
labelled zedBSD-root; and the swap partition labelled zedBSD-swap.  The
check reads the partition table and compares every partition's contents
with the inputs the image was built from.  It reads the image in pieces, so
a root of any size is checked in bounded memory.
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

SECTOR = 512
CHUNK = 1024 * 1024
ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
ROOT_TYPE = bytes.fromhex("b67c6e51cf6ed6118ff800022d09712b")
SWAP_TYPE = bytes.fromhex("b57c6e51cf6ed6118ff800022d09712b")
EXPECTED = (
    (ESP_TYPE, "zedBSD EFI System"),
    (ROOT_TYPE, "zedBSD-root"),
    (SWAP_TYPE, "zedBSD-swap"),
)


def fail(message: str) -> None:
    """Reports a failed check and stops."""
    print(f"check-amd64-native-image: {message}", file=sys.stderr)
    sys.exit(1)


def read_at(image, offset: int, length: int) -> bytes:
    """Reads exactly length bytes at an offset of the image."""
    image.seek(offset)
    data = image.read(length)
    if len(data) != length:
        fail(f"short read at {offset}")
    return data


def check_header(image, lba: int, expected_backup: int,
                 expected_entries: int) -> tuple[bytes, int, int]:
    """Checks one GPT header and its entry array; returns the array."""
    header = read_at(image, lba * SECTOR, SECTOR)
    if header[:8] != b"EFI PART":
        fail(f"no GPT header at LBA {lba}")
    size = struct.unpack_from("<I", header, 12)[0]
    stored = struct.unpack_from("<I", header, 16)[0]
    copy = bytearray(header[:size])
    struct.pack_into("<I", copy, 16, 0)
    if zlib.crc32(bytes(copy)) != stored:
        fail(f"GPT header CRC mismatch at LBA {lba}")
    current, backup, first, last = struct.unpack_from("<QQQQ", header, 24)
    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    count, entry_size, entries_crc = struct.unpack_from("<III", header, 80)
    if current != lba or backup != expected_backup:
        fail(f"GPT header at LBA {lba} names the wrong copies")
    if entries_lba != expected_entries or count != 128 or entry_size != 128:
        fail(f"GPT header at LBA {lba} describes an unexpected entry array")
    entries = read_at(image, entries_lba * SECTOR, count * entry_size)
    if zlib.crc32(entries) != entries_crc:
        fail(f"GPT entry array CRC mismatch for the header at LBA {lba}")
    return entries, first, last


def compare_file(image, offset: int, path: Path, length: int) -> None:
    """Compares a partition's bytes with the input file it was made from."""
    if path.stat().st_size != length:
        fail(f"partition size differs from {path}")
    with path.open("rb") as source:
        done = 0
        while done < length:
            want = min(CHUNK, length - done)
            if read_at(image, offset + done, want) != source.read(want):
                fail(f"partition contents differ from {path} at {done}")
            done += want


def compare_esp_file(image_path: Path, offset: int, name: str,
                     path: Path) -> None:
    """Compares a file on the ESP with its input, through mtools."""
    result = subprocess.run(
        ["mcopy", "-n", "-i", f"{image_path}@@{offset}", f"::{name}", "-"],
        check=False, capture_output=True)
    if result.returncode != 0:
        fail(f"ESP has no {name}")
    if result.stdout != path.read_bytes():
        fail(f"ESP {name} differs from {path}")


def main() -> None:
    """Checks the image named on the command line."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--bootx64", type=Path, required=True)
    parser.add_argument("--zedbsd-config", type=Path, required=True)
    parser.add_argument("--ufs-root", type=Path, required=True)
    parser.add_argument("--swap", type=Path, required=True)
    parser.add_argument("image", type=Path)
    arguments = parser.parse_args()

    size = arguments.image.stat().st_size
    if size % SECTOR != 0:
        fail("image is not whole sectors")
    last_lba = size // SECTOR - 1

    with arguments.image.open("rb") as image:
        # The protective MBR covers the disk with one 0xEE partition.
        mbr = read_at(image, 0, SECTOR)
        if mbr[510:512] != b"\x55\xaa" or mbr[0x1be + 4] != 0xEE:
            fail("no protective MBR")

        # Both GPT copies must be valid and describe the same partitions.
        primary, first, last = check_header(image, 1, last_lba, 2)
        backup, _, _ = check_header(image, last_lba, 1, last_lba - 32)
        if primary != backup:
            fail("primary and backup GPT entry arrays differ")

        # Exactly the three expected partitions, in order, inside the usable range.
        extents = []
        for index in range(128):
            entry = primary[index * 128:(index + 1) * 128]
            if index >= len(EXPECTED):
                if entry[:16] != bytes(16):
                    fail(f"unexpected partition {index + 1}")
                continue
            kind, label = EXPECTED[index]
            name = entry[56:128].decode("utf-16-le").rstrip("\0")
            start, end = struct.unpack_from("<QQ", entry, 32)
            if entry[:16] != kind or name != label:
                fail(f"partition {index + 1} is not {label}")
            if start < first or end > last or start > end:
                fail(f"partition {index + 1} lies outside the usable range")
            if extents and start <= extents[-1][1]:
                fail(f"partition {index + 1} overlaps the one before it")
            extents.append((start, end))

        # The root and swap partitions hold their inputs exactly.
        root_start, root_end = extents[1]
        compare_file(image, root_start * SECTOR, arguments.ufs_root,
                     (root_end - root_start + 1) * SECTOR)
        swap_start, swap_end = extents[2]
        compare_file(image, swap_start * SECTOR, arguments.swap,
                     (swap_end - swap_start + 1) * SECTOR)

    # The ESP holds the loader, the kernel and the configuration.
    esp_offset = extents[0][0] * SECTOR
    compare_esp_file(arguments.image, esp_offset, "/EFI/BOOT/BOOTX64.EFI",
                     arguments.bootx64)
    compare_esp_file(arguments.image, esp_offset, "/vmunix", arguments.kernel)
    compare_esp_file(arguments.image, esp_offset, "/zedbsd.cfg",
                     arguments.zedbsd_config)
    print(f"check-amd64-native-image: {arguments.image}: OK")


if __name__ == "__main__":
    main()
