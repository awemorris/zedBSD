#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Validate a 68040 Macintosh disk image (make-mac68k-image.py).

It checks the Driver Descriptor Map and the partition map (the entries,
their count, that the partitions lie in the disk and do not overlap,
the driver area), the HFS boot partition (the master directory block,
the boot block's header and zedBSD fields, the second stage and the
kernel at the recorded places with the recorded sums, and, when given,
that they are the files built), and the UFS root (with
check-ufs-image.py's checker and the root marker).  With --driver-dump
it checks that the dump's drivers are at their blocks and that block 0
names them.

  check-mac68k-image.py IMAGE [--kernel vmunix] [--stage2 zedboot]
                        [--driver-dump FILE | --driver-area free|empty-driver43]
"""

import argparse
import importlib.util
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import apm  # noqa: E402

SECTOR = apm.SECTOR
DRIVER_AREA_END = 8192
BOOT_INFO = 148
BOOT_INFO_MAGIC = 0x5A454442
BOOT_VERSION = 0x4418
STAGE2_MAGIC = 0x5A425332
ELF_MAGIC = b"\x7fELF"


def fail(message):
    """Stops with a message."""
    raise SystemExit(f"check-mac68k-image: {message}")


def ufs_checker():
    """check-ufs-image.py's UFS reader."""
    spec = importlib.util.spec_from_file_location("check_ufs_image", HERE / "check-ufs-image.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.UFS


def read_map(image):
    """The partition map's entries: (start, count, name, type, status, raw entry)."""
    first = image[SECTOR:2 * SECTOR]
    if first[:2] != b"PM":
        fail("no partition map in block 1")
    count, = struct.unpack_from(">I", first, apm.MAP_COUNT_OFFSET)
    if count < 2 or count > apm.MAP_BLOCKS:
        fail(f"the map counts {count} entries")
    entries = []
    for index in range(count):
        raw = image[(1 + index) * SECTOR:(2 + index) * SECTOR]
        start, blocks, name, kind = apm.entry_fields(raw)
        entry_count, = struct.unpack_from(">I", raw, apm.MAP_COUNT_OFFSET)
        if entry_count != count:
            fail(f"entry {index + 1} counts {entry_count} entries, not {count}")
        status, = struct.unpack_from(">I", raw, 88)
        entries.append((start, blocks, name, kind, status, raw))
    return entries


def check_layout(image, entries, total):
    """Checks that the partitions lie in the disk, in order, without overlapping, and the map's own entry."""
    start, blocks, _, kind, _, _ = entries[0]
    if (start, blocks, kind) != (1, apm.MAP_BLOCKS, "Apple_partition_map"):
        fail("the first entry is not the map's own")
    end = apm.FIRST_PARTITION
    for start, blocks, name, kind, _, _ in entries[1:]:
        if start < end:
            fail(f"{name} ({kind}) at {start} overlaps what comes before it")
        if start + blocks > total:
            fail(f"{name} ({kind}) lies past the disk's end")
        end = start + blocks


def check_driver_area(image, entries, args):
    """Checks the driver area against what was asked: free, an empty Driver43, or the dump's drivers."""
    block_zero = image[:SECTOR]
    driver_count, = struct.unpack_from(">H", block_zero, 16)
    drivers = [e for e in entries if apm.is_driver_type(e[3])]
    if args.driver_dump:
        dump_zero, dumped = apm.read_dump(args.driver_dump)
        if block_zero[8:] != dump_zero[8:]:
            fail("block 0 is not the dump's (beyond the block count)")
        for entry, data in dumped:
            start, blocks, name, kind = apm.entry_fields(entry)
            if image[start * SECTOR:(start + blocks) * SECTOR] != data:
                fail(f"{name} ({kind}) is not the dump's at block {start}")
            if not any(e[0] == start and e[1] == blocks and e[3] == kind for e in drivers):
                fail(f"the map has no entry for {name} ({kind}) at {start}")
        for index in range(driver_count):
            block, size, _ = struct.unpack_from(">IHH", block_zero, apm.DRIVER_DESCRIPTORS + index * apm.DRIVER_DESCRIPTOR_SIZE)
            if not any(e[0] <= block < e[0] + e[1] for e in drivers):
                fail(f"driver descriptor {index} at block {block} lies in no driver partition")
        return f"{len(dumped)} drivers from the dump, {driver_count} descriptors"
    if driver_count != 0:
        fail(f"block 0 names {driver_count} drivers in an image without drivers")
    if args.driver_area == "empty-driver43":
        if len(drivers) != 1 or drivers[0][3] != "Apple_Driver43" or any(image[drivers[0][0] * SECTOR:(drivers[0][0] + drivers[0][1]) * SECTOR]):
            fail("no single empty Apple_Driver43 partition")
        return "an empty Apple_Driver43 partition"
    if drivers:
        fail("a driver partition in an image without drivers")
    free = [e for e in entries if e[3] == "Apple_Free" and e[0] == apm.FIRST_PARTITION]
    if not free or free[0][1] != DRIVER_AREA_END - apm.FIRST_PARTITION:
        fail("the driver area is not one Apple_Free partition up to 4 MiB")
    if any(image[apm.FIRST_PARTITION * SECTOR:DRIVER_AREA_END * SECTOR]):
        fail("the free driver area is not empty")
    return "a free driver area"


def check_boot(image, entries, args):
    """Checks the HFS boot partition, its boot block and what the boot block records."""
    found = [e for e in entries if e[3] == "Apple_HFS" and e[2] == "zedBSD"]
    if len(found) != 1:
        fail("no single Apple_HFS partition named zedBSD")
    start, blocks, _, _, _, _ = found[0]
    part = image[start * SECTOR:(start + blocks) * SECTOR]
    if part[1024:1026] != b"BD":
        fail("the boot partition has no HFS master directory block")
    if part[:2] != b"LK":
        fail("the boot block has no 'LK' signature")
    if part[2:4] != b"\x60\x00":
        fail("the boot block's entry is not a branch")
    version, = struct.unpack_from(">H", part, 6)
    if version != BOOT_VERSION:
        fail(f"the boot block's version is {version:#x}")
    (magic, layout, _, partition_block, stage2_offset, stage2_bytes, stage2_memory, stage2_crc,
     kernel_offset, kernel_bytes, kernel_crc, mark) = struct.unpack_from(">IHHIIIIIIIII", part, BOOT_INFO)
    if magic != BOOT_INFO_MAGIC or layout != 1:
        fail("the boot block's zedBSD fields are missing")
    if partition_block != start:
        fail(f"the boot block records the partition at {partition_block}, not {start}")

    # The second stage at its recorded place.
    stage2 = part[stage2_offset:stage2_offset + stage2_bytes]
    if len(stage2) != stage2_bytes or zlib.crc32(stage2) != stage2_crc:
        fail("the second stage does not match its recorded sum")
    if struct.unpack_from(">I", stage2, 4)[0] != STAGE2_MAGIC:
        fail("the second stage has no header")
    if struct.unpack_from(">I", stage2, 16)[0] != stage2_memory or stage2_memory < stage2_bytes:
        fail("the second stage's memory is not recorded right")
    if args.stage2 and stage2 != Path(args.stage2).read_bytes():
        fail(f"the second stage is not {args.stage2}")

    # The kernel at its recorded place.
    kernel = part[kernel_offset:kernel_offset + kernel_bytes]
    if len(kernel) != kernel_bytes or zlib.crc32(kernel) != kernel_crc:
        fail("the kernel does not match its recorded sum")
    if kernel[:4] != ELF_MAGIC or kernel[5] != 2 or struct.unpack_from(">H", kernel, 18)[0] != 4:
        fail("the kernel is not a big-endian m68k ELF file")
    if args.kernel and kernel != Path(args.kernel).read_bytes():
        fail(f"the kernel is not {args.kernel}")
    if mark == 0:
        fail("the boot block has no image mark")
    return start, stage2_bytes, kernel_bytes


def check_root(image, entries):
    """Checks the UFS root and its marker."""
    found = [e for e in entries if e[3] == "Apple_UNIX_SVR2" and e[2] == "zedBSD-root"]
    if len(found) != 1:
        fail("no single Apple_UNIX_SVR2 partition named zedBSD-root")
    start, blocks, _, _, _, _ = found[0]
    fs = ufs_checker()(image[start * SECTOR:(start + blocks) * SECTOR])
    fs.validate()
    if fs.read_file(fs.lookup("/etc/zedbsd-root")) != b"zedBSD ufs root v1\n":
        fail("the root has no zedBSD root marker")
    return start


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--kernel")
    parser.add_argument("--stage2")
    parser.add_argument("--driver-dump")
    parser.add_argument("--driver-area", choices=("free", "empty-driver43"), default="free")
    args = parser.parse_args()

    image = Path(args.image).read_bytes()
    if len(image) % SECTOR:
        fail("the image is not whole sectors")
    total = len(image) // SECTOR
    if image[:2] != b"ER":
        fail("no Driver Descriptor Map in block 0")
    block_size, block_count = struct.unpack_from(">HI", image, 2)
    if block_size != SECTOR or block_count != total:
        fail(f"block 0 says {block_count} blocks of {block_size} bytes")

    entries = read_map(image)
    check_layout(image, entries, total)
    drivers = check_driver_area(image, entries, args)
    boot_start, stage2_bytes, kernel_bytes = check_boot(image, entries, args)
    root_start = check_root(image, entries)
    print(f"{args.image}: OK: {len(entries)} map entries, {drivers}; boot at {boot_start} "
          f"(zedboot {stage2_bytes} bytes, vmunix {kernel_bytes} bytes); root at {root_start}")


if __name__ == "__main__":
    main()
