#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Make a 68040 Macintosh disk image: APM, the HFS boot partition, UFS.

The disk is Apple-partitioned:

  block 0          the Driver Descriptor Map
  blocks 1-63      the partition map
  blocks 64-8191   the driver area: empty (Apple_Free) in the image that is
                   handed out, where Apple's tools install their driver;
                   or the driver partitions of a dump (--driver-dump) at
                   the blocks they had, for a developer's own image
  Apple_HFS        "zedBSD": the boot block, the second stage zedboot,
                   the kernel vmunix and the loader's zedbsd.cfg
  Apple_UNIX_SVR2  "zedBSD-root": the UFS root (marked etc/zedbsd-root)
  Apple_UNIX_SVR2  "zedBSD-swap": swap, when asked for

The ROM reads the HFS partition's first two sectors (the boot block)
through the disk's driver and runs them.  This tool writes the boot
block from the loader's build and records in it where the second stage
and the kernel are in the partition (the HFS volume keeps each file in
one extent), their sums and the partition's own block, as
bootloader/mac68k/boot-layout.h describes.

The driver area may instead hold an empty Apple_Driver43 partition
(--driver-area empty-driver43), for trying whether Apple's tools update
the driver of such a disk more readily.

  make-mac68k-image.py --kernel vmunix --boot-block bootblock.bin
                       --stage2 zedboot (--root-tree DIR | --root-image IMG)
                       [--config zedbsd.cfg] [--driver-dump FILE]
                       [--driver-area free|empty-driver43]
                       [--boot-mib 16] [--root-mib 64] [--swap-mib 0]
                       --output IMAGE

Values referenced: Apple, Inside Macintosh: Files (1992), p. 2-57
"Boot Blocks" (the boot blocks are the HFS volume's first two sectors);
Inside Macintosh: Devices (1994), chapter 3 (the Driver Descriptor Map
and the partition map entry); bootloader/mac68k/boot-layout.h (the
zedBSD fields).
"""

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import apm  # noqa: E402
import hfs  # noqa: E402

SECTOR = apm.SECTOR

# The driver area: from the end of the map up to 4 MiB; the zedBSD
# partitions start on a 4 MiB boundary after the drivers.
DRIVER_AREA_END = 8192
ALIGNMENT = 8192

# The empty driver partition of --driver-area empty-driver43.
EMPTY_DRIVER_BLOCKS = 64

# The boot block and the second stage's header (boot-layout.h).
BOOT_BLOCK_SIZE = 1024
BOOT_SIGNATURE = b"LK"
BOOT_INFO_MAGIC = 0x5A454442
BOOT_INFO_VERSION = 1
BOOT_INFO = 148
BOOT_PARTITION_BLOCK = BOOT_INFO + 8
STAGE2_MAGIC = 0x5A425332
STAGE2_MAGIC_OFFSET = 4
STAGE2_MEMORY_OFFSET = 16


def fail(message):
    """Stops with a message."""
    raise SystemExit(f"make-mac68k-image: {message}")


def checked_boot_block(path):
    """The loader's boot block, padded to two sectors, after checking its identity."""
    data = Path(path).read_bytes()
    if len(data) > BOOT_BLOCK_SIZE:
        fail(f"{path}: the boot block exceeds {BOOT_BLOCK_SIZE} bytes")
    data = bytearray(data.ljust(BOOT_BLOCK_SIZE, b"\0"))
    magic, version = struct.unpack_from(">IH", data, BOOT_INFO)
    if data[:2] != BOOT_SIGNATURE or magic != BOOT_INFO_MAGIC or version != BOOT_INFO_VERSION:
        fail(f"{path}: not the zedBSD boot block of this layout")
    return data


def checked_stage2(path):
    """The second stage and the memory it needs, after checking its identity."""
    data = Path(path).read_bytes()
    magic, = struct.unpack_from(">I", data, STAGE2_MAGIC_OFFSET)
    if magic != STAGE2_MAGIC:
        fail(f"{path}: not the zedBSD second stage")
    memory, = struct.unpack_from(">I", data, STAGE2_MEMORY_OFFSET)
    return data, memory


def driver_area(args):
    """The block 0 and the map entries and pieces of the driver area, and where it ends."""
    if args.driver_dump:
        block_zero, drivers = apm.read_dump(args.driver_dump)
        entries = []
        pieces = []
        end = apm.FIRST_PARTITION
        for entry, data in sorted(drivers, key=lambda d: apm.entry_fields(d[0])[0]):
            start, count, name, kind = apm.entry_fields(entry)
            if start < end:
                fail(f"{args.driver_dump}: {name} ({kind}) overlaps the map or another driver")
            entries.append(("dump", entry))
            pieces.append((start, data))
            end = start + count
        zedbsd_start = max(DRIVER_AREA_END, (end + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT)
        if zedbsd_start > end:
            entries.append(("new", (end, zedbsd_start - end, "Extra", "Apple_Free", 0)))
        return block_zero, entries, pieces, zedbsd_start

    if args.driver_area == "empty-driver43":
        start = apm.FIRST_PARTITION
        entries = [("new", (start, EMPTY_DRIVER_BLOCKS, "Macintosh", "Apple_Driver43", apm.STATUS_DATA)),
                   ("new", (start + EMPTY_DRIVER_BLOCKS, DRIVER_AREA_END - start - EMPTY_DRIVER_BLOCKS,
                            "Extra", "Apple_Free", 0))]
        return None, entries, [], DRIVER_AREA_END

    entries = [("new", (apm.FIRST_PARTITION, DRIVER_AREA_END - apm.FIRST_PARTITION, "Extra", "Apple_Free", 0))]
    return None, entries, [], DRIVER_AREA_END


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--boot-block", required=True)
    parser.add_argument("--stage2", required=True)
    parser.add_argument("--config")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--root-tree")
    source.add_argument("--root-image")
    parser.add_argument("--driver-dump")
    parser.add_argument("--driver-area", choices=("free", "empty-driver43"), default="free")
    parser.add_argument("--boot-mib", type=int, default=16)
    parser.add_argument("--root-mib", type=int, default=64)
    parser.add_argument("--swap-mib", type=int, default=0)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    # The dates are the build's, for a reproducible image.
    when = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))

    boot_block = checked_boot_block(args.boot_block)
    stage2, stage2_memory = checked_stage2(args.stage2)
    kernel = Path(args.kernel).read_bytes()
    if kernel[:4] != b"\x7fELF":
        fail(f"{args.kernel}: not an ELF file")
    config = Path(args.config).read_bytes() if args.config else b"kernel=vmunix\n"

    # The boot volume: the second stage, the kernel and the configuration
    # (a text file Mac OS opens with its own editor).
    top = hfs.Folder("zedBSD")
    stage2_file = hfs.File("zedboot", stage2)
    kernel_file = hfs.File("vmunix", kernel)
    top.children["zedboot"] = stage2_file
    top.children["vmunix"] = kernel_file
    top.children["zedbsd.cfg"] = hfs.File("zedbsd.cfg", config, b"TEXT", b"ttxt")
    boot = bytearray(hfs.make_volume(args.boot_mib * 1024 * 1024, top, "zedBSD", when=when))

    # The root: a UFS image as given, or one made from a tree.
    if args.root_image:
        root = Path(args.root_image).read_bytes()
    else:
        from ufs_format import create  # noqa: E402
        root = create(args.root_mib * 1024 * 1024, args.root_tree)
    if len(root) % SECTOR:
        fail("the root image is not whole sectors")
    swap = b"\0" * (args.swap_mib * 1024 * 1024)

    # The driver area, then the zedBSD partitions one after another.
    block_zero, driver_entries, pieces, start = driver_area(args)
    parts = [("zedBSD", "Apple_HFS", apm.STATUS_DATA, boot),
             ("zedBSD-root", "Apple_UNIX_SVR2", apm.STATUS_DATA, root)]
    if swap:
        parts.append(("zedBSD-swap", "Apple_UNIX_SVR2", apm.STATUS_DATA, swap))
    boot_start = start
    places = []
    for name, kind, status, data in parts:
        places.append((start, len(data) // SECTOR, name, kind, status, data))
        start += len(data) // SECTOR
    total = start

    # The boot block's fields: where the partition, the second stage and the kernel are, and their sums.
    mark = zlib.crc32(stage2 + kernel + struct.pack(">II", total, when & 0xFFFFFFFF))
    struct.pack_into(">IIIIIIIII", boot_block, BOOT_PARTITION_BLOCK,
                     boot_start,
                     stage2_file.offset, len(stage2), stage2_memory, zlib.crc32(stage2),
                     kernel_file.offset, len(kernel), zlib.crc32(kernel),
                     mark)
    boot[0:BOOT_BLOCK_SIZE] = boot_block

    # The map: itself, the driver area, the zedBSD partitions.
    map_count = 1 + len(driver_entries) + len(places)
    if map_count > apm.MAP_BLOCKS:
        fail("the partition map has too many entries")
    entries = [apm.map_entry(map_count, 1, apm.MAP_BLOCKS, "Apple", "Apple_partition_map", apm.STATUS_DATA)]
    for origin, value in driver_entries:
        if origin == "dump":
            entries.append(apm.with_map_count(value, map_count))
        else:
            entries.append(apm.map_entry(map_count, *value))
    for place, count, name, kind, status, _ in places:
        entries.append(apm.map_entry(map_count, place, count, name, kind, status))

    # Block 0: the dump's, with this disk's size, or one with no driver.
    if block_zero is not None:
        ddm = bytearray(block_zero)
        struct.pack_into(">I", ddm, 4, total)
        ddm = bytes(ddm)
    else:
        ddm = apm.ddm(total)

    # The disk.
    disk = [(0, ddm)]
    disk.extend((1 + index, entry) for index, entry in enumerate(entries))
    disk.extend(pieces)
    disk.extend((place, data) for place, _, _, _, _, data in places)
    apm.write_disk(args.output, total, disk)
    print(f"{args.output}: {total} sectors; " +
          ", ".join(f"{name}@{place}" for place, _, name, _, _, _ in places) +
          (f"; drivers from {args.driver_dump}" if args.driver_dump else f"; driver area {args.driver_area}"))


if __name__ == "__main__":
    main()
