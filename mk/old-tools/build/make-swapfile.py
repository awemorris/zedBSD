#!/usr/bin/env python3
"""Create a zedBSD swapfile with a validated on-disk header."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import os
import struct
from pathlib import Path

PAGE_SIZE = 4096
HEADER_SIZE = 64


def checksum(header: bytes | bytearray) -> int:
    value = 2166136261
    for index, byte in enumerate(header):
        if 28 <= index < 32:
            byte = 0
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def create(output: Path, size_mib: int) -> None:
    if size_mib not in (32, 64):
        raise SystemExit("swapfile size must be 32 or 64 MiB")
    size = size_mib * 1024 * 1024
    slots = size // PAGE_SIZE - 1
    header = bytearray(HEADER_SIZE)
    header[:8] = b"ZEDSWAP1"
    struct.pack_into("<IIIIII", header, 8, 1, HEADER_SIZE, PAGE_SIZE,
                     size, slots, 0)
    struct.pack_into("<I", header, 28, checksum(header))
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    try:
        with temporary.open("wb") as stream:
            stream.truncate(size)
            stream.seek(0)
            stream.write(header)
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=64)
    args = parser.parse_args()
    create(args.output, args.size_mib)


if __name__ == "__main__":
    main()
