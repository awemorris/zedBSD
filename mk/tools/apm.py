#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Write the parts of an Apple Partition Map disk: the Driver Descriptor
Map in block 0, the partition map's entries from block 1, and the disk
itself from its pieces.

Blocks are 512 bytes.  The map takes 63 blocks (as Apple's tools leave
it), so the first partition after it starts at block 64.

Values referenced: Apple, Inside Macintosh: Devices (1994), chapter 3,
the Driver Descriptor Record (sbSig 'ER', sbBlkSize, sbBlkCount,
sbDevType, sbDevId, sbData, sbDrvrCount and the driver descriptors
ddBlock, ddSize, ddType) and the partition map entry (pmSig 'PM',
pmMapBlkCnt, pmPyPartStart, pmPartBlkCnt, pmPartName, pmParType,
pmLgDataStart, pmDataCnt, pmPartStatus).
"""

import struct
from pathlib import Path

SECTOR = 512
MAP_BLOCKS = 63
FIRST_PARTITION = 1 + MAP_BLOCKS

# Partition status: valid, allocated, in use, readable, writable; boot adds valid boot information.
STATUS_DATA = 0x00000037
STATUS_BOOT = 0x0000007F

# Where the driver descriptors start in block 0, and how long each is.
DRIVER_DESCRIPTORS = 18
DRIVER_DESCRIPTOR_SIZE = 8

# The partition map entry's field that counts the map's entries.
MAP_COUNT_OFFSET = 4


def ddm(total_sectors, drivers=()):
    """The Driver Descriptor Map: signature, block size, block count and the drivers (block, size, type)."""
    block = bytearray(SECTOR)
    struct.pack_into(">2sHIHHIH", block, 0, b"ER", SECTOR, total_sectors, 0, 0, 0, len(drivers))
    for index, (start, size, kind) in enumerate(drivers):
        struct.pack_into(">IHH", block, DRIVER_DESCRIPTORS + index * DRIVER_DESCRIPTOR_SIZE, start, size, kind)
    return bytes(block)


def map_entry(map_count, start, count, name, kind, status):
    """One partition map entry."""
    block = bytearray(SECTOR)
    struct.pack_into(">2sHIII32s32sIII", block, 0, b"PM", 0, map_count, start, count,
                     name.encode("ascii"), kind.encode("ascii"), 0, count, status)
    return bytes(block)


def with_map_count(entry, map_count):
    """A copy of a partition map entry that counts map_count entries."""
    block = bytearray(entry)
    struct.pack_into(">I", block, MAP_COUNT_OFFSET, map_count)
    return bytes(block)


def write_disk(output, total_sectors, pieces):
    """Write a disk of total_sectors from its pieces (start block, bytes), in place, published by a rename."""
    output = Path(output)
    temporary = output.with_name(output.name + ".tmp")
    with open(temporary, "wb") as handle:
        for start, data in pieces:
            handle.seek(start * SECTOR)
            handle.write(data)
        handle.truncate(total_sectors * SECTOR)
    temporary.replace(output)


#
# Driver dumps
#
# A dump holds the disk drivers of a disk that Apple's tools initialized:
# its Driver Descriptor Map, then each driver partition's map entry and
# its blocks.  zedBSD does not ship Apple's drivers; a developer takes a
# dump from his own disk and merges it into an image built for himself.
#
#   "ZEDAPMD1", the entry count (32-bit, big-endian),
#   block 0 of the disk, each driver partition's map entry,
#   each driver partition's blocks, in the entries' order.
#

DUMP_MAGIC = b"ZEDAPMD1"

# The partition types that hold a driver or the driver's patches.
DRIVER_TYPES = ("Apple_Driver", "Apple_Patches", "Apple_FWDriver")


def entry_fields(entry):
    """A map entry's start block, block count, name and type."""
    signature, _, _, start, count, name, kind = struct.unpack_from(">2sHIII32s32s", entry, 0)
    if signature != b"PM":
        raise ValueError("not a partition map entry")
    return start, count, name.split(b"\0")[0].decode("ascii", "replace"), kind.split(b"\0")[0].decode("ascii", "replace")


def is_driver_type(kind):
    """Whether a partition type holds a driver or its patches."""
    return any(kind.startswith(prefix) for prefix in DRIVER_TYPES)


def write_dump(path, block_zero, drivers):
    """Write a dump: block 0 and the drivers (map entry, blocks)."""
    body = bytearray(DUMP_MAGIC + struct.pack(">I", len(drivers)))
    body += block_zero
    for entry, _ in drivers:
        body += entry
    for _, data in drivers:
        body += data
    Path(path).write_bytes(bytes(body))


def read_dump(path):
    """Read a dump: block 0 and the drivers (map entry, blocks)."""
    data = Path(path).read_bytes()
    if data[:len(DUMP_MAGIC)] != DUMP_MAGIC:
        raise ValueError(f"{path}: not a driver dump")
    count, = struct.unpack_from(">I", data, len(DUMP_MAGIC))
    at = len(DUMP_MAGIC) + 4
    block_zero = data[at:at + SECTOR]
    at += SECTOR
    entries = []
    for _ in range(count):
        entries.append(data[at:at + SECTOR])
        at += SECTOR
    drivers = []
    for entry in entries:
        _, blocks, _, _ = entry_fields(entry)
        drivers.append((entry, data[at:at + blocks * SECTOR]))
        at += blocks * SECTOR
    if at != len(data) or len(block_zero) != SECTOR or block_zero[:2] != b"ER":
        raise ValueError(f"{path}: a damaged driver dump")
    return block_zero, drivers
