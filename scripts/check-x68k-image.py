#!/usr/bin/env python3
"""Independently validate the zedBSD X68000 SCSI disk image."""

import argparse
import binascii
import hashlib
import struct
from pathlib import Path

SECTOR = 512
IMAGE_SECTORS = 131072
ROOT_LBA = 4096
MANIFEST_LBA = 8
MANIFEST_MAGIC = 0x5A36384D


def fail(message: str) -> None:
    raise SystemExit("X68k image check: " + message)


def read_source(path: Path | None) -> bytes | None:
    if path is None:
        return None
    if not path.is_file():
        fail(f"missing comparison input {path}")
    return path.read_bytes()


def fat16_file(data: bytes, path: list[bytes]) -> bytes:
    partition = ROOT_LBA * SECTOR
    boot = data[partition:partition + SECTOR]
    sectors_per_cluster = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    root_entries = struct.unpack_from("<H", boot, 17)[0]
    sectors_per_fat = struct.unpack_from("<H", boot, 22)[0]
    root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR
    fat_start = partition + reserved * SECTOR
    root_start = partition + (reserved + fats * sectors_per_fat) * SECTOR
    data_start = root_start + root_sectors * SECTOR
    cluster_bytes = sectors_per_cluster * SECTOR
    first_fat = data[fat_start:fat_start + sectors_per_fat * SECTOR]
    second_fat = data[fat_start + sectors_per_fat * SECTOR:
                      fat_start + 2 * sectors_per_fat * SECTOR]
    if first_fat != second_fat:
        fail("FAT16 copies differ")

    def chain(first: int) -> bytes:
        output = bytearray()
        cluster = first
        visited: set[int] = set()
        while 2 <= cluster < 0xfff8:
            if cluster in visited:
                fail("FAT16 cluster chain loops")
            visited.add(cluster)
            offset = data_start + (cluster - 2) * cluster_bytes
            output += data[offset:offset + cluster_bytes]
            if cluster * 2 + 2 > len(first_fat):
                fail("FAT16 cluster lies outside the FAT")
            cluster = struct.unpack_from("<H", first_fat, cluster * 2)[0]
        if cluster < 0xfff8:
            fail("FAT16 cluster chain has an invalid terminator")
        return bytes(output)

    directory = data[root_start:root_start + root_entries * 32]
    entry = None
    for component_index, component in enumerate(path):
        entry = None
        for offset in range(0, len(directory), 32):
            candidate = directory[offset:offset + 32]
            if candidate[0] == 0:
                break
            if candidate[0] != 0xe5 and candidate[:11] == component:
                entry = candidate
                break
        if entry is None:
            fail("missing FAT16 path /" + "/".join(
                item.decode("ascii").rstrip() for item in path))
        cluster = struct.unpack_from("<H", entry, 26)[0]
        if component_index + 1 != len(path):
            if (entry[11] & 0x10) == 0:
                fail("FAT16 path component is not a directory")
            directory = chain(cluster)
    assert entry is not None
    if (entry[11] & 0x10) != 0:
        fail("FAT16 path resolves to a directory")
    size = struct.unpack_from("<I", entry, 28)[0]
    return chain(struct.unpack_from("<H", entry, 26)[0])[:size]


def check(args: argparse.Namespace) -> None:
    data = args.image.read_bytes()
    if len(data) != IMAGE_SECTORS * SECTOR:
        fail(f"unexpected image size {len(data)}")
    if data[:8] != b"X68SCSI1":
        fail("missing X68SCSI1 disk mark")
    if struct.unpack_from(">I", data, 10)[0] != IMAGE_SECTORS:
        fail("disk mark sector count is inconsistent")
    if data[2048:2052] != b"X68K":
        fail("missing X68K partition header")
    root = 2048 + 16
    if data[root:root + 8] != b"ZEDBSD  " or data[root + 8] != 0:
        fail("partition 1 is not the bootable zedBSD root")
    start1024 = int.from_bytes(data[root + 9:root + 12], "big")
    size1024 = struct.unpack_from(">I", data, root + 12)[0]
    if start1024 * 2 != ROOT_LBA or (start1024 + size1024) * 2 > IMAGE_SECTORS:
        fail("partition 1 has an invalid 1024-byte extent")

    manifest = struct.unpack_from(">IHH14I", data, MANIFEST_LBA * SECTOR)
    if manifest[0:3] != (MANIFEST_MAGIC, 1, 64):
        fail("invalid raw manifest header")
    (stage2_lba, stage2_bytes, stage2_load, stage2_entry, stage2_crc,
     kernel_lba, kernel_bytes, kernel_crc, root_lba, root_sectors,
     ram_bytes, flags, reserved0, reserved1) = manifest[3:]
    if stage2_lba <= MANIFEST_LBA or not stage2_bytes or \
            stage2_load != 0x20000 or stage2_entry != 0x20000:
        fail("invalid stage 2 manifest extent/address")
    if root_lba != ROOT_LBA or root_sectors != IMAGE_SECTORS - ROOT_LBA:
        fail("manifest root extent is inconsistent")
    if ram_bytes < 4 * 1024 * 1024 or ram_bytes > 12 * 1024 * 1024 or \
            flags or reserved0 or reserved1:
        fail("manifest RAM/flags/reserved fields are invalid")
    stage2_end = stage2_lba * SECTOR + stage2_bytes
    kernel_end = kernel_lba * SECTOR + kernel_bytes
    if stage2_end > kernel_lba * SECTOR or kernel_end > ROOT_LBA * SECTOR:
        fail("raw payload extents overlap")
    stage2 = data[stage2_lba * SECTOR:stage2_end]
    kernel = data[kernel_lba * SECTOR:kernel_end]
    if binascii.crc32(stage2) & 0xFFFFFFFF != stage2_crc:
        fail("stage 2 CRC32 mismatch")
    if binascii.crc32(kernel) & 0xFFFFFFFF != kernel_crc:
        fail("kernel CRC32 mismatch")
    if len(kernel) < 52 or kernel[:7] != b"\x7fELF\x01\x02\x01" or \
            struct.unpack_from(">H", kernel, 18)[0] != 4:
        fail("raw kernel is not ELF32/MSB/EM_68K")

    stage1_source = read_source(args.stage1)
    stage2_source = read_source(args.stage2)
    kernel_source = read_source(args.kernel)
    shell_source = read_source(args.shell)
    if stage1_source is not None:
        if len(stage1_source) > 1024 or data[1024:1024 + len(stage1_source)] != stage1_source:
            fail("stage 1 differs from its source")
        if any(data[1024 + len(stage1_source):2048]):
            fail("stage 1 padding is not zero")
    if stage2_source is not None and stage2 != stage2_source:
        fail("stage 2 differs from its source")
    if kernel_source is not None and kernel != kernel_source:
        fail("kernel differs from its source")

    boot = data[ROOT_LBA * SECTOR:(ROOT_LBA + 1) * SECTOR]
    if struct.unpack_from("<H", boot, 11)[0] != SECTOR or boot[13] == 0 or \
            boot[16] != 2 or struct.unpack_from("<H", boot, 22)[0] == 0 or \
            boot[54:62] != b"FAT16   " or boot[510:512] != b"\x55\xaa":
        fail("root partition does not contain the expected FAT16 BPB")
    if shell_source is not None:
        installed_shell = fat16_file(
            data, [b"X68K       ", b"BIN        ", b"SH         "])
        if installed_shell != shell_source:
            fail("/x68k/bin/sh differs from its source")
        if len(installed_shell) < 52 or \
                installed_shell[:7] != b"\x7fELF\x01\x02\x01" or \
                struct.unpack_from(">H", installed_shell, 18)[0] != 4:
            fail("/x68k/bin/sh is not ELF32/MSB/EM_68K")

    forbidden = (b"IPLROM", b"CGROM", b"X68BIOS3.LZH")
    if any(token in data for token in forbidden):
        fail("image appears to contain a ROM archive or ROM payload marker")
    print("X68k image check: PASS "
          f"(stage2={stage2_bytes} bytes, kernel={kernel_bytes} bytes, "
          f"sha256={hashlib.sha256(data).hexdigest()})")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", type=Path)
    parser.add_argument("--stage2", type=Path)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()
