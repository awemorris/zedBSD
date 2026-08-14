#!/usr/bin/env python3
"""Validate a zedBSD SPARC V9 Sun disklabel/FAT16 image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
IMAGE_SECTORS = 128 * 2048
HEADS = 64
SECTORS_PER_TRACK = 64
SECTORS_PER_CYLINDER = HEADS * SECTORS_PER_TRACK
FAT_LBA = SECTORS_PER_CYLINDER
STAGE1_LBA = 1
STAGE1_LIMIT = 15 * SECTOR_SIZE
STAGE1_AOUT_SIZE = 32 + STAGE1_LIMIT
STAGE2_HEADER_LBA = 32
STAGE2_PAYLOAD_LBA = 33
STAGE2_MAGIC = 0x5A533932
SUN_LABEL_MAGIC = 0xDABE


def fail(message: str) -> None:
    raise SystemExit("SPARC V9 image check: " + message)


def extract(image: Path, name: str) -> bytes:
    with tempfile.TemporaryDirectory(prefix="zedbsd-sparcv9-check-") as work:
        output = Path(work) / "payload"
        subprocess.run(
            [
                "mcopy", "-n", "-i",
                f"{image}@@{FAT_LBA * SECTOR_SIZE}",
                f"::{name}", str(output),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return output.read_bytes()


def same_file(image: Path, name: str, source: Path) -> None:
    actual = hashlib.sha256(extract(image, name)).digest()
    expected = hashlib.sha256(source.read_bytes()).digest()
    if actual != expected:
        fail(f"/{name} differs from {source}")


def check_label(label: bytes) -> None:
    if len(label) != SECTOR_SIZE:
        fail("short Sun disklabel")
    if struct.unpack_from(">H", label, 508)[0] != SUN_LABEL_MAGIC:
        fail("bad Sun disklabel magic")
    checksum = 0
    for offset in range(0, SECTOR_SIZE, 2):
        checksum ^= struct.unpack_from(">H", label, offset)[0]
    if checksum:
        fail("bad Sun disklabel checksum")
    ntracks = struct.unpack_from(">H", label, 436)[0]
    nsectors = struct.unpack_from(">H", label, 438)[0]
    if (ntracks, nsectors) != (HEADS, SECTORS_PER_TRACK):
        fail("unexpected Sun geometry")
    partitions = [
        struct.unpack_from(">II", label, 444 + index * 8)
        for index in range(8)
    ]
    expected = [
        (0, SECTORS_PER_CYLINDER),
        (1, IMAGE_SECTORS - FAT_LBA),
        (0, IMAGE_SECTORS),
    ]
    if partitions[:3] != expected or any(
            start or size for start, size in partitions[3:]):
        fail("unexpected Sun slices")


def check(args: argparse.Namespace) -> None:
    if args.image.stat().st_size != IMAGE_SECTORS * SECTOR_SIZE:
        fail("unexpected image size")
    stage1 = args.stage1.read_bytes()
    stage2 = args.stage2.read_bytes()
    if not stage1 or len(stage1) > STAGE1_LIMIT:
        fail("invalid stage1")

    with args.image.open("rb") as image:
        check_label(image.read(SECTOR_SIZE))
        image.seek(STAGE1_LBA * SECTOR_SIZE)
        wrapper = image.read(STAGE1_AOUT_SIZE)
        fields = struct.unpack_from(">BBHIIIIIII", wrapper)
        if fields != (0, 3, 0o407, len(stage1), 0, 0, 0,
                      0x4000, 0, 0):
            fail("invalid stage1 OpenBIOS a.out header")
        if wrapper[32:32 + len(stage1)] != stage1:
            fail("stage1 a.out payload differs from input")
        if any(wrapper[32 + len(stage1):]):
            fail("stage1 a.out load padding is not zero")
        image.seek(STAGE2_HEADER_LBA * SECTOR_SIZE)
        header = image.read(SECTOR_SIZE)
        fields = struct.unpack_from(">IHHIIQII", header)
        if fields[:4] != (STAGE2_MAGIC, 1, 32, STAGE2_PAYLOAD_LBA):
            fail("invalid stage2 header")
        if fields[4] != len(stage2) or fields[5:7] != (0x00100000, 0):
            fail("invalid stage2 range")
        if fields[7] != sum(stage2) & 0xFFFFFFFF:
            fail("invalid stage2 checksum")
        image.seek(STAGE2_PAYLOAD_LBA * SECTOR_SIZE)
        if image.read(len(stage2)) != stage2:
            fail("stage2 differs from input")
        image.seek(FAT_LBA * SECTOR_SIZE)
        bpb = image.read(SECTOR_SIZE)
        if struct.unpack_from("<H", bpb, 11)[0] != SECTOR_SIZE:
            fail("FAT has a non-512-byte sector")
        if struct.unpack_from("<H", bpb, 22)[0] == 0:
            fail("slice b is not FAT16")

    same_file(args.image, "VMUNIX.S9", args.kernel)
    kernel = extract(args.image, "VMUNIX.S9")
    if kernel[:7] != b"\x7fELF\x02\x02\x01":
        fail("/VMUNIX.S9 is not ELF64 big-endian")
    if struct.unpack_from(">H", kernel, 18)[0] != 43:
        fail("/VMUNIX.S9 is not EM_SPARCV9")
    if args.shell is not None:
        same_file(args.image, "sparcv9/bin/sh", args.shell)
    print("SPARC V9 image check: PASS")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--stage2", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()
