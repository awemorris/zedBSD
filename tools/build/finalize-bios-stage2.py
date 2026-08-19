#!/usr/bin/env python3
"""Finalize and verify a ZBL2 raw Stage 2 image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import struct
from pathlib import Path

MAGIC = 0x324C425A
VERSION = 1
HEADER_SIZE = 32
MACHINES = {"pcat": 1, "pc98": 2}
MAX_SECTORS = 127


def dword_sum(data: bytes) -> int:
    if len(data) % 4:
        raise ValueError("checksum input is not dword aligned")
    return sum(struct.unpack_from("<I", data, offset)[0]
               for offset in range(0, len(data), 4)) & 0xFFFFFFFF


def finalize(machine: str, source: Path, output: Path) -> None:
    if source.resolve() == output.resolve():
        raise SystemExit("input and output must be different files")
    raw = bytearray(source.read_bytes())
    if len(raw) < HEADER_SIZE:
        raise SystemExit("Stage 2 is shorter than its header")
    magic, version, header_size = struct.unpack_from("<IHH", raw, 0)
    if (magic, version, header_size) != (MAGIC, VERSION, HEADER_SIZE):
        raise SystemExit("invalid ZBL2 template header")
    declared_machine = struct.unpack_from("<H", raw, 14)[0]
    if declared_machine != MACHINES[machine]:
        raise SystemExit("Stage 2 machine does not match --machine")
    entry = struct.unpack_from("<I", raw, 16)[0]
    if entry < HEADER_SIZE or entry >= len(raw):
        raise SystemExit("invalid Stage 2 entry offset")
    sectors = (len(raw) + 511) // 512
    if not 1 <= sectors <= MAX_SECTORS:
        raise SystemExit("Stage 2 must occupy 1..127 sectors")
    image_size = len(raw)
    raw.extend(bytes(sectors * 512 - len(raw)))
    struct.pack_into("<I", raw, 8, image_size)
    struct.pack_into("<H", raw, 12, sectors)
    struct.pack_into("<I", raw, 20, 0)
    checksum = (-dword_sum(raw)) & 0xFFFFFFFF
    struct.pack_into("<I", raw, 20, checksum)
    if dword_sum(raw) != 0:
        raise SystemExit("internal Stage 2 checksum failure")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(raw)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=sorted(MACHINES), required=True)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    finalize(args.machine, args.input, args.output)


if __name__ == "__main__":
    main()

