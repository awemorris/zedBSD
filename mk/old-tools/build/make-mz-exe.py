#!/usr/bin/env python3
"""Wrap a flat real-mode image in a minimal relocatable DOS MZ container."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--entry", type=lambda value: int(value, 0), default=0)
    args = parser.parse_args()

    image = args.input.read_bytes()
    header_size = 64
    total = header_size + len(image)
    pages, tail = divmod(total, 512)
    if tail:
        pages += 1
    header = bytearray(header_size)
    image_paragraphs = (len(image) + 15) // 16
    struct.pack_into("<14H", header, 0,
                     0x5A4D, tail, pages, 0, 4, 0x110, 0xFFFF,
                     image_paragraphs + 0x10, 0x0FFE, 0,
                     args.entry, 0, 0x40, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + image)


if __name__ == "__main__":
    main()
