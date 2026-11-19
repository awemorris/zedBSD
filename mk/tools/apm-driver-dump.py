#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Take the disk drivers out of a disk image that Apple's tools initialized.

A 68k Macintosh's ROM starts a SCSI disk through the driver on the disk
itself.  zedBSD's images carry no driver; Apple's tools (Drive Setup or
Apple HD SC Setup) install one.  For development, this tool copies the
driver partitions (Apple_Driver*, Apple_Patches) and block 0 out of a
disk image those tools initialized (for example a BlueSCSI image file)
into a dump, which make-mac68k-image.py --driver-dump merges into a
zedBSD image at the same blocks.  The dump holds Apple's code: keep it
out of the repository and do not hand it on.

  apm-driver-dump.py --input apple-initialized.img --output drivers.dump
"""

import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import apm  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    with open(args.input, "rb") as handle:
        block_zero = handle.read(apm.SECTOR)
        if block_zero[:2] != b"ER":
            raise SystemExit(f"{args.input}: no Driver Descriptor Map in block 0")
        block_size, = struct.unpack_from(">H", block_zero, 2)
        if block_size != apm.SECTOR:
            raise SystemExit(f"{args.input}: blocks of {block_size} bytes, not {apm.SECTOR}")

        # The map's entries, from block 1, as many as the first one counts.
        handle.seek(apm.SECTOR)
        first = handle.read(apm.SECTOR)
        if first[:2] != b"PM":
            raise SystemExit(f"{args.input}: no partition map in block 1")
        map_count, = struct.unpack_from(">I", first, apm.MAP_COUNT_OFFSET)
        drivers = []
        for index in range(map_count):
            handle.seek((1 + index) * apm.SECTOR)
            entry = handle.read(apm.SECTOR)
            start, count, name, kind = apm.entry_fields(entry)
            if not apm.is_driver_type(kind):
                continue
            handle.seek(start * apm.SECTOR)
            data = handle.read(count * apm.SECTOR)
            if len(data) != count * apm.SECTOR:
                raise SystemExit(f"{args.input}: {name} ({kind}) lies past the image's end")
            drivers.append((entry, data))
            print(f"  {kind} \"{name}\" at block {start}, {count} blocks")

    if not drivers:
        raise SystemExit(f"{args.input}: no driver partition")
    apm.write_dump(args.output, block_zero, drivers)
    print(f"{args.output}: {len(drivers)} driver partitions")


if __name__ == "__main__":
    main()
