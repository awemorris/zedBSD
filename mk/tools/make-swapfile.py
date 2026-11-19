#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""Makes a zedBSD swapfile: a 64-byte header and zeros to the size.

  make-swapfile.py --output PATH [--size-mib N] [--format v1|v2]
                   [--uuid HEX] [--label TEXT]

Format v2 (the default) carries the byte count, the page count, an
optional 8-byte UUID (16 hexadecimal digits, dashes ignored) and an
optional label of up to 19 printable ASCII bytes.  Format v1 is 32 or
64 MiB and carries neither.  The rest of the file is a hole.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zedbuild import ensure_parent, fail, parse_number, put16le, put32le  # noqa: E402

HEADER_SIZE = 64
PAGE_SIZE = 4096


def swap_checksum(header: bytearray, checksum_offset: int) -> int:
    """FNV-1a over the header with the checksum's own 4 bytes as zeros."""
    value = 2166136261
    for index, byte in enumerate(header):
        if checksum_offset <= index < checksum_offset + 4:
            byte = 0
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def put_uuid(header: bytearray, text: str) -> None:
    compact = text.replace("-", "")
    if compact == "":
        return
    if len(compact) != 16:
        fail("swap UUID must contain exactly 16 hexadecimal digits")
    for i in range(8):
        pair = compact[i * 2:i * 2 + 2]
        if any(c not in "0123456789abcdefABCDEF" for c in pair):
            fail("invalid swap UUID")
        header[32 + i] = int(pair, 16)


def put_label(header: bytearray, text: str) -> None:
    encoded = text.encode("utf-8")
    if len(encoded) > 19 or len(encoded) != len(text):
        fail("swap label must be at most 19 ASCII bytes")
    for i, code in enumerate(encoded):
        if code < 32 or code > 126:
            fail("invalid swap label")
        header[40 + i] = code


def main(args: list[str]) -> None:
    output = ""
    size_mib = 64
    swap_format = "v2"
    uuid = ""
    label = ""
    i = 0
    while i < len(args):
        if args[i] == "--output" and i + 1 < len(args):
            output = args[i + 1]
        elif args[i] == "--size-mib" and i + 1 < len(args):
            size_mib = parse_number(args[i + 1])
        elif args[i] == "--format" and i + 1 < len(args):
            swap_format = args[i + 1]
        elif args[i] == "--uuid" and i + 1 < len(args):
            uuid = args[i + 1]
        elif args[i] == "--label" and i + 1 < len(args):
            label = args[i + 1]
        else:
            fail("invalid make-swapfile argument: " + args[i])
        i += 2
    if output == "":
        fail("--output is required")
    if swap_format not in ("v1", "v2"):
        fail("--format must be v1 or v2")
    if swap_format == "v1" and size_mib not in (32, 64):
        fail("v1 swapfile size must be 32 or 64 MiB")
    if size_mib <= 0:
        fail("swapfile size must be positive")
    if swap_format == "v1" and (uuid != "" or label != ""):
        fail("v1 swapfiles do not carry UUID or label")

    size = size_mib * 1024 * 1024
    header = bytearray(HEADER_SIZE)
    if swap_format == "v1":
        header[0:8] = b"ZEDSWAP1"
        put32le(header, 8, 1)
        put32le(header, 12, HEADER_SIZE)
        put32le(header, 16, PAGE_SIZE)
        put32le(header, 20, size)
        put32le(header, 24, size // PAGE_SIZE - 1)
        put32le(header, 28, swap_checksum(header, 28))
    else:
        header[0:8] = b"ZEDSWAP2"
        put16le(header, 8, 2)
        put16le(header, 10, HEADER_SIZE)
        put32le(header, 12, PAGE_SIZE)
        # The byte count as two 32-bit halves.
        put32le(header, 16, size & 0xFFFFFFFF)
        put32le(header, 20, size >> 32)
        put32le(header, 24, size // PAGE_SIZE - 1)
        put32le(header, 28, 0)
        put_uuid(header, uuid)
        put_label(header, label)
        put32le(header, 60, swap_checksum(header, 60))

    ensure_parent(output)
    temporary = output + ".tmp"
    try:
        with open(temporary, "wb") as stream:
            stream.write(header)
            stream.truncate(size)
        os.replace(temporary, output)
    except OSError as error:
        fail("cannot write swapfile: %s" % error)


if __name__ == "__main__":
    main(sys.argv[1:])
