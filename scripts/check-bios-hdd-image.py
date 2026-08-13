#!/usr/bin/env python3
"""Validate the native zedBSD MBR/ZBL2/FAT16 disk layout."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path

MACHINES = {"pcat": 1, "pc98": 2}
FAT16_TYPES = {0x04, 0x06, 0x0E}


def fail(message: str) -> None:
    raise SystemExit("bios image check: " + message)


def check(args: argparse.Namespace) -> None:
    image = args.image
    size = image.stat().st_size
    if size == 0 or size % 512:
        fail("image size is not a positive sector multiple")
    with image.open("rb") as stream:
        mbr = stream.read(512)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR signature")
        entries = []
        for index in range(4):
            raw = mbr[0x1BE + index * 16:0x1CE + index * 16]
            status, ptype, start, blocks = raw[0], raw[4], \
                struct.unpack_from("<I", raw, 8)[0], \
                struct.unpack_from("<I", raw, 12)[0]
            if status not in (0, 0x80):
                fail(f"invalid partition status in entry {index + 1}")
            entries.append((status, ptype, start, blocks))
        active = [item for item in enumerate(entries, 1) if item[1][0] == 0x80]
        if len(active) != 1:
            fail("image must have exactly one active primary partition")
        index, (_, ptype, start, blocks) = active[0]
        if ptype not in FAT16_TYPES:
            fail("active partition is not FAT16")
        if start != 2048:
            fail("active partition does not start at LBA 2048")
        if blocks == 0 or start + blocks > size // 512:
            fail("active partition lies outside the image")

        stream.seek(512)
        first = stream.read(512)
        if len(first) != 512:
            fail("missing Stage 2")
        magic, version, header_size, image_size = struct.unpack_from(
            "<IHHI", first, 0)
        sectors, machine, entry = struct.unpack_from("<HHI", first, 12)
        if (magic, version, header_size) != (0x324C425A, 1, 32):
            fail("invalid ZBL2 header")
        if machine != MACHINES[args.machine]:
            fail("ZBL2 machine mismatch")
        if not 1 <= sectors <= 127 or 1 + sectors > 2048:
            fail("invalid ZBL2 sector count")
        if not 32 <= entry < image_size <= sectors * 512:
            fail("invalid ZBL2 size or entry")
        stream.seek(512)
        stage2 = stream.read(sectors * 512)
        total = sum(struct.unpack_from("<I", stage2, offset)[0]
                    for offset in range(0, len(stage2), 4)) & 0xFFFFFFFF
        if total:
            fail("ZBL2 checksum mismatch")
        stream.seek((1 + sectors) * 512)
        gap = stream.read((2048 - 1 - sectors) * 512)
        if any(gap):
            fail("reserved pre-partition gap is not zero")
        stream.seek(start * 512)
        bpb = stream.read(512)
        if struct.unpack_from("<H", bpb, 11)[0] != 512:
            fail("FAT bytes/sector is not 512")
        if struct.unpack_from("<H", bpb, 22)[0] == 0:
            fail("volume is not FAT12/16")

    with tempfile.TemporaryDirectory() as directory:
        extracted = Path(directory) / "VMUNIX"
        subprocess.run(["mcopy", "-n", "-i", f"{image}@@{start * 512}",
                        "::VMUNIX", str(extracted)], check=True,
                       stdout=subprocess.DEVNULL)
        if args.kernel and hashlib.sha256(extracted.read_bytes()).digest() != \
                hashlib.sha256(args.kernel.read_bytes()).digest():
            fail("VMUNIX content differs from the input kernel")
    print(f"BIOS image check: PASS ({args.machine}, partition {index}, "
          f"Stage 2 {sectors} sectors)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=sorted(MACHINES), required=True)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()

