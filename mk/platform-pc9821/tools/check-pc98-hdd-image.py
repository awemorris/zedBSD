#!/usr/bin/env python3
"""Validate a PC-98 HDD disk image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR = 512
HEADS = 8
SECTORS_PER_TRACK = 17
CYLINDER_SECTORS = HEADS * SECTORS_PER_TRACK  # 136 sectors
CYLINDER_BYTES = CYLINDER_SECTORS * SECTOR


def fail(message: str) -> None:
    raise SystemExit(f"check-pc98-hdd-image: {message}")


def extract_fat_file(image_path: Path, offset_bytes: int, name: str) -> bytes:
    with tempfile.TemporaryDirectory() as work:
        out = Path(work) / "extracted"
        subprocess.run(["mcopy", "-n", "-i", f"{image_path}@@{offset_bytes}",
                        f"::{name}", str(out)], check=True,
                       stdout=subprocess.DEVNULL)
        return out.read_bytes()


def same_fat_file(image_path: Path, offset_bytes: int, name: str,
                  source_path: Path) -> None:
    extracted = extract_fat_file(image_path, offset_bytes, name)
    if hashlib.sha256(extracted).digest() != hashlib.sha256(source_path.read_bytes()).digest():
        fail(f"/{name} on FAT partition differs from {source_path}")


def compare_extent(image_file, offset_bytes: int, source_path: Path) -> None:
    image_file.seek(offset_bytes)
    with source_path.open("rb") as src:
        while chunk := src.read(1024 * 1024):
            data = image_file.read(len(chunk))
            if data != chunk:
                fail(f"image extent at {offset_bytes} differs from {source_path}")


def chs_to_lba(chs_bytes: bytes) -> int:
    sec, head, cyl = struct.unpack("<BBH", chs_bytes)
    return (cyl * HEADS + head) * SECTORS_PER_TRACK + sec


def check(args: argparse.Namespace) -> None:
    size = args.image.stat().st_size
    if size % SECTOR:
        fail("image size is not sector-aligned")
    if (size // SECTOR) % CYLINDER_SECTORS != 0:
        fail("image size is not cylinder-aligned")

    with args.image.open("rb") as f:
        # Check Sector 0: IPL
        s0 = f.read(SECTOR)
        if len(s0) != 512 or s0[510:512] != b"\x55\xaa":
            fail("missing boot signature 0x55AA at sector 0")
        if s0[4:8] != b"IPL1":
            fail("missing IPL1 signature at sector 0 offsets 4..7")
        if args.stage1 is not None:
            expected_s1 = args.stage1.read_bytes()
            if s0 != expected_s1:
                fail("sector 0 differs from stage1 input")

        # Check Sector 1: PC-98 Partition Table
        pt = f.read(SECTOR)
        if len(pt) != 512:
            fail("short read of partition table")

        # Entry 0: BOOT
        e0 = pt[0:32]
        mid0, sid0 = e0[0], e0[1]
        if mid0 != 0xa1 or sid0 != 0x91:
            fail(f"partition 1 mid/sid is {hex(mid0)}/{hex(sid0)}, expected 0xa1/0x91")
        start_lba0 = chs_to_lba(e0[4:8])
        data_lba0 = chs_to_lba(e0[8:12])
        end_lba0 = chs_to_lba(e0[12:16])
        if start_lba0 != data_lba0 or start_lba0 < 2048 or start_lba0 % CYLINDER_SECTORS != 0:
            fail("partition 1 start is invalid")
        name0 = e0[16:32].decode("ascii", errors="replace").strip()
        if name0 != "BOOT":
            fail(f"partition 1 name is {name0!r}, expected 'BOOT'")

        boot_start = start_lba0

        # Entry 1: ROOT
        e1 = pt[32:64]
        mid1, sid1 = e1[0], e1[1]
        if mid1 != 0x21 or sid1 != 0x01:
            fail(f"partition 2 mid/sid is {hex(mid1)}/{hex(sid1)}, expected 0x21/0x01")
        start_lba1 = chs_to_lba(e1[4:8])
        data_lba1 = chs_to_lba(e1[8:12])
        end_lba1 = chs_to_lba(e1[12:16])
        if start_lba1 != data_lba1 or start_lba1 != end_lba0 + 1:
            fail("partition 2 does not immediately follow partition 1")
        name1 = e1[16:32].decode("ascii", errors="replace").strip()
        if name1 not in ("ROOT", "zedBSD-root"):
            fail(f"partition 2 name is {name1!r}, expected 'ROOT' or 'zedBSD-root'")

        root_start = start_lba1

        # Entry 2: SWAP
        e2 = pt[64:96]
        mid2, sid2 = e2[0], e2[1]
        if mid2 != 0x21 or sid2 not in (0x01, 0x02):
            fail(f"partition 3 mid/sid is {hex(mid2)}/{hex(sid2)}, expected 0x21/0x02")
        start_lba2 = chs_to_lba(e2[4:8])
        data_lba2 = chs_to_lba(e2[8:12])
        end_lba2 = chs_to_lba(e2[12:16])
        if start_lba2 != data_lba2 or start_lba2 != end_lba1 + 1:
            fail("partition 3 does not immediately follow partition 2")
        name2 = e2[16:32].decode("ascii", errors="replace").strip()
        if name2 not in ("SWAP", "zedBSD-swap"):
            fail(f"partition 3 name is {name2!r}, expected 'SWAP' or 'zedBSD-swap'")

        swap_start = start_lba2

        # Entries 3..15 must be empty
        if any(pt[96:]):
            fail("partition entries 4..16 are not empty")

        # Check Sector 2..: Stage 2
        f.seek(2 * SECTOR)
        s2_sig = f.read(512)
        if s2_sig[510:512] != b"\x55\xaa":
            fail("missing Stage 2 signature 0x55AA at LBA 2")
        if args.stage2 is not None:
            expected_s2 = args.stage2.read_bytes()
            f.seek(2 * SECTOR)
            actual_s2 = f.read(len(expected_s2))
            if actual_s2 != expected_s2:
                fail("LBA 2 Stage 2 differs from input")

        # Check BOOT partition
        f.seek(boot_start * SECTOR)
        bpb = f.read(512)
        sec_per_fat = struct.unpack_from("<H", bpb, 22)[0]
        if sec_per_fat == 0:
            fail("BOOT partition is FAT32, expected FAT16")
        if struct.unpack_from("<H", bpb, 11)[0] != 1024:
            fail("BOOT partition FAT16 must use 1024-byte logical sectors")

        if args.partition_pbr is not None:
            expected_pbr = args.partition_pbr.read_bytes()
            f.seek(boot_start * SECTOR)
            actual_pbr = bytearray(f.read(len(expected_pbr)))
            # check jump
            if actual_pbr[0:3] != expected_pbr[0:3]:
                fail("partition PBR jump instruction differs")
            # check code after BPB
            if actual_pbr[0x3E:510] != expected_pbr[0x3E:510]:
                fail("partition PBR first-stage code differs")
            if actual_pbr[510:512] != b"\x55\xaa":
                fail("partition PBR signature differs")
            if len(expected_pbr) > 512:
                if actual_pbr[512:2048] != expected_pbr[512:2048]:
                    fail("partition PBR continuation differs")

        boot_offset = boot_start * SECTOR
        if args.bootzbsd is not None:
            same_fat_file(args.image, boot_offset, "BOOTZBSD.EXE", args.bootzbsd)
        if args.kernel is not None:
            same_fat_file(args.image, boot_offset, "vmunix", args.kernel)
        if args.zedbsd_config is not None:
            same_fat_file(args.image, boot_offset, "BOOTZBSD.CFG", args.zedbsd_config)
        if args.logo is not None and args.logo.is_file():
            same_fat_file(args.image, boot_offset, "logo.ppm", args.logo)

        # Check UFS root
        if args.ufs_root is not None:
            compare_extent(f, root_start * SECTOR, args.ufs_root)

        # Check Swap
        if args.swap_image is not None:
            compare_extent(f, swap_start * SECTOR, args.swap_image)

    print("PC-98 HDD image check: PASS")


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate PC-98 HDD image")
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--zedbsd-config", type=Path)
    parser.add_argument("--logo", type=Path)
    parser.add_argument("--ufs-root", type=Path)
    parser.add_argument("--swap-image", "--swapfile", dest="swap_image", type=Path)
    parser.add_argument("--stage1", type=Path)
    parser.add_argument("--stage2", type=Path)
    parser.add_argument("--partition-pbr", type=Path)
    parser.add_argument("--bootzbsd", type=Path)
    parser.add_argument("image", type=Path)

    args = parser.parse_args()
    check(args)


if __name__ == "__main__":
    main()
