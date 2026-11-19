#!/usr/bin/env python3
"""Finish a BIOS Stage 2 image: pad it to whole sectors and fill its ZBL2 header.

The linked Stage 2 starts with a 32-byte ZBL2 template header.  This writes the
image size (offset 8), the sector count (offset 12) and a dword checksum
(offset 20) that makes the little-endian dword sum of the padded image zero,
which Stage 1 checks after loading.

usage: finalize-bios-stage2.py --machine pcat|pc98 INPUT OUTPUT
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import struct
import sys

ZBL2_MAGIC = 0x324C425A  # "ZBL2"
ZBL2_VERSION = 1
ZBL2_HEADER_SIZE = 32
MACHINES = {"pcat": 1, "pc98": 2}
SECTOR = 512
SECTORS_MAX = 127


def fail(message: str) -> None:
    """Reports an error and stops."""
    sys.exit(f"finalize-bios-stage2: {message}")


def dword_sum(image: bytes | bytearray) -> int:
    """The 32-bit sum of the image's little-endian dwords."""
    total = 0
    for (value,) in struct.iter_unpack("<I", image):
        total += value
    return total & 0xFFFFFFFF


def finalize(raw: bytes, machine: str) -> bytearray:
    """Checks the template header and returns the padded image with its header filled."""
    if len(raw) < ZBL2_HEADER_SIZE:
        fail("Stage 2 is shorter than its header")
    magic, version, header_size = struct.unpack_from("<IHH", raw, 0)
    if magic != ZBL2_MAGIC or version != ZBL2_VERSION or header_size != ZBL2_HEADER_SIZE:
        fail("invalid ZBL2 template header")
    (machine_id,) = struct.unpack_from("<H", raw, 14)
    if machine_id != MACHINES[machine]:
        fail("Stage 2 machine mismatch")
    (entry,) = struct.unpack_from("<I", raw, 16)
    if entry < ZBL2_HEADER_SIZE or entry >= len(raw):
        fail("invalid Stage 2 entry")
    sectors = (len(raw) + SECTOR - 1) // SECTOR
    if sectors < 1 or sectors > SECTORS_MAX:
        fail(f"Stage 2 must occupy 1..{SECTORS_MAX} sectors")

    image = bytearray(sectors * SECTOR)
    image[: len(raw)] = raw
    struct.pack_into("<I", image, 8, len(raw))
    struct.pack_into("<H", image, 12, sectors)
    struct.pack_into("<I", image, 20, 0)
    struct.pack_into("<I", image, 20, (-dword_sum(image)) & 0xFFFFFFFF)
    if dword_sum(image) != 0:
        fail("internal Stage 2 checksum failure")
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--machine", required=True, choices=sorted(MACHINES))
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()
    if args.input == args.output:
        fail("input and output must differ")
    with open(args.input, "rb") as source:
        raw = source.read()
    image = finalize(raw, args.machine)
    with open(args.output, "wb") as destination:
        destination.write(image)


if __name__ == "__main__":
    main()
