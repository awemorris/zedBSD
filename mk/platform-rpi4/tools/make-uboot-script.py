#!/usr/bin/env python3
"""Wrap a U-Boot shell script in the legacy image header `source` reads."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The legacy image is a 64-byte big-endian header (magic, header CRC,
# time, data size, load and entry addresses, data CRC, OS, architecture,
# type, compression, name) followed by the data.  A script's data is a
# list of part sizes ending with zero, then the parts; here one part, the
# script itself.  The time is zero so that the same script gives the same
# bytes.
#
# Values referenced: U-Boot v2023.01 include/image.h (IH_MAGIC, IH_OS_LINUX,
# IH_ARCH_ARM64, IH_TYPE_SCRIPT, IH_COMP_NONE, IH_NMLEN and the header's
# layout).

import argparse
import struct
import zlib
from pathlib import Path

IMAGE_MAGIC = 0x27051956
OS_LINUX = 5
ARCH_ARM64 = 22
TYPE_SCRIPT = 6
COMPRESSION_NONE = 0
NAME_BYTES = 32


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="zedBSD boot script")
    parser.add_argument("script", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    script = args.script.read_bytes()
    data = struct.pack(">II", len(script), 0) + script
    name = args.name.encode("ascii")[:NAME_BYTES - 1].ljust(NAME_BYTES, b"\0")
    data_crc = zlib.crc32(data) & 0xffffffff
    header = struct.pack(">IIIIIIIBBBB32s", IMAGE_MAGIC, 0, 0, len(data), 0, 0,
                         data_crc, OS_LINUX, ARCH_ARM64, TYPE_SCRIPT,
                         COMPRESSION_NONE, name)
    header_crc = zlib.crc32(header) & 0xffffffff
    header = header[:4] + struct.pack(">I", header_crc) + header[8:]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + data)


if __name__ == "__main__":
    main()
