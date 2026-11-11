#!/usr/bin/env python3
"""Wrap a U-Boot command script in a legacy image header (boot.scr).

  make-uboot-script.py SCRIPT.cmd OUTPUT.scr

The output is what `mkimage -A riscv -O linux -T script -C none` writes,
with a zero time stamp so the same script gives the same bytes: the 64-byte
header, then the image's length table (the script's length and a zero
terminator) and the script.

Values referenced: U-Boot v2021.10 include/image.h (struct
legacy_img_hdr, IH_MAGIC 0x27051956, IH_OS_LINUX 5, IH_ARCH_RISCV 26,
IH_TYPE_SCRIPT 6, IH_COMP_NONE 0, IH_NMLEN 32) and cmd/source.c (the
length table before the script).
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

MAGIC = 0x27051956
OS_LINUX = 5
ARCH_RISCV = 26
TYPE_SCRIPT = 6
COMP_NONE = 0
NAME = b"zedBSD boot script"


def wrap(script: bytes) -> bytes:
    data = struct.pack(">II", len(script), 0) + script
    data_crc = zlib.crc32(data) & 0xFFFFFFFF

    def header(crc: int) -> bytes:
        return struct.pack(">IIIIIIIBBBB32s", MAGIC, crc, 0, len(data), 0, 0,
                           data_crc, OS_LINUX, ARCH_RISCV, TYPE_SCRIPT,
                           COMP_NONE, NAME)

    header_crc = zlib.crc32(header(0)) & 0xFFFFFFFF
    return header(header_crc) + data


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: make-uboot-script.py SCRIPT.cmd OUTPUT.scr")
    Path(sys.argv[2]).write_bytes(wrap(Path(sys.argv[1]).read_bytes()))


if __name__ == "__main__":
    main()
