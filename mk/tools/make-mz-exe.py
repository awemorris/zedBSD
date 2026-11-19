#!/usr/bin/env python3
"""Wrap a flat real-mode image in a DOS MZ executable header.

The output is a 64-byte MZ header followed by the image, so the same file runs
from DOS: the header places the code at the start of the load module, gives it
a 64 KiB segment, a stack at 0x0ffe in the paragraph after the image, and
starts it at --entry (an offset into the image, CS:IP = 0:entry).

usage: make-mz-exe.py [--entry N] INPUT OUTPUT
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
import sys

HEADER_SIZE = 64
PAGE = 512


def fail(message: str) -> None:
    """Reports an error and stops."""
    sys.exit(f"make-mz-exe: {message}")


def number(text: str) -> int:
    """A decimal or 0x-prefixed hexadecimal number, optionally negative."""
    try:
        return int(text, 0)
    except ValueError:
        fail(f"invalid number: {text}")
        return 0


def make_exe(image: bytes, entry: int) -> bytes:
    """Returns the MZ header and the image."""
    total = HEADER_SIZE + len(image)
    pages = (total + PAGE - 1) // PAGE
    tail = total % PAGE
    fields = (
        0x5A4D,                         # "MZ"
        tail,                           # bytes in the last page
        pages,                          # pages in the file
        0,                              # relocations
        HEADER_SIZE // 16,              # header paragraphs
        0x110,                          # minimum extra paragraphs
        0xFFFF,                         # maximum extra paragraphs
        (len(image) + 15) // 16 + 0x10,  # initial SS
        0x0FFE,                         # initial SP
        0,                              # checksum
        entry,                          # initial IP
        0,                              # initial CS
        HEADER_SIZE,                    # relocation table offset
        0,                              # overlay number
    )
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<14H", header, 0, *(value & 0xFFFF for value in fields))
    return bytes(header) + image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--entry", default="0", type=str)
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()
    with open(args.input, "rb") as source:
        image = source.read()
    output = make_exe(image, number(args.entry))
    parent = os.path.dirname(args.output)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(args.output, "wb") as destination:
        destination.write(output)


if __name__ == "__main__":
    main()
